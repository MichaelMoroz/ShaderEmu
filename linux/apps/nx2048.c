/*
 * nx2048: slide the numbered tiles of a 4 x 4 board; two of the same number that meet become their
 * sum, and every move brings a new 2 (or, one time in ten, a 4). 2048 wins, and the game can go on;
 * it ends when no move changes the board. One move can be taken back. The best score is the
 * registry's (nx2048.best). A move is shown moving: the tiles slide to their places (SLIDE_MS), then
 * a joined tile swells and settles and the new one grows from nothing (POP_MS); each frame is drawn
 * into a picture of the board and copied into the window whole. A key during a move ends it at once.
 *
 * Keys: the arrows or W, A, S, D move, U or Ctrl+Z takes the last move back, F2 starts again, F10
 * the menus. The pointer: a drag across the board is a move that way. A controller (ui_pad.h): the
 * left stick's notches move, the wheel moves up and down, A plays again once it is over, B undoes.
 *
 *   NX2048_SEED=N     the tiles' order (a test's)
 *   NX2048_PRINT=1    each move said: "nx2048: left: score S, tile T, free F, sum X"
 *   NX2048_AUTO=N     N moves made by itself (down, left, right, else up), then what it came to
 *   NX2048_ANIM=0     no animation: each move drawn at once
 *   NX2048_HOLD_MS=N  every move's animation held at its frame of N ms (a capture's)
 */
#include <ctype.h>
#include "ui.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include "ui_pad.h"
#include <time.h>

#define N 4
#define MENU_H 20
#define PAD 12
#define HEAD 64			/* the score boxes over the board */
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | \
	GR_EVENT_MASK_CLOSE_REQ)

enum { LEFT, RIGHT, UP, DOWN };
static const char *const way_names[] = { "left", "right", "up", "down" };
enum { PLAYING, WON, OVER };

/* the game: a tile is its power of two (1: 2, 11: 2048), 0 empty */
static unsigned char board[N][N], before[N][N];
static int score, score_before, can_undo, state, kept_going, best;
static unsigned seed = 1;

/* a move's animation: each tile from where it was to where it went (one that stayed too), then which
 * cells joined (1) and which one is new (2) */
#define SLIDE_MS 100
#define POP_MS 120
static struct { signed char fr, fc, tr, tc; unsigned char p; } movers[N * N];
static int nmovers, recording, animate = 1, hold_ms = -1;
static unsigned char popped[N][N];
static long long anim_start;	/* 0: no move is being shown */

/* the window */
static GR_WINDOW_ID window;
static GR_WINDOW_ID board_pix;	/* the board's picture, each frame drawn there and copied whole (no flicker) */
static GR_FONT_ID font_tile[3], font_head;
static int T, G, BX, BY, BW, width, height;	/* a tile, the gap, the board's corner and side */
static int menu_x[3], press_x, press_y, pressed, print, auto_left;
static struct ui_pad pad;

/* a tile's face and its number's colour, by its power: cool greys to blues, purples, then warm */
static const unsigned char faces[18][3] = {
	{ 205, 210, 220 }, { 236, 240, 246 }, { 220, 232, 246 }, { 160, 205, 245 }, { 110, 175, 240 }, { 70, 140, 225 },
	{ 120, 110, 225 }, { 160, 90, 215 }, { 205, 80, 190 }, { 235, 95, 140 }, { 245, 130, 70 }, { 250, 185, 40 },
	{ 60, 60, 72 }, { 50, 50, 62 }, { 40, 40, 52 }, { 30, 30, 42 }, { 20, 20, 32 }, { 10, 10, 22 },
};

enum { C_NEW = 1, C_UNDO, C_CLEAR, C_EXIT, C_KEYS, C_ABOUT };
static const char *const menu_names[] = { "Game", "Help" };
static const struct ui_menu_item game_items[] = {
	{ "New game   F2", -1, C_NEW }, { "Undo   U", -1, C_UNDO }, { "Clear best score...", -1, C_CLEAR, NULL, 0, UI_MENU_LINE },
	{ "Exit", -1, C_EXIT, NULL, 0, UI_MENU_LINE },
};
static const struct ui_menu_item help_items[] = { { "Keys", -1, C_KEYS }, { "About 2048", -1, C_ABOUT } };

static unsigned
roll(void)
{
	seed = seed * 1103515245u + 12345u;
	return seed >> 16;
}

/* ---- the game ---- */

static int
free_cells(void)
{
	int r, c, n = 0;

	for (r = 0; r < N; r++)
		for (c = 0; c < N; c++)
			n += !board[r][c];
	return n;
}

static int
top_tile(void)
{
	int r, c, top = 0;

	for (r = 0; r < N; r++)
		for (c = 0; c < N; c++)
			top = board[r][c] > top ? board[r][c] : top;
	return top ? 1 << top : 0;
}

static unsigned
board_sum(void)
{
	unsigned h = 2166136261u;
	int r, c;

	for (r = 0; r < N; r++)
		for (c = 0; c < N; c++)
			h = (h ^ board[r][c]) * 16777619u;
	return h ^ (unsigned)score;
}

static void
add_tile(void)
{
	int k = free_cells(), r, c;

	if (!k)
		return;
	k = (int)(roll() % (unsigned)k);
	for (r = 0; r < N; r++)
		for (c = 0; c < N; c++)
			if (!board[r][c] && k-- == 0) {
				board[r][c] = roll() % 10 ? 1 : 2;
				if (recording)
					popped[r][c] = 2;
			}
}

/* One line of four slid toward its start: equal neighbours join once. Returns the points. `at` is
 * each place's cell, for the animation: where each tile went and which joined (while recording). */
static int
slide(unsigned char *line[N], const int at[N][2])
{
	unsigned char out[N] = { 0 };
	int i, n = 0, points = 0, joined = 0, to;

	for (i = 0; i < N; i++) {
		if (!*line[i])
			continue;
		if (n && !joined && out[n - 1] == *line[i]) {
			out[n - 1]++;
			points += 1 << out[n - 1];
			joined = 1;
			to = n - 1;
			if (recording)
				popped[at[to][0]][at[to][1]] = 1;
		} else {
			out[n++] = *line[i];
			joined = 0;
			to = n - 1;
		}
		if (recording && nmovers < N * N) {
			movers[nmovers].fr = (signed char)at[i][0], movers[nmovers].fc = (signed char)at[i][1];
			movers[nmovers].tr = (signed char)at[to][0], movers[nmovers].tc = (signed char)at[to][1];
			movers[nmovers++].p = *line[i];
		}
	}
	for (i = 0; i < N; i++)
		*line[i] = out[i];
	return points;
}

/* The board slid one way; 1 if anything moved. `apply` 0 only asks. */
static int
move(int way, int apply)
{
	unsigned char save[N][N], *line[N];
	int k, i, points = 0, moved, at[N][2];

	memcpy(save, board, sizeof board);
	recording = apply;
	if (apply)
		nmovers = 0, memset(popped, 0, sizeof popped);
	for (k = 0; k < N; k++) {
		for (i = 0; i < N; i++) {
			at[i][0] = way == LEFT || way == RIGHT ? k : way == UP ? i : N - 1 - i;
			at[i][1] = way == UP || way == DOWN ? k : way == LEFT ? i : N - 1 - i;
			line[i] = &board[at[i][0]][at[i][1]];
		}
		points += slide(line, at);
	}
	moved = memcmp(save, board, sizeof board) != 0;
	if (!apply || !moved) {
		memcpy(board, save, sizeof board);
		recording = 0;
		if (apply)
			nmovers = 0;
		return moved;
	}
	memcpy(before, save, sizeof board);
	score_before = score, can_undo = 1;
	score += points;
	add_tile();
	recording = 0;
	if (score > best) {
		best = score;
		reg_set_int("nx2048.best", best);
	}
	if (top_tile() >= 2048 && !kept_going && state == PLAYING)
		state = WON;
	else if (!move(LEFT, 0) && !move(RIGHT, 0) && !move(UP, 0) && !move(DOWN, 0))
		state = OVER;
	return 1;
}

static void
new_game(void)
{
	memset(board, 0, sizeof board);
	score = 0, can_undo = 0, state = PLAYING, kept_going = 0;
	add_tile();
	add_tile();
}

static void
undo(void)
{
	if (!can_undo)
		return;
	memcpy(board, before, sizeof board);
	score = score_before, can_undo = 0;
	if (state == OVER || (state == WON && top_tile() < 2048))
		state = PLAYING;
}

/* ---- drawing ---- */

static void
text_on(GR_DRAW_ID d, int x, int y, int w, int h, const char *text, GR_COLOR colour, GR_FONT_ID font)
{
	GR_SIZE tw, th, tb;

	GrSetGCFont(ui_gc, font);
	GrGetGCTextSize(ui_gc, (void *)text, -1, GR_TFASCII, &tw, &th, &tb);
	GrSetGCForeground(ui_gc, colour);
	GrText(d, ui_gc, x + (w - tw) / 2, y + (h - th) / 2, (void *)text, -1, GR_TFASCII | GR_TFTOP);
}

static void
text_in(int x, int y, int w, int h, const char *text, GR_COLOR colour, GR_FONT_ID font)
{
	text_on(window, x, y, w, h, text, colour, font);
}

/* A tile of power p (0: an empty cell) with its corner at x, y of the board's picture, `size` pixels a
 * side about the middle of the cell's T (a tile swelling or growing); its number only once it is half there. */
static void
tile_at(int x, int y, int p, int size)
{
	const unsigned char *f = faces[p > 17 ? 17 : p];
	int shade = size > 16 ? 4 : 0;
	char text[12];

	x += (T - size) / 2, y += (T - size) / 2;
	if (size <= 0)
		return;
	ui_fill(board_pix, x, y, size, size, MWRGB(f[0], f[1], f[2]));
	if (!p)
		return;
	ui_fill(board_pix, x, y + size - shade, size, shade, MWRGB(f[0] * 3 / 4, f[1] * 3 / 4, f[2] * 3 / 4));	/* (a shade under it) */
	if (size < T / 2)
		return;
	snprintf(text, sizeof text, "%d", 1 << p);
	text_on(board_pix, x, y, size, size - shade, text, p <= 2 ? MWRGB(70, 75, 90) : WHITE,
		font_tile[strlen(text) <= 2 ? 0 : strlen(text) == 3 ? 1 : 2]);
}

/* The board at `ms` into a move's animation (or as it is, with none), into its picture and onto the window. */
static void
draw_board(long long ms)
{
	int r, c, i, k;

	ui_fill(board_pix, 0, 0, BW, BW, MWRGB(150, 160, 180));
	for (r = 0; r < N; r++)
		for (c = 0; c < N; c++)
			tile_at(G + c * (T + G), G + r * (T + G), 0, T);
	if (anim_start && ms < SLIDE_MS) {
		/* sliding, slowing as it comes (1 - (1 - u)^2), each tile with the number it had */
		k = (int)(1000 - (1000 - ms * 1000 / SLIDE_MS) * (1000 - ms * 1000 / SLIDE_MS) / 1000);
		for (i = 0; i < nmovers; i++) {
			int x0 = G + movers[i].fc * (T + G), y0 = G + movers[i].fr * (T + G);
			int x1 = G + movers[i].tc * (T + G), y1 = G + movers[i].tr * (T + G);

			tile_at(x0 + (x1 - x0) * k / 1000, y0 + (y1 - y0) * k / 1000, movers[i].p, T);
		}
	} else {
		/* the board as it is now: a joined tile swells by a fifth and back (4u(1 - u)), the new one grows (1 - (1 - u)^2) */
		int u = anim_start ? (int)((ms - SLIDE_MS) * 1000 / POP_MS) : 1000;

		u = u > 1000 ? 1000 : u;
		for (r = 0; r < N; r++)
			for (c = 0; c < N; c++) {
				int size = T;

				if (!board[r][c])
					continue;
				if (anim_start && popped[r][c] == 1)
					size = T + T * 4 * u * (1000 - u) / 1000 / 1000 / 5;
				else if (anim_start && popped[r][c] == 2)
					size = T * (1000 - (1000 - u) * (1000 - u) / 1000) / 1000;
				tile_at(G + c * (T + G), G + r * (T + G), board[r][c], size);
			}
	}
	GrCopyArea(window, ui_gc, BX, BY, BW, BW, board_pix, 0, 0, MWROP_COPY);
}

static long long
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void
draw_box(int x, const char *label, int value)
{
	char text[16];

	ui_fill(window, x, MENU_H + 10, 110, HEAD - 18, MWRGB(70, 80, 105));
	text_in(x, MENU_H + 12, 110, 16, label, MWRGB(200, 210, 230), ui_var);
	snprintf(text, sizeof text, "%d", value);
	text_in(x, MENU_H + 28, 110, 24, text, WHITE, font_head);
}

static void
draw_overlay(void)
{
	const char *title = state == WON ? "2048!" : "No moves left", *line = state == WON ? "Enter keeps going, F2 starts again" :
		"F2 or Enter: a new game, U takes the last move back";
	int w = BW - 40, h = 96, x = BX + 20, y = BY + (BW - h) / 2;

	ui_fill(window, x, y, w, h, ui_seven ? WHITE : UI_FACE);
	ui_edges(window, x, y, w, h, MWRGB(120, 120, 120), MWRGB(120, 120, 120));
	text_in(x, y + 10, w, 36, title, state == WON ? MWRGB(220, 140, 20) : MWRGB(60, 70, 100), font_head);
	text_in(x, y + 54, w, 20, line, BLACK, ui_var);
}

static void
draw_all(void)
{
	int i;

	ui_fill(window, 0, 0, width, height, UI_FACE);
	for (i = 0; i < 2; i++)
		ui_text(window, menu_x[i] + 8, 3, menu_names[i], -1, BLACK, 0);
	ui_fill(window, 0, MENU_H - 1, width, 1, UI_SHADOW);
	draw_box(BX, "SCORE", score);
	draw_box(BX + 120, "BEST", best);
	ui_text(window, BX + 250, MENU_H + 20, "Join the tiles,", -1, UI_SHADOW, 0);
	ui_text(window, BX + 250, MENU_H + 36, "get to 2048.", -1, UI_SHADOW, 0);
	anim_start = 0;		/* (the whole window drawn: the board as it is, any move shown to its end) */
	draw_board(0);
	if (state == OVER || state == WON)
		draw_overlay();
}

/* Sizes from the screen's: the tile as large as the screen's height leaves room for. */
static void
layout(void)
{
	const GR_SCREEN_INFO *screen = ui_screen();
	int x;

	T = (screen->rows - UI_MENU_BAR - 34 - MENU_H - HEAD - 2 * PAD - 5 * 10) / N;
	T = T > 110 ? 110 : T < 48 ? 48 : T;
	G = T / 8 < 6 ? 6 : T / 8;
	BW = N * T + (N + 1) * G;
	BX = PAD, BY = MENU_H + HEAD;
	width = BW + 2 * PAD, height = BY + BW + PAD;
	if (width < 420)
		width = 420, BX = (width - BW) / 2;
	x = 2;
	menu_x[0] = x;
	x += ui_width(&ui_var_widths, ui_var, menu_names[0], -1) + 16;
	menu_x[1] = x;
	menu_x[2] = x + ui_width(&ui_var_widths, ui_var, menu_names[1], -1) + 16;
}

static void
fonts(void)
{
	static const int sizes[3] = { 45, 36, 28 };	/* hundredths of a tile: two digits, three, four and more */
	int i;

	for (i = 0; i < 3; i++) {
		if (font_tile[i])
			GrDestroyFont(font_tile[i]);
		font_tile[i] = GrCreateFontEx(ui_smooth ? "/usr/share/font-ui-b-32.sef" : GR_FONT_SYSTEM_VAR, ui_smooth ? T * sizes[i] / 100 : 0, 0, NULL);
	}
	if (font_head)
		GrDestroyFont(font_head);
	font_head = GrCreateFontEx(ui_smooth ? "/usr/share/font-ui-b-24.sef" : GR_FONT_SYSTEM_VAR, 0, 0, NULL);
}

/* ---- input ---- */

static void
moved_by(int way)
{
	if (state == OVER || state == WON)
		return;
	anim_start = 0;		/* (a move still being shown is at its end: the board is already its last) */
	if (!move(way, 1))
		return;
	if (print) {
		printf("nx2048: %s: score %d, tile %d, free %d, sum %08x%s\n", way_names[way], score, top_tile(), free_cells(), board_sum(),
		       state == WON ? ", 2048 made" : state == OVER ? ", no moves left" : "");
		fflush(stdout);
	}
	if (!animate || state != PLAYING) {
		draw_all();
		return;
	}
	/* the scores at once, the board over the next frames (the main loop's) */
	draw_box(BX, "SCORE", score);
	draw_box(BX + 120, "BEST", best);
	anim_start = now_ms();
	draw_board(0);
}

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
command(int id)
{
	static const char *const ok[] = { "OK" };
	static const char *const sure[] = { "Clear", "Cancel" };

	switch (id) {
	case C_NEW:
		new_game();
		break;
	case C_UNDO:
		undo();
		break;
	case C_CLEAR:
		if (ui_ask("2048", "Clear the best score?", sure, 2, others) == 0) {
			reg_set("nx2048.best", NULL);
			best = score;
		}
		GrSetFocus(window);
		break;
	case C_EXIT:
		GrClose();
		exit(0);
	case C_KEYS:
		ui_ask("Keys", "Arrows or W, A, S, D: slide the tiles\nA drag across the board: the same\n"
			"U or Ctrl+Z: take the last move back\nF2: a new game     F10: the menus", ok, 1, others);
		GrSetFocus(window);
		break;
	case C_ABOUT:
		ui_ask("About 2048", "Slide the tiles: two of a number that meet\njoin into their sum. Make a 2048 tile.", ok, 1, others);
		GrSetFocus(window);
		break;
	}
	draw_all();
}

static void
open_menu(int which, int keyboard)
{
	int x, y;

	ui_window_origin(window, &x, &y);
	if (which)
		ui_menu_open(help_items, 2, x + menu_x[1], y + MENU_H, 0, keyboard);
	else
		ui_menu_open(game_items, 4, x + menu_x[0], y + MENU_H, 0, keyboard);
}

static void
key_down(int ch, int modifiers)
{
	if (ch < 128)
		ch = tolower(ch);
	switch (ch) {
	case MWKEY_LEFT: case 'a': case MWKEY_KP4: moved_by(LEFT); break;
	case MWKEY_RIGHT: case 'd': case MWKEY_KP6: moved_by(RIGHT); break;
	case MWKEY_UP: case 'w': case MWKEY_KP8: moved_by(UP); break;
	case MWKEY_DOWN: case 's': case MWKEY_KP2: moved_by(DOWN); break;
	case 'u': undo(); draw_all(); break;
	case 'z': if (modifiers & MWKMOD_CTRL) undo(), draw_all(); break;
	case 26: undo(); draw_all(); break;	/* (Ctrl+Z as a character) */
	case MWKEY_F2: new_game(); draw_all(); break;
	case MWKEY_F10: open_menu(0, 1); break;
	case MWKEY_ENTER: case ' ':
		if (state == WON)
			state = PLAYING, kept_going = 1, draw_all();
		else if (state == OVER)
			new_game(), draw_all();
		break;
	}
}

/* NX2048_AUTO: down, left, right, else up, as a simple player does */
static void
auto_play(int moves)
{
	static const int order[4] = { DOWN, LEFT, RIGHT, UP };
	int m, k;

	for (m = 0; m < moves && state != OVER; m++) {
		if (state == WON)
			state = PLAYING, kept_going = 1;
		for (k = 0; k < 4 && !move(order[k], 1); k++)
			continue;
	}
	printf("nx2048: auto %d moves: score %d, tile %d, free %d, sum %08x%s\n", m, score, top_tile(), free_cells(), board_sum(),
	       state == OVER ? ", no moves left" : "");
	fflush(stdout);
}

int
main(void)
{
	GR_EVENT event = { 0 };
	const char *a;
	int id, wait;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	seed = getenv("NX2048_SEED") ? (unsigned)atoi(getenv("NX2048_SEED")) : (unsigned)time(NULL);
	print = getenv("NX2048_PRINT") != NULL;
	animate = !((a = getenv("NX2048_ANIM")) && atoi(a) == 0);
	hold_ms = (a = getenv("NX2048_HOLD_MS")) ? atoi(a) : -1;
	best = reg_int("nx2048.best", 0);
	layout();
	fonts();
	new_game();
	window = GrNewWindowEx(UI_FIXED, "2048", GR_ROOT_WINDOW_ID, -1, -1, width, height, UI_FACE);
	board_pix = GrNewPixmap(BW, BW, NULL);
	GrSelectEvents(window, EVENTS);
	ui_pad_ask(window);
	GrMapWindow(window);
	GrSetFocus(window);
	if ((a = getenv("NX2048_AUTO")) && (auto_left = atoi(a)) > 0)
		auto_play(auto_left);
	for (;;) {
		ui_pad_poll(&pad);
		if (pad.down & INPUT_JUMP)
			key_down(MWKEY_ENTER, 0);
		if (pad.down & INPUT_RIGHT_UPPER)
			key_down('u', 0);
		if (anim_start) {	/* a move being shown: its next frame, and at its end the board as it is */
			long long ms = now_ms() - anim_start;

			if (hold_ms >= 0 && ms >= hold_ms)
				ms = hold_ms;
			else if (ms >= SLIDE_MS + POP_MS)
				anim_start = 0;
			draw_board(ms);
			if (!anim_start && (state == OVER || state == WON))
				draw_overlay();
		}
		wait = ui_pad_wait(&pad, anim_start ? 15 : -1);
		GrGetNextEventTimeout(&event, wait < 0 ? 0 : wait);
		if (event.type == GR_EVENT_TYPE_TIMEOUT)
			continue;
		if (ui_restyle()) {	/* (Settings changed the look) */
			ui_menu_restyle();
			layout();
			fonts();
			GrDestroyWindow(board_pix);
			board_pix = GrNewPixmap(BW, BW, NULL);
			GrSetWindowBackgroundColor(window, UI_FACE);
			draw_all();
		}
		ui_origin_note(&event);
		switch (ui_menu_event(&event, &id)) {
		case UI_MENU_CHOSEN:
			GrSetFocus(window);
			command(id);
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
			key_down(event.keystroke.ch, event.keystroke.modifiers);
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN: {
			int arrow = ui_pad_arrow(&event), wheel = ui_wheel(&event);

			if (event.button.wid != window)
				break;
			if (arrow) {
				key_down(arrow, 0);
				break;
			}
			if (wheel) {
				moved_by(wheel < 0 ? UP : DOWN);
				break;
			}
			if (event.button.y < MENU_H) {
				int i;

				for (i = 0; i < 2; i++)
					if (event.button.x >= menu_x[i] && event.button.x < menu_x[i + 1])
						open_menu(i, 0);
				break;
			}
			press_x = event.button.x, press_y = event.button.y, pressed = 1;
			break;
		}
		case GR_EVENT_TYPE_BUTTON_UP:
			if (pressed && event.button.wid == window) {
				int dx = event.button.x - press_x, dy = event.button.y - press_y;

				pressed = 0;
				if (abs(dx) < T / 3 && abs(dy) < T / 3) {	/* a click, not a drag: the overlay's way on */
					if (state != PLAYING)
						key_down(MWKEY_ENTER, 0);
				} else
					moved_by(abs(dx) > abs(dy) ? (dx < 0 ? LEFT : RIGHT) : (dy < 0 ? UP : DOWN));
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
	}
}
