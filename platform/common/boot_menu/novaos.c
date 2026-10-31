/*
 * NovaOS: a small touch environment that runs inside LK.
 *
 * It is not an operating system in the Android/Linux sense (LK has no
 * processes, no filesystem drivers for apps and no modem/GPU/Wi-Fi), but it
 * gives the bootloader a real home screen with applications:
 *
 *   Info      real device readings (lock state, serial, preloader, chip
 *             revision, battery, charger, uptime, screen, touch, lock)
 *   Storage   the partition table read from the storage device
 *   Calc      four-function calculator (fixed point, 4 decimals)
 *   Paint     draw with the finger, six colours
 *   Clock     uptime and a stopwatch
 *   Snake     the existing game
 *   Security  the existing boot menu password settings
 *   Power     reboot, recovery, power off (two taps to confirm)
 *
 * Everything is drawn with the plain video_* primitives and driven by the
 * same touch polling as the boot menu.  POWER leaves an app, BACK too.
 */

#include <string.h>
#include <printf.h>
#include <sys/types.h>
#include <platform/mt_typedefs.h>
#include <platform/boot_mode.h>
#include <platform/mtk_key.h>
#include <platform/mt_gpt.h>
#include <platform/mt_rtc.h>
#include <platform/mtk_wdt.h>
#include <platform/timer.h>
#include <target/cust_key.h>
#include <video.h>
#include <boot_menu.h>
#include "lock_store.h"
#ifdef MTK_TPD_SUPPORT
#include <dev/tpd.h>
#include <target/cust_tpd.h>
#endif

#ifndef MTK_PMIC_PWR_KEY
#define MTK_PMIC_PWR_KEY 8
#endif

/* Sources shared with the fastboot screen (real hardware readings). */
extern char sn_buf[];
extern int get_bat_volt(int times);
extern unsigned get_unlocked_status(void);
extern kal_bool pmic_chrdet_status(void);
extern int partition_get_name(int index, char **p_name);
extern u64 partition_get_size(int index);

/* NovaLK palette, 0x00RRGGBB. */
#define C_BG		0x0A0F1A
#define C_SURFACE	0x141E33
#define C_SURFACE_SEL	0x0F3A52
#define C_BORDER	0x26385A
#define C_ACCENT	0x2ED3FF
#define C_TEXT		0xF2F7FF
#define C_DIM		0x8FA6C4
#define C_ERROR		0xFF5A6E
#define C_OK		0x4DE08A

struct rect {
	int x, y, w, h;
};

static int W, H;
static int g_touch;
static struct rect g_back;

/* ------------------------------------------------------------------ */
/* drawing helpers                                                    */
/* ------------------------------------------------------------------ */

static int inside(const struct rect *r, int x, int y)
{
	return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

static void text_center(int cx, int y, int scale, u32 fg, u32 bg,
			const char *s)
{
	video_draw_text(cx - video_text_width(scale, s) / 2, y, scale, fg, bg,
			s);
}

static void text_right(int rx, int y, int scale, u32 fg, u32 bg,
		       const char *s)
{
	video_draw_text(rx - video_text_width(scale, s), y, scale, fg, bg, s);
}

static void draw_frame(const struct rect *r, u32 fill, u32 frame)
{
	video_fill_rect(fill, r->x, r->y, r->w, r->h);
	video_fill_rect(frame, r->x, r->y, r->w, 2);
	video_fill_rect(frame, r->x, r->y + r->h - 2, r->w, 2);
	video_fill_rect(frame, r->x, r->y, 2, r->h);
	video_fill_rect(frame, r->x + r->w - 2, r->y, 2, r->h);
}

static void draw_button(const struct rect *r, const char *label, int scale,
			int pressed, u32 fg_override)
{
	u32 fill = pressed ? C_SURFACE_SEL : C_SURFACE;
	u32 frame = pressed ? C_ACCENT : C_BORDER;
	u32 fg = fg_override ? fg_override : (pressed ? C_ACCENT : C_TEXT);

	draw_frame(r, fill, frame);
	text_center(r->x + r->w / 2, r->y + (r->h - 16 * scale) / 2, scale, fg,
		    fill, label);
}

static void tap_feedback(const struct rect *r, const char *label, int scale)
{
	draw_button(r, label, scale, 1, 0);
	video_refresh();
	mdelay(70);
	draw_button(r, label, scale, 0, 0);
	video_refresh();
}

static void draw_thick_line(int x0, int y0, int x1, int y1, u32 color, int t)
{
	int dx = x1 - x0;
	int dy = y1 - y0;
	int adx = dx < 0 ? -dx : dx;
	int ady = dy < 0 ? -dy : dy;
	int n = (adx > ady ? adx : ady) / (t / 2 > 0 ? t / 2 : 1) + 1;
	int i;

	for (i = 0; i <= n; i++)
		video_fill_rect(color, x0 + dx * i / n - t / 2,
				y0 + dy * i / n - t / 2, t, t);
}

/* ------------------------------------------------------------------ */
/* input (same debounce as the lock screens)                          */
/* ------------------------------------------------------------------ */

#define EV_TAP		0x01
#define EV_PWR		0x02

static struct {
	int down, miss, pwr;
} in;

static void in_init(void)
{
	in.down = 1;		/* ignore a finger already on the glass */
	in.miss = 0;
	in.pwr = mtk_detect_key(MTK_PMIC_PWR_KEY);
}

static int in_poll(int *x, int *y, int *touching)
{
	int ev = 0;
	int pwr = mtk_detect_key(MTK_PMIC_PWR_KEY);

	if (pwr && !in.pwr)
		ev |= EV_PWR;
	in.pwr = pwr;

	*touching = 0;

#ifdef MTK_TPD_SUPPORT
	if (g_touch) {
		int tx = 0, ty = 0;

		if (tpd_read(&tx, &ty) == 1) {
			*x = tx * W / TPD_PANEL_WIDTH;
			*y = ty * H / TPD_PANEL_HEIGHT;
			*touching = 1;
			in.miss = 0;
			if (!in.down) {
				in.down = 1;
				ev |= EV_TAP;
			}
		} else if (in.down && ++in.miss >= 4) {
			in.down = 0;
		}
	}
#else
	(void)x;
	(void)y;
#endif

	return ev;
}

/* ------------------------------------------------------------------ */
/* common app frame                                                   */
/* ------------------------------------------------------------------ */

static void app_begin(const char *title)
{
	video_fill_rect(C_BG, 0, 0, W, H);

	g_back.x = 24;
	g_back.y = 24;
	g_back.w = 170;
	g_back.h = 64;
	draw_button(&g_back, "BACK", 2, 0, 0);

	text_center(W / 2 + 80, 36, 3, C_ACCENT, C_BG, title);
	video_fill_rect(C_BORDER, 24, 104, W - 48, 2);
}

/* 1 when the app should close (BACK tapped or POWER pressed). */
static int app_back(int ev, int x, int y)
{
	if (ev & EV_PWR)
		return 1;

	if ((ev & EV_TAP) && inside(&g_back, x, y)) {
		tap_feedback(&g_back, "BACK", 2);
		return 1;
	}

	return 0;
}

/* Seconds since the platform timer started. */
static u32 uptime_s(void)
{
	return (u32)(get_timer(0) / 1000);
}

static void fmt_hms(char *buf, int len, u32 s)
{
	snprintf(buf, len, "%02u:%02u:%02u", (unsigned)(s / 3600),
		 (unsigned)((s / 60) % 60), (unsigned)(s % 60));
}

static void fmt_size(char *buf, int len, u64 b)
{
	if (b >= (1ULL << 30))
		snprintf(buf, len, "%u.%u GB", (unsigned)(b >> 30),
			 (unsigned)(((b * 10) >> 30) % 10));
	else if (b >= (1ULL << 20))
		snprintf(buf, len, "%u MB", (unsigned)(b >> 20));
	else
		snprintf(buf, len, "%u KB", (unsigned)(b >> 10));
}

/* ------------------------------------------------------------------ */
/* app: Info                                                          */
/* ------------------------------------------------------------------ */

struct info_row {
	const char *label;
	char value[32];
	u32 color;
};

static void info_add(struct info_row *rows, int *n, const char *label,
		     const char *value, u32 color)
{
	if (*n >= 14)
		return;

	rows[*n].label = label;
	snprintf(rows[*n].value, sizeof(rows[*n].value), "%s", value);
	rows[*n].color = color;
	(*n)++;
}

static void info_collect(struct info_row *rows, int *count)
{
	char b[40];
	unsigned unlocked;
	unsigned hw;
	int mv;
	int n = 0;

	unlocked = get_unlocked_status();
	if (unlocked <= 1)
		info_add(rows, &n, "STATE", unlocked ? "UNLOCKED" : "LOCKED",
			 unlocked ? C_ACCENT : C_TEXT);

	if (sn_buf[0] && strcmp(sn_buf, "0123456789ABCDEF") != 0)
		info_add(rows, &n, "SERIAL", sn_buf, C_TEXT);

	if (g_boot_arg) {
		int i;

		for (i = 0; i < (int)sizeof(b) - 1 &&
			    i < (int)sizeof(g_boot_arg->pl_version) &&
			    g_boot_arg->pl_version[i] >= 0x20 &&
			    g_boot_arg->pl_version[i] < 0x7f; i++)
			b[i] = (char)g_boot_arg->pl_version[i];
		b[i] = '\0';
		if (b[0])
			info_add(rows, &n, "BOOTLOADER", b, C_TEXT);
	}

	hw = mt_get_chip_hw_ver();
	if (hw) {
		snprintf(b, sizeof(b), "0x%04X", hw);
		info_add(rows, &n, "HW REV", b, C_TEXT);
	}

	hw = mt_get_chip_sw_ver();
	snprintf(b, sizeof(b), "0x%04X", hw);
	info_add(rows, &n, "SW REV", b, C_TEXT);

	mv = get_bat_volt(1);
	if (mv > 0) {
		snprintf(b, sizeof(b), "%d mV", mv);
		info_add(rows, &n, "BATTERY", b, C_TEXT);
	}

	info_add(rows, &n, "CHARGER",
		 pmic_chrdet_status() == KAL_TRUE ? "CONNECTED" : "NONE",
		 C_TEXT);

	fmt_hms(b, sizeof(b), uptime_s());
	info_add(rows, &n, "UPTIME", b, C_TEXT);

	snprintf(b, sizeof(b), "%d x %d", W, H);
	info_add(rows, &n, "SCREEN", b, C_TEXT);

#ifdef MTK_TPD_SUPPORT
	if (tpd_available()) {
		snprintf(b, sizeof(b), "%s%s", tpd_name(),
			 tpd_get_diag()->handshake_ok ? "  FW OK" : "");
		info_add(rows, &n, "TOUCH", b, C_OK);
	} else {
		info_add(rows, &n, "TOUCH", "NOT AVAILABLE", C_ERROR);
	}
#endif

	info_add(rows, &n, "MENU LOCK",
		 lockstore_enabled() ?
		 (lockstore_type() == LOCK_PATTERN ? "PATTERN" : "PIN") : "OFF",
		 lockstore_enabled() ? C_OK : C_DIM);

	*count = n;
}

static void info_draw(void)
{
	struct info_row rows[14];
	int n, i;
	int y = 140;

	info_collect(rows, &n);

	video_fill_rect(C_BG, 0, 120, W, H - 120);
	for (i = 0; i < n; i++) {
		video_draw_text(40, y, 2, C_DIM, C_BG, rows[i].label);
		text_right(W - 40, y, 2, rows[i].color, C_BG, rows[i].value);
		video_fill_rect(C_SURFACE, 40, y + 38, W - 80, 1);
		y += 56;
	}
	video_refresh();
}

static void app_info(void)
{
	int x = 0, y = 0, touching;
	ulong last = get_timer(0);

	app_begin("INFO");
	info_draw();

	while (1) {
		int ev = in_poll(&x, &y, &touching);

		if (app_back(ev, x, y))
			return;

		if (get_timer(last) >= 1000) {
			last = get_timer(0);
			info_draw();
		}
		mdelay(10);
	}
}

/* ------------------------------------------------------------------ */
/* app: Storage (partition table)                                     */
/* ------------------------------------------------------------------ */

#define PART_ROW_H	54
#define PART_MAX	128

static void app_storage(void)
{
	static char names[PART_MAX][24];
	static u64 sizes[PART_MAX];
	struct rect up, down;
	int count = 0;
	int top = 0;
	int rows_vis;
	int list_y = 190;
	int drag_y = 0, dragging = 0;
	int x = 0, y = 0, touching;
	int i;
	char line[40];
	char tot[24];
	u64 total = 0;

	for (i = 0; i < PART_MAX; i++) {
		char *nm = NULL;

		if (partition_get_name(i, &nm) != 0 || !nm)
			continue;

		snprintf(names[count], sizeof(names[count]), "%s", nm);
		sizes[count] = partition_get_size(i);
		if ((s64)sizes[count] < 0)
			sizes[count] = 0;
		total += sizes[count];
		count++;
	}

	up.w = down.w = 250;
	up.h = down.h = 80;
	up.y = down.y = H - 130;
	up.x = W / 2 - 250 - 15;
	down.x = W / 2 + 15;

	rows_vis = (up.y - 20 - list_y) / PART_ROW_H;

	app_begin("STORAGE");

	fmt_size(tot, sizeof(tot), total);
	snprintf(line, sizeof(line), "%d partitions   %s", count, tot);
	text_center(W / 2, 122, 2, C_DIM, C_BG, line);

	while (1) {
		int redraw = 1;

		while (redraw) {
			int r;

			redraw = 0;
			video_fill_rect(C_BG, 0, list_y - 8, W,
					up.y - 10 - (list_y - 8));

			for (r = 0; r < rows_vis && top + r < count; r++) {
				int idx = top + r;
				int ry = list_y + r * PART_ROW_H;

				video_draw_text(40, ry, 2, C_TEXT, C_BG,
						names[idx]);
				fmt_size(line, sizeof(line), sizes[idx]);
				text_right(W - 40, ry, 2, C_DIM, C_BG, line);
				video_fill_rect(C_SURFACE, 40, ry + 38,
						W - 80, 1);
			}

			draw_button(&up, "UP", 2, 0, top > 0 ? 0 : C_BORDER);
			draw_button(&down, "DOWN", 2, 0,
				    top + rows_vis < count ? 0 : C_BORDER);
			video_refresh();
		}

		while (!redraw) {
			int ev = in_poll(&x, &y, &touching);

			if (app_back(ev, x, y))
				return;

			if (ev & EV_TAP) {
				dragging = 1;
				drag_y = y;

				if (inside(&up, x, y) && top > 0) {
					top -= rows_vis - 1;
					if (top < 0)
						top = 0;
					redraw = 1;
				} else if (inside(&down, x, y) &&
					   top + rows_vis < count) {
					top += rows_vis - 1;
					if (top + rows_vis > count)
						top = count - rows_vis;
					if (top < 0)
						top = 0;
					redraw = 1;
				}
			} else if (touching && dragging) {
				/* Drag the list: one row per row height. */
				int dy = y - drag_y;

				if (dy <= -PART_ROW_H &&
				    top + rows_vis < count) {
					top++;
					drag_y = y;
					redraw = 1;
				} else if (dy >= PART_ROW_H && top > 0) {
					top--;
					drag_y = y;
					redraw = 1;
				}
			} else if (!touching) {
				dragging = 0;
			}

			mdelay(10);
		}
	}
}

/* ------------------------------------------------------------------ */
/* app: Calculator (fixed point, 4 decimals)                          */
/* ------------------------------------------------------------------ */

#define CALC_SCALE	10000LL
#define CALC_MAX	99999999999LL	/* 7 integer digits + 4 decimals */

struct calc_key {
	const char *label;
	int col, row, cs, rs;	/* grid cell and span */
	char code;
};

static const struct calc_key calc_keys[] = {
	{ "C",   0, 0, 1, 1, 'C' }, { "DEL", 1, 0, 1, 1, 'D' },
	{ "/",   2, 0, 1, 1, '/' }, { "*",   3, 0, 1, 1, '*' },
	{ "7",   0, 1, 1, 1, '7' }, { "8",   1, 1, 1, 1, '8' },
	{ "9",   2, 1, 1, 1, '9' }, { "-",   3, 1, 1, 1, '-' },
	{ "4",   0, 2, 1, 1, '4' }, { "5",   1, 2, 1, 1, '5' },
	{ "6",   2, 2, 1, 1, '6' }, { "+",   3, 2, 1, 1, '+' },
	{ "1",   0, 3, 1, 1, '1' }, { "2",   1, 3, 1, 1, '2' },
	{ "3",   2, 3, 1, 1, '3' }, { "=",   3, 3, 1, 2, '=' },
	{ "0",   0, 4, 2, 1, '0' }, { ".",   2, 4, 1, 1, '.' },
};

#define CALC_NKEYS	((int)(sizeof(calc_keys) / sizeof(calc_keys[0])))

/* Parse "123.45" (as typed) into fixed point. */
static s64 calc_parse(const char *s)
{
	s64 ip = 0, fp = 0;
	int fd = 0;
	int dot = 0;
	int neg = 0;

	if (*s == '-') {
		neg = 1;
		s++;
	}

	for (; *s; s++) {
		if (*s == '.') {
			dot = 1;
		} else if (!dot) {
			ip = ip * 10 + (*s - '0');
		} else if (fd < 4) {
			fp = fp * 10 + (*s - '0');
			fd++;
		}
	}
	while (fd++ < 4)
		fp *= 10;

	return neg ? -(ip * CALC_SCALE + fp) : ip * CALC_SCALE + fp;
}

static void calc_format(char *buf, int len, s64 v)
{
	char frac[8];
	s64 a = v < 0 ? -v : v;
	int n;

	snprintf(frac, sizeof(frac), "%04d", (int)(a % CALC_SCALE));
	n = (int)strlen(frac);
	while (n > 0 && frac[n - 1] == '0')
		frac[--n] = '\0';

	if (n)
		snprintf(buf, len, "%s%d.%s", v < 0 ? "-" : "",
			 (int)(a / CALC_SCALE), frac);
	else
		snprintf(buf, len, "%s%d", v < 0 ? "-" : "",
			 (int)(a / CALC_SCALE));
}

/* Returns 0 on success, -1 on overflow / division by zero. */
static int calc_apply(s64 a, char op, s64 b, s64 *out)
{
	s64 r;

	switch (op) {
	case '+':
		r = a + b;
		break;
	case '-':
		r = a - b;
		break;
	case '*':
		/* (a / S) * b + ((a % S) * b) / S keeps within 64 bit. */
		r = (a / CALC_SCALE) * b + ((a % CALC_SCALE) * b) / CALC_SCALE;
		break;
	case '/':
		if (b == 0)
			return -1;
		r = (a * CALC_SCALE) / b;
		break;
	default:
		r = b;
		break;
	}

	if (r > CALC_MAX || r < -CALC_MAX)
		return -1;

	*out = r;

	return 0;
}

static void app_calc(void)
{
	struct rect keyr[CALC_NKEYS];
	struct rect disp;
	char entry[16];
	char shown[24];
	s64 acc = 0;
	char op = 0;
	int fresh = 1;		/* next digit starts a new number */
	int error = 0;
	int x = 0, y = 0, touching;
	int cw = 160, ch = 140, gap = 16;
	int gx = (W - (4 * cw + 3 * gap)) / 2;
	int gy = 420;
	int i;

	strcpy(entry, "0");

	for (i = 0; i < CALC_NKEYS; i++) {
		keyr[i].x = gx + calc_keys[i].col * (cw + gap);
		keyr[i].y = gy + calc_keys[i].row * (ch + gap);
		keyr[i].w = calc_keys[i].cs * cw + (calc_keys[i].cs - 1) * gap;
		keyr[i].h = calc_keys[i].rs * ch + (calc_keys[i].rs - 1) * gap;
	}

	disp.x = gx;
	disp.y = 150;
	disp.w = 4 * cw + 3 * gap;
	disp.h = 230;

	app_begin("CALC");
	for (i = 0; i < CALC_NKEYS; i++)
	{
		char c = calc_keys[i].code;
		int plain = (c >= '0' && c <= '9') || c == '.';

		draw_button(&keyr[i], calc_keys[i].label, 4, 0,
			    plain ? 0 : (c == '=' ? C_ACCENT : C_DIM));
	}

	while (1) {
		int redraw = 1;

		while (redraw) {
			int sc = 6;
			char opstr[2];

			redraw = 0;
			draw_frame(&disp, C_SURFACE, C_BORDER);

			if (error)
				snprintf(shown, sizeof(shown), "ERROR");
			else
				snprintf(shown, sizeof(shown), "%s", entry);

			if ((int)strlen(shown) > 11)
				sc = 4;
			text_right(disp.x + disp.w - 24, disp.y + 90, sc,
				   error ? C_ERROR : C_TEXT, C_SURFACE, shown);

			if (op) {
				opstr[0] = op;
				opstr[1] = '\0';
				video_draw_text(disp.x + 24, disp.y + 24, 3,
						C_ACCENT, C_SURFACE, opstr);
			}
			video_refresh();
		}

		while (!redraw) {
			int ev = in_poll(&x, &y, &touching);

			if (app_back(ev, x, y))
				return;

			if (!(ev & EV_TAP)) {
				mdelay(10);
				continue;
			}

			for (i = 0; i < CALC_NKEYS; i++) {
				char c;

				if (!inside(&keyr[i], x, y))
					continue;

				c = calc_keys[i].code;
				tap_feedback(&keyr[i], calc_keys[i].label, 4);

				if (c == 'C') {
					strcpy(entry, "0");
					acc = 0;
					op = 0;
					fresh = 1;
					error = 0;
				} else if (error) {
					/* only C clears an error */
				} else if (c == 'D') {
					int n = (int)strlen(entry);

					if (!fresh && n > 1)
						entry[n - 1] = '\0';
					else if (!fresh)
						strcpy(entry, "0");
				} else if ((c >= '0' && c <= '9') || c == '.') {
					int n = (int)strlen(entry);
					const char *dot = strchr(entry, '.');
					int ipd = dot ? (int)(dot - entry) : n;
					int fd = dot ? n - ipd - 1 : 0;

					if (fresh) {
						strcpy(entry, c == '.' ? "0." : "");
						if (c != '.') {
							entry[0] = c;
							entry[1] = '\0';
						}
						fresh = 0;
					} else if (c == '.') {
						if (!dot && n < 12) {
							entry[n] = '.';
							entry[n + 1] = '\0';
						}
					} else if (!dot && ipd >= 7) {
						/* integer part full */
					} else if (dot && fd >= 4) {
						/* decimals full */
					} else if (n < 13) {
						if (strcmp(entry, "0") == 0) {
							entry[0] = c;
						} else {
							entry[n] = c;
							entry[n + 1] = '\0';
						}
					}
				} else {
					/* operator or '=' */
					s64 cur = calc_parse(entry);

					if (op && !fresh) {
						if (calc_apply(acc, op, cur, &acc)) {
							error = 1;
							op = 0;
						}
					} else if (!op) {
						acc = cur;
					}

					if (!error) {
						calc_format(entry, sizeof(entry), acc);
						op = (c == '=') ? 0 : c;
						fresh = 1;
					}
				}

				redraw = 1;
				break;
			}

			mdelay(10);
		}
	}
}

/* ------------------------------------------------------------------ */
/* app: Paint                                                         */
/* ------------------------------------------------------------------ */

static void app_paint(void)
{
	static const u32 pal[6] = {
		C_ACCENT, 0xFFFFFF, C_ERROR, C_OK, 0xFFD24A, 0xD070FF
	};
	struct rect sw[6];
	struct rect clear;
	struct rect canvas;
	int cur = 0;
	int px = 0, py = 0, have_prev = 0, miss = 0;
	int x = 0, y = 0, touching;
	int i;

	app_begin("PAINT");

	canvas.x = 24;
	canvas.y = 120;
	canvas.w = W - 48;
	canvas.h = H - 120 - 220;

	for (i = 0; i < 6; i++) {
		sw[i].w = sw[i].h = 90;
		sw[i].x = 24 + i * ((W - 48 - 90) / 5);
		sw[i].y = H - 190;
	}
	clear.w = 300;
	clear.h = 70;
	clear.x = (W - 300) / 2;
	clear.y = H - 80;

	video_fill_rect(C_SURFACE, canvas.x, canvas.y, canvas.w, canvas.h);

	for (;;) {
		int redraw_pal = 1;
		int ev;

		if (redraw_pal) {
			for (i = 0; i < 6; i++) {
				video_fill_rect(i == cur ? C_TEXT : C_BG,
						sw[i].x - 5, sw[i].y - 5,
						sw[i].w + 10, sw[i].h + 10);
				video_fill_rect(pal[i], sw[i].x, sw[i].y,
						sw[i].w, sw[i].h);
			}
			draw_button(&clear, "CLEAR", 2, 0, 0);
			video_refresh();
		}

		while (1) {
			int select = -1;
			int do_clear = 0;

			ev = in_poll(&x, &y, &touching);

			if (app_back(ev, x, y))
				return;

			if (ev & EV_TAP) {
				for (i = 0; i < 6; i++)
					if (inside(&sw[i], x, y))
						select = i;
				if (inside(&clear, x, y))
					do_clear = 1;
			}

			if (select >= 0 && select != cur) {
				cur = select;
				break;
			}
			if (do_clear) {
				video_fill_rect(C_SURFACE, canvas.x, canvas.y,
						canvas.w, canvas.h);
				video_refresh();
				have_prev = 0;
			}

			if (touching && inside(&canvas, x, y)) {
				miss = 0;
				if (have_prev)
					draw_thick_line(px, py, x, y, pal[cur],
							14);
				else
					video_fill_rect(pal[cur], x - 7, y - 7,
							14, 14);
				px = x;
				py = y;
				have_prev = 1;
				video_refresh();
			} else if (!touching && have_prev && ++miss >= 4) {
				have_prev = 0;
			}

			mdelay(6);
		}
	}
}

/* ------------------------------------------------------------------ */
/* app: Clock (uptime + stopwatch)                                    */
/* ------------------------------------------------------------------ */

static void app_clock(void)
{
	struct rect btn_run, btn_reset;
	int running = 0;
	ulong t_start = 0;
	ulong acc_ms = 0;
	ulong last_draw = 0;
	int x = 0, y = 0, touching;
	char line[24];

	btn_run.w = btn_reset.w = 280;
	btn_run.h = btn_reset.h = 110;
	btn_run.y = btn_reset.y = 900;
	btn_run.x = W / 2 - 280 - 15;
	btn_reset.x = W / 2 + 15;

	app_begin("CLOCK");
	text_center(W / 2, 170, 2, C_DIM, C_BG, "UPTIME");
	text_center(W / 2, 560, 2, C_DIM, C_BG, "STOPWATCH");
	draw_button(&btn_run, "START", 3, 0, 0);
	draw_button(&btn_reset, "RESET", 3, 0, 0);
	video_refresh();

	for (;;) {
		int ev = in_poll(&x, &y, &touching);

		if (app_back(ev, x, y))
			return;

		if (ev & EV_TAP) {
			if (inside(&btn_run, x, y)) {
				if (running) {
					acc_ms += get_timer(t_start);
					running = 0;
				} else {
					t_start = get_timer(0);
					running = 1;
				}
				draw_button(&btn_run, running ? "STOP" : "START",
					    3, 0, 0);
				last_draw = 0;
			} else if (inside(&btn_reset, x, y)) {
				running = 0;
				acc_ms = 0;
				last_draw = 0;
			}
		}

		if (get_timer(last_draw) >= 100) {
			ulong ms = acc_ms + (running ? get_timer(t_start) : 0);

			last_draw = get_timer(0);

			video_fill_rect(C_BG, 0, 220, W, 150);
			fmt_hms(line, sizeof(line), uptime_s());
			text_center(W / 2, 250, 8, C_TEXT, C_BG, line);

			video_fill_rect(C_BG, 0, 610, W, 150);
			snprintf(line, sizeof(line), "%02u:%02u.%u",
				 (unsigned)(ms / 60000),
				 (unsigned)((ms / 1000) % 60),
				 (unsigned)((ms / 100) % 10));
			text_center(W / 2, 640, 8, C_ACCENT, C_BG, line);
			video_refresh();
		}

		mdelay(10);
	}
}

/* ------------------------------------------------------------------ */
/* app: Power                                                         */
/* ------------------------------------------------------------------ */

static void power_do(int idx)
{
	int i;

	if (idx == 1) {
		/* Same marker the fastboot screen uses (see boot_mode.c). */
		Set_RTC_Recovery_Mode(1);
		Set_Clr_RTC_PDN1_bit13(true);
		mtk_arch_reset(1);
		return;
	}

	if (idx == 0) {
		mtk_arch_reset(1);
		return;
	}

	/* Power off; with a charger attached the PMIC cannot stay off. */
	rtc_bbpu_power_down();
	for (i = 0; i < 10; i++) {
		mdelay(100);
		if (pmic_chrdet_status() == KAL_TRUE)
			break;
	}
	mtk_arch_reset(0);
}

static void app_power(void)
{
	static const char *const label[3] = { "REBOOT", "RECOVERY", "POWER OFF" };
	struct rect b[3];
	int armed = -1;
	ulong armed_at = 0;
	int x = 0, y = 0, touching;
	int i;

	for (i = 0; i < 3; i++) {
		b[i].w = 560;
		b[i].h = 140;
		b[i].x = (W - 560) / 2;
		b[i].y = 300 + i * 190;
	}

	app_begin("POWER");
	text_center(W / 2, 170, 2, C_DIM, C_BG, "TAP TWICE TO CONFIRM");

	for (;;) {
		for (i = 0; i < 3; i++)
			draw_button(&b[i], armed == i ? "TAP AGAIN" : label[i], 3,
				    armed == i, armed == i ? C_ERROR : 0);
		video_refresh();

		for (;;) {
			int ev = in_poll(&x, &y, &touching);
			int hit = -1;

			if (app_back(ev, x, y))
				return;

			if (armed >= 0 && get_timer(armed_at) > 3000) {
				armed = -1;
				break;
			}

			if (ev & EV_TAP) {
				for (i = 0; i < 3; i++)
					if (inside(&b[i], x, y))
						hit = i;

				if (hit >= 0 && hit == armed) {
					text_center(W / 2, 1000, 3, C_ERROR, C_BG,
						    "PLEASE WAIT");
					video_refresh();
					power_do(hit);
					armed = -1;
					break;
				}
				if (hit >= 0) {
					armed = hit;
					armed_at = get_timer(0);
					break;
				}
			}

			mdelay(10);
		}
	}
}

/* ------------------------------------------------------------------ */
/* app: Wi-Fi (stage 1: power up the connectivity block)              */
/* ------------------------------------------------------------------ */

static void app_wifi(void)
{
	static char lines[20][46];
	struct rect btn1, btn2;
	const char *title = NULL;
	int count = 0;
	int ran = 0;
	int ok = 0;
	int x = 0, y = 0, touching;
	int i;

	btn1.w = btn2.w = 330;
	btn1.h = btn2.h = 110;
	btn1.y = btn2.y = H - 300;
	btn1.x = W / 2 - 330 - 15;
	btn2.x = W / 2 + 15;

	app_begin("WI-FI");

	for (;;) {
		video_fill_rect(C_BG, 0, 120, W, H - 120);

		if (!ran) {
			text_center(W / 2, 170, 3, C_ERROR, C_BG, "EXPERIMENTAL");
			text_center(W / 2, 250, 2, C_TEXT, C_BG,
				    "STAGE 1  power the block, read ID");
			text_center(W / 2, 300, 2, C_TEXT, C_BG,
				    "STAGE 2  load patches, start MCU");
			text_center(W / 2, 380, 2, C_DIM, C_BG,
				    "Browsing is NOT available yet.");
			text_center(W / 2, 450, 2, C_ERROR, C_BG,
				    "If the screen freezes, hold");
			text_center(W / 2, 490, 2, C_ERROR, C_BG,
				    "POWER for 10 s to reboot.");
		} else {
			text_center(W / 2, 140, 3, ok ? C_OK : C_ERROR, C_BG,
				    title);
			for (i = 0; i < count; i++)
				video_draw_text(24, 200 + i * 38, 2, C_TEXT,
						C_BG, lines[i]);
		}

		draw_button(&btn1, "STAGE 1", 3, 0, 0);
		draw_button(&btn2, "STAGE 2", 3, 0, 0);
		video_refresh();

		for (;;) {
			int ev = in_poll(&x, &y, &touching);
			int run = 0;

			if (app_back(ev, x, y))
				return;

			if (ev & EV_TAP) {
				if (inside(&btn1, x, y)) {
					tap_feedback(&btn1, "STAGE 1", 3);
					run = 1;
				} else if (inside(&btn2, x, y)) {
					tap_feedback(&btn2, "STAGE 2", 3);
					run = 2;
				}
			}

			if (run) {
				text_center(W / 2, H - 400, 2, C_ACCENT, C_BG,
					    "RUNNING...");
				video_refresh();

				count = 0;
				if (run == 1) {
					ok = novalk_consys_probe(lines, 20,
								 &count);
					title = ok ? "CONNSYS ALIVE" :
						     "NO VALID ID";
				} else {
					ok = novalk_consys_stage2(lines, 20,
								  &count);
					title = ok ? "MCU RUNNING" :
						     "MCU NOT READY";
				}
				ran = 1;
				in_init();
				break;
			}

			mdelay(10);
		}
	}
}

/* ------------------------------------------------------------------ */
/* wrappers for existing features                                     */
/* ------------------------------------------------------------------ */

static void app_snake(void)
{
	novalk_snake_play(g_touch);
}

static void app_security(void)
{
	novalk_lock_settings(g_touch);
}

/* ------------------------------------------------------------------ */
/* home screen                                                        */
/* ------------------------------------------------------------------ */

struct app {
	const char *name;
	const char *glyph;
	void (*run)(void);
};

static const struct app apps[] = {
	{ "Info",     "i", app_info },
	{ "Storage",  "D", app_storage },
	{ "Calc",     "=", app_calc },
	{ "Paint",    "P", app_paint },
	{ "Clock",    "C", app_clock },
	{ "Snake",    "S", app_snake },
	{ "Security", "L", app_security },
	{ "Wi-Fi",    "W", app_wifi },
	{ "Power",    "O", app_power },
};

#define NAPPS		((int)(sizeof(apps) / sizeof(apps[0])))
#define GRID_COLS	3

static struct rect tile[NAPPS];
static struct rect btn_exit;

static void home_status(void)
{
	char line[48];
	char up[16];
	int mv = get_bat_volt(1);

	fmt_hms(up, sizeof(up), uptime_s());
	if (mv > 0)
		snprintf(line, sizeof(line), "UP %s    BAT %d mV", up, mv);
	else
		snprintf(line, sizeof(line), "UP %s", up);

	video_fill_rect(C_BG, 0, 170, W, 40);
	text_center(W / 2, 176, 2, C_DIM, C_BG, line);
}

static void home_draw(void)
{
	int nova_w, os_w, x, i;

	video_fill_rect(C_BG, 0, 0, W, H);

	nova_w = video_text_width(5, "Nova");
	os_w = video_text_width(5, "OS");
	x = (W - (nova_w + os_w)) / 2;
	video_draw_text(x, 70, 5, C_ACCENT, C_BG, "Nova");
	video_draw_text(x + nova_w, 70, 5, C_TEXT, C_BG, "OS");

	video_fill_rect(C_ACCENT, W / 2 - 60, 250, 120, 4);

	for (i = 0; i < NAPPS; i++) {
		int gw = video_text_width(6, apps[i].glyph);

		draw_frame(&tile[i], C_SURFACE, C_BORDER);
		video_draw_text(tile[i].x + (tile[i].w - gw) / 2,
				tile[i].y + 28, 6, C_ACCENT, C_SURFACE,
				apps[i].glyph);
		text_center(tile[i].x + tile[i].w / 2,
			    tile[i].y + tile[i].h - 52, 2, C_TEXT, C_SURFACE,
			    apps[i].name);
	}

	draw_button(&btn_exit, "BACK TO MENU", 2, 0, 0);
	text_center(W / 2, H - 70, 2, C_DIM, C_BG,
		    NOVALK_CREDIT_PREFIX NOVALK_CREDIT_NAME);

	home_status();
	video_refresh();
}

void novalk_os_run(int touch_ok)
{
	int x = 0, y = 0, touching;
	int tw, th, gap = 26, mx = 40;
	int i;
	ulong last = 0;

	W = video_screen_width();
	H = video_screen_height();
	if (W <= 0 || H <= 0)
		return;

	g_touch = touch_ok;

	tw = (W - 2 * mx - (GRID_COLS - 1) * gap) / GRID_COLS;
	th = 210;
	for (i = 0; i < NAPPS; i++) {
		tile[i].x = mx + (i % GRID_COLS) * (tw + gap);
		tile[i].y = 300 + (i / GRID_COLS) * (th + gap);
		tile[i].w = tw;
		tile[i].h = th;
	}

	btn_exit.w = 420;
	btn_exit.h = 84;
	btn_exit.x = (W - 420) / 2;
	btn_exit.y = H - 200;

	in_init();
	home_draw();

	for (;;) {
		int ev = in_poll(&x, &y, &touching);

		if (ev & EV_PWR)
			return;

		if (ev & EV_TAP) {
			if (inside(&btn_exit, x, y)) {
				tap_feedback(&btn_exit, "BACK TO MENU", 2);
				return;
			}

			for (i = 0; i < NAPPS; i++) {
				if (!inside(&tile[i], x, y))
					continue;

				apps[i].run();

				/* Back home: fresh redraw, fresh input state. */
				W = video_screen_width();
				H = video_screen_height();
				in_init();
				home_draw();
				break;
			}
		}

		if (get_timer(last) >= 1000) {
			last = get_timer(0);
			home_status();
			video_refresh();
		}

		mdelay(10);
	}
}
