/*
 * NovaLK boot menu lock: user interface.
 *
 *   novalk_lock_gate()      shown before the boot menu when protection is on
 *   novalk_lock_settings()  SECURITY screen: set / change / remove password
 *
 * Two credential types (see lock_store.h for how they are stored):
 *   PIN      4..8 digits on an on-screen keypad
 *   PATTERN  4..9 of the nine dots of a 3x3 grid, drawn with one finger
 *
 * Without a working touch controller everything can still be entered with
 * the hardware keys: VOL+ cycles the candidate digit, VOL- adds it, POWER
 * confirms.  (A pattern is the sequence of dot numbers 0..8, row by row.)
 *
 * Wrong attempts are counted persistently.  From the 5th failure on every
 * further attempt is delayed (30 s, 60 s, ... up to 10 min), and the delay
 * also applies after a reboot.
 */

#include <string.h>
#include <printf.h>
#include <sys/types.h>
#include <platform/mt_typedefs.h>
#include <platform/mtk_key.h>
#include <platform/mt_gpt.h>
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

#define FREE_TRIES	5	/* attempts before delays start */
#define MAX_WAIT_S	600
#define VERIFY_TRIES	5	/* tries inside the settings flows */

struct rect {
	int x, y, w, h;
};

static int W, H;

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

static void draw_frame(const struct rect *r, u32 fill, u32 frame)
{
	video_fill_rect(fill, r->x, r->y, r->w, r->h);
	video_fill_rect(frame, r->x, r->y, r->w, 2);
	video_fill_rect(frame, r->x, r->y + r->h - 2, r->w, 2);
	video_fill_rect(frame, r->x, r->y, 2, r->h);
	video_fill_rect(frame, r->x + r->w - 2, r->y, 2, r->h);
}

static void draw_button(const struct rect *r, const char *label, int scale,
			int pressed, int dim)
{
	u32 fill = pressed ? C_SURFACE_SEL : C_SURFACE;
	u32 frame = pressed ? C_ACCENT : C_BORDER;
	u32 fg = dim ? C_BORDER : (pressed ? C_ACCENT : C_TEXT);

	draw_frame(r, fill, frame);
	text_center(r->x + r->w / 2, r->y + (r->h - 16 * scale) / 2, scale, fg,
		    fill, label);
}

/* Short visual feedback for a tapped button. */
static void tap_feedback(const struct rect *r, const char *label, int scale)
{
	draw_button(r, label, scale, 1, 0);
	video_refresh();
	mdelay(70);
	draw_button(r, label, scale, 0, 0);
	video_refresh();
}

static void draw_line(int x0, int y0, int x1, int y1, u32 color)
{
	int dx = x1 - x0;
	int dy = y1 - y0;
	int adx = dx < 0 ? -dx : dx;
	int ady = dy < 0 ? -dy : dy;
	int n = (adx > ady ? adx : ady) / 4 + 1;
	int i;

	for (i = 0; i <= n; i++)
		video_fill_rect(color, x0 + dx * i / n - 4,
				y0 + dy * i / n - 4, 8, 8);
}

/* Heading, sub heading and status line shared by all screens. */
static void draw_header(const char *heading, const char *sub,
			const char *status, int status_is_error)
{
	video_fill_rect(C_BG, 0, 0, W, 400);

	text_center(W / 2, 90, 4, C_ACCENT, C_BG, heading);
	if (sub)
		text_center(W / 2, 90 + 64 + 18, 2, C_TEXT, C_BG, sub);
	if (status)
		text_center(W / 2, 90 + 64 + 18 + 32 + 150, 2,
			    status_is_error ? C_ERROR : C_DIM, C_BG, status);
}

/* Entered length as filled squares (never the digits themselves). */
static void draw_dots(int len, int max)
{
	int sz = 34;
	int gap = 16;
	int total = max * sz + (max - 1) * gap;
	int x0 = (W - total) / 2;
	int y = 90 + 64 + 18 + 32 + 50;
	int i;

	video_fill_rect(C_BG, 0, y - 6, W, sz + 12);
	for (i = 0; i < max; i++) {
		int x = x0 + i * (sz + gap);

		video_fill_rect(i < len ? C_ACCENT : C_BORDER, x, y, sz, sz);
		if (i >= len)
			video_fill_rect(C_BG, x + 3, y + 3, sz - 6, sz - 6);
	}
}

static void message_screen(const char *title, const char *text, u32 color,
			   int ms)
{
	video_fill_rect(C_BG, 0, 0, W, H);
	text_center(W / 2, H / 2 - 70, 4, color, C_BG, title);
	if (text)
		text_center(W / 2, H / 2 + 20, 2, C_DIM, C_BG, text);
	video_refresh();
	mdelay(ms);
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */

#define EV_TAP		0x01	/* fresh touch, position in *x / *y */
#define EV_VOLUP	0x02
#define EV_VOLDN	0x04
#define EV_PWR		0x08

static struct {
	int touch_ok;
	int down;
	int miss;
	int up, dn, pwr;
} in;

static void in_init(int touch_ok)
{
	in.touch_ok = touch_ok;
	in.down = 1;		/* a finger already on the glass is ignored */
	in.miss = 0;
	in.up = mtk_detect_key(MT65XX_MENU_SELECT_KEY);
	in.dn = mtk_detect_key(MT65XX_MENU_OK_KEY);
	in.pwr = mtk_detect_key(MTK_PMIC_PWR_KEY);
}

/*
 * One poll.  *touching tells whether a finger is on the glass right now
 * (position in *x / *y); the return value carries the edge events.
 */
static int in_poll(int *x, int *y, int *touching)
{
	int ev = 0;
	int up = mtk_detect_key(MT65XX_MENU_SELECT_KEY);
	int dn = mtk_detect_key(MT65XX_MENU_OK_KEY);
	int pwr = mtk_detect_key(MTK_PMIC_PWR_KEY);

	if (up && !in.up)
		ev |= EV_VOLUP;
	if (dn && !in.dn)
		ev |= EV_VOLDN;
	if (pwr && !in.pwr)
		ev |= EV_PWR;
	in.up = up;
	in.dn = dn;
	in.pwr = pwr;

	*touching = 0;

#ifdef MTK_TPD_SUPPORT
	if (in.touch_ok) {
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
			/* A few empty frames in a row: the finger is gone. */
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
/* PIN entry (touch keypad)                                           */
/* ------------------------------------------------------------------ */

static struct rect keyr[12];
static struct rect btn_cancel;
static const char *const key_label[12] = {
	"1", "2", "3", "4", "5", "6", "7", "8", "9", "DEL", "0", "OK"
};

static void pad_layout(void)
{
	int bw = 190, bh = 120, gap = 20;
	int x0 = (W - (3 * bw + 2 * gap)) / 2;
	int y0 = 460;
	int i;

	for (i = 0; i < 12; i++) {
		keyr[i].x = x0 + (i % 3) * (bw + gap);
		keyr[i].y = y0 + (i / 3) * (bh + gap);
		keyr[i].w = bw;
		keyr[i].h = bh;
	}

	btn_cancel.w = 300;
	btn_cancel.h = 80;
	btn_cancel.x = (W - btn_cancel.w) / 2;
	btn_cancel.y = y0 + 4 * (bh + gap) + 20;
}

static void pad_draw(int cancellable)
{
	int i;

	for (i = 0; i < 12; i++)
		draw_button(&keyr[i], key_label[i], 4, 0, 0);
	if (cancellable)
		draw_button(&btn_cancel, "CANCEL", 2, 0, 0);
}

/* Returns 1 with the digits in out, 0 when cancelled. */
static int pin_prompt_touch(const char *heading, const char *sub,
			    const char *status, int status_err,
			    int cancellable, char *out)
{
	int len = 0;
	const char *msg = status;
	int msg_err = status_err;
	int x = 0, y = 0, touching;

	pad_layout();

	video_fill_rect(C_BG, 0, 0, W, H);
	draw_header(heading, sub, msg, msg_err);
	draw_dots(len, LOCK_PIN_MAX);
	pad_draw(cancellable);
	video_refresh();

	out[0] = '\0';

	while (1) {
		int ev = in_poll(&x, &y, &touching);
		int i;

		if (ev & EV_TAP) {
			if (cancellable && inside(&btn_cancel, x, y)) {
				tap_feedback(&btn_cancel, "CANCEL", 2);
				out[0] = '\0';
				return 0;
			}

			for (i = 0; i < 12; i++) {
				if (!inside(&keyr[i], x, y))
					continue;

				tap_feedback(&keyr[i], key_label[i], 4);

				if (i == 9) {			/* DEL */
					if (len > 0)
						out[--len] = '\0';
					msg = NULL;
				} else if (i == 11) {		/* OK */
					if (len >= LOCK_PIN_MIN)
						return 1;
					msg = "At least 4 digits";
					msg_err = 1;
				} else if (len < LOCK_PIN_MAX) {
					out[len++] = (i == 10) ? '0' :
						     (char)('1' + i);
					out[len] = '\0';
					msg = NULL;
				}

				draw_header(heading, sub, msg, msg_err);
				draw_dots(len, LOCK_PIN_MAX);
				video_refresh();
				break;
			}
		}

		mdelay(8);
	}
}

/* ------------------------------------------------------------------ */
/* Pattern entry (touch grid)                                         */
/* ------------------------------------------------------------------ */

#define NODE_SZ		80
#define NODE_HIT	85

static int pat_gap, pat_cx0, pat_cy0;

static void pat_layout(void)
{
	pat_gap = (W - 240) / 2;
	if (pat_gap > 200)
		pat_gap = 200;
	pat_cx0 = W / 2 - pat_gap;
	pat_cy0 = 560;

	pad_layout();
	btn_cancel.y = pat_cy0 + 2 * pat_gap + NODE_SZ / 2 + 90;
}

static int node_x(int i)
{
	return pat_cx0 + (i % 3) * pat_gap;
}

static int node_y(int i)
{
	return pat_cy0 + (i / 3) * pat_gap;
}

static void node_draw(int i, int sel)
{
	struct rect r;

	r.x = node_x(i) - NODE_SZ / 2;
	r.y = node_y(i) - NODE_SZ / 2;
	r.w = NODE_SZ;
	r.h = NODE_SZ;

	draw_frame(&r, sel ? C_ACCENT : C_SURFACE, sel ? C_TEXT : C_BORDER);
}

/* Node under (x, y), or -1. */
static int node_at(int x, int y)
{
	int i;

	for (i = 0; i < 9; i++) {
		int dx = x - node_x(i);
		int dy = y - node_y(i);

		if (dx < 0)
			dx = -dx;
		if (dy < 0)
			dy = -dy;
		if (dx <= NODE_HIT / 2 + 20 && dy <= NODE_HIT / 2 + 20)
			return i;
	}

	return -1;
}

static void pat_draw_all(int cancellable)
{
	int i;

	for (i = 0; i < 9; i++)
		node_draw(i, 0);
	if (cancellable)
		draw_button(&btn_cancel, "CANCEL", 2, 0, 0);
}

static int pattern_prompt_touch(const char *heading, const char *sub,
				const char *status, int status_err,
				int cancellable, char *out)
{
	int used[9];
	int count = 0;
	int last = -1;
	int px = 0, py = 0, have_prev = 0;
	int miss = 0;
	int x = 0, y = 0, touching;
	const char *msg = status;
	int msg_err = status_err;

	pat_layout();

restart:
	memset(used, 0, sizeof(used));
	count = 0;
	last = -1;
	have_prev = 0;
	miss = 0;
	out[0] = '\0';

	video_fill_rect(C_BG, 0, 0, W, H);
	draw_header(heading, sub, msg, msg_err);
	pat_draw_all(cancellable);
	video_refresh();

	while (1) {
		int ev = in_poll(&x, &y, &touching);

		if (touching) {
			int steps, s, dx, dy;

			miss = 0;

			if (!have_prev) {
				px = x;
				py = y;
				have_prev = 1;
			}

			if (count == 0 && (ev & EV_TAP) && cancellable &&
			    inside(&btn_cancel, x, y)) {
				tap_feedback(&btn_cancel, "CANCEL", 2);
				out[0] = '\0';
				return 0;
			}

			/*
			 * Follow the finger path in small steps so a fast
			 * swipe cannot jump over a dot.
			 */
			dx = x - px;
			dy = y - py;
			steps = ((dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ?
				 (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy)) /
				16 + 1;

			for (s = 1; s <= steps; s++) {
				int sx = px + dx * s / steps;
				int sy = py + dy * s / steps;
				int n = node_at(sx, sy);

				if (n < 0 || used[n] || count >= LOCK_PAT_MAX)
					continue;

				used[n] = 1;
				out[count++] = (char)('0' + n);
				out[count] = '\0';

				if (last >= 0)
					draw_line(node_x(last), node_y(last),
						  node_x(n), node_y(n),
						  C_ACCENT);
				if (last >= 0)
					node_draw(last, 1);
				node_draw(n, 1);
				last = n;
				video_refresh();
			}

			px = x;
			py = y;
		} else if (count > 0) {
			/* Finger lifted (debounced): the pattern is done. */
			if (++miss >= 6) {
				if (count >= LOCK_PAT_MIN)
					return 1;

				msg = "Connect at least 4 dots";
				msg_err = 1;
				goto restart;
			}
		}

		mdelay(8);
	}
}

/* ------------------------------------------------------------------ */
/* Key-only entry (no touch controller)                               */
/* ------------------------------------------------------------------ */

static int key_prompt(int type, const char *heading, const char *sub,
		      const char *status, int status_err, char *out)
{
	int max = (type == LOCK_PIN) ? LOCK_PIN_MAX : LOCK_PAT_MAX;
	int min = (type == LOCK_PIN) ? LOCK_PIN_MIN : LOCK_PAT_MIN;
	int top = (type == LOCK_PIN) ? 9 : 8;
	int cand = 0;
	int len = 0;
	int x = 0, y = 0, touching;
	char num[4];
	const char *msg = status;
	int msg_err = status_err;

	out[0] = '\0';

	video_fill_rect(C_BG, 0, 0, W, H);
	draw_header(heading, sub, msg, msg_err);
	draw_dots(len, max);

	text_center(W / 2, 700, 2, C_DIM, C_BG, "VOL+   next digit");
	text_center(W / 2, 750, 2, C_DIM, C_BG, "VOL-   add digit");
	text_center(W / 2, 800, 2, C_DIM, C_BG, "POWER  confirm");
	if (type == LOCK_PATTERN)
		text_center(W / 2, 870, 1, C_DIM, C_BG,
			    "dots numbered 0-8, row by row");

	snprintf(num, sizeof(num), "%d", cand);
	text_center(W / 2, 540, 8, C_ACCENT, C_BG, num);
	video_refresh();

	while (1) {
		int ev = in_poll(&x, &y, &touching);
		int redraw = 0;

		if (ev & EV_VOLUP) {
			cand = (cand + 1) % (top + 1);
			redraw = 1;
		}
		if ((ev & EV_VOLDN) && len < max) {
			/* A pattern cannot use the same dot twice. */
			if (type == LOCK_PATTERN &&
			    strchr(out, (char)('0' + cand)) != NULL) {
				msg = "Dot already used";
				msg_err = 1;
			} else {
				out[len++] = (char)('0' + cand);
				out[len] = '\0';
				msg = NULL;
			}
			redraw = 1;
		}
		if (ev & EV_PWR) {
			if (len >= min)
				return 1;
			msg = "Too short";
			msg_err = 1;
			redraw = 1;
		}

		if (redraw) {
			draw_header(heading, sub, msg, msg_err);
			draw_dots(len, max);
			video_fill_rect(C_BG, 0, 520, W, 140);
			snprintf(num, sizeof(num), "%d", cand);
			text_center(W / 2, 540, 8, C_ACCENT, C_BG, num);
			video_refresh();
		}

		mdelay(8);
	}
}

/* ------------------------------------------------------------------ */
/* Credential prompt dispatcher                                       */
/* ------------------------------------------------------------------ */

/* Returns 1 with the credential in out, 0 when cancelled. */
static int prompt(int type, const char *heading, const char *sub,
		  const char *status, int status_err, int cancellable,
		  char *out)
{
	if (!in.touch_ok)
		return key_prompt(type, heading, sub, status, status_err, out);

	if (type == LOCK_PATTERN)
		return pattern_prompt_touch(heading, sub, status, status_err,
					    cancellable, out);

	return pin_prompt_touch(heading, sub, status, status_err,
				cancellable, out);
}

static const char *type_name(int type)
{
	return type == LOCK_PATTERN ? "PATTERN" : "PIN";
}

/* ------------------------------------------------------------------ */
/* Attempt delay                                                      */
/* ------------------------------------------------------------------ */

static u32 wait_seconds(u32 fails)
{
	u32 s;

	if (fails < FREE_TRIES)
		return 0;

	s = 30 * (fails - FREE_TRIES + 1);

	return s > MAX_WAIT_S ? MAX_WAIT_S : s;
}

/* Blocks while the attempt delay is running. */
static void lockout_wait(void)
{
	u32 left = wait_seconds(lockstore_fail_count());
	char line[48];

	while (left) {
		ulong t0 = get_timer(0);

		video_fill_rect(C_BG, 0, 0, W, H);
		text_center(W / 2, H / 2 - 120, 4, C_ERROR, C_BG, "TOO MANY TRIES");
		snprintf(line, sizeof(line), "Try again in %u s", left);
		text_center(W / 2, H / 2 - 20, 3, C_TEXT, C_BG, line);
		video_refresh();

		while (get_timer(t0) < 1000)
			mdelay(20);
		left--;
	}
}

/* ------------------------------------------------------------------ */
/* Gate                                                               */
/* ------------------------------------------------------------------ */

void novalk_lock_gate(int touch_ok)
{
	char cred[16];
	char status[40];
	const char *st = NULL;
	int type;

	lockstore_init();

	if (!lockstore_enabled())
		return;

	W = video_screen_width();
	H = video_screen_height();
	if (W <= 0 || H <= 0)
		return;

	in_init(touch_ok);
	type = lockstore_type();

	while (1) {
		lockout_wait();

		prompt(type, "NovaLK",
		       type == LOCK_PATTERN ? "DRAW PATTERN TO UNLOCK" :
					      "ENTER PIN TO UNLOCK",
		       st, st != NULL, 0, cred);

		if (lockstore_verify(cred)) {
			lockstore_note_ok();
			memset(cred, 0, sizeof(cred));
			return;
		}

		memset(cred, 0, sizeof(cred));
		lockstore_note_fail();

		if (lockstore_fail_count() >= FREE_TRIES) {
			snprintf(status, sizeof(status), "Wrong %s", type_name(type));
		} else {
			snprintf(status, sizeof(status), "Wrong %s - %u tries left",
				 type_name(type),
				 (unsigned)(FREE_TRIES - lockstore_fail_count()));
		}
		st = status;
	}
}

/* ------------------------------------------------------------------ */
/* Settings                                                           */
/* ------------------------------------------------------------------ */

/* Ask for the current credential.  1 = verified, 0 = cancelled/failed. */
static int verify_current(void)
{
	char cred[16];
	char status[40];
	const char *st = NULL;
	int type = lockstore_type();
	int tries;

	for (tries = 0; tries < VERIFY_TRIES; tries++) {
		lockout_wait();

		if (!prompt(type, "CURRENT",
			    type == LOCK_PATTERN ? "DRAW CURRENT PATTERN" :
						   "ENTER CURRENT PIN",
			    st, st != NULL, 1, cred))
			return 0;

		if (lockstore_verify(cred)) {
			lockstore_note_ok();
			memset(cred, 0, sizeof(cred));
			return 1;
		}

		memset(cred, 0, sizeof(cred));
		lockstore_note_fail();
		snprintf(status, sizeof(status), "Wrong %s", type_name(type));
		st = status;
	}

	message_screen("TOO MANY TRIES", "Nothing was changed", C_ERROR, 1800);

	return 0;
}

static struct rect btn_a, btn_b, btn_c;

/* PIN or PATTERN?  Returns the type, or LOCK_NONE when cancelled. */
static int choose_type(void)
{
	int x = 0, y = 0, touching;

	if (!in.touch_ok)
		return LOCK_PIN;

	btn_a.w = btn_b.w = 560;
	btn_a.h = btn_b.h = 130;
	btn_a.x = btn_b.x = (W - 560) / 2;
	btn_a.y = 420;
	btn_b.y = btn_a.y + btn_a.h + 30;
	btn_c.w = 300;
	btn_c.h = 80;
	btn_c.x = (W - 300) / 2;
	btn_c.y = btn_b.y + btn_b.h + 60;

	video_fill_rect(C_BG, 0, 0, W, H);
	draw_header("PASSWORD TYPE", "CHOOSE HOW TO UNLOCK", NULL, 0);
	draw_button(&btn_a, "PIN CODE", 3, 0, 0);
	draw_button(&btn_b, "PATTERN", 3, 0, 0);
	draw_button(&btn_c, "CANCEL", 2, 0, 0);
	video_refresh();

	while (1) {
		int ev = in_poll(&x, &y, &touching);

		if (ev & EV_TAP) {
			if (inside(&btn_a, x, y)) {
				tap_feedback(&btn_a, "PIN CODE", 3);
				return LOCK_PIN;
			}
			if (inside(&btn_b, x, y)) {
				tap_feedback(&btn_b, "PATTERN", 3);
				return LOCK_PATTERN;
			}
			if (inside(&btn_c, x, y)) {
				tap_feedback(&btn_c, "CANCEL", 2);
				return LOCK_NONE;
			}
		}

		mdelay(8);
	}
}

/* Create or change the password. */
static void do_set(void)
{
	char first[16], second[16];
	char status[40];
	const char *st = NULL;
	int type;

	if (lockstore_enabled() && !verify_current())
		return;

	type = choose_type();
	if (type == LOCK_NONE)
		return;

	while (1) {
		if (!prompt(type, "NEW PASSWORD",
			    type == LOCK_PATTERN ? "DRAW A NEW PATTERN" :
						   "ENTER A NEW PIN",
			    st, st != NULL, 1, first))
			return;

		if (!prompt(type, "CONFIRM",
			    type == LOCK_PATTERN ? "DRAW IT AGAIN" :
						   "ENTER IT AGAIN",
			    NULL, 0, 1, second))
			return;

		if (strcmp(first, second) != 0) {
			snprintf(status, sizeof(status), "They do not match");
			st = status;
			continue;
		}

		break;
	}

	if (lockstore_set(type, first) == 0)
		message_screen("PASSWORD SAVED", "The boot menu is now locked",
			       C_OK, 1600);
	else
		message_screen("NOT SAVED", "Storage is not available", C_ERROR,
			       2200);

	memset(first, 0, sizeof(first));
	memset(second, 0, sizeof(second));
}

static void do_remove(void)
{
	if (!lockstore_enabled())
		return;

	if (!verify_current())
		return;

	if (lockstore_clear() == 0)
		message_screen("PROTECTION REMOVED", "The boot menu is open",
			       C_OK, 1600);
	else
		message_screen("NOT REMOVED", "Storage error", C_ERROR, 2200);
}

static void settings_draw(void)
{
	const char *status;
	char line[48];
	int on = lockstore_enabled();
	int can = lockstore_writable();

	if (on)
		snprintf(line, sizeof(line), "PROTECTION: ON  (%s)",
			 type_name(lockstore_type()));
	else
		snprintf(line, sizeof(line), "PROTECTION: OFF");
	status = line;

	video_fill_rect(C_BG, 0, 0, W, H);
	text_center(W / 2, 90, 4, C_ACCENT, C_BG, "SECURITY");
	text_center(W / 2, 90 + 64 + 18, 2, on ? C_OK : C_DIM, C_BG, status);

	if (!can && !on)
		text_center(W / 2, 90 + 64 + 18 + 44, 2, C_ERROR, C_BG,
			    "Storage not available");

	draw_button(&btn_a, on ? "CHANGE PASSWORD" : "SET PASSWORD", 3, 0,
		    !can);
	draw_button(&btn_b, "REMOVE PASSWORD", 3, 0, !on);
	draw_button(&btn_c, "BACK", 2, 0, 0);
	video_refresh();
}

void novalk_lock_settings(int touch_ok)
{
	int x = 0, y = 0, touching;

	W = video_screen_width();
	H = video_screen_height();
	if (W <= 0 || H <= 0)
		return;

	in_init(touch_ok);

	/* Re-read so the screen reflects what is really stored. */
	lockstore_init();

	btn_a.w = btn_b.w = 600;
	btn_a.h = btn_b.h = 130;
	btn_a.x = btn_b.x = (W - 600) / 2;
	btn_a.y = 440;
	btn_b.y = btn_a.y + btn_a.h + 30;
	btn_c.w = 300;
	btn_c.h = 80;
	btn_c.x = (W - 300) / 2;
	btn_c.y = btn_b.y + btn_b.h + 70;

	settings_draw();

	while (1) {
		int ev = in_poll(&x, &y, &touching);

		if (ev & EV_PWR)
			return;

		if (ev & EV_TAP) {
			if (inside(&btn_c, x, y)) {
				tap_feedback(&btn_c, "BACK", 2);
				return;
			}

			if (inside(&btn_a, x, y) && lockstore_writable()) {
				do_set();
				in_init(touch_ok);
				settings_draw();
			} else if (inside(&btn_b, x, y) &&
				   lockstore_enabled()) {
				do_remove();
				in_init(touch_ok);
				settings_draw();
			}
		}

		mdelay(8);
	}
}
