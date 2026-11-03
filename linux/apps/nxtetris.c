/*
 * nxtetris: falling blocks by the guideline's rules: the seven pieces from a shuffled bag, SRS
 * turns with their wall kicks, a held piece, five to come, a ghost, soft and hard drops, a lock
 * delay, levels that speed it up, T-spins, combos and back-to-back. The best five scores are
 * the registry's (nxtetris.best). NXTETRIS_AUTO=N plays N pieces itself and says what it took.
 * A controller plays it too (docs/machine/input.md, "Game controls"; ui_pad.h): the left stick moves and
 * drops a row a notch (up holds), the right stick or the wheel turns, a click on the well turns, the
 * right button or the jump button (A) drops, B holds, Y pauses. NXTETRIS_PRINT=1 says each move.
 */
#include "ui.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include "ui_pad.h"
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

#define COLS 10
#define ROWS 24			/* the top four are hidden: pieces start there */
#define HIDDEN 4
#define SHOWN (ROWS - HIDDEN)
#define MENU_H 20
#define PAD 8
#define NEXT 5
#define BEST 5
#define LOCK_US 500000
#define LOCK_MOVES 15		/* moves and turns on the ground before the piece locks at once */
#define CLEAR_US 180000		/* how long cleared rows flash */
#define NOTE_US 2000000
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN | \
	GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_FOCUS_OUT)
#define FONT_BIG "/usr/share/font-ui-b-17.sef"

enum { EMPTY, P_I, P_J, P_L, P_O, P_S, P_T, P_Z, GREY, GHOST = 16, FLASH = 32, CODES = 33 };
enum { READY, PLAYING, PAUSED, CLEARING, OVER };

/* each piece as it starts, in its box (4 wide for I and O, else 3); y grows downwards */
static const signed char spawn_cells[8][4][2] = {
	{ { 0 } },
	{ { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } },
	{ { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },
	{ { 2, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },
	{ { 1, 0 }, { 2, 0 }, { 1, 1 }, { 2, 1 } },
	{ { 1, 0 }, { 2, 0 }, { 0, 1 }, { 1, 1 } },
	{ { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } },
	{ { 0, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 } },
};
static signed char shape[8][4][4][2];	/* [kind][turn][cell][x, y] */

/* SRS's kicks, [from][0 clockwise, 1 back][test][x, y], y downwards */
static const signed char kick_jlstz[4][2][5][2] = {
	{ { { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } }, { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } } },
	{ { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, -2 }, { 1, -2 } }, { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, -2 }, { 1, -2 } } },
	{ { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } }, { { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } } },
	{ { { 0, 0 }, { -1, 0 }, { -1, 1 }, { 0, -2 }, { -1, -2 } }, { { 0, 0 }, { -1, 0 }, { -1, 1 }, { 0, -2 }, { -1, -2 } } },
};
static const signed char kick_i[4][2][5][2] = {
	{ { { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, 1 }, { 1, -2 } }, { { 0, 0 }, { -1, 0 }, { 2, 0 }, { -1, -2 }, { 2, 1 } } },
	{ { { 0, 0 }, { -1, 0 }, { 2, 0 }, { -1, -2 }, { 2, 1 } }, { { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, -1 }, { -1, 2 } } },
	{ { { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, -1 }, { -1, 2 } }, { { 0, 0 }, { 1, 0 }, { -2, 0 }, { 1, 2 }, { -2, -1 } } },
	{ { { 0, 0 }, { 1, 0 }, { -2, 0 }, { 1, 2 }, { -2, -1 } }, { { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, 1 }, { 1, -2 } } },
};

/* the guideline's time a row takes to fall, levels 1 to 20, in microseconds */
static const int gravity_us[20] = {
	1000000, 793000, 617796, 472729, 355197, 262004, 189677, 134735, 93882, 64152,
	42976, 28218, 18153, 11439, 7059, 4264, 2520, 1457, 824, 455,
};

static const unsigned char rgb[9][3] = {
	{ 0, 0, 0 }, { 0, 200, 230 }, { 40, 90, 230 }, { 245, 140, 20 }, { 240, 205, 0 },
	{ 60, 200, 70 }, { 170, 60, 210 }, { 230, 40, 50 }, { 128, 128, 136 },
};
#define BG_R 18
#define BG_G 20
#define BG_B 32
#define WELL_BG MWRGB(BG_R, BG_G, BG_B)
#define WELL_GRID MWRGB(30, 33, 48)

/* the game */
static unsigned char board[ROWS][COLS];
static int state, kind, turn, px, py, lowest;
static int held, hold_used, ghost_on = 1;
static int bag[7], bag_left, queue[NEXT];
static int score, lines, level, start_level = 1, combo, b2b;
static int grounded, lock_moves, last_turned, last_kick;
static long long fall_next, lock_at, das_next, clear_end, note_end;
static int clearing[ROWS];
static char note[2][32];		/* what the last lock did ("T-Spin Double", "Combo 2") */
static int note_gen;		/* counts the changes to it */
static int dir, soft;			/* the sideways key held (-1, 0, 1), and the soft drop's */
static int keys_held[8];
static int das_us = 170000, arr_us = 40000;
static struct { int score, lines, level; } best[BEST];
static int best_new = -1;		/* the place the last game took among the best */

/* the window */
static GR_WINDOW_ID window;
static GR_FONT_ID big;
static struct ui_widths big_widths;
static int C, S, T;			/* a cell of the well, of the first preview and of the others */
static int WX, WY, LX, RX, PW, width, height;
static int hold_y, next_y, next_h, stats_y, best_y, keys_y;
static int button_x, button_y, button_w = 96, button_h = 24;
static int menu_x[3];
static long long drawn;			/* requests sent, for NXTETRIS_AUTO */

/* what is on the screen now, so that only what changes is drawn */
static unsigned char shown[SHOWN][COLS];
static int well_valid, overlay_shown = -1, hold_shown = -1, next_shown[NEXT], stats_shown, best_shown, note_shown;
static int score_shown = -1, level_shown = -1, lines_shown = -1;

/* NXTETRIS_AUTO: the pieces still to play, and the place chosen for this one */
static int auto_left, auto_turn, auto_x, auto_pieces;
static long long auto_next;

enum { C_NEW = 1, C_PAUSE, C_GHOST, C_CLEAR, C_EXIT, C_KEYS, C_ABOUT, C_LEVEL = 100 };
static const char *const menu_names[] = { "Game", "Help" };
static struct ui_menu_item level_items[15];
static char level_labels[15][4];
static struct ui_menu_item game_items[] = {
	{ "New game   F2", -1, C_NEW }, { "Pause   P", -1, C_PAUSE },
	{ "Start level", -1, 0, level_items, 15, UI_MENU_LINE }, { "Ghost piece", -1, C_GHOST },
	{ "Clear best scores...", -1, C_CLEAR }, { "Exit", -1, C_EXIT, NULL, 0, UI_MENU_LINE },
};
static const struct ui_menu_item help_items[] = { { "Keys", -1, C_KEYS }, { "About Tetris", -1, C_ABOUT } };

static long long
now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void
make_shapes(void)
{
	int k, t, i, box;

	for (k = 1; k <= 7; k++) {
		box = k == P_I || k == P_O ? 4 : 3;
		for (i = 0; i < 4; i++)
			shape[k][0][i][0] = spawn_cells[k][i][0], shape[k][0][i][1] = spawn_cells[k][i][1];
		for (t = 1; t < 4; t++)
			for (i = 0; i < 4; i++) {
				int x = shape[k][t - 1][i][0], y = shape[k][t - 1][i][1];

				shape[k][t][i][0] = k == P_O ? x : box - 1 - y;
				shape[k][t][i][1] = k == P_O ? y : x;
			}
	}
}

static int
blocked(int x, int y)
{
	return x < 0 || x >= COLS || y < 0 || y >= ROWS || board[y][x];
}

static int
fits(int k, int t, int x, int y)
{
	int i;

	for (i = 0; i < 4; i++)
		if (blocked(x + shape[k][t][i][0], y + shape[k][t][i][1]))
			return 0;
	return 1;
}

static int
drop_distance(void)
{
	int d = 0;

	while (fits(kind, turn, px, py + d + 1))
		d++;
	return d;
}

static int
fall_us(void)
{
	int g = gravity_us[(level > 20 ? 20 : level) - 1];

	return soft ? (g / 20 > 0 ? g / 20 : 1) : g;
}

/* the next piece of the bag, a new bag shuffled when it is empty */
static int
from_bag(void)
{
	int i, j, t;

	if (!bag_left) {
		for (i = 0; i < 7; i++)
			bag[i] = i + 1;
		for (i = 6; i > 0; i--)
			j = rand() % (i + 1), t = bag[i], bag[i] = bag[j], bag[j] = t;
		bag_left = 7;
	}
	return bag[--bag_left];
}

static void best_load(void);
static void auto_plan(void);

static void
game_over(void)
{
	char text[BEST * 24 + 1];
	int i, at = 0, n;

	state = OVER;
	dir = soft = 0;
	best_new = -1;
	for (i = 0; i < BEST; i++)
		if (score > best[i].score) {
			memmove(&best[i + 1], &best[i], (BEST - 1 - i) * sizeof best[0]);
			best[i].score = score, best[i].lines = lines, best[i].level = level;
			best_new = i;
			break;
		}
	if (best_new >= 0) {
		for (i = 0; i < BEST && best[i].score > 0; i++) {
			n = snprintf(text + at, sizeof text - at, "%s%d/%d/%d", i ? " " : "", best[i].score, best[i].lines, best[i].level);
			if (n < 0 || at + n >= (int)sizeof text)
				break;
			at += n;
		}
		text[at] = 0;
		reg_set("nxtetris.best", text);
		best_shown = 0;
	}
}

/* The next piece into the well at the top; false when there is no room for it (the game is over). */
static int
spawn(int k, long long now)
{
	kind = k, turn = 0, px = 3, py = HIDDEN - 2;
	if (!fits(kind, turn, px, py)) {
		game_over();
		return 0;
	}
	if (fits(kind, turn, px, py + 1))
		py++;
	lowest = py, lock_moves = 0, grounded = 0, last_turned = 0;
	fall_next = now + fall_us();
	if (auto_left)
		auto_plan();
	return 1;
}

static void
next_piece(long long now)
{
	int k = queue[0], i;

	for (i = 0; i < NEXT - 1; i++)
		queue[i] = queue[i + 1];
	queue[NEXT - 1] = from_bag();
	hold_used = 0;
	spawn(k, now);
}

static void
new_game(long long now)
{
	int i;

	memset(board, 0, sizeof board);
	memset(clearing, 0, sizeof clearing);
	bag_left = 0;
	for (i = 0; i < NEXT; i++)
		queue[i] = from_bag();
	held = 0, hold_used = 0, score = 0, lines = 0, level = start_level, combo = -1, b2b = 0;
	dir = soft = 0;
	note[0][0] = note[1][0] = 0, note_gen++;
	best_new = -1;
	best_shown = 0;
	state = PLAYING;
	next_piece(now);
}

/* A move or a turn that worked: on the ground it gives the piece more time, up to LOCK_MOVES. */
static void
moved(long long now)
{
	if (py > lowest)
		lowest = py, lock_moves = 0;
	if (grounded && lock_moves < LOCK_MOVES) {
		lock_moves++;
		lock_at = now + LOCK_US;
	}
	if (fits(kind, turn, px, py + 1)) {
		if (grounded)
			fall_next = now + fall_us();
		grounded = 0;
	}
}

static int
shift(int dx, long long now)
{
	if (!fits(kind, turn, px + dx, py))
		return 0;
	px += dx;
	last_turned = 0;
	moved(now);
	return 1;
}

static int
rotate(int back, long long now)
{
	int to = (turn + (back ? 3 : 1)) & 3, i, dx, dy;

	if (kind == P_O)
		return 0;
	for (i = 0; i < 5; i++) {
		dx = (kind == P_I ? kick_i : kick_jlstz)[turn][back][i][0];
		dy = (kind == P_I ? kick_i : kick_jlstz)[turn][back][i][1];
		if (fits(kind, to, px + dx, py + dy)) {
			turn = to, px += dx, py += dy;
			last_turned = 1, last_kick = i;
			moved(now);
			return 1;
		}
	}
	return 0;
}

/* A T's last move a turn, with three of its box's corners filled: 2 a T-spin, 1 a mini one. */
static int
t_spin(void)
{
	static const signed char corner[4][2] = { { 0, 0 }, { 2, 0 }, { 2, 2 }, { 0, 2 } };
	int i, filled = 0, front;

	if (kind != P_T || !last_turned)
		return 0;
	for (i = 0; i < 4; i++)
		filled += blocked(px + corner[i][0], py + corner[i][1]);
	if (filled < 3)
		return 0;
	/* the two corners on the side the T points to: turn 0 points up, 1 right, 2 down, 3 left */
	front = blocked(px + corner[turn][0], py + corner[turn][1]) + blocked(px + corner[(turn + 1) & 3][0], py + corner[(turn + 1) & 3][1]);
	return front == 2 || last_kick == 4 ? 2 : 1;
}

static void
scored(int n, int spin, long long now)
{
	static const int plain[5] = { 0, 100, 300, 500, 800 }, full[4] = { 400, 800, 1200, 1600 }, mini[3] = { 100, 200, 400 };
	static const char *const names[5] = { "", "Single", "Double", "Triple", "Tetris" };
	int points = spin == 2 ? full[n > 3 ? 3 : n] : spin == 1 ? mini[n > 2 ? 2 : n] : plain[n];
	int hard = n == 4 || (spin && n > 0), extra = 0;

	note[0][0] = note[1][0] = 0;
	if (spin)
		snprintf(note[0], sizeof note[0], "%sT-Spin%s%s", spin == 1 ? "Mini " : "", n ? " " : "", n ? names[n] : "");
	else if (n)
		snprintf(note[0], sizeof note[0], "%s", names[n]);
	if (n) {
		if (hard && b2b) {
			points = points * 3 / 2;
			snprintf(note[1], sizeof note[1], "Back-to-Back");
		}
		b2b = hard;
		combo++;
		if (combo > 0) {
			extra = 50 * combo;
			if (!note[1][0])
				snprintf(note[1], sizeof note[1], "Combo %d", combo);
		}
	} else
		combo = -1;
	score += (points + extra) * level;
	note_gen++;
	if (note[0][0] || note[1][0])
		note_end = now + NOTE_US;
}

static void
lock_piece(long long now)
{
	int i, r, c, n = 0, spin = t_spin(), above = 1;

	for (i = 0; i < 4; i++) {
		r = py + shape[kind][turn][i][1];
		board[r][px + shape[kind][turn][i][0]] = (unsigned char)kind;
		above = above && r < HIDDEN;
	}
	if (above) {	/* all of it locked above the well's top */
		game_over();
		return;
	}
	for (r = 0; r < ROWS; r++) {
		for (c = 0; c < COLS && board[r][c]; c++)
			;
		clearing[r] = c == COLS;
		n += clearing[r];
	}
	scored(n, spin, now);
	if (auto_left && --auto_left == 0) {
		printf("NXTETRIS-END pieces %d lines %d score %d requests %lld\n", auto_pieces, lines + n, score, drawn);
		fflush(stdout);
		GrClose();
		exit(0);
	}
	auto_pieces++;
	if (n) {
		state = CLEARING;
		clear_end = now + CLEAR_US;
		return;
	}
	next_piece(now);
}

/* The flashing rows go, the rows above come down, and the next piece comes. */
static void
clear_rows(long long now)
{
	int r, to = ROWS - 1, n = 0, empty = 1, c;

	for (r = ROWS - 1; r >= 0; r--) {
		if (clearing[r]) {
			n++;
			continue;
		}
		if (to != r)
			memcpy(board[to], board[r], COLS);
		to--;
	}
	for (; to >= 0; to--)
		memset(board[to], 0, COLS);
	memset(clearing, 0, sizeof clearing);
	for (r = 0; r < ROWS && empty; r++)
		for (c = 0; c < COLS; c++)
			empty = empty && !board[r][c];
	if (empty) {
		static const int perfect[5] = { 0, 800, 1200, 1800, 2000 };

		score += perfect[n] * level;
		snprintf(note[1], sizeof note[1], "Perfect Clear");
		note_gen++;
		note_end = now + NOTE_US;
	}
	lines += n;
	if (level < 1 + lines / 10)
		level = 1 + lines / 10;
	state = PLAYING;
	/* (what was pressed during the flash: its repeats are not looked at) */
	if (!dir && keys_held[0] != keys_held[1]) {	/* K_LEFT, K_RIGHT */
		dir = keys_held[0] ? -1 : 1;
		das_next = now + das_us;
	}
	soft = keys_held[2];	/* K_DOWN */
	next_piece(now);
}

static void
hard_drop(long long now)
{
	int d = drop_distance();

	if (d)
		last_turned = 0;
	py += d;
	score += 2 * d;
	lock_piece(now);
}

static void
hold(long long now)
{
	int k = kind;

	if (hold_used)
		return;
	hold_used = 1;
	if (held)
		spawn(held, now);
	else
		next_piece(now), hold_used = 1;
	held = k;
}

/* Everything time does up to `now`: sideways repeat, falling, locking, the rows' flash. */
static void
advance(long long now)
{
	if (state == CLEARING && now >= clear_end)
		clear_rows(now);
	if ((note[0][0] || note[1][0]) && now >= note_end)
		note[0][0] = note[1][0] = 0, note_gen++;
	if (state != PLAYING)
		return;
	while (dir && now >= das_next)
		if (shift(dir, now))
			das_next += arr_us;
		else {
			das_next = now + arr_us;
			break;
		}
	while (now >= fall_next && fits(kind, turn, px, py + 1)) {
		py++;
		last_turned = 0;
		score += soft;
		if (py > lowest)
			lowest = py, lock_moves = 0;
		fall_next += fall_us();
	}
	if (fits(kind, turn, px, py + 1)) {
		grounded = 0;
		return;
	}
	if (!grounded) {
		grounded = 1;
		lock_at = now + (lock_moves >= LOCK_MOVES ? 0 : LOCK_US);
	}
	if (now >= lock_at)
		lock_piece(now);
}

/* When something must next be done, in milliseconds from now (-1 when nothing waits). */
static int
wake_ms(long long now)
{
	long long none = now + 1000000000LL, at = none;

	if (state == PLAYING) {
		if (dir && das_next < at)
			at = das_next;
		if (fits(kind, turn, px, py + 1) ? fall_next < at : lock_at < at)
			at = fits(kind, turn, px, py + 1) ? fall_next : lock_at;
		if (auto_left && auto_next < at)
			at = auto_next;
	}
	if (state == CLEARING && clear_end < at)
		at = clear_end;
	if ((note[0][0] || note[1][0]) && note_end < at)
		at = note_end;
	return at == none ? -1 : at <= now ? 1 : (int)((at - now + 999) / 1000);
}

/*
 * NXTETRIS_AUTO: a place for each piece by the usual measures (height, holes, bumps, lines),
 * reached by turns and moves made as a player's keys would, 50 ms apart, then a hard drop.
 */
static int
auto_value(int k, int t, int x)
{
	unsigned char saved[ROWS][COLS];
	int y = 0, i, r, c, full = 0, height_sum = 0, holes = 0, bumps = 0, last = -1, h;

	while (!fits(k, t, x, y) && y < HIDDEN)
		y++;
	if (!fits(k, t, x, y))
		return -1000000;
	while (fits(k, t, x, y + 1))
		y++;
	memcpy(saved, board, sizeof board);
	for (i = 0; i < 4; i++)
		board[y + shape[k][t][i][1]][x + shape[k][t][i][0]] = 1;
	for (r = 0; r < ROWS; r++) {
		for (c = 0; c < COLS && board[r][c]; c++)
			;
		full += c == COLS;
	}
	for (c = 0; c < COLS; c++) {
		for (r = 0; r < ROWS && !board[r][c]; r++)
			;
		h = ROWS - r;
		height_sum += h;
		for (; r < ROWS; r++)
			holes += !board[r][c];
		if (last >= 0)
			bumps += h > last ? h - last : last - h;
		last = h;
	}
	memcpy(board, saved, sizeof board);
	return 76 * full - 51 * height_sum - 356 * holes - 18 * bumps;
}

static void
auto_plan(void)
{
	int t, x, v, top = -2000000000;

	auto_turn = 0, auto_x = px;
	for (t = 0; t < (kind == P_O ? 1 : 4); t++)
		for (x = -2; x < COLS; x++)
			if ((v = auto_value(kind, t, x)) > top)
				top = v, auto_turn = t, auto_x = x;
}

static void
auto_step(long long now)
{
	if (!auto_left || state != PLAYING || now < auto_next)
		return;
	auto_next = now + 50000;
	if (turn != auto_turn) {
		if (rotate(0, now))
			return;
	} else if (px != auto_x && shift(px < auto_x ? 1 : -1, now))
		return;
	hard_drop(now);
}

/* ---- drawing ---- */

static GR_COLOR
mix(int k, int r, int g, int b, int a)
{
	return MWRGB(rgb[k][0] + (r - rgb[k][0]) * a / 256, rgb[k][1] + (g - rgb[k][1]) * a / 256, rgb[k][2] + (b - rgb[k][2]) * a / 256);
}

static void
fill(int x, int y, int w, int h, GR_COLOR colour)
{
	ui_fill(window, x, y, w, h, colour);
	drawn++;
}

/* Cells of one look, `count` of them at (xs, ys), n pixels a side: a block is four rectangles
 * (its shaded edge, its lit edge, its face and the face's shine), drawn a colour at a time. */
static void
draw_cells(int code, const short *xs, const short *ys, int count, int n)
{
	int i, k = code & 15;

	if (code == EMPTY || code == FLASH) {
		for (i = 0; i < count; i++)
			fill(xs[i], ys[i], n, n, code == FLASH ? MWRGB(250, 250, 255) : WELL_BG);
		return;
	}
	if (code & GHOST) {
		for (i = 0; i < count; i++)
			fill(xs[i], ys[i], n, n, mix(k, BG_R, BG_G, BG_B, 120));
		for (i = 0; i < count; i++)
			fill(xs[i] + 2, ys[i] + 2, n - 4, n - 4, mix(k, BG_R, BG_G, BG_B, 220));
		return;
	}
	for (i = 0; i < count; i++)
		fill(xs[i], ys[i], n, n, mix(k, 0, 0, 0, 100));
	for (i = 0; i < count; i++)
		fill(xs[i], ys[i], n - 2, n - 2, mix(k, 255, 255, 255, 120));
	for (i = 0; i < count; i++)
		fill(xs[i] + 2, ys[i] + 2, n - 4, n - 4, mix(k, 0, 0, 0, 0));
	for (i = 0; i < count && n >= 10; i++)
		fill(xs[i] + 3, ys[i] + 3, n - 6, (n - 6) / 2, mix(k, 255, 255, 255, 50));
}

/* What each shown cell of the well should be now. */
static void
well_wanted(unsigned char want[SHOWN][COLS])
{
	int r, c, i, d;

	memset(want, EMPTY, SHOWN * COLS);
	if (state == PAUSED || state == READY)
		return;
	for (r = 0; r < SHOWN; r++)
		for (c = 0; c < COLS; c++)
			if (board[r + HIDDEN][c])
				want[r][c] = clearing[r + HIDDEN] ? FLASH : state == OVER ? GREY : board[r + HIDDEN][c];
	if (state != PLAYING)
		return;
	d = ghost_on ? drop_distance() : 0;
	for (i = 0; i < 4 && d; i++) {
		r = py + d + shape[kind][turn][i][1] - HIDDEN;
		if (r >= 0)
			want[r][px + shape[kind][turn][i][0]] = (unsigned char)(GHOST | kind);
	}
	for (i = 0; i < 4; i++) {
		r = py + shape[kind][turn][i][1] - HIDDEN;
		if (r >= 0)
			want[r][px + shape[kind][turn][i][0]] = (unsigned char)kind;
	}
}

static void
text_at(int x, int y, const char *text, GR_COLOR colour, GR_FONT_ID font)
{
	GrSetGCFont(ui_gc, font);
	GrSetGCForeground(ui_gc, colour);
	GrText(window, ui_gc, x, y - (ui_smooth ? 1 : 0), (void *)text, (int)strlen(text), GR_TFASCII | GR_TFTOP);
	drawn++;
}

static int
text_width(const char *text, int is_big)
{
	return is_big ? ui_width(&big_widths, big, text, -1) : ui_width(&ui_var_widths, ui_var, text, -1);
}

#define LABEL (ui_seven ? MWRGB(0, 51, 153) : BLACK)

/* The box over the well when the game is not going: what to do, and a button that does it. */
static void
draw_overlay(void)
{
	const char *title = state == READY ? "Tetris" : state == PAUSED ? "Paused" : "Game over";
	const char *button = state == READY ? "Play" : state == PAUSED ? "Resume" : "Play again";
	char line[2][40];
	int w = 8 * C > 180 ? 8 * C : 10 * C - 9 < 180 ? 10 * C - 9 : 180, h = 120, x = WX + (10 * C - w) / 2, y = WY + (20 * C - h) / 2 - C, i;

	if (state == READY)
		snprintf(line[0], 40, "Level %d", start_level), snprintf(line[1], 40, "Enter to play");
	else if (state == PAUSED)
		snprintf(line[0], 40, "P or Enter goes on"), snprintf(line[1], 40, "F2 starts again");
	else
		snprintf(line[0], 40, "Score %d", score), snprintf(line[1], 40, "%s", best_new == 0 ? "The best yet!" : best_new > 0 ? "A new best score" : "");
	fill(x, y, w, h, ui_seven ? WHITE : UI_FACE);
	if (ui_seven)
		ui_edges(window, x, y, w, h, MWRGB(120, 120, 120), MWRGB(120, 120, 120));
	else
		ui_bevel(window, x, y, w, h, 0);
	text_at(x + (w - text_width(title, 1)) / 2, y + 12, title, LABEL, big);
	for (i = 0; i < 2; i++)
		text_at(x + (w - text_width(line[i], 0)) / 2, y + 40 + i * 16, line[i], BLACK, ui_var);
	button_x = x + (w - button_w) / 2, button_y = y + h - button_h - 12;
	ui_button(window, button_x, button_y, button_w, button_h, "", 0);
	text_at(button_x + (button_w - text_width(button, 0)) / 2, button_y + (button_h - 12) / 2, button, BLACK, ui_var);
}

static void
draw_well(void)
{
	static short xs[CODES][SHOWN * COLS], ys[CODES][SHOWN * COLS];
	static int counts[CODES];
	unsigned char want[SHOWN][COLS];
	int overlay = state == READY || state == PAUSED || state == OVER ? state : -1, r, c, k, changed = 0;

	if (overlay != overlay_shown && overlay < 0)
		well_valid = 0;
	if (!well_valid) {
		fill(WX, WY, 10 * C - 1, 20 * C - 1, WELL_BG);
		for (c = 1; c < COLS; c++)
			fill(WX + c * C - 1, WY, 1, 20 * C - 1, WELL_GRID);
		for (r = 1; r < SHOWN; r++)
			fill(WX, WY + r * C - 1, 10 * C - 1, 1, WELL_GRID);
		memset(shown, EMPTY, sizeof shown);
		well_valid = 1, changed = 1;
	}
	well_wanted(want);
	memset(counts, 0, sizeof counts);
	for (r = 0; r < SHOWN; r++)
		for (c = 0; c < COLS; c++)
			if (want[r][c] != shown[r][c]) {
				k = want[r][c];
				xs[k][counts[k]] = (short)(WX + c * C), ys[k][counts[k]] = (short)(WY + r * C);
				counts[k]++;
				shown[r][c] = want[r][c];
			}
	for (k = 0; k < CODES; k++)
		if (counts[k])
			draw_cells(k, xs[k], ys[k], counts[k], C - 1), changed = 1;
	if (overlay >= 0 && (changed || overlay != overlay_shown))
		draw_overlay();
	overlay_shown = overlay;
}

/* A piece by itself, in the middle of a box, cells n pixels a side. */
static void
draw_piece_in(int k, int code, int x, int y, int w, int h, int n)
{
	short xs[4], ys[4];
	int i, left = 4, right = 0, top = 4, bottom = 0;

	fill(x, y, w, h, WELL_BG);
	if (!k)
		return;
	for (i = 0; i < 4; i++) {
		int cx = spawn_cells[k][i][0], cy = spawn_cells[k][i][1];

		left = cx < left ? cx : left, right = cx > right ? cx : right;
		top = cy < top ? cy : top, bottom = cy > bottom ? cy : bottom;
	}
	for (i = 0; i < 4; i++) {
		xs[i] = (short)(x + (w - (right - left + 1) * n) / 2 + (spawn_cells[k][i][0] - left) * n);
		ys[i] = (short)(y + (h - (bottom - top + 1) * n) / 2 + (spawn_cells[k][i][1] - top) * n);
	}
	draw_cells(code, xs, ys, 4, n - 1);
}

static void
number(char *to, int room, int value)
{
	char digits[16];
	int n = snprintf(digits, sizeof digits, "%d", value), i, at = 0;

	for (i = 0; i < n && at < room - 2; i++) {
		if (i && (n - i) % 3 == 0)
			to[at++] = ',';
		to[at++] = digits[i];
	}
	to[at] = 0;
}

static void
draw_value(int y, int value)
{
	char text[20];

	number(text, sizeof text, value);
	fill(LX, y, PW, 22, UI_FACE);
	text_at(LX + PW - 4 - text_width(text, 1), y + 2, text, BLACK, big);
}

/* A sunk frame round a box of the panels or of the well. */
static void
frame(int x, int y, int w, int h)
{
	fill(x - 1, y - 1, w + 2, h + 2, MWRGB(0, 0, 0));
	ui_bevel(window, x - 2, y - 2, w + 4, h + 4, 1);
}

static void
draw_panels(void)
{
	static const char *const keys[] = { "Left, Right", "move", "Up, X / Z", "turn", "Down", "soft drop",
		"Space", "hard drop", "C, Shift", "hold", "P, Esc", "pause", "F2", "new game" };
	int i;

	text_at(LX, hold_y - 16, "Hold", LABEL, ui_var);
	frame(LX, hold_y, PW, 2 * S + 16);
	text_at(RX, next_y - 16, "Next", LABEL, ui_var);
	frame(RX, next_y, PW, next_h);
	fill(RX, next_y, PW, next_h, WELL_BG);
	text_at(LX, stats_y, "Score", LABEL, ui_var);
	text_at(LX, stats_y + 44, "Level", LABEL, ui_var);
	text_at(LX, stats_y + 88, "Lines", LABEL, ui_var);
	text_at(RX, best_y - 16, "Best scores", LABEL, ui_var);
	frame(RX, best_y, PW, BEST * 17 + 6);
	for (i = 0; i < 14 && keys_y + (i / 2) * 15 + 12 < WY + 20 * C; i += 2) {
		if (keys_y + (i / 2) * 15 < stats_y + 170)	/* (a small well: no room under the notes) */
			continue;
		text_at(LX, keys_y + (i / 2) * 15, keys[i], BLACK, ui_var);
		text_at(LX + PW - text_width(keys[i + 1], 0), keys_y + (i / 2) * 15, keys[i + 1], UI_SHADOW, ui_var);
	}
}

static void
draw_best(void)
{
	char text[40], value[20];
	int i, y;

	fill(RX, best_y, PW, BEST * 17 + 6, WHITE);
	for (i = 0; i < BEST; i++) {
		y = best_y + 3 + i * 17;
		if (i == best_new)
			fill(RX, y, PW, 17, UI_SELECTED);
		if (best[i].score <= 0) {
			text_at(RX + 6, y + 2, i ? "" : "None yet", UI_SHADOW, ui_var);
			continue;
		}
		snprintf(text, sizeof text, "%d.", i + 1);
		text_at(RX + 6, y + 2, text, i == best_new ? WHITE : UI_SHADOW, ui_var);
		number(value, sizeof value, best[i].score);
		text_at(RX + 24, y + 2, value, i == best_new ? WHITE : BLACK, ui_var);
		snprintf(text, sizeof text, "L%d", best[i].level);
		text_at(RX + PW - 6 - text_width(text, 0), y + 2, text, i == best_new ? WHITE : UI_SHADOW, ui_var);
	}
}

/* Whatever changed since the last time it was drawn. */
static void
draw_changes(void)
{
	int i, y, hold_now = state == READY || state == PAUSED ? 0 : held | (hold_used ? 64 : 0);

	draw_well();
	if (hold_now != hold_shown) {
		draw_piece_in(hold_now & 15, hold_used ? GREY : hold_now & 15, LX, hold_y, PW, 2 * S + 16, S);
		hold_shown = hold_now;
	}
	for (i = 0, y = next_y + 4; i < NEXT; i++) {
		int k = state == READY || state == PAUSED ? 0 : queue[i], h = i ? 2 * T + 8 : 2 * S + 12;

		if (k != next_shown[i])
			draw_piece_in(k, k, RX, y, PW, h, i ? T : S);
		next_shown[i] = k;
		y += h;
	}
	if (!stats_shown || score != score_shown)
		draw_value(stats_y + 14, score), score_shown = score;
	if (!stats_shown || level != level_shown)
		draw_value(stats_y + 58, level), level_shown = level;
	if (!stats_shown || lines != lines_shown)
		draw_value(stats_y + 102, lines), lines_shown = lines;
	stats_shown = 1;
	if (note_shown != note_gen) {
		fill(LX, stats_y + 132, PW, 36, UI_FACE);
		for (i = 0; i < 2; i++)
			if (note[i][0])
				text_at(LX, stats_y + 132 + i * 17, note[i], i ? UI_SHADOW : LABEL, i ? ui_var : big);
		note_shown = note_gen;
	}
	if (!best_shown)
		draw_best(), best_shown = 1;
}

static void
draw_all(void)
{
	int i;

	fill(0, 0, width, height, UI_FACE);
	for (i = 0; i < 2; i++)
		text_at(menu_x[i] + 8, 3, menu_names[i], BLACK, ui_var);
	fill(0, MENU_H - 1, width, 1, UI_SHADOW);
	frame(WX, WY, 10 * C - 1, 20 * C - 1);
	draw_panels();
	well_valid = 0, overlay_shown = -2, hold_shown = -1, stats_shown = 0, best_shown = 0, note_shown = -1;
	for (i = 0; i < NEXT; i++)
		next_shown[i] = -1;
	draw_changes();
}

/* Sizes from the screen's: the well's cell as large as the screen's height leaves room for. */
static void
layout(void)
{
	const GR_SCREEN_INFO *screen = ui_screen();
	int x;

	C = (screen->rows - UI_MENU_BAR - 34 - MENU_H - 2 * PAD) / 20;
	C = C > 24 ? 24 : C < 12 ? 12 : C;
	S = C * 3 / 4, T = C / 2;
	PW = 4 * S + 16 > 118 ? 4 * S + 16 : 118;
	LX = PAD + 2;
	WX = LX + PW + PAD + 4, WY = MENU_H + PAD + 2;
	RX = WX + 10 * C - 1 + PAD + 4;
	width = RX + PW + PAD + 2, height = WY + 20 * C - 1 + PAD + 2;
	hold_y = WY + 16;
	stats_y = hold_y + 2 * S + 16 + 12;
	keys_y = WY + 20 * C - 7 * 15;
	next_y = WY + 16;
	next_h = 2 * S + 12 + (NEXT - 1) * (2 * T + 8) + 8;
	best_y = next_y + next_h + 28;
	x = 2;
	menu_x[0] = x;
	x += ui_width(&ui_var_widths, ui_var, menu_names[0], -1) + 16;
	menu_x[1] = x;
	menu_x[2] = x + ui_width(&ui_var_widths, ui_var, menu_names[1], -1) + 16;
}

static void
fonts(void)
{
	if (big)
		GrDestroyFont(big);
	big = GrCreateFontEx(ui_smooth ? FONT_BIG : GR_FONT_SYSTEM_VAR, 0, 0, NULL);
	big_widths.known = 0;
}

/* ---- the registry ---- */

static void
best_load(void)
{
	const char *at = reg_get("nxtetris.best", "");
	int i;

	memset(best, 0, sizeof best);
	for (i = 0; i < BEST && *at; i++) {
		if (sscanf(at, "%d/%d/%d", &best[i].score, &best[i].lines, &best[i].level) != 3)
			break;
		at += strcspn(at, " ");
		at += *at == ' ';
	}
}

/* ---- events ---- */

/* The next event, or a timeout: the window system is asked once and the wait is this
 * program's, on its socket (as nxmon does: GrGetNextEventTimeout asks anew every call). */
static GR_EVENT taken;
static int took;

static void
take(GR_EVENT *event)
{
	taken = *event;
	took = 1;
}

static void
next_event(GR_EVENT *event, int ms)
{
	static int asked, fd = -1;
	struct timeval limit;
	fd_set set;
	int most = 0;

	GrFlush();
	FD_ZERO(&set);
	took = 0;
	GrServiceSelect(&set, take);
	if (took)
		asked = 0;
	if (!took && !asked) {
		GrPrepareSelect(&most, &set);
		for (fd = most; fd > 0 && !FD_ISSET(fd, &set); fd--)
			continue;
		asked = 1;
	}
	if (!took) {
		FD_ZERO(&set);
		FD_SET(fd, &set);
		limit.tv_sec = ms / 1000, limit.tv_usec = ms % 1000 * 1000;
		if (select(fd + 1, &set, NULL, NULL, ms < 0 ? NULL : &limit) > 0) {
			GrServiceSelect(&set, take);
			asked = 0;
		}
	}
	if (took)
		*event = taken;
	else
		event->type = GR_EVENT_TYPE_TIMEOUT;
}

static void
pause_game(void)
{
	if (state == PLAYING || state == CLEARING)
		state = PAUSED;
	dir = soft = 0;
	memset(keys_held, 0, sizeof keys_held);
}

static void
resume(long long now)
{
	int r;

	state = PLAYING;
	fall_next = now + fall_us();
	lock_at = now + LOCK_US;
	for (r = 0; r < ROWS; r++)	/* (rows that were flashing when it paused flash again) */
		if (clearing[r])
			state = CLEARING, clear_end = now + CLEAR_US;
}

/* The overlay's button, Enter or Space: play, go on, or play again. */
static void
go(long long now)
{
	if (state == PAUSED)
		resume(now);
	else if (state == READY || state == OVER)
		new_game(now);
}

static int menu_paused;	/* the menu open now paused the game */

static void
open_menu(int which, int keyboard)
{
	int x, y, i;

	menu_paused = state != PAUSED;
	pause_game();
	for (i = 0; i < 15; i++)
		level_items[i].flags = i + 1 == start_level ? UI_MENU_CHECK : 0;
	game_items[3].flags = ghost_on ? UI_MENU_CHECK : 0;
	ui_window_origin(window, &x, &y);
	if (which)
		ui_menu_open(help_items, 2, x + menu_x[1], y + MENU_H, 0, keyboard);
	else
		ui_menu_open(game_items, 6, x + menu_x[0], y + MENU_H, 0, keyboard);
}

static void draw_all(void);

/* The window's own events while a question is asked. */
static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw_all();
	if (event->type == GR_EVENT_TYPE_CLOSE_REQ && event->general.wid == window) {
		GrClose();
		exit(0);
	}
}

static void
command(int id, long long now)
{
	static const char *const ok[] = { "OK" };
	static const char *const sure[] = { "Clear", "Cancel" };

	if (id > C_LEVEL && id <= C_LEVEL + 15) {
		start_level = id - C_LEVEL;
		reg_set_int("nxtetris.level", start_level);
		if (state == READY)
			overlay_shown = -2;
		return;
	}
	switch (id) {
	case C_NEW:
		new_game(now);
		break;
	case C_PAUSE:
		/* (the menu paused it to show itself: Pause from there is to stay so) */
		if (state == PAUSED && !menu_paused)
			resume(now);
		else
			pause_game();
		break;
	case C_GHOST:
		ghost_on = !ghost_on;
		reg_set_int("nxtetris.ghost", ghost_on);
		break;
	case C_CLEAR:
		if (ui_ask("Tetris", "Clear the best scores?", sure, 2, others) == 0) {
			reg_set("nxtetris.best", NULL);
			best_load();
			best_new = -1, best_shown = 0;
		}
		break;
	case C_EXIT:
		GrClose();
		exit(0);
	case C_KEYS:
		ui_ask("Keys", "Left, Right: move     Down: soft drop     Space: hard drop\n"
			"Up or X: turn right     Z or Ctrl: turn left\nC or Shift: hold the piece for later\n"
			"P or Escape: pause     F2: a new game     F10: the menus", ok, 1, others);
		break;
	case C_ABOUT:
		ui_ask("About Tetris", "Tetris for the desktop: the guideline's rules,\nSRS turns, a held piece and five to come.\n"
			"Ten lines a level; T-spins, combos and\nback-to-back clears score more.", ok, 1, others);
		break;
	}
	if (id == C_CLEAR || id == C_KEYS || id == C_ABOUT)
		GrSetFocus(window);	/* (the question had the keyboard) */
}

enum { K_LEFT, K_RIGHT, K_DOWN, K_CW, K_CCW, K_DROP, K_HOLD };

static struct ui_pad pad;
static int pad_print;

/* A row down now (a notch of the left stick down): a soft drop's step. */
static void
step_down(long long now)
{
	if (fits(kind, turn, px, py + 1)) {
		py++;
		score++;
		last_turned = 0;
		moved(now);
		fall_next = now + fall_us();
	}
}

/* A controller's move (docs/machine/input.md, "Game controls"): what a key would do, once. */
static void
pad_move(int k, long long now)
{
	if (state != PLAYING)
		return;
	switch (k) {
	case K_LEFT: case K_RIGHT:
		shift(k == K_LEFT ? -1 : 1, now);
		break;
	case K_DOWN:
		step_down(now);
		break;
	case K_CW: case K_CCW:
		rotate(k == K_CCW, now);
		break;
	case K_DROP:
		hard_drop(now);
		break;
	case K_HOLD:
		hold(now);
		break;
	}
	if (pad_print) {
		printf("nxtetris: pad %s: piece %d at %d,%d turned %d, score %d\n",
		       (const char *[]){ "left", "right", "down", "cw", "ccw", "drop", "hold" }[k], kind, px, py, turn, score);
		fflush(stdout);
	}
}

/* The wheels: the left stick (the second wheel) moves, the right one (or a mouse's wheel) turns. */
static int
pad_scroll(const GR_EVENT *event, long long now)
{
	int x, y, which;

	if (!ui_pad_scroll(event, &x, &y, &which))
		return 0;
	if (which)
		pad_move(x < 0 ? K_LEFT : x > 0 ? K_RIGHT : y < 0 ? K_DOWN : K_HOLD, now);
	else
		pad_move(y > 0 || x > 0 ? K_CW : K_CCW, now);
	return 1;
}

/* The hands' buttons, read from the machine's word where the loop wakes: A drops (or plays), B holds, Y pauses. */
static void
pad_buttons(long long now)
{
	ui_pad_poll(&pad);
	if (!pad.down)
		return;
	if (state != PLAYING && state != CLEARING) {
		if (pad.down & (INPUT_JUMP | INPUT_LEFT_UPPER))
			go(now);
		return;
	}
	if (pad.down & INPUT_LEFT_UPPER)
		pause_game();
	else if (pad.down & INPUT_JUMP)
		pad_move(K_DROP, now);
	else if (pad.down & INPUT_RIGHT_UPPER)
		pad_move(K_HOLD, now);
}

static int
key_of(int ch)
{
	switch (ch) {
	case MWKEY_LEFT: case MWKEY_KP4: return K_LEFT;
	case MWKEY_RIGHT: case MWKEY_KP6: return K_RIGHT;
	case MWKEY_DOWN: case MWKEY_KP2: return K_DOWN;
	case MWKEY_UP: case 'x': case MWKEY_KP8: return K_CW;
	case 'z': case MWKEY_LCTRL: case MWKEY_RCTRL: return K_CCW;
	case ' ': return K_DROP;
	case 'c': case MWKEY_LSHIFT: case MWKEY_RSHIFT: return K_HOLD;
	}
	return -1;
}

static void
key_down(int ch, long long now)
{
	int k;

	if (ch < 128)
		ch = tolower(ch);
	if (ch == MWKEY_F10) {
		open_menu(0, 1);
		return;
	}
	if (ch == MWKEY_F2) {
		new_game(now);
		return;
	}
	if (state != PLAYING && state != CLEARING) {
		if (ch == MWKEY_ENTER || ch == '\n' || ch == '\r' || (ch == ' ' && state != PAUSED) || (state == PAUSED && ch == 'p')) {
			if (ch == ' ') {	/* (the key's repeat, from the last drop or into the first piece, is not a press) */
				if (keys_held[K_DROP])
					return;
				keys_held[K_DROP] = 1;
			}
			go(now);
		}
		return;
	}
	if (ch == 'p' || ch == MWKEY_ESCAPE || ch == MWKEY_PAUSE) {
		pause_game();
		return;
	}
	if ((k = key_of(ch)) < 0 || keys_held[k])
		return;	/* (the kernel's own repeat: this program repeats by itself) */
	keys_held[k] = 1;
	if (state != PLAYING)
		return;
	switch (k) {
	case K_LEFT: case K_RIGHT:
		dir = k == K_LEFT ? -1 : 1;
		shift(dir, now);
		das_next = now + das_us;
		break;
	case K_DOWN:
		soft = 1;
		fall_next = now;
		break;
	case K_CW: case K_CCW:
		rotate(k == K_CCW, now);
		break;
	case K_DROP:
		hard_drop(now);
		break;
	case K_HOLD:
		hold(now);
		break;
	}
}

static void
key_up(int ch, long long now)
{
	int k;

	if (ch < 128)
		ch = tolower(ch);
	if ((k = key_of(ch)) < 0)
		return;
	keys_held[k] = 0;
	if (k == K_LEFT || k == K_RIGHT) {
		int other = k == K_LEFT ? K_RIGHT : K_LEFT;

		if (dir == (k == K_LEFT ? -1 : 1)) {
			dir = keys_held[other] ? -dir : 0;
			das_next = now + das_us;
		}
	} else if (k == K_DOWN && soft) {
		soft = 0;
		fall_next = now + fall_us();
	}
}

int
main(void)
{
	GR_EVENT event = { 0 };
	const char *a;
	int i, id;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	fonts();
	make_shapes();
	srand(getenv("NXTETRIS_SEED") ? (unsigned)atoi(getenv("NXTETRIS_SEED")) : (unsigned)now_us());
	start_level = reg_int("nxtetris.level", 1);
	start_level = start_level < 1 || start_level > 15 ? 1 : start_level;
	ghost_on = reg_int("nxtetris.ghost", 1) != 0;
	das_us = reg_int("nxtetris.das", 170) * 1000;
	arr_us = reg_int("nxtetris.arr", 40) * 1000;
	arr_us = arr_us < 1000 ? 1000 : arr_us;
	best_load();
	for (i = 0; i < 15; i++) {
		snprintf(level_labels[i], sizeof level_labels[i], "%d", i + 1);
		level_items[i].label = level_labels[i], level_items[i].icon = -1, level_items[i].id = C_LEVEL + i + 1;
	}
	layout();
	window = GrNewWindowEx(UI_FIXED, "Tetris", GR_ROOT_WINDOW_ID, -1, -1, width, height, UI_FACE);
	GrSelectEvents(window, EVENTS);
	ui_pad_ask(window);
	pad_print = getenv("NXTETRIS_PRINT") != NULL;
	GrMapWindow(window);
	GrSetFocus(window);
	if ((a = getenv("NXTETRIS_AUTO")) && atoi(a) > 0) {
		auto_left = atoi(a);
		printf("NXTETRIS-GO\n");
		fflush(stdout);
		new_game(now_us());
	}
	for (;;) {
		long long now = now_us();

		pad_buttons(now);
		advance(now);
		auto_step(now);
		draw_changes();
		i = wake_ms(now_us());
		if (pad.held & INPUT_HANDS && (i < 0 || i > 100))
			i = 100;	/* (a headset's buttons are no event: looked at ten times a second) */
		next_event(&event, i);
		now = now_us();
		if (event.type != GR_EVENT_TYPE_TIMEOUT) {
			if (ui_restyle()) {	/* (Settings changed the look) */
				ui_menu_restyle();
				fonts();
				layout();
				GrSetWindowBackgroundColor(window, UI_FACE);
				draw_all();
			}
		}
		ui_origin_note(&event);
		switch (ui_menu_event(&event, &id)) {
		case UI_MENU_CHOSEN:
			GrSetFocus(window);
			command(id, now);
			continue;
		case UI_MENU_CLOSED:
			GrSetFocus(window);
			/* fall through */
		case UI_MENU_TOOK:
			continue;
		}
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (event.exposure.wid == window)
				draw_all();
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key_down(event.keystroke.ch, now);
			break;
		case GR_EVENT_TYPE_KEY_UP:
			key_up(event.keystroke.ch, now);
			break;
		case GR_EVENT_TYPE_FOCUS_OUT:
			if (!auto_left)
				pause_game();
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.wid != window || pad_scroll(&event, now) || ui_wheel(&event))
				break;
			if (event.button.y < MENU_H) {
				for (i = 0; i < 2; i++)
					if (event.button.x >= menu_x[i] && event.button.x < menu_x[i + 1])
						open_menu(i, 0);
			} else if (overlay_shown >= 0 && ui_inside(event.button.x, event.button.y, button_x, button_y, button_w, button_h))
				go(now);
			else if (state == PLAYING && (event.button.changebuttons & GR_BUTTON_L))
				pad_move(K_CW, now);	/* (a click on the well turns: a trigger, with the beam on it) */
			else if (state == PLAYING && (event.button.changebuttons & GR_BUTTON_R))
				pad_move(K_DROP, now);	/* (and the right button, a grip, drops) */
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
	}
}
