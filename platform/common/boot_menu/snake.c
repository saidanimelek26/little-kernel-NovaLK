/*
 * NovaLK Snake: a small touch game reachable from the boot menu.
 *
 * Controls
 *   touch : swipe anywhere on the field, or tap one of the four arrow
 *           buttons at the bottom
 *   keys  : VOL+ turns left, VOL- turns right, POWER leaves the game
 *   EXIT  : button in the top right corner
 *
 * The snake dies on a wall or on itself.  Every food gives 10 points and
 * makes the game a little faster.  Nothing is written to storage; the best
 * score only lives until the next reboot.
 *
 * Drawing is incremental (head, tail and food cells only) so a tick costs a
 * handful of small rectangles and one video_refresh().
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
#ifdef MTK_TPD_SUPPORT
#include <dev/tpd.h>
#include <target/cust_tpd.h>
#endif

#ifndef MTK_PMIC_PWR_KEY
#define MTK_PMIC_PWR_KEY 8
#endif

/* NovaLK palette, 0x00RRGGBB. */
#define C_BG		0x0A0F1A
#define C_FIELD		0x0E1626
#define C_SURFACE	0x141E33
#define C_SURFACE_SEL	0x0F3A52
#define C_BORDER	0x26385A
#define C_ACCENT	0x2ED3FF
#define C_TEXT		0xF2F7FF
#define C_DIM		0x8FA6C4
#define C_SNAKE		0x1FA3C7
#define C_HEAD		0xE8FAFF
#define C_FOOD		0xFF5A6E

#define CELL		40
#define HUD_H		110
#define PAD_H		190		/* arrow button bar at the bottom */
#define MAX_CELLS	1024		/* ring buffer / occupancy size */
#define RING_MASK	(MAX_CELLS - 1)

#define TICK_START_MS	190
#define TICK_MIN_MS	80
#define TICK_STEP_MS	5
#define SWIPE_MIN_PX	50

enum { DIR_UP = 0, DIR_RIGHT = 1, DIR_DOWN = 2, DIR_LEFT = 3 };

static const int dir_dx[4] = { 0, 1, 0, -1 };
static const int dir_dy[4] = { -1, 0, 1, 0 };

struct rect {
	int x, y, w, h;
};

static int W, H;
static int cols, rows, ox, oy;

static u8 body_x[MAX_CELLS];
static u8 body_y[MAX_CELLS];
static u8 occ[MAX_CELLS];		/* y * cols + x */
static int head, len;
static int dir, pend_dir;
static int food_x, food_y;
static int score;
static int best;
static u32 rng = 0x2545F491;

static struct rect btn_exit, btn_dir[4], btn_again;

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static u32 rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;

	return rng;
}

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

static void draw_button(const struct rect *r, const char *label, int scale,
			int pressed)
{
	u32 fill = pressed ? C_SURFACE_SEL : C_SURFACE;
	u32 frame = pressed ? C_ACCENT : C_BORDER;

	video_fill_rect(fill, r->x, r->y, r->w, r->h);
	video_fill_rect(frame, r->x, r->y, r->w, 2);
	video_fill_rect(frame, r->x, r->y + r->h - 2, r->w, 2);
	video_fill_rect(frame, r->x, r->y, 2, r->h);
	video_fill_rect(frame, r->x + r->w - 2, r->y, 2, r->h);

	text_center(r->x + r->w / 2, r->y + (r->h - 16 * scale) / 2, scale,
		    pressed ? C_ACCENT : C_TEXT, fill, label);
}

/* ------------------------------------------------------------------ */
/* layout and drawing                                                 */
/* ------------------------------------------------------------------ */

static void layout(void)
{
	int i;
	int gap = 20;
	int bw = (W - 2 * 30 - 3 * gap) / 4;
	int by = H - PAD_H + 40;
	int bh = 110;

	cols = (W - 24) / CELL;
	rows = (H - HUD_H - PAD_H) / CELL;

	while (cols * rows > MAX_CELLS)
		rows--;

	ox = (W - cols * CELL) / 2;
	oy = HUD_H;

	btn_exit.w = 170;
	btn_exit.h = 64;
	btn_exit.x = W - 24 - btn_exit.w;
	btn_exit.y = 24;

	for (i = 0; i < 4; i++) {
		btn_dir[i].x = 30 + i * (bw + gap);
		btn_dir[i].y = by;
		btn_dir[i].w = bw;
		btn_dir[i].h = bh;
	}

	btn_again.w = 320;
	btn_again.h = 84;
	btn_again.x = (W - btn_again.w) / 2;
	btn_again.y = oy + (rows * CELL) / 2 + 40;
}

static void cell_clear(int x, int y)
{
	video_fill_rect(C_FIELD, ox + x * CELL, oy + y * CELL, CELL, CELL);
}

static void cell_draw(int x, int y, u32 color)
{
	video_fill_rect(color, ox + x * CELL + 2, oy + y * CELL + 2,
			CELL - 4, CELL - 4);
}

static void draw_hud(void)
{
	char line[48];

	video_fill_rect(C_BG, 0, 0, W, HUD_H - 6);

	video_draw_text(24, 20, 3, C_ACCENT, C_BG, "SNAKE");

	snprintf(line, sizeof(line), "SCORE %d   BEST %d", score, best);
	video_draw_text(24, 20 + 48 + 6, 2, C_TEXT, C_BG, line);

	draw_button(&btn_exit, "EXIT", 2, 0);
}

static void draw_pad(void)
{
	static const char *const label[4] = { "^", ">", "v", "<" };
	int i;

	video_fill_rect(C_BG, 0, H - PAD_H, W, PAD_H);

	for (i = 0; i < 4; i++)
		draw_button(&btn_dir[i], label[i], 5, 0);
}

static void draw_field(void)
{
	int i;

	video_fill_rect(C_BORDER, ox - 4, oy - 4, cols * CELL + 8,
			rows * CELL + 8);
	video_fill_rect(C_FIELD, ox, oy, cols * CELL, rows * CELL);

	for (i = 0; i < len; i++) {
		int idx = (head - i) & RING_MASK;

		cell_draw(body_x[idx], body_y[idx], i == 0 ? C_HEAD : C_SNAKE);
	}

	cell_draw(food_x, food_y, C_FOOD);
}

static void draw_game_over(int won)
{
	int px = ox + 20;
	int pw = cols * CELL - 40;
	int py = oy + (rows * CELL) / 2 - 170;
	char line[32];

	video_fill_rect(C_SURFACE, px, py, pw, 380);
	video_fill_rect(C_ACCENT, px, py, pw, 3);
	video_fill_rect(C_ACCENT, px, py + 377, pw, 3);
	video_fill_rect(C_ACCENT, px, py, 3, 380);
	video_fill_rect(C_ACCENT, px + pw - 3, py, 3, 380);

	text_center(W / 2, py + 36, 4, won ? C_ACCENT : C_TEXT, C_SURFACE,
		    won ? "YOU WIN" : "GAME OVER");

	snprintf(line, sizeof(line), "SCORE %d", score);
	text_center(W / 2, py + 36 + 64 + 20, 3, C_ACCENT, C_SURFACE, line);

	snprintf(line, sizeof(line), "BEST %d", best);
	text_center(W / 2, py + 36 + 64 + 20 + 48 + 10, 2, C_DIM, C_SURFACE,
		    line);

	btn_again.y = py + 380 - 84 - 28;
	draw_button(&btn_again, "PLAY AGAIN", 2, 0);
}

/* ------------------------------------------------------------------ */
/* game logic                                                         */
/* ------------------------------------------------------------------ */

/* Put the food on a random free cell.  Returns 0 when the field is full. */
static int spawn_food(void)
{
	int total = cols * rows;
	int free_cells = total - len;
	int k;
	int i;

	if (free_cells <= 0)
		return 0;

	k = (int)(rnd() % (u32)free_cells);

	for (i = 0; i < total; i++) {
		if (occ[i])
			continue;
		if (k-- == 0) {
			food_x = i % cols;
			food_y = i / cols;
			return 1;
		}
	}

	return 0;
}

static void game_reset(void)
{
	int i;
	int sx = cols / 2;
	int sy = rows / 2;

	memset(occ, 0, sizeof(occ));

	len = 3;
	head = 2;
	for (i = 0; i < len; i++) {
		body_x[i] = (u8)(sx - 2 + i);
		body_y[i] = (u8)sy;
		occ[sy * cols + (sx - 2 + i)] = 1;
	}

	dir = DIR_RIGHT;
	pend_dir = DIR_RIGHT;
	score = 0;

	spawn_food();
}

/*
 * Advance one step.  Returns 0 while the game goes on, 1 when the snake
 * died, 2 when the field is full.
 */
static int game_step(int *tick_ms)
{
	int nx, ny;
	int tail_idx = (head - len + 1) & RING_MASK;
	int tx = body_x[tail_idx];
	int ty = body_y[tail_idx];
	int eat;

	/* No instant reversal. */
	if (((pend_dir + 2) & 3) != dir)
		dir = pend_dir;

	nx = body_x[head] + dir_dx[dir];
	ny = body_y[head] + dir_dy[dir];

	if (nx < 0 || ny < 0 || nx >= cols || ny >= rows)
		return 1;

	eat = (nx == food_x && ny == food_y);

	/* The tail cell is free again unless the snake grows. */
	if (occ[ny * cols + nx] && (eat || !(nx == tx && ny == ty)))
		return 1;

	/* old head becomes body */
	cell_draw(body_x[head], body_y[head], C_SNAKE);

	if (!eat) {
		occ[ty * cols + tx] = 0;
		cell_clear(tx, ty);
	} else {
		len++;
		score += 10;
		if (score > best)
			best = score;
		if (*tick_ms > TICK_MIN_MS)
			*tick_ms -= TICK_STEP_MS;
	}

	head = (head + 1) & RING_MASK;
	body_x[head] = (u8)nx;
	body_y[head] = (u8)ny;
	occ[ny * cols + nx] = 1;
	cell_draw(nx, ny, C_HEAD);

	if (eat) {
		if (!spawn_food())
			return 2;
		cell_draw(food_x, food_y, C_FOOD);
		draw_hud();
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */

struct input_state {
	int touch_down;
	int start_x, start_y;
	int up, down, pwr;	/* previous key levels */
};

#define EV_NONE		0
#define EV_EXIT		1
#define EV_TAP		2	/* a fresh touch, position in *x / *y */

/* Poll touch + keys.  Updates pend_dir for steering. */
static int poll_input(struct input_state *in, int touch_ok, int *x, int *y)
{
	int up = mtk_detect_key(MT65XX_MENU_SELECT_KEY);
	int down = mtk_detect_key(MT65XX_MENU_OK_KEY);
	int pwr = mtk_detect_key(MTK_PMIC_PWR_KEY);
	int ev = EV_NONE;

	if (up && !in->up)
		pend_dir = (dir + 3) & 3;
	if (down && !in->down)
		pend_dir = (dir + 1) & 3;
	if (pwr && !in->pwr)
		ev = EV_EXIT;

	in->up = up;
	in->down = down;
	in->pwr = pwr;

#ifdef MTK_TPD_SUPPORT
	if (touch_ok) {
		int tx = 0, ty = 0;

		if (tpd_read(&tx, &ty) == 1) {
			int sx = tx * W / TPD_PANEL_WIDTH;
			int sy = ty * H / TPD_PANEL_HEIGHT;

			if (!in->touch_down) {
				in->touch_down = 1;
				in->start_x = sx;
				in->start_y = sy;
				*x = sx;
				*y = sy;
				if (ev == EV_NONE)
					ev = EV_TAP;
			} else {
				int dx = sx - in->start_x;
				int dy = sy - in->start_y;
				int adx = dx < 0 ? -dx : dx;
				int ady = dy < 0 ? -dy : dy;

				if (adx >= SWIPE_MIN_PX || ady >= SWIPE_MIN_PX) {
					if (adx > ady)
						pend_dir = dx > 0 ? DIR_RIGHT :
								    DIR_LEFT;
					else
						pend_dir = dy > 0 ? DIR_DOWN :
								    DIR_UP;
					in->start_x = sx;
					in->start_y = sy;
				}
			}
		} else {
			in->touch_down = 0;
		}
	}
#else
	(void)touch_ok;
	(void)x;
	(void)y;
#endif

	return ev;
}

/* ------------------------------------------------------------------ */
/* entry point                                                        */
/* ------------------------------------------------------------------ */

void novalk_snake_play(int touch_ok)
{
	struct input_state in;
	int tick_ms;
	int over;		/* 0 running, 1 dead, 2 won */
	ulong last_tick;
	ulong lock_until = 0;
	int x = 0, y = 0;
	int i;

	W = video_screen_width();
	H = video_screen_height();
	if (W <= 0 || H <= 0)
		return;

	rng ^= (u32)get_timer(0) * 2654435761u;
	if (!rng)
		rng = 0x2545F491;

	layout();

	memset(&in, 0, sizeof(in));
	in.up = mtk_detect_key(MT65XX_MENU_SELECT_KEY);
	in.down = mtk_detect_key(MT65XX_MENU_OK_KEY);
	in.pwr = mtk_detect_key(MTK_PMIC_PWR_KEY);
	in.touch_down = 1;	/* ignore the tap that opened the game */

restart:
	game_reset();
	tick_ms = TICK_START_MS;
	over = 0;

	video_fill_rect(C_BG, 0, 0, W, H);
	draw_hud();
	draw_pad();
	draw_field();
	video_refresh();

	last_tick = get_timer(0);

	while (1) {
		int ev = poll_input(&in, touch_ok, &x, &y);

		if (ev == EV_EXIT)
			break;

		if (ev == EV_TAP) {
			if (inside(&btn_exit, x, y))
				break;

			if (over) {
				if (get_timer(0) >= lock_until &&
				    inside(&btn_again, x, y))
					goto restart;
			} else {
				for (i = 0; i < 4; i++) {
					if (inside(&btn_dir[i], x, y)) {
						pend_dir = i;
						break;
					}
				}
			}
		}

		if (!over && get_timer(last_tick) >= (ulong)tick_ms) {
			last_tick = get_timer(0);
			over = game_step(&tick_ms);

			if (over) {
				lock_until = get_timer(0) + 600;
				draw_hud();
				draw_game_over(over == 2);
			}
			video_refresh();
		}

		mdelay(8);
	}

	/* Wait until the finger is gone so the menu does not see the tap. */
#ifdef MTK_TPD_SUPPORT
	if (touch_ok) {
		int tx, ty;
		ulong t0 = get_timer(0);

		while (tpd_read(&tx, &ty) == 1 && get_timer(t0) < 1500)
			mdelay(20);
	}
#endif
}
