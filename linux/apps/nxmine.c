/*
 * nxmine: Minesweeper, as Windows has it. The Game menu has the three levels and a board of
 * one's own; the first square opened is never a mine and opens an area round it. The left
 * button opens a square, the right one puts a flag and then a question mark on it, and both
 * (or the middle one) on a number open the squares round it once its flags are all there.
 * The face starts a new game. The fastest time of each level is kept in the registry with
 * its player's name (nxmine.time.LEVEL, nxmine.name.LEVEL).
 */
#include "ui.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include <stdint.h>
#include <time.h>

#define MENU_H 20
#define TILE 16
#define MOST_W 30
#define MOST_H 24
#define BX 12			/* the board's top left corner in the window */
#define BY (MENU_H + 55)
#define STATUS_Y (MENU_H + 9)	/* the strip with the counters and the face */
#define STATUS_H 37
#define FACE 26
#define LCD_W 41
#define LCD_H 25
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | \
	GR_EVENT_MASK_CLOSE_REQ)

/* a square's bits */
#define C_MINE 1
#define C_OPEN 2
#define C_FLAG 4
#define C_QUEST 8
#define C_DIRTY 16		/* to be drawn again */
#define C_PRESSED 32		/* held down under the pointer */

/* the pictures of the sheet (a pixmap made once a look): squares in its first row */
enum { T_COVER, T_OPEN0, T_FLAG = T_OPEN0 + 9, T_QUEST, T_QUEST_DOWN, T_MINE, T_BOOM, T_WRONG, T_COUNT };
#define SHEET_W (MOST_W * TILE)
#define SHEET_RUN_Y 16		/* a row of 30 covered squares, copied as one */
#define SHEET_DIGIT_Y 32	/* the counters' digits, 13 x 23: 0 to 9 and the minus */
#define SHEET_FACE_Y 55		/* the faces, 26 x 26 */
#define SHEET_H (SHEET_FACE_Y + FACE)
enum { F_SMILE, F_PRESSED, F_OH, F_COOL, F_DEAD, F_COUNT };

enum { READY, PLAYING, WON, LOST };
enum { H_NONE, H_ONE, H_CHORD, H_FACE, H_SPENT };	/* what a held button is doing */
enum { BEGINNER, INTERMEDIATE, EXPERT, CUSTOM };

static const struct { int rows, cols, mines; } levels[3] = { { 9, 9, 10 }, { 16, 16, 40 }, { 16, 30, 99 } };
static const char *const level_names[3] = { "beginner", "intermediate", "expert" };

static unsigned char cell[MOST_H][MOST_W], around[MOST_H][MOST_W];
static int rows, cols, mines, level, marks;
static int state, opened, flags, hit_r = -1, hit_c;
static int seconds;
static struct timespec started;
static int holding, hold_r = -1, hold_c, face_down;
static int kx, ky, kshown;	/* the keyboard's square, shown once an arrow is pressed */
static GR_WINDOW_ID window, sheet;
static int width, height;
static int printing;		/* NXMINE_PRINT: what happens, on standard output, for a test */
static uint32_t seed;

enum { C_NEW = 1, C_BEGINNER, C_INTERMEDIATE, C_EXPERT, C_CUSTOM, C_MARKS, C_BEST, C_EXIT, C_HELP, C_ABOUT };
static const char *const menu_names[] = { "Game", "Help" };
static int menu_x[3];
static struct ui_menu_item game_items[] = {
	{ "New   F2", -1, C_NEW }, { "Beginner", -1, C_BEGINNER, NULL, 0, UI_MENU_LINE }, { "Intermediate", -1, C_INTERMEDIATE },
	{ "Expert", -1, C_EXPERT }, { "Custom...", -1, C_CUSTOM }, { "Marks (?)", -1, C_MARKS, NULL, 0, UI_MENU_LINE },
	{ "Best times...", -1, C_BEST, NULL, 0, UI_MENU_LINE }, { "Exit", -1, C_EXIT, NULL, 0, UI_MENU_LINE },
};
static const struct ui_menu_item help_items[] = { { "How to play", -1, C_HELP }, { "About Minesweeper", -1, C_ABOUT } };
static const struct { const struct ui_menu_item *items; int count; } menus[2] = { { game_items, 8 }, { help_items, 2 } };

/* The numbers and the question mark, 8 x 10, bold as Windows draws them. */
static const char *const glyphs[9][10] = {
	{ "   ##   ", "  ###   ", " ####   ", "   ##   ", "   ##   ", "   ##   ", "   ##   ", "   ##   ", " ###### ", " ###### " },
	{ " ###### ", "###  ###", "     ###", "    ### ", "  ####  ", " ###    ", "###     ", "###     ", "########", "########" },
	{ "####### ", "########", "     ###", "     ###", "  ##### ", "  ##### ", "     ###", "     ###", "########", "####### " },
	{ "    ### ", "   #### ", "  ## ## ", " ##  ## ", "##   ## ", "########", "########", "     ## ", "     ## ", "     ## " },
	{ "########", "########", "###     ", "###     ", "####### ", "########", "     ###", "     ###", "########", "####### " },
	{ " ###### ", "########", "###     ", "###     ", "####### ", "########", "###  ###", "###  ###", "########", " ###### " },
	{ "########", "########", "     ###", "    ### ", "    ### ", "   ###  ", "   ###  ", "  ###   ", "  ###   ", "  ###   " },
	{ " ###### ", "########", "###  ###", "###  ###", " ###### ", " ###### ", "###  ###", "###  ###", "########", " ###### " },
	{ " ###### ", "###  ###", "###  ###", "    ### ", "   ###  ", "   ###  ", "        ", "   ###  ", "   ###  ", "        " },
};

static GR_COLOR
number_colour(int n)
{
	static const GR_COLOR classic[8] = { MWRGB(0, 0, 255), MWRGB(0, 128, 0), MWRGB(255, 0, 0), MWRGB(0, 0, 128),
		MWRGB(128, 0, 0), MWRGB(0, 128, 128), MWRGB(0, 0, 0), MWRGB(128, 128, 128) };
	static const GR_COLOR seven[8] = { MWRGB(64, 80, 190), MWRGB(30, 110, 10), MWRGB(178, 22, 22), MWRGB(16, 16, 130),
		MWRGB(128, 12, 12), MWRGB(10, 120, 130), MWRGB(40, 40, 40), MWRGB(120, 120, 120) };

	return ui_seven ? seven[n - 1] : classic[n - 1];
}

static void
glyph(GR_DRAW_ID d, int x, int y, int which, GR_COLOR colour)
{
	GR_BITMAP bits[10];
	int r, c;

	for (r = 0; r < 10; r++)
		for (bits[r] = 0, c = 0; c < 8; c++)
			if (glyphs[which][r][c] == '#')
				bits[r] |= 0x8000 >> c;
	GrSetGCForeground(ui_gc, colour);
	GrBitmap(d, ui_gc, x, y, 8, 10, bits);
}

static void
line(GR_DRAW_ID d, int x0, int y0, int x1, int y1, GR_COLOR colour)
{
	GrSetGCForeground(ui_gc, colour);
	GrLine(d, ui_gc, x0, y0, x1, y1);
}

/* A square's background, covered or open; in the Windows 7 look a square is 15 pixels and a
 * line of the board's between them. */
static void
square(GR_DRAW_ID d, int x, int y, int covered, GR_COLOR open)
{
	if (ui_seven) {
		GR_COLOR gap = MWRGB(196, 206, 226);

		ui_fill(d, x, y, TILE, TILE, gap);
		if (!covered) {
			ui_fill(d, x, y, TILE - 1, TILE - 1, open);
			return;
		}
		ui_fill(d, x, y, TILE - 1, 7, MWRGB(116, 160, 234));
		ui_fill(d, x, y + 7, TILE - 1, TILE - 8, MWRGB(78, 126, 216));
		line(d, x + 1, y, x + 13, y, MWRGB(176, 204, 246));
		line(d, x, y + 1, x, y + 13, MWRGB(146, 182, 240));
		line(d, x + 1, y + 14, x + 13, y + 14, MWRGB(50, 88, 176));
		line(d, x + 14, y + 1, x + 14, y + 13, MWRGB(56, 96, 186));
		return;
	}
	ui_fill(d, x, y, TILE, TILE, covered ? UI_FACE : open);
	if (!covered) {
		line(d, x, y, x + TILE - 1, y, UI_SHADOW);
		line(d, x, y, x, y + TILE - 1, UI_SHADOW);
		return;
	}
	ui_fill(d, x, y, 15, 1, WHITE), ui_fill(d, x, y + 1, 14, 1, WHITE);
	ui_fill(d, x, y, 1, 15, WHITE), ui_fill(d, x + 1, y, 1, 14, WHITE);
	ui_fill(d, x + 1, y + 15, 15, 1, UI_SHADOW), ui_fill(d, x + 2, y + 14, 14, 1, UI_SHADOW);
	ui_fill(d, x + 15, y + 1, 1, 15, UI_SHADOW), ui_fill(d, x + 14, y + 2, 1, 14, UI_SHADOW);
}

static void
mine_art(GR_DRAW_ID d, int x, int y)
{
	int cx = x + (ui_seven ? 7 : 8), cy = y + (ui_seven ? 7 : 8);
	GR_COLOR ink = ui_seven ? MWRGB(24, 28, 40) : BLACK;

	line(d, cx - 6, cy, cx + 6, cy, ink);
	line(d, cx, cy - 6, cx, cy + 6, ink);
	line(d, cx - 4, cy - 4, cx + 4, cy + 4, ink);
	line(d, cx - 4, cy + 4, cx + 4, cy - 4, ink);
	GrSetGCForeground(ui_gc, ink);
	GrFillEllipse(d, ui_gc, cx, cy, 4, 4);
	ui_fill(d, cx - 2, cy - 2, 2, 2, WHITE);
}

static void
flag_art(GR_DRAW_ID d, int x, int y)
{
	int cx = x + (ui_seven ? 8 : 8), k;
	static const int reach[5] = { 2, 4, 5, 4, 2 };
	GR_COLOR ink = ui_seven ? MWRGB(30, 30, 40) : BLACK;

	for (k = 0; k < 5; k++)
		ui_fill(d, cx - reach[k], y + 3 + k, reach[k], 1, ui_seven ? MWRGB(236, 40, 30) : MWRGB(255, 0, 0));
	ui_fill(d, cx, y + 3, 1, 8, ink);
	ui_fill(d, cx - 2, y + 10, 5, 1, ink);
	ui_fill(d, cx - 4, y + 11, 9, 2, ink);
}

static void
tile_art(GR_DRAW_ID d, int t, int x, int y)
{
	GR_COLOR open = ui_seven ? MWRGB(238, 242, 250) : UI_FACE;
	int gx = x + (ui_seven ? 4 : 4), gy = y + (ui_seven ? 3 : 3);

	switch (t) {
	case T_COVER:
		square(d, x, y, 1, open);
		break;
	case T_FLAG:
		square(d, x, y, 1, open);
		flag_art(d, x, y);
		break;
	case T_QUEST:
		square(d, x, y, 1, open);
		glyph(d, gx, gy, 8, ui_seven ? WHITE : BLACK);
		break;
	case T_QUEST_DOWN:
		square(d, x, y, 0, open);
		glyph(d, gx, gy, 8, BLACK);
		break;
	case T_MINE:
		square(d, x, y, 0, open);
		mine_art(d, x, y);
		break;
	case T_BOOM:
		square(d, x, y, 0, ui_seven ? MWRGB(240, 108, 96) : MWRGB(255, 0, 0));
		mine_art(d, x, y);
		break;
	case T_WRONG:
		square(d, x, y, 0, open);
		mine_art(d, x, y);
		line(d, x + 2, y + 2, x + 12, y + 12, MWRGB(220, 0, 0));
		line(d, x + 3, y + 2, x + 13, y + 12, MWRGB(220, 0, 0));
		line(d, x + 2, y + 12, x + 12, y + 2, MWRGB(220, 0, 0));
		line(d, x + 3, y + 12, x + 13, y + 2, MWRGB(220, 0, 0));
		break;
	default:
		square(d, x, y, 0, open);
		if (t > T_OPEN0)
			glyph(d, gx, gy, t - T_OPEN0 - 1, number_colour(t - T_OPEN0));
	}
}

/* A digit of the counters, segments lit or dark: a b c d e f g as bits 0 to 6. */
static void
digit_art(GR_DRAW_ID d, int x, int y, int which)
{
	static const unsigned char segments[11] = { 0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f, 0x40 };
	GR_COLOR on = MWRGB(255, 0, 0), off = MWRGB(80, 0, 0);
	int lit = segments[which], t;

	ui_fill(d, x, y, 13, 23, BLACK);
	for (t = 0; t < 3; t++) {
		line(d, x + 2 + t, y + 1 + t, x + 10 - t, y + 1 + t, lit & 1 ? on : off);
		line(d, x + 11 - t, y + 2 + t, x + 11 - t, y + 10 - t, lit & 2 ? on : off);
		line(d, x + 11 - t, y + 12 + t, x + 11 - t, y + 20 - t, lit & 4 ? on : off);
		line(d, x + 2 + t, y + 21 - t, x + 10 - t, y + 21 - t, lit & 8 ? on : off);
		line(d, x + 1 + t, y + 12 + t, x + 1 + t, y + 20 - t, lit & 16 ? on : off);
		line(d, x + 1 + t, y + 2 + t, x + 1 + t, y + 10 - t, lit & 32 ? on : off);
	}
	line(d, x + 3, y + 11, x + 9, y + 11, lit & 64 ? on : off);
	line(d, x + 4, y + 10, x + 8, y + 10, lit & 64 ? on : off);
	line(d, x + 4, y + 12, x + 8, y + 12, lit & 64 ? on : off);
}

/* The face button: smiling, pressed, surprised (a button held on the board), cool, dead. */
static void
face_art(GR_DRAW_ID d, int x, int y, int f)
{
	int down = f == F_PRESSED, cx = x + 13 + down, cy = y + 13 + down, k;
	GR_COLOR ink = ui_seven ? MWRGB(70, 50, 0) : BLACK;

	if (ui_seven) {
		ui_fill(d, x, y, FACE, FACE, UI_FACE);
		ui_button_seven(d, x, y, FACE, FACE, down);
	} else {
		ui_fill(d, x, y, FACE, FACE, UI_FACE);
		GrSetGCForeground(ui_gc, UI_SHADOW);
		GrRect(d, ui_gc, x, y, FACE, FACE);
		if (down)
			ui_edges(d, x + 1, y + 1, FACE - 2, FACE - 2, UI_SHADOW, UI_FACE);
		else
			for (k = 1; k < 3; k++)
				ui_edges(d, x + k, y + k, FACE - 2 * k, FACE - 2 * k, WHITE, UI_SHADOW);
	}
	GrSetGCForeground(ui_gc, ui_seven ? MWRGB(255, 214, 40) : MWRGB(255, 255, 0));
	GrFillEllipse(d, ui_gc, cx, cy, 8, 8);
	GrSetGCForeground(ui_gc, ink);
	GrEllipse(d, ui_gc, cx, cy, 8, 8);
	switch (f) {
	case F_COOL:
		ui_fill(d, cx - 6, cy - 3, 13, 1, ink);
		ui_fill(d, cx - 5, cy - 3, 5, 3, ink), ui_fill(d, cx + 1, cy - 3, 5, 3, ink);
		line(d, cx - 7, cy - 4, cx - 6, cy - 3, ink), line(d, cx + 7, cy - 4, cx + 6, cy - 3, ink);
		break;
	case F_DEAD:
		for (k = -1; k <= 1; k += 2) {
			line(d, cx + 3 * k - 1, cy - 4, cx + 3 * k + 1, cy - 2, ink);
			line(d, cx + 3 * k - 1, cy - 2, cx + 3 * k + 1, cy - 4, ink);
		}
		break;
	default:
		ui_fill(d, cx - 4, cy - 3, 2, 2, ink), ui_fill(d, cx + 2, cy - 3, 2, 2, ink);
	}
	switch (f) {
	case F_OH:
		GrSetGCForeground(ui_gc, ink);
		GrEllipse(d, ui_gc, cx, cy + 3, 2, 2);
		break;
	case F_DEAD:
		line(d, cx - 4, cy + 5, cx - 3, cy + 4, ink);
		line(d, cx - 2, cy + 3, cx + 2, cy + 3, ink);
		line(d, cx + 3, cy + 4, cx + 4, cy + 5, ink);
		break;
	default:
		line(d, cx - 4, cy + 2, cx - 3, cy + 3, ink);
		line(d, cx - 2, cy + 4, cx + 2, cy + 4, ink);
		line(d, cx + 3, cy + 3, cx + 4, cy + 2, ink);
	}
}

/* Everything the board is drawn from, in a pixmap the window copies out of: a square is one copy. */
static void
make_sheet(void)
{
	int k;

	if (sheet)
		GrDestroyWindow(sheet);
	sheet = GrNewPixmap(SHEET_W, SHEET_H, NULL);
	ui_fill(sheet, 0, 0, SHEET_W, SHEET_H, UI_FACE);
	for (k = 0; k < T_COUNT; k++)
		tile_art(sheet, k, k * TILE, 0);
	for (k = 0; k < MOST_W; k++)
		tile_art(sheet, T_COVER, k * TILE, SHEET_RUN_Y);
	for (k = 0; k < 11; k++)
		digit_art(sheet, k * 13, SHEET_DIGIT_Y, k);
	for (k = 0; k < F_COUNT; k++)
		face_art(sheet, k * FACE, SHEET_FACE_Y, k);
}

static void
say(const char *what)
{
	static const char *const states[] = { "ready", "playing", "won", "lost" };

	if (!printing)
		return;
	printf("nxmine: %s: %dx%d %d mines, %d opened, %d flags, %d s, %s\n", what, cols, rows, mines, opened, flags, seconds, states[state]);
	fflush(stdout);
}

static int
tile_of(int r, int c)
{
	int v = cell[r][c];

	if (v & C_OPEN)
		return (v & C_MINE) ? (r == hit_r && c == hit_c ? T_BOOM : T_MINE) : T_OPEN0 + around[r][c];
	if (state == LOST && (v & C_MINE) && !(v & C_FLAG))
		return T_MINE;
	if (state == LOST && (v & C_FLAG) && !(v & C_MINE))
		return T_WRONG;
	if (v & C_FLAG)
		return T_FLAG;
	if (v & C_PRESSED)
		return (v & C_QUEST) ? T_QUEST_DOWN : T_OPEN0;
	return (v & C_QUEST) ? T_QUEST : T_COVER;
}

static void
draw_focus(void)
{
	GrSetGCForeground(ui_gc, ui_seven ? MWRGB(255, 200, 0) : BLACK);
	GrRect(window, ui_gc, BX + kx * TILE + 1, BY + ky * TILE + 1, TILE - 3, TILE - 3);
	if (ui_seven)
		GrRect(window, ui_gc, BX + kx * TILE + 2, BY + ky * TILE + 2, TILE - 5, TILE - 5);
}

/* The squares marked to be drawn (all of them with `all`); a run of covered ones is one copy. */
static void
draw_cells(int all)
{
	int r, c, n, t, focus = 0;

	for (r = 0; r < rows; r++)
		for (c = 0; c < cols; c += n) {
			n = 1;
			if (!all && !(cell[r][c] & C_DIRTY))
				continue;
			t = tile_of(r, c);
			if (kshown && r == ky && c == kx)
				focus = 1;
			if (t == T_COVER)
				while (c + n < cols && (all || (cell[r][c + n] & C_DIRTY)) && tile_of(r, c + n) == T_COVER &&
				       !(kshown && r == ky && c + n == kx))
					n++;
			if (t == T_COVER)
				GrCopyArea(window, ui_gc, BX + c * TILE, BY + r * TILE, n * TILE, TILE, sheet, 0, SHEET_RUN_Y, MWROP_COPY);
			else
				GrCopyArea(window, ui_gc, BX + c * TILE, BY + r * TILE, TILE, TILE, sheet, t * TILE, 0, MWROP_COPY);
		}
	for (r = 0; r < rows; r++)
		for (c = 0; c < cols; c++)
			cell[r][c] &= ~C_DIRTY;
	if (focus)
		draw_focus();
}

static void
lcd(int x, int y, int value)
{
	int k, d[3];

	value = value > 999 ? 999 : value < -99 ? -99 : value;
	if (value < 0)
		d[0] = 10, d[1] = -value / 10, d[2] = -value % 10;
	else
		d[0] = value / 100, d[1] = value / 10 % 10, d[2] = value % 10;
	if (ui_seven)
		ui_edges(window, x, y, LCD_W, LCD_H, MWRGB(90, 90, 90), MWRGB(90, 90, 90));
	else
		ui_edges(window, x, y, LCD_W, LCD_H, UI_SHADOW, WHITE);
	for (k = 0; k < 3; k++)
		GrCopyArea(window, ui_gc, x + 1 + k * 13, y + 1, 13, 23, sheet, d[k] * 13, SHEET_DIGIT_Y, MWROP_COPY);
}

static void
draw_counter(void)
{
	lcd(16, STATUS_Y + 6, mines - flags);
}

static void
draw_timer(void)
{
	lcd(width - 16 - LCD_W, STATUS_Y + 6, seconds);
}

static int
face_now(void)
{
	if (holding == H_FACE)
		return face_down ? F_PRESSED : F_SMILE;
	if (state == WON)
		return F_COOL;
	if (state == LOST)
		return F_DEAD;
	return (holding == H_ONE || holding == H_CHORD) && hold_r >= 0 ? F_OH : F_SMILE;
}

static void
draw_face(void)
{
	GrCopyArea(window, ui_gc, (width - FACE) / 2, STATUS_Y + 5, FACE, FACE, sheet, face_now() * FACE, SHEET_FACE_Y, MWROP_COPY);
}

static void
draw_all(void)
{
	int i, bw = cols * TILE, bh = rows * TILE;

	ui_fill(window, 0, 0, width, MENU_H, UI_FACE);
	for (i = 0; i < 2; i++)
		ui_text(window, menu_x[i] + 8, 3, menu_names[i], -1, BLACK, 0);
	ui_fill(window, 0, MENU_H - 1, width, 1, UI_SHADOW);
	ui_fill(window, 0, MENU_H, width, height - MENU_H, UI_FACE);
	if (ui_seven) {
		ui_edges(window, 9, STATUS_Y, width - 18, STATUS_H, WHITE, MWRGB(214, 216, 222));
		ui_edges(window, BX - 1, BY - 1, bw + 1, bh + 1, MWRGB(132, 146, 172), MWRGB(196, 206, 226));
	} else {
		for (i = 0; i < 3; i++)
			ui_edges(window, i, MENU_H + i, width - 2 * i, height - MENU_H - 2 * i, WHITE, UI_SHADOW);
		for (i = 0; i < 2; i++)
			ui_edges(window, 9 + i, STATUS_Y + i, width - 18 - 2 * i, STATUS_H - 2 * i, UI_SHADOW, WHITE);
		for (i = 0; i < 3; i++)
			ui_edges(window, BX - 3 + i, BY - 3 + i, bw + 6 - 2 * i, bh + 6 - 2 * i, UI_SHADOW, WHITE);
	}
	draw_counter();
	draw_timer();
	draw_face();
	draw_cells(1);
}

static void
mark(int r, int c)
{
	if (r >= 0 && r < rows && c >= 0 && c < cols)
		cell[r][c] |= C_DIRTY;
}

static uint32_t
next_random(void)
{
	seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
	return seed;
}

static int
near_cell(int r, int c, int r0, int c0)
{
	return r >= r0 - 1 && r <= r0 + 1 && c >= c0 - 1 && c <= c0 + 1;
}

/* The mines, once the first square is chosen: never on it, nor round it while there is room. */
static void
lay_mines(int r0, int c0)
{
	static short free_[MOST_H * MOST_W];
	int r, c, n = 0, zone = 0, k, keep;

	for (r = r0 - 1; r <= r0 + 1; r++)
		for (c = c0 - 1; c <= c0 + 1; c++)
			zone += r >= 0 && r < rows && c >= 0 && c < cols;
	keep = rows * cols - zone >= mines;
	for (r = 0; r < rows; r++)
		for (c = 0; c < cols; c++)
			if (keep ? !near_cell(r, c, r0, c0) : !(r == r0 && c == c0))
				free_[n++] = (short)(r * MOST_W + c);
	for (k = 0; k < mines && k < n; k++) {
		int pick = k + (int)(next_random() % (uint32_t)(n - k));
		short t = free_[pick];

		free_[pick] = free_[k], free_[k] = t;
		cell[t / MOST_W][t % MOST_W] |= C_MINE;
	}
	for (r = 0; r < rows; r++)
		for (c = 0; c < cols; c++) {
			int dr, dc, sum = 0;

			for (dr = -1; dr <= 1; dr++)
				for (dc = -1; dc <= 1; dc++)
					if (r + dr >= 0 && r + dr < rows && c + dc >= 0 && c + dc < cols)
						sum += cell[r + dr][c + dc] & C_MINE;
			around[r][c] = (unsigned char)sum;
		}
	if (printing > 1)
		for (r = 0; r < rows; r++) {
			printf("nxmine: row %2d ", r);
			for (c = 0; c < cols; c++)
				putchar(cell[r][c] & C_MINE ? '*' : '0' + around[r][c]);
			putchar('\n');
		}
}

static int
elapsed_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int)((now.tv_sec - started.tv_sec) * 1000 + (now.tv_nsec - started.tv_nsec) / 1000000);
}

static void
new_game(void)
{
	memset(cell, 0, sizeof cell);
	memset(around, 0, sizeof around);
	state = READY, opened = flags = seconds = 0, hit_r = -1;
	holding = H_NONE, hold_r = -1;
	if (kx >= cols)
		kx = cols - 1;
	if (ky >= rows)
		ky = rows - 1;
	draw_all();
	say("new game");
}

static void
lose(int r, int c)
{
	cell[r][c] |= C_OPEN;
	hit_r = r, hit_c = c;
	state = LOST;
	for (r = 0; r < rows; r++)
		for (c = 0; c < cols; c++)
			if (cell[r][c] & (C_MINE | C_FLAG))
				mark(r, c);
}

static void best_time(void);

static void
win(void)
{
	int r, c;

	state = WON;
	for (r = 0; r < rows; r++)
		for (c = 0; c < cols; c++)
			if ((cell[r][c] & C_MINE) && !(cell[r][c] & C_FLAG)) {
				cell[r][c] = (cell[r][c] & ~C_QUEST) | C_FLAG;
				mark(r, c);
			}
	flags = mines;
}

/* Opens a square and, round a square with no mine near, every square it reaches. */
static void
reveal(int r0, int c0)
{
	static short stack[MOST_H * MOST_W];
	int top = 0, r, c, dr, dc;

	if (cell[r0][c0] & C_FLAG)
		return;		/* (before the mines are laid: the first square opened is never one) */
	if (state == READY) {
		lay_mines(r0, c0);
		state = PLAYING;
		clock_gettime(CLOCK_MONOTONIC, &started);
		seconds = 1;
		draw_timer();
	}
	if (state != PLAYING || (cell[r0][c0] & (C_OPEN | C_FLAG)))
		return;
	if (cell[r0][c0] & C_MINE) {
		lose(r0, c0);
		return;
	}
	cell[r0][c0] |= C_OPEN;
	stack[top++] = (short)(r0 * MOST_W + c0);
	while (top > 0) {
		r = stack[--top] / MOST_W, c = stack[top] % MOST_W;
		cell[r][c] = (cell[r][c] & ~C_QUEST) | C_DIRTY;
		opened++;
		if (around[r][c])
			continue;
		for (dr = -1; dr <= 1; dr++)
			for (dc = -1; dc <= 1; dc++) {
				int nr = r + dr, nc = c + dc;

				if (nr < 0 || nr >= rows || nc < 0 || nc >= cols || (cell[nr][nc] & (C_OPEN | C_FLAG | C_MINE)))
					continue;
				cell[nr][nc] |= C_OPEN;
				stack[top++] = (short)(nr * MOST_W + nc);
			}
	}
	if (opened == rows * cols - mines)
		win();
}

/* A number with as many flags round it as it says opens the other squares round it. */
static void
chord(int r, int c)
{
	int dr, dc, sum = 0;

	if (state != PLAYING || !(cell[r][c] & C_OPEN) || !around[r][c])
		return;
	for (dr = -1; dr <= 1; dr++)
		for (dc = -1; dc <= 1; dc++)
			if (r + dr >= 0 && r + dr < rows && c + dc >= 0 && c + dc < cols)
				sum += (cell[r + dr][c + dc] & C_FLAG) != 0;
	if (sum != around[r][c])
		return;
	for (dr = -1; dr <= 1; dr++)
		for (dc = -1; dc <= 1; dc++)
			if (r + dr >= 0 && r + dr < rows && c + dc >= 0 && c + dc < cols && state == PLAYING)
				reveal(r + dr, c + dc);
}

/* Covered, a flag, a question mark (when marks are on), covered again. */
static void
toggle_mark(int r, int c)
{
	int v = cell[r][c];

	if (state > PLAYING || (v & C_OPEN))
		return;
	if (v & C_FLAG)
		v = (v & ~C_FLAG) | (marks ? C_QUEST : 0), flags--;
	else if (v & C_QUEST)
		v &= ~C_QUEST;
	else
		v |= C_FLAG, flags++;
	cell[r][c] = (unsigned char)(v | C_DIRTY);
	draw_cells(0);
	draw_counter();
}

/* After a square was opened: the board, and what the game's end changes. */
static void
after_move(const char *what)
{
	draw_cells(0);
	draw_counter();
	draw_face();
	say(what);
	if (state == WON) {
		draw_timer();
		best_time();
	}
}

/* The squares held down: the one under the pointer, or the nine round it for a chord. */
static void
press(int r, int c, int nine)
{
	int i, j;

	for (i = 0; i < rows; i++)
		for (j = 0; j < cols; j++)
			if (cell[i][j] & C_PRESSED)
				cell[i][j] = (cell[i][j] & ~C_PRESSED) | C_DIRTY;
	hold_r = r, hold_c = c;
	for (i = r - 1; r >= 0 && i <= r + 1; i++)
		for (j = c - 1; j <= c + 1; j++)
			if ((nine || (i == r && j == c)) && i >= 0 && i < rows && j >= 0 && j < cols)
				cell[i][j] |= C_PRESSED | C_DIRTY;
	draw_cells(0);
	draw_face();
}

static int
board_cell(int x, int y, int *r, int *c)
{
	if (x < BX || y < BY || x >= BX + cols * TILE || y >= BY + rows * TILE)
		return 0;
	*r = (y - BY) / TILE, *c = (x - BX) / TILE;
	return 1;
}

static int
on_face(int x, int y)
{
	return ui_inside(x, y, (width - FACE) / 2, STATUS_Y + 5, FACE, FACE);
}

static void
other_event(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw_all();
	if (event->type == GR_EVENT_TYPE_CLOSE_REQ && event->general.wid == window) {
		GrClose();
		exit(0);
	}
}

/*
 * A small form: a text, then fields to type in, then OK and Cancel. True for OK. A field's
 * text is offered picked out: the first key typed replaces it. `digits` takes digits only.
 */
struct field { const char *label; char text[24]; int digits; };

#define FORM_W 300
#define FORM_FX 130
#define FORM_FW 150
#define FORM_ROW 28

static int
form(const char *title, const char *text, struct field *f, int n)
{
	const GR_SCREEN_INFO *screen = ui_screen();
	const char *lines[3];
	int lengths[3], count = 0, focus = 0, offered = 1, answer = -1, i, fy, h, by, ch, x, y;
	GR_WINDOW_ID w;
	GR_EVENT event;
	const char *const answers[2] = { "OK", "Cancel" };
	int bx[2] = { FORM_W - 6 - 76 - 8 - 76, FORM_W - 6 - 76 };

	while (count < 3 && *text) {
		lengths[count] = (int)strcspn(text, "\n");
		lines[count++] = text;
		text += lengths[count - 1] + (text[lengths[count - 1]] == '\n');
	}
	fy = 14 + count * 16 + 8;
	by = fy + n * FORM_ROW + 6;
	h = by + UI_ASK_BUTTON_H + 10;
	w = GrNewWindowEx(UI_FIXED | GR_WM_PROPS_NOAUTOMOVE, (char *)title, GR_ROOT_WINDOW_ID, (screen->cols - FORM_W) / 2,
		(screen->rows - h) / 3, FORM_W, h, UI_FACE);
	GrSelectEvents(w, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(w);
	GrRaiseWindow(w);
	GrSetFocus(w);
	for (;;) {
		int redraw = 0;

		GrGetNextEvent(&event);
		if (event.general.wid != w) {
			other_event(&event);
			continue;
		}
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (printing) {
				ui_window_origin(w, &x, &y);
				printf("nxmine: form %s at %d,%d, OK at %d,%d\n", title, x, y, x + bx[0] + 38, y + by + 12);
				fflush(stdout);
			}
			redraw = 1;
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			answer = 1;
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			for (i = 0; i < n; i++)
				if (ui_inside(event.button.x, event.button.y, FORM_FX, fy + i * FORM_ROW, FORM_FW, 22))
					focus = i, offered = 0, redraw = 1;
			for (i = 0; i < 2; i++)
				if (ui_inside(event.button.x, event.button.y, bx[i], by, 76, UI_ASK_BUTTON_H))
					answer = i;
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			ch = event.keystroke.ch;
			redraw = 1;
			if (ch == MWKEY_ESCAPE)
				answer = 1;
			else if (ch == MWKEY_ENTER || ch == '\r' || ch == '\n')
				answer = focus == n + 1 ? 1 : 0;
			else if (ch == ' ' && focus >= n)
				answer = focus - n;
			else if (ui_tab(&event, &focus, n + 2) || ch == MWKEY_UP || ch == MWKEY_DOWN) {
				if (ch == MWKEY_UP || ch == MWKEY_DOWN)
					focus = (focus + (ch == MWKEY_UP ? n + 1 : 1)) % (n + 2);
				offered = 1;
			} else if (focus < n && ch == MWKEY_BACKSPACE) {
				size_t l = strlen(f[focus].text);

				if (offered)
					f[focus].text[0] = 0;
				else if (l)
					f[focus].text[l - 1] = 0;
				offered = 0;
			} else if (focus < n && ch >= ' ' && ch < 127 && (!f[focus].digits || isdigit(ch))) {
				size_t l;

				if (offered)
					f[focus].text[0] = 0;
				offered = 0;
				l = strlen(f[focus].text);
				if (l + 1 < sizeof f[focus].text && (!f[focus].digits || l < 3))
					f[focus].text[l] = (char)ch, f[focus].text[l + 1] = 0;
			} else
				redraw = 0;
			break;
		}
		if (answer >= 0)
			break;
		if (!redraw)
			continue;
		ui_fill(w, 0, 0, FORM_W, h, UI_FACE);
		if (ui_seven)
			ui_fill(w, 0, 0, FORM_W, by - 8, WHITE);
		for (i = 0; i < count; i++)
			ui_text(w, 14, 14 + i * 16, lines[i], lengths[i], BLACK, 0);
		for (i = 0; i < n; i++) {
			int y0 = fy + i * FORM_ROW, tw = ui_width(&ui_var_widths, ui_var, f[i].text, -1);

			ui_text(w, 14, y0 + 4, f[i].label, -1, BLACK, 0);
			ui_fill(w, FORM_FX, y0, FORM_FW, 22, WHITE);
			if (ui_seven)
				ui_edges(w, FORM_FX, y0, FORM_FW, 22, i == focus ? MWRGB(61, 123, 173) : MWRGB(171, 173, 179),
					i == focus ? MWRGB(61, 123, 173) : MWRGB(171, 173, 179));
			else {
				ui_edges(w, FORM_FX, y0, FORM_FW, 22, UI_SHADOW, WHITE);
				ui_edges(w, FORM_FX + 1, y0 + 1, FORM_FW - 2, 20, MWRGB(64, 64, 64), UI_FACE);
			}
			if (i == focus && offered && f[i].text[0]) {
				ui_fill(w, FORM_FX + 4, y0 + 4, tw + 1, 14, UI_SELECTED);
				ui_text(w, FORM_FX + 4, y0 + 5, f[i].text, -1, WHITE, 0);
			} else
				ui_text(w, FORM_FX + 4, y0 + 5, f[i].text, -1, BLACK, 0);
			if (i == focus && !offered)
				ui_fill(w, FORM_FX + 5 + tw, y0 + 4, 1, 14, BLACK);
		}
		for (i = 0; i < 2; i++) {
			int tw = ui_width(&ui_var_widths, ui_var, answers[i], -1);

			if (ui_seven)
				ui_button_seven(w, bx[i], by, 76, UI_ASK_BUTTON_H, 0);
			else
				ui_bevel(w, bx[i], by, 76, UI_ASK_BUTTON_H, 0);
			ui_text(w, bx[i] + (76 - tw) / 2, by + 6, answers[i], -1, BLACK, 0);
			if (focus == n + i)
				ui_focus(w, bx[i] + 5, by + 5, 66, UI_ASK_BUTTON_H - 10);
		}
	}
	GrDestroyWindow(w);
	GrSetFocus(window);
	return answer == 0;
}

static void
show_best(void)
{
	static const char *const answers[2] = { "OK", "Reset scores" };
	char text[300], key[40];
	int k, at = 0;

	for (;;) {
		for (k = 0, at = 0; k < 3; k++) {
			snprintf(key, sizeof key, "nxmine.time.%s", level_names[k]);
			at += snprintf(text + at, sizeof text - at, "%c%s:   %d seconds   ", toupper(level_names[k][0]), level_names[k] + 1,
				reg_int(key, 999));
			snprintf(key, sizeof key, "nxmine.name.%s", level_names[k]);
			at += snprintf(text + at, sizeof text - at, "%.40s\n", reg_get(key, "Anonymous"));
		}
		if (ui_ask("Fastest mine sweepers", text, answers, 2, other_event) != 1)
			break;
		for (k = 0; k < 3; k++) {
			snprintf(key, sizeof key, "nxmine.time.%s", level_names[k]);
			reg_set(key, NULL);
			snprintf(key, sizeof key, "nxmine.name.%s", level_names[k]);
			reg_set(key, NULL);
		}
	}
	GrSetFocus(window);
}

/* A win faster than the level's best asks the winner's name, and shows the best times. */
static void
best_time(void)
{
	struct field name = { "Name:", "", 0 };
	char key[40], text[120];

	if (level == CUSTOM)
		return;
	snprintf(key, sizeof key, "nxmine.time.%s", level_names[level]);
	if (seconds >= reg_int(key, 999) && reg_get(key, NULL))
		return;
	snprintf(name.text, sizeof name.text, "%s", reg_get("nxmine.player", getenv("USER") ? getenv("USER") : "Anonymous"));
	snprintf(text, sizeof text, "You have the fastest time\nfor the %s level.\nPlease type your name.", level_names[level]);
	if (!form("Congratulations", text, &name, 1))
		return;
	if (!name.text[0])
		snprintf(name.text, sizeof name.text, "Anonymous");
	reg_set_int(key, seconds);
	snprintf(key, sizeof key, "nxmine.name.%s", level_names[level]);
	reg_set(key, name.text);
	reg_set("nxmine.player", name.text);
	if (printing) {
		printf("nxmine: best %s %d s by %s\n", level_names[level], seconds, name.text);
		fflush(stdout);
	}
	show_best();
}

static void
size_window(void)
{
	int w = cols * TILE + 24, h = BY + rows * TILE + 12;

	if (w == width && h == height)
		return;
	width = w, height = h;
	if (window)
		GrResizeWindow(window, width, height);
}

/* A level, or a board of one's own; kept for the next game in the registry. */
static void
set_level(int l, int r, int c, int m)
{
	const GR_SCREEN_INFO *screen = ui_screen();
	int most_c = (screen->cols - 24 - 10) / TILE, most_r = (screen->rows - BY - 12 - 30 - UI_MENU_BAR) / TILE;

	most_c = most_c > MOST_W ? MOST_W : most_c < 9 ? 9 : most_c;
	most_r = most_r > MOST_H ? MOST_H : most_r < 9 ? 9 : most_r;
	if (l != CUSTOM)
		r = levels[l].rows, c = levels[l].cols, m = levels[l].mines;
	rows = r < 9 ? 9 : r > most_r ? most_r : r;
	cols = c < 9 ? 9 : c > most_c ? most_c : c;
	m = m < 10 ? 10 : m;
	mines = m > (rows - 1) * (cols - 1) ? (rows - 1) * (cols - 1) : m;
	level = l;
	if (kx >= cols)
		kx = cols - 1;
	if (ky >= rows)
		ky = rows - 1;
	size_window();
}

static void
keep_level(void)
{
	reg_set_int("nxmine.level", level);
	if (level == CUSTOM) {
		reg_set_int("nxmine.height", rows);
		reg_set_int("nxmine.width", cols);
		reg_set_int("nxmine.mines", mines);
	}
}

static void
custom(void)
{
	struct field f[3] = { { "Height:", "", 1 }, { "Width:", "", 1 }, { "Mines:", "", 1 } };

	snprintf(f[0].text, sizeof f[0].text, "%d", rows);
	snprintf(f[1].text, sizeof f[1].text, "%d", cols);
	snprintf(f[2].text, sizeof f[2].text, "%d", mines);
	if (!form("Custom field", "Rows from 9 to 24, columns from 9 to 30.", f, 3))
		return;
	set_level(CUSTOM, atoi(f[0].text), atoi(f[1].text), atoi(f[2].text));
	keep_level();
	new_game();
}

static void
command(int id)
{
	static const char *const ok[1] = { "OK" };

	switch (id) {
	case C_NEW:
		new_game();
		break;
	case C_BEGINNER: case C_INTERMEDIATE: case C_EXPERT:
		set_level(id - C_BEGINNER, 0, 0, 0);
		keep_level();
		new_game();
		break;
	case C_CUSTOM:
		custom();
		break;
	case C_MARKS:
		marks = !marks;
		reg_set_int("nxmine.marks", marks);
		break;
	case C_BEST:
		show_best();
		break;
	case C_HELP:
		ui_ask("How to play", "Open every square that has no mine under it. A number says how\n"
			"many of the eight squares round it have mines. The right button\n"
			"puts a flag on a square, then a question mark. Both buttons, or the\n"
			"middle one, on a number with all its flags open the squares round it.\n"
			"Keys: the arrows, Space opens, F puts a flag, F2 is a new game.", ok, 1, other_event);
		GrSetFocus(window);
		break;
	case C_ABOUT:
		ui_ask("About Minesweeper", "Minesweeper\nThe classic game, for the ShaderEmu desktop.", ok, 1, other_event);
		GrSetFocus(window);
		break;
	case C_EXIT:
		GrClose();
		exit(0);
	}
}

static void
open_menu(int which, int keyboard)
{
	int wx, wy, i;

	for (i = 0; i < 4; i++)
		game_items[1 + i].flags = (game_items[1 + i].flags & ~UI_MENU_CHECK) | (level == i ? UI_MENU_CHECK : 0);
	game_items[5].flags = (game_items[5].flags & ~UI_MENU_CHECK) | (marks ? UI_MENU_CHECK : 0);
	ui_window_origin(window, &wx, &wy);
	ui_menu_open(menus[which].items, menus[which].count, wx + menu_x[which], wy + MENU_H, 0, keyboard);
}

static void
move_focus(int dx, int dy)
{
	mark(ky, kx);
	kx = (kx + dx + cols) % cols, ky = (ky + dy + rows) % rows;
	kshown = 1;
	mark(ky, kx);
	draw_cells(0);
}

static void
key(int ch)
{
	switch (ch) {
	case MWKEY_F2:
		new_game();
		break;
	case MWKEY_F10:
		open_menu(0, 1);
		break;
	case MWKEY_LEFT: move_focus(-1, 0); break;
	case MWKEY_RIGHT: move_focus(1, 0); break;
	case MWKEY_UP: move_focus(0, -1); break;
	case MWKEY_DOWN: move_focus(0, 1); break;
	case ' ': case MWKEY_ENTER:
		if (!kshown) {
			move_focus(0, 0);
			break;
		}
		if (state > PLAYING)
			break;
		if (cell[ky][kx] & C_OPEN)
			chord(ky, kx);
		else
			reveal(ky, kx);
		mark(ky, kx);
		after_move("open");
		break;
	case 'f': case 'F':
		if (!kshown)
			move_focus(0, 0);
		toggle_mark(ky, kx);
		say("mark");
		break;
	case MWKEY_ESCAPE:
		if (kshown) {
			kshown = 0;
			mark(ky, kx);
			draw_cells(0);
		}
		break;
	}
}

static void
button_down(GR_EVENT_BUTTON *b)
{
	int r, c, i, both = (b->buttons & GR_BUTTON_L) && (b->buttons & GR_BUTTON_R);

	if (b->y < MENU_H) {
		for (i = 0; i < 2; i++)
			if (b->x >= menu_x[i] && b->x < menu_x[i + 1])
				open_menu(i, 0);
		return;
	}
	ui_moves(window, EVENTS, 1);
	if (on_face(b->x, b->y) && (b->changebuttons & GR_BUTTON_L) && holding == H_NONE) {
		holding = H_FACE, face_down = 1;
		draw_face();
		return;
	}
	if (state > PLAYING || holding == H_FACE || holding == H_SPENT)
		return;
	if (!board_cell(b->x, b->y, &r, &c))
		r = -1, c = 0;
	if ((b->buttons & GR_BUTTON_M) || both) {
		holding = H_CHORD;
		press(r, c, 1);
	} else if (b->changebuttons & GR_BUTTON_L) {
		holding = H_ONE;
		press(r, c, 0);
	} else if ((b->changebuttons & GR_BUTTON_R) && r >= 0) {
		toggle_mark(r, c);
		say("mark");
	}
}

static void
pointer_moved(GR_EVENT_MOUSE *m)
{
	int r, c;

	if (holding == H_FACE) {
		if (on_face(m->x, m->y) != face_down) {
			face_down = !face_down;
			draw_face();
		}
		return;
	}
	if (holding != H_ONE && holding != H_CHORD)
		return;
	if (!board_cell(m->x, m->y, &r, &c))
		r = -1, c = 0;
	if (r != hold_r || (r >= 0 && c != hold_c))
		press(r, c, holding == H_CHORD);
}

static void
button_up(GR_EVENT_BUTTON *b)
{
	int r = hold_r, c = hold_c, was = holding;

	if (!(b->buttons & (GR_BUTTON_L | GR_BUTTON_R | GR_BUTTON_M)))
		ui_moves(window, EVENTS, 0);
	if (was == H_FACE) {
		if (b->changebuttons & GR_BUTTON_L) {
			holding = H_NONE;
			if (on_face(b->x, b->y))
				new_game();
			else
				draw_face();
		}
		return;
	}
	if (was == H_SPENT) {
		if (!(b->buttons & (GR_BUTTON_L | GR_BUTTON_R | GR_BUTTON_M)))
			holding = H_NONE;
		return;
	}
	if (was == H_ONE && !(b->changebuttons & GR_BUTTON_L))
		return;
	if (was != H_ONE && was != H_CHORD)
		return;
	holding = (b->buttons & (GR_BUTTON_L | GR_BUTTON_R | GR_BUTTON_M)) ? H_SPENT : H_NONE;
	press(-1, 0, 0);
	if (r >= 0 && was == H_ONE)
		reveal(r, c);
	else if (r >= 0)
		chord(r, c);
	after_move(was == H_ONE ? "open" : "chord");
}

static void
measure_menus(void)
{
	int i, x;

	for (i = 0, x = 2; i < 2; i++) {
		menu_x[i] = x;
		x += ui_width(&ui_var_widths, ui_var, menu_names[i], -1) + 16;
	}
	menu_x[2] = x;
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	int id, l;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	printing = getenv("NXMINE_PRINT") ? atoi(getenv("NXMINE_PRINT")) : 0;
	seed = getenv("NXMINE_SEED") ? (uint32_t)strtoul(getenv("NXMINE_SEED"), NULL, 0) : (uint32_t)time(NULL) ^ (uint32_t)getpid() << 16;
	seed = seed ? seed : 1;
	marks = reg_int("nxmine.marks", 1);
	l = reg_int("nxmine.level", BEGINNER);
	l = l < BEGINNER || l > CUSTOM ? BEGINNER : l;
	if (argc > 1)	/* (a level for a test or a script: beginner, intermediate, expert) */
		for (id = 0; id < 3; id++)
			if (!strcmp(argv[1], level_names[id]))
				l = id;
	set_level(l, reg_int("nxmine.height", 16), reg_int("nxmine.width", 30), reg_int("nxmine.mines", 99));
	measure_menus();
	make_sheet();
	window = GrNewWindowEx(UI_FIXED, "Minesweeper", GR_ROOT_WINDOW_ID, -1, -1, width, height, UI_FACE);
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	GrSetFocus(window);
	if (printing) {
		printf("nxmine: %d x %d, the board at %d,%d in the window, squares of %d\n", width, height, BX, BY, TILE);
		fflush(stdout);
	}
	for (;;) {
		int wait = 1000 - elapsed_ms() % 1000;

		if (state == PLAYING && seconds < 999)	/* (the clock ticks only while it counts) */
			GrGetNextEventTimeout(&event, (GR_TIMEOUT)(wait < 1 ? 1 : wait));
		else
			GrGetNextEvent(&event);
		ui_origin_note(&event);
		if (state == PLAYING) {
			int now = 1 + elapsed_ms() / 1000;

			now = now > 999 ? 999 : now;
			if (now != seconds) {
				seconds = now;
				draw_timer();
			}
		}
		if (event.type != GR_EVENT_TYPE_TIMEOUT) {
			if (ui_restyle()) {	/* (Settings changed the look) */
				ui_menu_restyle();
				measure_menus();
				make_sheet();
				draw_all();
			}
		}
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
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.wid == window && !ui_wheel(&event))
				button_down(&event.button);
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (event.mouse.wid == window)
				pointer_moved(&event.mouse);
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			if (event.button.wid == window)
				button_up(&event.button);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			if (event.keystroke.wid == window)
				key(event.keystroke.ch);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			if (event.general.wid == window)
				command(C_EXIT);
			break;
		}
	}
}
