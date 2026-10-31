/*
 * nxterm: the desktop's terminal, an xterm-like screen over a pseudo-terminal that runs the
 * user's shell. nxterm [COMMAND] runs a shell's command line instead (an executable file as
 * it is), nxterm -e PROGRAM [ARGUMENT ...] a program; the window closes when it ends.
 */
#define _GNU_SOURCE
#include "ui.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <sys/ioctl.h>
#include <termios.h>

#define COLS_MOST 256
#define ROWS_MOST 128
#define BACK_MOST 1000		/* lines kept to look back through */
#define PAD 2			/* pixels round the cells */
#define BLINK_MS 530
#define BLINK_FOR 10000		/* the cursor blinks this long after the last key or output, then stands */
#define TITLE "Terminal"
/* (the pointer's moves are asked for while a button is held: ui_moves) */
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | \
	GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_FOCUS_IN | GR_EVENT_MASK_FOCUS_OUT | GR_EVENT_MASK_CLOSE_REQ)

/* A cell: a character (16 bits), colours of the 256, A_ bits; F_FG and F_BG: the scheme's own colour. */
enum { A_BOLD = 1, A_DIM = 2, A_ITALIC = 4, A_UNDER = 8, A_REVERSE = 16, A_HIDDEN = 32, A_STRIKE = 64 };
enum { F_FG = 1, F_BG = 2 };
struct cell { uint16_t ch; uint8_t fg, bg, at, flags; };
struct line { struct cell *c; int wrapped; };		/* wrapped: the text goes on in the next row */
struct kept { uint16_t n, wrapped; struct cell c[]; };	/* a line looked back to, without its blanks at the end */
/* A cell as it is on the window: its colours worked out, and D_ bits (the cursor's among them). */
enum { D_BOLD = 1, D_ITALIC = 2, D_UNDER = 4, D_STRIKE = 8, D_HOLLOW = 16, D_LOW = 32, D_BAR = 64, D_BLOCK = 128 };
struct drawn { GR_COLOR fg, bg; uint16_t ch; uint8_t at; };

static GR_WINDOW_ID win;
static GR_GC_ID tgc;
static int win_w, win_h, focused, exposed;
static int master = -1, wake[2] = { -1, -1 };
static pid_t shell;

/* the two screens (the alternate one is a full-screen program's), and the lines gone off the top */
static struct line screens[2][ROWS_MOST], *scr = screens[0];
static int alt, cols = 80, rows = 24, cap, made;	/* cap: cells allocated a line; made: lines allocated */
static struct kept *back[BACK_MOST];
static int back_first, back_count, view;		/* view: lines looked back; 0 is the live screen */
static long back_total;					/* lines ever kept: a line's number is counted from it */

static int cx, cy, wrapnext, top, bottom;		/* wrapnext: a character in the last column, the next one wraps */
static struct cell pen;
static struct { int cx, cy, wrapnext, origin, gl; struct cell pen; unsigned char g[2]; } saved[2];
static int origin, autowrap, insert, newline_mode, cursor_keys, cursor_shown, screen_reverse, bracketed;
static int mouse_mode, mouse_sgr, cursor_style, cursor_blinks;	/* style: 0 block, 1 underline, 2 bar */
static unsigned char tabs[COLS_MOST], charset[3];
static int gl, charset_slot;
static unsigned last_ch;

enum { S_GROUND, S_ESC, S_CSI, S_OSC, S_STRING, S_CHARSET, S_HASH, S_SKIP };
static int state, esc_from;
static int par[16], sub[16], npar, priv, inter;
static char osc[4096];
static int osc_n;
static unsigned utf;
static int utf_need;
static char reply_buf[256];	/* answers to the program's questions, sent once its output is read */
static int reply_n;

static struct drawn *shown[ROWS_MOST];
static unsigned char dirty[ROWS_MOST];	/* rows of the window to compare with what they show */
static int pend_t, pend_b, pend_n;	/* rows moved up (down: negative) and not yet copied on the window */
static int cur_row = -1, blink_on = 1, bar_total = -1, bar_top = -1;
static long last_active;

static const int sizes[] = { 0, 10, 11, 13, 15, 17, 20, 24 };	/* 0: the window system's fixed font */
static int font_size, rom, cw, chh, ascent, font_last;		/* rom: the ROM's fonts, 16-bit text */
static GR_FONT_ID fonts[4];
static int light;
static GR_COLOR palette[256], scheme_fg, scheme_bg;

static int sel_on, sel_held, sel_kind;	/* kind: 0 characters, 1 words, 2 lines */
static long sa_line, sb_line, s0_line, s1_line, click_line;
static int sa_col, sb_col, s0_col, s1_col, click_col, clicks;
static long click_ms;
static int bar_held, mouse_held;

static void flush(void);
static void finish(void);

static int
clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static long
ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

static void
activity(void)
{
	last_active = ms();
	blink_on = 1;
}

/* ---------------- colours */

static void
set_scheme(void)
{
	static const GR_COLOR base[16] = {
		MWRGB(12, 12, 12), MWRGB(197, 15, 31), MWRGB(19, 161, 14), MWRGB(193, 156, 0),
		MWRGB(0, 55, 218), MWRGB(136, 23, 152), MWRGB(58, 150, 221), MWRGB(204, 204, 204),
		MWRGB(118, 118, 118), MWRGB(231, 72, 86), MWRGB(22, 198, 12), MWRGB(249, 241, 165),
		MWRGB(59, 120, 255), MWRGB(180, 0, 158), MWRGB(97, 214, 214), MWRGB(242, 242, 242),
	};
	static const unsigned char level[6] = { 0, 95, 135, 175, 215, 255 };
	int i;

	for (i = 0; i < 256; i++)
		palette[i] = i < 16 ? base[i] : i < 232 ? MWRGB(level[(i - 16) / 36], level[(i - 16) / 6 % 6], level[(i - 16) % 6]) :
			MWRGB(8 + (i - 232) * 10, 8 + (i - 232) * 10, 8 + (i - 232) * 10);
	scheme_fg = light ? MWRGB(30, 30, 30) : MWRGB(204, 204, 204);
	scheme_bg = light ? MWRGB(255, 255, 255) : MWRGB(12, 12, 12);
}

static GR_COLOR
default_fg(void)
{
	return screen_reverse ? scheme_bg : scheme_fg;
}

static GR_COLOR
default_bg(void)
{
	return screen_reverse ? scheme_fg : scheme_bg;
}

/* `part` quarters of the way from a to b */
static GR_COLOR
mix(GR_COLOR a, GR_COLOR b, int part)
{
	int r = ((a & 255) * (4 - part) + (b & 255) * part) / 4;
	int g = ((a >> 8 & 255) * (4 - part) + (b >> 8 & 255) * part) / 4;
	int l = ((a >> 16 & 255) * (4 - part) + (b >> 16 & 255) * part) / 4;

	return MWRGB(r, g, l);
}

/* The nearest of the 256 to a colour given whole (SGR 38;2;R;G;B). */
static int
nearest(int r, int g, int b)
{
	int ci[3], v[3] = { r, g, b }, k, best, grey, gd, cd = 0;
	static const unsigned char level[6] = { 0, 95, 135, 175, 215, 255 };

	for (k = 0; k < 3; k++) {
		ci[k] = v[k] < 48 ? 0 : v[k] < 115 ? 1 : (v[k] - 35) / 40;
		ci[k] = ci[k] > 5 ? 5 : ci[k];
		cd += (v[k] - level[ci[k]]) * (v[k] - level[ci[k]]);
	}
	best = 16 + ci[0] * 36 + ci[1] * 6 + ci[2];
	grey = clamp(((r + g + b) / 3 - 3) / 10, 0, 23);
	for (k = 0, gd = 0; k < 3; k++)
		gd += (v[k] - (8 + grey * 10)) * (v[k] - (8 + grey * 10));
	return gd < cd ? 232 + grey : best;
}

/* ---------------- lines */

/* n blank cells; with `erasing` they take the pen's background, as an erase does */
static void
blank(struct cell *c, int n, int erasing)
{
	struct cell b = { ' ', 7, 0, 0, F_FG | F_BG };

	if (erasing)
		b.bg = pen.bg, b.flags = F_FG | (pen.flags & F_BG);
	while (n-- > 0)
		*c++ = b;
}

static int
plain_blank(const struct cell *c)
{
	return c->ch == ' ' && (c->flags & F_BG) && !(c->at & (A_REVERSE | A_UNDER));
}

/* Room for nc columns and nr rows in both screens and in what is drawn; 0 without memory. */
static int
ensure(int nc, int nr)
{
	int s, r;

	if (nc > cap) {
		for (s = 0; s < 2; s++)
			for (r = 0; r < made; r++) {
				struct cell *c = realloc(screens[s][r].c, nc * sizeof *c);

				if (!c)
					return 0;
				blank(c + cap, nc - cap, 0);
				screens[s][r].c = c;
			}
		for (r = 0; r < made; r++) {
			struct drawn *d = realloc(shown[r], nc * sizeof *d);

			if (!d)
				return 0;
			shown[r] = d;
		}
		cap = nc;
	}
	for (; made < nr; made++) {
		for (s = 0; s < 2; s++) {
			if (!(screens[s][made].c = malloc(cap * sizeof(struct cell))))
				return 0;
			blank(screens[s][made].c, cap, 0);
			screens[s][made].wrapped = 0;
		}
		if (!(shown[made] = malloc(cap * sizeof(struct drawn))))
			return 0;
	}
	return 1;
}

static struct kept *
kept_at(int i)
{
	return back[(back_first + i) % BACK_MOST];
}

static void
mark_all(void)
{
	memset(dirty, 1, sizeof dirty);
}

/* screen row r changed */
static void
mark(int r)
{
	if (r + view < rows)
		dirty[r + view] = 1;
}

static void
invalidate(int r)
{
	int c;

	for (c = 0; c < cols; c++)
		shown[r][c].ch = 0xffff;
}

/* A line of the main screen goes off its top: it is kept to look back to. */
static void
keep_line(const struct line *l)
{
	struct kept *k;
	int n = cols;

	while (n > 0 && plain_blank(&l->c[n - 1]))
		n--;
	if (!(k = malloc(sizeof *k + n * sizeof(struct cell))))
		return;
	k->n = n, k->wrapped = l->wrapped;
	memcpy(k->c, l->c, n * sizeof(struct cell));
	if (back_count == BACK_MOST) {
		free(back[back_first]);
		back[back_first] = k;
		back_first = (back_first + 1) % BACK_MOST;
		if (view) {
			if (view < back_count)
				view++;	/* (what is looked at stays where it is, until it is the oldest there is) */
			mark_all();
		}
	} else {
		back[(back_first + back_count) % BACK_MOST] = k;
		back_count++;
		if (view)
			view++;	/* (what is looked at stays where it is) */
	}
	back_total++;
}

/* The cells of a line by its number, how many there are, and whether it wraps; NULL for none. */
static const struct cell *
line_cells(long line, int *n, int *wrapped)
{
	long a = line - (back_total - back_count);
	struct kept *k;

	if (a < 0 || a >= back_count + rows)
		return NULL;
	if (a >= back_count) {
		*n = cols, *wrapped = scr[a - back_count].wrapped;
		return scr[a - back_count].c;
	}
	k = kept_at((int)a);
	*n = k->n < cols ? k->n : cols, *wrapped = k->wrapped;
	return k->c;
}

/* The number of the line the window's row r shows. */
static long
view_line(int r)
{
	return back_total - view + r;
}

/* ---------------- what is on the window */

/* The rows moved by the scrolls since the last flush, moved on the window too: one copy. */
static void
apply_scroll(void)
{
	struct drawn *moved[ROWS_MOST];
	int t = pend_t, b = pend_b, n = pend_n, h = b - t + 1, k = n < 0 ? -n : n, i;

	pend_n = 0;
	if (n == 0 || !exposed)
		return;
	if (k >= h) {
		for (i = t; i <= b; i++)
			invalidate(i);
		return;
	}
	if (n > 0) {
		GrCopyArea(win, tgc, PAD, PAD + t * chh, cols * cw, (h - k) * chh, win, PAD, PAD + (t + k) * chh, MWROP_COPY);
		memcpy(moved, shown + t, k * sizeof *moved);
		memmove(shown + t, shown + t + k, (h - k) * sizeof *moved);
		memcpy(shown + b - k + 1, moved, k * sizeof *moved);
		for (i = b - k + 1; i <= b; i++)
			invalidate(i);
	} else {
		GrCopyArea(win, tgc, PAD, PAD + (t + k) * chh, cols * cw, (h - k) * chh, win, PAD, PAD + t * chh, MWROP_COPY);
		memcpy(moved, shown + b - k + 1, k * sizeof *moved);
		memmove(shown + t + k, shown + t, (h - k) * sizeof *moved);
		memcpy(shown + t, moved, k * sizeof *moved);
		for (i = t; i < t + k; i++)
			invalidate(i);
	}
}

/* Rows t to b moved up by n (down: negative): on the window it waits for the flush. */
static void
scrolled(int t, int b, int n)
{
	int i;

	if (view) {
		mark_all();
		return;
	}
	if (pend_n && (pend_t != t || pend_b != b || (pend_n > 0) != (n > 0)))
		apply_scroll();
	pend_t = t, pend_b = b, pend_n += n;
	for (i = t; i <= b; i++)
		dirty[i] = 1;
}

static void
set_view(int v)
{
	v = clamp(v, 0, alt ? 0 : back_count);
	if (v == view)
		return;
	if (pend_n)
		apply_scroll();
	pend_t = 0, pend_b = rows - 1, pend_n = view - v;
	view = v;
	mark_all();
}

static int
word_char(unsigned c)
{
	return c > 127 || isalnum(c) || (c && strchr("-_./~:@%+=#?&", (int)c));
}

static unsigned
char_at(long line, int col)
{
	int n, w;
	const struct cell *c = line_cells(line, &n, &w);

	return c && col >= 0 && col < n ? c[col].ch : ' ';
}

/* The selection from its anchor to where the pointer is, in order, whole words or lines for its kind. */
static void
sel_extend(void)
{
	int ahead = sa_line < sb_line || (sa_line == sb_line && sa_col <= sb_col);

	s0_line = ahead ? sa_line : sb_line, s0_col = ahead ? sa_col : sb_col;
	s1_line = ahead ? sb_line : sa_line, s1_col = ahead ? sb_col : sa_col;
	if (sel_kind == 1) {
		if (word_char(char_at(s0_line, s0_col)))
			while (s0_col > 0 && word_char(char_at(s0_line, s0_col - 1)))
				s0_col--;
		if (word_char(char_at(s1_line, s1_col)))
			while (s1_col < cols - 1 && word_char(char_at(s1_line, s1_col + 1)))
				s1_col++;
	} else if (sel_kind == 2) {
		s0_col = 0, s1_col = cols - 1;
	}
	mark_all();
}

static void
sel_drop(void)
{
	if (sel_on)
		mark_all();
	sel_on = 0;
}

/* The cursor's row on the window, or -1 when it is not shown there. */
static int
cursor_row(void)
{
	return cursor_shown && cy + view < rows ? cy + view : -1;
}

/* Row r of the window as it should be. */
static void
resolve(int r, struct drawn *out)
{
	static const struct cell plain = { ' ', 7, 0, 0, F_FG | F_BG };
	long line = view_line(r);
	int n, wrapped, c, from = cols, to = -1, cursor = -1, deco = 0;
	const struct cell *cells = line_cells(line, &n, &wrapped);
	GR_COLOR dfg = default_fg(), dbg = default_bg(), t;

	if (!cells)
		n = 0;
	if (sel_on && line >= s0_line && line <= s1_line) {
		from = line == s0_line ? s0_col : 0;
		to = line == s1_line ? s1_col : cols - 1;
	}
	if (r == cursor_row()) {
		cursor = cx;
		deco = !focused ? D_HOLLOW : !blink_on ? 0 : cursor_style == 1 ? D_LOW : cursor_style == 2 ? D_BAR : D_BLOCK;
	}
	for (c = 0; c < cols; c++) {
		const struct cell *x = c < n ? &cells[c] : &plain;
		int fi = (x->flags & F_FG) ? -1 : x->fg, bi = (x->flags & F_BG) ? -1 : x->bg;
		struct drawn *d = &out[c];

		if ((x->at & A_BOLD) && fi >= 0 && fi < 8)
			fi += 8;	/* (bold is bright, as on most terminals) */
		d->fg = fi < 0 ? dfg : palette[fi];
		d->bg = bi < 0 ? dbg : palette[bi];
		if (x->at & A_DIM)
			d->fg = mix(d->fg, d->bg, 2);
		if (x->at & A_REVERSE)
			t = d->fg, d->fg = d->bg, d->bg = t;
		if (x->at & A_HIDDEN)
			d->fg = d->bg;
		if (c >= from && c <= to)
			d->fg = WHITE, d->bg = UI_SELECTED;
		d->ch = (x->at & A_HIDDEN) ? ' ' : x->ch;
		d->at = ((x->at & A_BOLD) ? D_BOLD : 0) | ((x->at & A_ITALIC) ? D_ITALIC : 0) |
			((x->at & A_UNDER) ? D_UNDER : 0) | ((x->at & A_STRIKE) ? D_STRIKE : 0);
		if (c == cursor && deco) {
			d->at |= deco;
			if (deco == D_BLOCK) {
				t = d->fg, d->fg = d->bg, d->bg = t;
				if (d->fg == d->bg)
					d->fg = dbg, d->bg = dfg;
			}
		}
	}
}

/* The arms of a box-drawing character (up, down, left, right; 1 light, 2 heavy, 3 double), 0 for others. */
static int
box_arms(unsigned ch)
{
#define B(u, d, l, r) ((u) | (d) << 2 | (l) << 4 | (r) << 6)
	switch (ch) {
	case 0x2500: return B(0, 0, 1, 1); case 0x2501: return B(0, 0, 2, 2);
	case 0x2502: return B(1, 1, 0, 0); case 0x2503: return B(2, 2, 0, 0);
	case 0x250c: case 0x256d: return B(0, 1, 0, 1); case 0x250f: return B(0, 2, 0, 2);
	case 0x2510: case 0x256e: return B(0, 1, 1, 0); case 0x2513: return B(0, 2, 2, 0);
	case 0x2514: case 0x2570: return B(1, 0, 0, 1); case 0x2517: return B(2, 0, 0, 2);
	case 0x2518: case 0x256f: return B(1, 0, 1, 0); case 0x251b: return B(2, 0, 2, 0);
	case 0x251c: return B(1, 1, 0, 1); case 0x2523: return B(2, 2, 0, 2);
	case 0x2524: return B(1, 1, 1, 0); case 0x252b: return B(2, 2, 2, 0);
	case 0x252c: return B(0, 1, 1, 1); case 0x2533: return B(0, 2, 2, 2);
	case 0x2534: return B(1, 0, 1, 1); case 0x253b: return B(2, 0, 2, 2);
	case 0x253c: return B(1, 1, 1, 1); case 0x254b: return B(2, 2, 2, 2);
	case 0x2550: return B(0, 0, 3, 3); case 0x2551: return B(3, 3, 0, 0);
	case 0x2554: return B(0, 3, 0, 3); case 0x2557: return B(0, 3, 3, 0);
	case 0x255a: return B(3, 0, 0, 3); case 0x255d: return B(3, 0, 3, 0);
	case 0x2560: return B(3, 3, 0, 3); case 0x2563: return B(3, 3, 3, 0);
	case 0x2566: return B(0, 3, 3, 3); case 0x2569: return B(3, 0, 3, 3);
	case 0x256c: return B(3, 3, 3, 3);
	case 0x2574: return B(0, 0, 1, 0); case 0x2575: return B(1, 0, 0, 0);
	case 0x2576: return B(0, 0, 0, 1); case 0x2577: return B(0, 1, 0, 0);
	}
	return 0;
#undef B
}

/* Lines and blocks are drawn to fill their cells, so that a frame has no gaps between rows. */
static int
is_drawn_shape(unsigned ch)
{
	return (ch >= 0x2580 && ch <= 0x2593) || (ch >= 0x2500 && ch < 0x2580 && box_arms(ch));
}

static void
draw_shape(unsigned ch, int x, int y, GR_COLOR fg, GR_COLOR bg)
{
	int arms = box_arms(ch), mx = x + cw / 2, my = y + chh / 2, k, w, o;

	if (ch >= 0x2580) {
		if (ch >= 0x2591)	/* the shades */
			GrSetGCForeground(tgc, mix(bg, fg, (int)(ch - 0x2590)));
		if (ch == 0x2580)
			GrFillRect(win, tgc, x, y, cw, chh / 2);
		else if (ch <= 0x2588)
			GrFillRect(win, tgc, x, y + chh - chh * (int)(ch - 0x2580) / 8, cw, chh * (int)(ch - 0x2580) / 8);
		else if (ch <= 0x258f)
			GrFillRect(win, tgc, x, y, cw * (8 - (int)(ch - 0x2588)) / 8, chh);
		else if (ch == 0x2590)
			GrFillRect(win, tgc, x + cw / 2, y, cw - cw / 2, chh);
		else
			GrFillRect(win, tgc, x, y, cw, chh);
		GrSetGCForeground(tgc, fg);
		return;
	}
	for (k = 0; k < 4; k++) {
		if (!(w = arms >> (2 * k) & 3))
			continue;
		for (o = w == 3 ? -1 : 0; o <= (w == 3 ? 1 : 0); o += 2) {
			int t = w == 2 ? 2 : 1, px = mx + o - (w == 2), py = my + o - (w == 2);

			if (k == 0)
				GrFillRect(win, tgc, px, y, t, my - y + 1);
			else if (k == 1)
				GrFillRect(win, tgc, px, my, t, y + chh - my);
			else if (k == 2)
				GrFillRect(win, tgc, x, py, mx - x + 1, t);
			else
				GrFillRect(win, tgc, mx, py, x + cw - mx, t);
			if (w != 3)
				break;
		}
	}
}

static GR_FONT_ID
font_for(int at)
{
	int k = rom ? ((at & D_BOLD) ? 1 : 0) | ((at & D_ITALIC) ? 2 : 0) : 0;
	char path[64];

	if (!fonts[k]) {
		/* (an italic is the upright face slanted by the GPU: docs/system/fonts.md) */
		snprintf(path, sizeof path, "/usr/share/font-mono%s-%d.sef", (k & 1) ? "-b" : "", font_size);
		fonts[k] = GrCreateFontEx(path, 0, (k & 2) ? 51 : 0, NULL);
	}
	return fonts[k];
}

/* A character of the window system's own font (one byte), for one it lacks something like it. */
static char
plain_char(unsigned ch)
{
	if (ch < 127)
		return ch;
	if (ch >= 0x2500 && ch < 0x2580) {
		int arms = box_arms(ch);

		return !(arms & 0x0f) ? '-' : !(arms & 0xf0) ? '|' : '+';
	}
	if (ch >= 0x2580 && ch < 0x25a0)
		return '#';
	return ch < 256 && (int)ch <= font_last ? (char)ch : '?';
}

/* n cells of one look at row r, column c. */
static void
draw_run(int r, int c, int n, const struct drawn *d)
{
	static unsigned short wide[COLS_MOST];
	static char narrow[COLS_MOST];
	int x = PAD + c * cw, y = PAD + r * chh, i, ink = 0, shapes = 0, uy;

	GrSetGCForeground(tgc, d->bg);
	GrFillRect(win, tgc, x, y, n * cw, chh);
	for (i = 0; i < n; i++) {
		unsigned ch = d[i].ch;

		if (ch >= 0x2500 && ch <= 0x2593 && is_drawn_shape(ch))
			shapes = 1, ch = ' ';
		if (ch != ' ')
			ink = i + 1;
		if (rom)
			wide[i] = ch;
		else
			narrow[i] = plain_char(ch);
	}
	GrSetGCForeground(tgc, d->fg);
	if (ink) {
		GrSetGCFont(tgc, font_for(d->at));
		if (rom) {
			GrText(win, tgc, x, y, wide, ink, GR_TFUC16 | GR_TFTOP);
		} else {
			GrText(win, tgc, x, y, narrow, ink, GR_TFASCII | GR_TFTOP);
			if (d->at & D_BOLD)	/* (the bitmap font has no bold: drawn twice) */
				GrText(win, tgc, x + 1, y, narrow, ink, GR_TFASCII | GR_TFTOP);
		}
	}
	for (i = 0; shapes && i < n; i++)
		if (d[i].ch >= 0x2500 && d[i].ch <= 0x2593 && is_drawn_shape(d[i].ch))
			draw_shape(d[i].ch, x + i * cw, y, d->fg, d->bg);
	uy = y + (ascent + 1 < chh ? ascent + 1 : chh - 1);
	if (d->at & D_UNDER)
		GrLine(win, tgc, x, uy, x + n * cw - 1, uy);
	if (d->at & D_STRIKE)
		GrLine(win, tgc, x, y + chh / 2, x + n * cw - 1, y + chh / 2);
	if (d->at & D_HOLLOW)
		GrRect(win, tgc, x, y, cw, chh);
	else if (d->at & D_LOW)
		GrFillRect(win, tgc, x, y + chh - 2, cw, 2);
	else if (d->at & D_BAR)
		GrFillRect(win, tgc, x, y, 2, chh);
}

static int
same(const struct drawn *a, const struct drawn *b)
{
	return a->ch == b->ch && a->at == b->at && a->fg == b->fg && a->bg == b->bg;
}

static void
draw_bar(void)
{
	int total = alt ? rows : back_count + rows, at = alt ? 0 : back_count - view;

	ui_scrollbar(win, win_w - UI_SCROLL_W, 0, win_h, total, rows, at);
	bar_total = total, bar_top = at;
}

/* Everything that changed since the last flush, drawn: the rows' changed spans, a run a look. */
static void
flush(void)
{
	static struct drawn row[COLS_MOST];
	int r, first, last, c, e, cr = cursor_row();

	if (!exposed)
		return;
	if (pend_n)
		apply_scroll();
	if (cur_row != cr && cur_row >= 0 && cur_row < rows)
		dirty[cur_row] = 1;
	if (cr >= 0)
		dirty[cr] = 1;
	cur_row = cr;
	for (r = 0; r < rows; r++) {
		if (!dirty[r])
			continue;
		dirty[r] = 0;
		resolve(r, row);
		for (first = 0; first < cols && same(&row[first], &shown[r][first]); first++)
			;
		if (first == cols)
			continue;
		for (last = cols - 1; same(&row[last], &shown[r][last]); last--)
			;
		for (c = first; c <= last; c = e) {
			for (e = c + 1; e <= last && row[e].fg == row[c].fg && row[e].bg == row[c].bg && row[e].at == row[c].at; e++)
				;
			draw_run(r, c, e - c, row + c);
		}
		memcpy(shown[r] + first, row + first, (last + 1 - first) * sizeof *row);
	}
	if (bar_total != (alt ? rows : back_count + rows) || bar_top != (alt ? 0 : back_count - view))
		draw_bar();
}

/* The window's edges round the cells, and everything drawn again. */
static void
paint_frame(void)
{
	int tw = PAD + cols * cw, th = PAD + rows * chh, right = win_w - UI_SCROLL_W, r;

	GrSetGCForeground(tgc, default_bg());
	GrFillRect(win, tgc, 0, 0, right, PAD);
	GrFillRect(win, tgc, 0, 0, PAD, win_h);
	if (tw < right)
		GrFillRect(win, tgc, tw, 0, right - tw, win_h);
	if (th < win_h)
		GrFillRect(win, tgc, 0, th, right, win_h - th);
	for (r = 0; r < rows; r++)
		invalidate(r);
	pend_n = 0;
	bar_total = -1;
	mark_all();
}

/* ---------------- the pseudo-terminal */

static void
tell_size(void)
{
	struct winsize size;

	memset(&size, 0, sizeof size);
	size.ws_col = cols, size.ws_row = rows;
	size.ws_xpixel = cols * cw, size.ws_ypixel = rows * chh;
	ioctl(master, TIOCSWINSZ, &size);
}

static void feed(const unsigned char *s, int n);

/* Sends to the program, reading its output meanwhile when it is not reading (or both would wait). */
static void
pty_send(const char *s, int n)
{
	static unsigned char buf[2048];
	int tries = 0, w, got;

	while (n > 0) {
		if ((w = (int)write(master, s, n)) > 0) {
			s += w, n -= w;
			tries = 0;
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && errno == EAGAIN && tries++ < 300) {
			if ((got = (int)read(master, buf, sizeof buf)) > 0)
				feed(buf, got);
			else
				usleep(10000);
			continue;
		}
		return;
	}
}

static void
reply(const char *s)
{
	int n = (int)strlen(s);

	if (reply_n + n <= (int)sizeof reply_buf)
		memcpy(reply_buf + reply_n, s, n), reply_n += n;
}

static void
send_replies(void)
{
	char out[sizeof reply_buf];
	int n = reply_n;

	if (!n)
		return;
	memcpy(out, reply_buf, n);
	reply_n = 0;
	pty_send(out, n);
}

/* What the program wrote: read for at most 40 ms before it is drawn, the rest after. */
static void
pty_read(void)
{
	static unsigned char buf[4096];
	long start = ms();
	int n;

	for (;;) {
		n = (int)read(master, buf, sizeof buf);
		if (n > 0) {
			feed(buf, n);
			if (ms() - start >= 40)
				break;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n == 0 || errno != EAGAIN) {
			/* (EIO: nothing has the terminal open any more, the shell has gone) */
			flush();
			finish();
		}
		break;
	}
	send_replies();
	activity();
}

/* ---------------- the emulator */

static void
erase_cells(int r, int from, int to)
{
	from = clamp(from, 0, cols), to = clamp(to, 0, cols);
	if (from >= to)
		return;
	blank(scr[r].c + from, to - from, 1);
	if (to == cols)
		scr[r].wrapped = 0;
	mark(r);
}

/* Rows t to b move up by n; with `keep` those leaving the main screen's top are kept. */
static void
scroll_up(int t, int b, int n, int keep)
{
	struct line moved[ROWS_MOST];
	int h = b - t + 1, i;

	if (n <= 0 || h <= 0)
		return;
	n = n > h ? h : n;
	if (keep && !alt && t == 0)
		for (i = 0; i < n; i++)
			keep_line(&scr[i]);
	memcpy(moved, scr + t, n * sizeof *moved);
	memmove(scr + t, scr + t + n, (h - n) * sizeof *moved);
	memcpy(scr + b - n + 1, moved, n * sizeof *moved);
	for (i = b - n + 1; i <= b; i++) {
		blank(scr[i].c, cols, 1);
		scr[i].wrapped = 0;
	}
	scrolled(t, b, n);
}

static void
scroll_down(int t, int b, int n)
{
	struct line moved[ROWS_MOST];
	int h = b - t + 1, i;

	if (n <= 0 || h <= 0)
		return;
	n = n > h ? h : n;
	memcpy(moved, scr + b - n + 1, n * sizeof *moved);
	memmove(scr + t + n, scr + t, (h - n) * sizeof *moved);
	memcpy(scr + t, moved, n * sizeof *moved);
	for (i = t; i < t + n; i++) {
		blank(scr[i].c, cols, 1);
		scr[i].wrapped = 0;
	}
	scrolled(t, b, -n);
}

static void
line_feed(void)
{
	if (cy == bottom)
		scroll_up(top, bottom, 1, 1);
	else if (cy < rows - 1)
		cy++;
}

static void
reverse_index(void)
{
	if (cy == top)
		scroll_down(top, bottom, 1);
	else if (cy > 0)
		cy--;
}

static void
insert_cells(int n)
{
	struct cell *row = scr[cy].c;

	n = clamp(n, 0, cols - cx);
	memmove(row + cx + n, row + cx, (cols - cx - n) * sizeof *row);
	blank(row + cx, n, 1);
	mark(cy);
}

static void
delete_cells(int n)
{
	struct cell *row = scr[cy].c;

	n = clamp(n, 0, cols - cx);
	memmove(row + cx, row + cx + n, (cols - cx - n) * sizeof *row);
	blank(row + cols - n, n, 1);
	mark(cy);
}

static void
put_char(unsigned ch)
{
	/* the DEC line-drawing set (ESC ( 0), from 0x5f */
	static const uint16_t dec[32] = {
		' ', 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0xb0, 0xb1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
		0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c, 0x2502, 0x2264, 0x2265, 0x3c0, 0x2260, 0xa3, 0xb7,
	};
	struct cell *c;

	if (charset[gl] == '0' && ch >= 0x5f && ch <= 0x7e)
		ch = dec[ch - 0x5f];
	if (wrapnext) {
		scr[cy].wrapped = 1;
		cx = 0;
		line_feed();
		wrapnext = 0;
	}
	if (insert)
		insert_cells(1);
	c = &scr[cy].c[cx];
	*c = pen;
	c->ch = ch;
	last_ch = ch;
	mark(cy);
	if (cx == cols - 1)
		wrapnext = autowrap;
	else
		cx++;
}

static void
put_code(unsigned u)
{
	if ((u >= 0x80 && u < 0xa0) || (u >= 0x300 && u < 0x370) || (u >= 0x200b && u <= 0x200f) || (u >= 0xfe00 && u <= 0xfe0f))
		return;	/* (marks that combine, and other characters of no width: not shown) */
	/* (below 0x80: a form too long, never a control; 0xffff marks a cell to draw again) */
	put_char(u >= 0xffff || u < 0x80 ? 0xfffd : u);
}

static void
cup(int r, int c)
{
	if (origin)
		r += top;
	cy = clamp(r, origin ? top : 0, origin ? bottom : rows - 1);
	cx = clamp(c, 0, cols - 1);
	wrapnext = 0;
}

static void
save_cursor(void)
{
	saved[alt].cx = cx, saved[alt].cy = cy, saved[alt].wrapnext = wrapnext, saved[alt].origin = origin;
	saved[alt].pen = pen, saved[alt].gl = gl;
	saved[alt].g[0] = charset[0], saved[alt].g[1] = charset[1];
}

static void
restore_cursor(void)
{
	cx = clamp(saved[alt].cx, 0, cols - 1), cy = clamp(saved[alt].cy, 0, rows - 1);
	wrapnext = saved[alt].wrapnext, origin = saved[alt].origin;
	pen = saved[alt].pen, gl = saved[alt].gl;
	charset[0] = saved[alt].g[0] ? saved[alt].g[0] : 'B', charset[1] = saved[alt].g[1] ? saved[alt].g[1] : 'B';
}

static void
pen_reset(void)
{
	pen.ch = ' ', pen.fg = 7, pen.bg = 0, pen.at = 0, pen.flags = F_FG | F_BG;
}

static void
modes_default(void)
{
	origin = insert = newline_mode = cursor_keys = screen_reverse = bracketed = 0;
	mouse_mode = mouse_sgr = cursor_style = 0;
	autowrap = cursor_shown = cursor_blinks = 1;
	top = 0, bottom = rows - 1;
	charset[0] = charset[1] = charset[2] = 'B', gl = 0;
	pen_reset();
}

static void
clear_screen(int s)
{
	int r;

	for (r = 0; r < rows; r++) {
		blank(screens[s][r].c, cols, s == alt);
		screens[s][r].wrapped = 0;
	}
	mark_all();
}

static void
switch_screen(int to)
{
	if (alt == to)
		return;
	set_view(0);
	alt = to;
	scr = screens[alt];
	sel_drop();
	mark_all();
}

static void
reset_all(void)
{
	int i;

	state = S_GROUND, esc_from = 0, utf_need = 0, osc_n = 0;	/* (the menu's Reset comes in any state: an open ESC ] swallows all) */
	switch_screen(0);
	modes_default();
	clear_screen(0);
	clear_screen(1);
	cx = cy = wrapnext = 0;
	for (i = 0; i < COLS_MOST; i++)
		tabs[i] = i % 8 == 0;
	memset(saved, 0, sizeof saved);
	saved[0].pen = saved[1].pen = pen;
	GrSetWindowTitle(win, TITLE);
	paint_frame();
}

static void
drop_history(void)
{
	int i;

	set_view(0);
	for (i = 0; i < back_count; i++)
		free(kept_at(i));
	back_first = back_count = 0;
	sel_drop();
	mark_all();
}

static void
erase_display(int how)
{
	int r;

	if (how == 3) {
		drop_history();
		return;
	}
	if (how == 0)
		erase_cells(cy, cx, cols);
	if (how == 1)
		erase_cells(cy, 0, cx + 1);
	for (r = 0; r < rows; r++)
		if (how == 2 || (how == 0 && r > cy) || (how == 1 && r < cy))
			erase_cells(r, 0, cols);
}

static void
dec_mode(int m, int on)
{
	switch (m) {
	case 1: cursor_keys = on; break;
	case 5: if (screen_reverse != on) { screen_reverse = on; paint_frame(); } break;
	case 6: origin = on; cup(0, 0); break;
	case 7: autowrap = on; wrapnext = wrapnext && on; break;
	case 12: cursor_blinks = on; break;
	case 25: cursor_shown = on; break;
	case 9: case 1000: case 1002: case 1003: mouse_mode = on ? m : 0; break;
	case 1006: mouse_sgr = on; break;
	case 47: switch_screen(on); break;
	case 1047:
		if (!on && alt)
			clear_screen(1);
		switch_screen(on);
		break;
	case 1048: if (on) save_cursor(); else restore_cursor(); break;
	case 1049:
		if (on) {
			save_cursor();
			switch_screen(1);
			clear_screen(1);
		} else {
			switch_screen(0);
		}
		if (!on)
			restore_cursor();
		break;
	case 2004: bracketed = on; break;
	}
}

static void
sgr(void)
{
	int i, j, idx, k;

	if (npar == 0)
		pen_reset();
	for (i = 0; i < npar; i++) {
		int p = par[i];

		if (p == 38 || p == 48) {
			/* 38;5;N, 38;2;R;G;B, and with colons 38:5:N, 38:2:[space]:R:G:B */
			for (j = i + 1; j < npar && sub[j]; j++)
				;
			if (i + 2 < npar && par[i + 1] == 5) {
				idx = par[i + 2] & 255;
				i += 2;
			} else if (i + 1 < npar && par[i + 1] == 2) {
				k = j - (i + 1) == 5 ? i + 3 : i + 2;
				if (k + 2 >= npar)
					return;
				idx = nearest(clamp(par[k], 0, 255), clamp(par[k + 1], 0, 255), clamp(par[k + 2], 0, 255));
				i = k + 2;
			} else {
				return;
			}
			if (p == 38)
				pen.fg = idx, pen.flags &= ~F_FG;
			else
				pen.bg = idx, pen.flags &= ~F_BG;
			continue;
		}
		if (sub[i])
			continue;
		switch (p) {
		case 0: pen_reset(); break;
		case 1: pen.at |= A_BOLD; break;
		case 2: pen.at |= A_DIM; break;
		case 3: pen.at |= A_ITALIC; break;
		case 4: case 21: pen.at |= A_UNDER; if (i + 1 < npar && sub[i + 1] && par[i + 1] == 0) pen.at &= ~A_UNDER; break;
		case 7: pen.at |= A_REVERSE; break;
		case 8: pen.at |= A_HIDDEN; break;
		case 9: pen.at |= A_STRIKE; break;
		case 22: pen.at &= ~(A_BOLD | A_DIM); break;
		case 23: pen.at &= ~A_ITALIC; break;
		case 24: pen.at &= ~A_UNDER; break;
		case 27: pen.at &= ~A_REVERSE; break;
		case 28: pen.at &= ~A_HIDDEN; break;
		case 29: pen.at &= ~A_STRIKE; break;
		case 39: pen.flags |= F_FG; break;
		case 49: pen.flags |= F_BG; break;
		default:
			if (p >= 30 && p <= 37)
				pen.fg = p - 30, pen.flags &= ~F_FG;
			else if (p >= 40 && p <= 47)
				pen.bg = p - 40, pen.flags &= ~F_BG;
			else if (p >= 90 && p <= 97)
				pen.fg = p - 90 + 8, pen.flags &= ~F_FG;
			else if (p >= 100 && p <= 107)
				pen.bg = p - 100 + 8, pen.flags &= ~F_BG;
		}
	}
}

static int
P(int i, int otherwise)
{
	return i < npar && par[i] ? par[i] : otherwise;
}

static void
tab_forward(int n)
{
	while (n-- > 0 && cx < cols - 1)
		for (cx++; cx < cols - 1 && !tabs[cx]; cx++)
			;
}

static void
tab_back(int n)
{
	while (n-- > 0 && cx > 0)
		for (cx--; cx > 0 && !tabs[cx]; cx--)
			;
}

static void
csi(int final)
{
	char out[48];
	int n = P(0, 1), i;

	if (inter == ' ' && final == 'q') {	/* the cursor's shape */
		cursor_style = n <= 2 ? 0 : n <= 4 ? 1 : 2;
		cursor_blinks = P(0, 0) == 0 || n % 2 == 1;
		return;
	}
	if (inter == '!' && final == 'p') {	/* a soft reset */
		modes_default();
		return;
	}
	if (inter)
		return;
	if (priv == '?' && (final == 'h' || final == 'l')) {
		for (i = 0; i < npar; i++)
			dec_mode(par[i], final == 'h');
		return;
	}
	if (priv == '>' && final == 'c') {
		reply("\033[>0;10;1c");
		return;
	}
	if (priv && !(priv == '?' && (final == 'J' || final == 'K')))
		return;
	if (!strchr("mnchlb", final))
		wrapnext = 0;
	switch (final) {
	case '@': insert_cells(n); break;
	case 'A': cy = clamp(cy - n, cy >= top ? top : 0, rows - 1); break;
	case 'B': case 'e': cy = clamp(cy + n, 0, cy <= bottom ? bottom : rows - 1); break;
	case 'C': case 'a': cx = clamp(cx + n, 0, cols - 1); break;
	case 'D': cx = clamp(cx - n, 0, cols - 1); break;
	case 'E': cx = 0, cy = clamp(cy + n, 0, cy <= bottom ? bottom : rows - 1); break;
	case 'F': cx = 0, cy = clamp(cy - n, cy >= top ? top : 0, rows - 1); break;
	case 'G': case '`': cx = clamp(n - 1, 0, cols - 1); break;
	case 'H': case 'f': cup(P(0, 1) - 1, P(1, 1) - 1); break;
	case 'I': tab_forward(n); break;
	case 'Z': tab_back(n); break;
	case 'J': erase_display(npar ? par[0] : 0); break;
	case 'K':
		i = npar ? par[0] : 0;
		erase_cells(cy, i == 0 ? cx : 0, i == 1 ? cx + 1 : cols);
		break;
	case 'L': if (cy >= top && cy <= bottom) scroll_down(cy, bottom, n), cx = 0; break;
	case 'M': if (cy >= top && cy <= bottom) scroll_up(cy, bottom, n, 0), cx = 0; break;
	case 'P': delete_cells(n); break;
	case 'S': scroll_up(top, bottom, n, 0); break;
	case 'T': if (npar <= 1) scroll_down(top, bottom, n); break;
	case 'X': erase_cells(cy, cx, cx + n); break;
	case 'b':
		for (n = n > cols * rows ? cols * rows : n; n > 0 && last_ch; n--)
			put_char(last_ch);
		break;
	case 'c': if (P(0, 0) == 0) reply("\033[?62;22c"); break;
	case 'd': cup(n - 1, cx); break;
	case 'g':
		if (P(0, 0) == 0)
			tabs[cx] = 0;
		else if (par[0] == 3)
			memset(tabs, 0, sizeof tabs);
		break;
	case 'h': case 'l':
		for (i = 0; i < npar; i++)
			if (par[i] == 4)
				insert = final == 'h';
			else if (par[i] == 20)
				newline_mode = final == 'h';
		break;
	case 'm': sgr(); break;
	case 'n':
		if (P(0, 0) == 5)
			reply("\033[0n");
		else if (P(0, 0) == 6) {
			snprintf(out, sizeof out, "\033[%d;%dR", cy - (origin ? top : 0) + 1, cx + 1);
			reply(out);
		}
		break;
	case 'r':
		i = clamp(P(0, 1) - 1, 0, rows - 1), n = clamp(P(1, rows) - 1, 0, rows - 1);
		if (i < n)
			top = i, bottom = n;
		else
			top = 0, bottom = rows - 1;
		cup(0, 0);
		break;
	case 's': save_cursor(); break;
	case 'u': restore_cursor(); break;
	case 't':
		if (P(0, 0) == 18) {
			snprintf(out, sizeof out, "\033[8;%d;%dt", rows, cols);
			reply(out);
		}
		break;
	}
}

static int
base64(const char *in, char *out)
{
	static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int n = 0, bits = 0, v = 0;
	const char *at;

	for (; *in && *in != '='; in++) {
		if (!(at = strchr(table, *in)))
			continue;
		v = (v << 6 | (int)(at - table)) & 0xffffff;
		if ((bits += 6) >= 8)
			bits -= 8, out[n++] = (char)(v >> bits & 255);
	}
	return n;
}

/* An operating system command: 0 and 2 name the window, 52 puts text on the clipboard. */
static void
osc_end(void)
{
	char title[64], *text;
	int which = atoi(osc), i, n;

	osc[osc_n] = 0;
	text = strchr(osc, ';');
	if (!text)
		return;
	text++;
	if (which == 0 || which == 2) {
		for (i = n = 0; text[i] && n < (int)sizeof title - 1; i++)
			if ((unsigned char)text[i] >= 32 && (unsigned char)text[i] < 127)
				title[n++] = text[i];
		title[n] = 0;
		GrSetWindowTitle(win, n ? title : TITLE);
	} else if (which == 52 && (text = strchr(text, ';')) != NULL && text[1] != '?') {
		static char data[sizeof osc];

		ui_clip_set(data, base64(text + 1, data));
	}
}

static void
esc(int b)
{
	int from = esc_from;

	esc_from = 0;
	state = S_GROUND;
	if (from && b == '\\') {	/* the string's end */
		if (from == S_OSC)
			osc_end();
		return;
	}
	switch (b) {
	case '[': state = S_CSI, npar = 0, par[0] = sub[0] = 0, priv = inter = 0; break;
	case ']': state = S_OSC, osc_n = 0; break;
	case 'P': case 'X': case '^': case '_': state = S_STRING; break;
	case '(': case ')': case '*': case '+': charset_slot = b == '(' ? 0 : b == ')' ? 1 : 2, state = S_CHARSET; break;
	case '%': case ' ': state = S_SKIP; break;
	case '#': state = S_HASH; break;
	case '7': save_cursor(); break;
	case '8': restore_cursor(); break;
	case 'D': wrapnext = 0, line_feed(); break;
	case 'E': wrapnext = 0, cx = 0, line_feed(); break;
	case 'M': wrapnext = 0, reverse_index(); break;
	case 'H': tabs[cx] = 1; break;
	case 'c': reset_all(); break;
	case 'Z': reply("\033[?62;22c"); break;
	}
}

static void
control(int b)
{
	utf_need = 0;
	switch (b) {
	case 8: if (cx > 0) cx--; wrapnext = 0; break;
	case 9: tab_forward(1); break;
	case 10: case 11: case 12: wrapnext = 0, line_feed(); if (newline_mode) cx = 0; break;
	case 13: cx = 0, wrapnext = 0; break;
	case 14: gl = 1; break;
	case 15: gl = 0; break;
	case 0x18: case 0x1a: state = S_GROUND; break;
	case 27: state = S_ESC, esc_from = 0; break;
	}
}

static void
byte(int b)
{
	if (state == S_OSC || state == S_STRING) {
		if (b == 7 && state == S_OSC) {
			osc_end();
			state = S_GROUND;
		} else if (b == 27) {
			esc_from = state, state = S_ESC;
		} else if (b == 0x18 || b == 0x1a) {
			state = S_GROUND;
		} else if (state == S_OSC && osc_n < (int)sizeof osc - 1) {
			osc[osc_n++] = (char)b;
		}
		return;
	}
	if (b < 32 || b == 127) {
		if (b != 127)
			control(b);
		return;
	}
	switch (state) {
	case S_GROUND:
		if (b < 0x80) {
			utf_need = 0;
			put_char(b);
		} else if (b < 0xc0) {
			if (utf_need && (utf = utf << 6 | (b & 63), --utf_need == 0))
				put_code(utf);
			else if (!utf_need)
				put_char(0xfffd);
		} else if (b < 0xe0) {
			utf = b & 31, utf_need = 1;
		} else if (b < 0xf0) {
			utf = b & 15, utf_need = 2;
		} else if (b < 0xf8) {
			utf = b & 7, utf_need = 3;
		} else {
			put_char(0xfffd);
		}
		break;
	case S_ESC:
		esc(b);
		break;
	case S_CSI:
		if (b >= '0' && b <= '9') {
			if (npar == 0)
				npar = 1;
			if (par[npar - 1] < 100000)
				par[npar - 1] = par[npar - 1] * 10 + b - '0';
		} else if (b == ';' || b == ':') {
			if (npar == 0)
				npar = 1;
			if (npar < 16)
				sub[npar] = b == ':', par[npar++] = 0;
		} else if (b >= '<' && b <= '?') {
			priv = b;
		} else if (b >= ' ' && b <= '/') {
			inter = b;
		} else {
			state = S_GROUND;
			if (b >= '@' && b <= '~')
				csi(b);
		}
		break;
	case S_CHARSET:
		charset[charset_slot] = (unsigned char)b;
		state = S_GROUND;
		break;
	case S_HASH:
		if (b == '8') {	/* the screen full of E, to line a screen up by */
			int r, c;

			for (r = 0; r < rows; r++)
				for (c = 0; c < cols; c++)
					scr[r].c[c] = pen, scr[r].c[c].ch = 'E';
			mark_all();
		}
		state = S_GROUND;
		break;
	default:
		state = S_GROUND;
	}
}

static void
feed(const unsigned char *s, int n)
{
	const unsigned char *end = s + n;

	while (s < end) {
		/* a run of plain text, most of any output, past the general path */
		if (state == S_GROUND && !utf_need && !insert && charset[gl] == 'B' && *s >= 32 && *s < 127) {
			while (s < end && *s >= 32 && *s < 127) {
				struct cell *c;

				if (wrapnext) {
					scr[cy].wrapped = 1;
					cx = 0;
					line_feed();
					wrapnext = 0;
				}
				c = &scr[cy].c[cx];
				*c = pen;
				c->ch = *s;
				last_ch = *s++;
				if (cx == cols - 1)
					wrapnext = autowrap;
				else
					cx++;
				if (s == end || *s < 32 || *s >= 127 || wrapnext)
					mark(cy);
			}
			continue;
		}
		byte(*s++);
	}
}

/* ---------------- size and font */

static void
resize(int nc, int nr)
{
	struct line moved[ROWS_MOST], *main_lines = screens[0];
	int r, s, n, pull, old_cols = cols;
	int *main_cy = alt ? &saved[0].cy : &cy;	/* (under a full-screen program the main screen is cut and comes back too) */

	nc = clamp(nc, 2, COLS_MOST), nr = clamp(nr, 1, ROWS_MOST);
	if (nc == cols && nr == rows)
		return;
	set_view(0);
	pend_n = 0;
	if (!ensure(nc, nr))
		return;
	if (nc > cols)
		for (s = 0; s < 2; s++)
			for (r = 0; r < made; r++)
				blank(screens[s][r].c + cols, nc - cols, 0);
	for (s = 0; s < 2; s++)
		for (r = rows; r < nr; r++) {
			blank(screens[s][r].c, nc, 0);
			screens[s][r].wrapped = 0;
		}
	cols = nc;
	if (nr < rows && *main_cy >= nr) {
		/* fewer rows: those over the cursor go to be looked back to */
		n = *main_cy - nr + 1;
		for (r = 0; r < n; r++)
			keep_line(&main_lines[r]);
		memcpy(moved, main_lines, n * sizeof *moved);
		memmove(main_lines, main_lines + n, (rows - n) * sizeof *moved);
		memcpy(main_lines + rows - n, moved, n * sizeof *moved);
		*main_cy -= n;
	} else if (nr > rows && (pull = nr - rows < back_count ? nr - rows : back_count) > 0) {
		/* more rows: the last lines looked back to come down onto the screen again */
		memcpy(moved, main_lines + rows, pull * sizeof *moved);
		memmove(main_lines + pull, main_lines, rows * sizeof *moved);
		memcpy(main_lines, moved, pull * sizeof *moved);
		for (r = pull - 1; r >= 0; r--) {
			struct kept *k = kept_at(back_count - 1);

			n = k->n < cols ? k->n : cols;
			memcpy(main_lines[r].c, k->c, n * sizeof(struct cell));
			blank(main_lines[r].c + n, cols - n, 0);
			main_lines[r].wrapped = k->wrapped;
			free(k);
			back_count--, back_total--;
		}
		cy = alt ? cy : cy + pull;
		saved[0].cy += pull;
	}
	rows = nr;
	top = 0, bottom = rows - 1;
	cx = clamp(cx, 0, cols - 1), cy = clamp(cy, 0, rows - 1);
	wrapnext = 0;
	for (r = old_cols; r < cols; r++)
		tabs[r] = r % 8 == 0;
	sel_on = 0;
	cur_row = -1;
	tell_size();
}

/* The cells' size for the window's: as many columns and rows as fit, the program told. */
static void
layout(void)
{
	resize((win_w - UI_SCROLL_W - 2 * PAD) / cw, (win_h - 2 * PAD) / chh);
	paint_frame();
}

static void
set_font(int size)
{
	GR_FONT_INFO info;
	char path[64];
	int k;

	for (k = 0; k < 4; k++)
		if (fonts[k])
			GrDestroyFont(fonts[k]), fonts[k] = 0;
	rom = 0;
	if (size) {
		snprintf(path, sizeof path, "/usr/share/font-mono-%d.sef", size);
		fonts[0] = GrCreateFontEx(path, 0, 0, NULL);
		GrGetFontInfo(fonts[0], &info);
		/* (a font the ROM does not have, or a server drawing in software, gives the built-in one) */
		if (info.lastchar == 0xffff)
			rom = 1;
		else
			GrDestroyFont(fonts[0]), fonts[0] = 0, size = 0;
	}
	if (!size) {
		fonts[0] = GrCreateFontEx(GR_FONT_SYSTEM_FIXED, 0, 0, NULL);
		GrGetFontInfo(fonts[0], &info);
	}
	font_size = size;
	cw = info.maxwidth > 0 ? info.maxwidth : 8;
	chh = info.height > 0 ? info.height : 13;
	ascent = info.baseline;
	font_last = info.lastchar;
}

/* Another font, the window made the size that keeps its columns and rows. */
/* As many cells as there are, or as fit on the screen under a caption, above the bar (6 and 55: the frame's). */
static int
fit_cols(int n)
{
	GR_SCREEN_INFO s;
	int most;

	GrGetScreenInfo(&s);
	most = (s.cols - 6 - 2 * PAD - UI_SCROLL_W) / cw;
	return n < most || most < 2 ? n : most;
}

static int
fit_rows(int n)
{
	GR_SCREEN_INFO s;
	int most;

	GrGetScreenInfo(&s);
	most = (s.rows - 55 - 2 * PAD) / chh;
	return n < most || most < 2 ? n : most;
}

static void
change_font(int size)
{
	char text[8];

	set_font(size);
	snprintf(text, sizeof text, "%d", font_size);
	reg_set("terminal.size", text);
	/* (a large size in fewer cells: the window's X and its last rows stay on the screen; its size event does the rest) */
	win_w = fit_cols(cols) * cw + 2 * PAD + UI_SCROLL_W, win_h = fit_rows(rows) * chh + 2 * PAD;
	GrResizeWindow(win, win_w, win_h);
	tell_size();
	paint_frame();
}

static void
font_step(int by)
{
	int i, n = (int)(sizeof sizes / sizeof sizes[0]);

	for (i = 0; i < n && sizes[i] != font_size; i++)
		;
	i = clamp((i == n ? 3 : i) + by, 1, n - 1);
	if (sizes[i] != font_size)
		change_font(sizes[i]);
}

/* ---------------- the clipboard */

static int
utf8(unsigned c, char *out)
{
	if (c < 0x80)
		return out[0] = (char)c, 1;
	if (c < 0x800)
		return out[0] = (char)(0xc0 | c >> 6), out[1] = (char)(0x80 | (c & 63)), 2;
	out[0] = (char)(0xe0 | c >> 12), out[1] = (char)(0x80 | (c >> 6 & 63)), out[2] = (char)(0x80 | (c & 63));
	return 3;
}

/* The selected text to the clipboard: rows without their blanks at the end, a line's end between. */
static void
sel_copy(void)
{
	long line;
	char *text;
	int n, wrapped, c, end, length = 0;

	if (!sel_on || !(text = malloc((size_t)(s1_line - s0_line + 1) * (cols * 3 + 1) + 1)))
		return;
	for (line = s0_line; line <= s1_line; line++) {
		const struct cell *cells = line_cells(line, &n, &wrapped);
		int from = line == s0_line ? s0_col : 0, to = line == s1_line ? s1_col : cols - 1;

		if (!cells)
			continue;
		end = to < n - 1 ? to : n - 1;
		if (!(wrapped && line != s1_line))
			while (end >= from && cells[end].ch == ' ')
				end--;
		for (c = from; c <= end; c++)
			length += utf8(cells[c].ch, text + length);
		if (line != s1_line && !(wrapped && end == to))
			text[length++] = '\n';
	}
	ui_clip_set(text, length);
	free(text);
}

static void
paste(void)
{
	char *text, *out;
	int n, i, k = 0;

	if (!(text = ui_clip_get(&n)))
		return;
	if ((out = malloc(n + 1)) != NULL) {
		/* a line's end typed is a return */
		for (i = 0; i < n; i++)
			if (text[i] == '\n' && i > 0 && text[i - 1] == '\r')
				continue;
			else
				out[k++] = text[i] == '\n' ? '\r' : text[i];
		set_view(0);
		sel_drop();
		if (bracketed)
			pty_send("\033[200~", 6);
		pty_send(out, k);
		if (bracketed)
			pty_send("\033[201~", 6);
		free(out);
	}
	free(text);
}

static void
select_all(void)
{
	sel_kind = 0;
	sa_line = back_total - back_count, sa_col = 0;
	sb_line = back_total + rows - 1, sb_col = cols - 1;
	sel_on = 1;
	sel_extend();
	sel_copy();
}

/* The screen and what was kept cleared, the cursor's line now at the top. */
static void
clear_all(void)
{
	int r;

	drop_history();
	if (cy > 0)
		scroll_up(0, rows - 1, cy, 0), cy = 0;
	for (r = 1; r < rows; r++)
		erase_cells(r, 0, cols);
}

/* ---------------- keys and the pointer */

/* What a key sends, as xterm sends it; 0 for a key that sends nothing. */
static int
key_bytes(int ch, int mods, char *out)
{
	int shift = (mods & MWKMOD_SHIFT) != 0, ctrl = (mods & MWKMOD_CTRL) != 0, altk = (mods & MWKMOD_ALT) != 0;
	int m = 1 + shift + 2 * altk + 4 * ctrl, number = 0, n = 0;
	char final = 0, ss3 = 0;

	switch (ch) {	/* the keypad with NumLock off, as the machine starts */
	case MWKEY_KP8: ch = MWKEY_UP; break;
	case MWKEY_KP2: ch = MWKEY_DOWN; break;
	case MWKEY_KP6: ch = MWKEY_RIGHT; break;
	case MWKEY_KP4: ch = MWKEY_LEFT; break;
	case MWKEY_KP7: ch = MWKEY_HOME; break;
	case MWKEY_KP1: ch = MWKEY_END; break;
	case MWKEY_KP9: ch = MWKEY_PAGEUP; break;
	case MWKEY_KP3: ch = MWKEY_PAGEDOWN; break;
	case MWKEY_KP0: ch = MWKEY_INSERT; break;
	}

	switch (ch) {
	case MWKEY_UP: final = 'A'; break;
	case MWKEY_DOWN: final = 'B'; break;
	case MWKEY_RIGHT: final = 'C'; break;
	case MWKEY_LEFT: final = 'D'; break;
	case MWKEY_HOME: final = 'H'; break;
	case MWKEY_END: final = 'F'; break;
	case MWKEY_INSERT: number = 2; break;
	case MWKEY_DELETE: number = 3; break;
	case MWKEY_PAGEUP: number = 5; break;
	case MWKEY_PAGEDOWN: number = 6; break;
	case MWKEY_F1: case MWKEY_F2: case MWKEY_F3: case MWKEY_F4: ss3 = (char)('P' + ch - MWKEY_F1); break;
	case MWKEY_F5: number = 15; break;
	case MWKEY_F6: number = 17; break;
	case MWKEY_F7: number = 18; break;
	case MWKEY_F8: number = 19; break;
	case MWKEY_F9: number = 20; break;
	case MWKEY_F10: number = 21; break;
	case MWKEY_F11: number = 23; break;
	case MWKEY_F12: number = 24; break;
	case MWKEY_BACKSPACE: ch = ctrl ? 8 : 0x7f; break;
	case MWKEY_ENTER: ch = '\r'; break;
	case MWKEY_TAB: if (shift) return sprintf(out, "\033[Z"); break;
	case MWKEY_ESCAPE: ch = 27; break;
	}
	if (final)
		return m > 1 ? sprintf(out, "\033[1;%d%c", m, final) : sprintf(out, "\033%c%c", cursor_keys ? 'O' : '[', final);
	if (number)
		return m > 1 ? sprintf(out, "\033[%d;%d~", number, m) : sprintf(out, "\033[%d~", number);
	if (ss3)
		return m > 1 ? sprintf(out, "\033[1;%d%c", m, ss3) : sprintf(out, "\033O%c", ss3);
	if (ch & MWKEY_NONASCII_MASK)
		return 0;
	if (ctrl && (ch == ' ' || ch == '2'))
		ch = 0;
	else if (ctrl && ch >= '3' && ch <= '7')
		ch = 0x1b + ch - '3';
	else if (ctrl && ch == '8')
		ch = 0x7f;
	else if (ctrl && ch == '/')
		ch = 0x1f;
	if (altk)
		out[n++] = 27;
	n += utf8((unsigned)ch, out + n);
	if (ch == '\r' && newline_mode)
		out[n++] = '\n';
	return n;
}

static void
mouse_report(int button, int row, int col, int release, int motion, int mods)
{
	char out[32];
	int b = button + ((mods & MWKMOD_ALT) ? 8 : 0) + ((mods & MWKMOD_CTRL) ? 16 : 0) + (motion ? 32 : 0), n;

	if (mouse_mode == 9 && (release || motion))
		return;
	if (mouse_sgr)
		n = snprintf(out, sizeof out, "\033[<%d;%d;%d%c", b, col + 1, row + 1, release ? 'm' : 'M');
	else
		n = snprintf(out, sizeof out, "\033[M%c%c%c", 32 + (release ? 3 | (b & ~3) : b),
			32 + (col + 1 > 223 ? 223 : col + 1), 32 + (row + 1 > 223 ? 223 : row + 1));
	pty_send(out, n);
}

enum { M_COPY = 1, M_PASTE, M_ALL, M_CLEAR, M_RESET, M_DARK, M_LIGHT, M_NEW, M_FONT = 100 };

static void
new_terminal(void)
{
	char link[40], folder[256];
	ssize_t n;
	int i;

	snprintf(link, sizeof link, "/proc/%d/cwd", (int)shell);
	n = readlink(link, folder, sizeof folder - 1);
	folder[n > 0 ? n : 0] = 0;
	if (real_vfork() != 0)
		return;
	/* (the child is in our memory until its exec: system calls only) */
	signal(SIGCHLD, SIG_DFL);
	for (i = 3; i < 64; i++)
		close(i);
	if (folder[0] && chdir(folder) != 0)
		;	/* (the folder has gone: the new shell starts where this one was started) */
	execlp("nxterm", "nxterm", (char *)NULL);
	_exit(127);
}

static void
command(int id)
{
	if (id >= M_FONT) {
		if (sizes[id - M_FONT] != font_size)
			change_font(sizes[id - M_FONT]);
		return;
	}
	switch (id) {
	case M_COPY: sel_copy(); break;
	case M_PASTE: paste(); break;
	case M_ALL: select_all(); break;
	case M_CLEAR: clear_all(); break;
	case M_RESET: reset_all(); break;
	case M_DARK: case M_LIGHT:
		light = id == M_LIGHT;
		reg_set("terminal.colours", light ? "light" : "dark");
		set_scheme();
		paint_frame();
		break;
	case M_NEW: new_terminal(); break;
	}
}

static void
context_menu(int x, int y, int keyboard)
{
	static const char *const size_names[] = { "Window system's font", "10 pixels", "11 pixels", "13 pixels", "15 pixels",
		"17 pixels", "20 pixels", "24 pixels" };
	static struct ui_menu_item menu[9], fonts_menu[8], colours[2];
	int n = 0, i;
#define ROW(text, what, mark) (menu[n].label = (text), menu[n].id = (what), menu[n].icon = -1, menu[n].sub = NULL, menu[n].flags = (mark), n++)

	for (i = 0; i < 8; i++) {
		fonts_menu[i].label = size_names[i], fonts_menu[i].id = M_FONT + i, fonts_menu[i].icon = -1;
		fonts_menu[i].flags = (sizes[i] == font_size ? UI_MENU_CHECK : 0) | (i == 1 ? UI_MENU_LINE : 0);
	}
	colours[0].label = "Dark", colours[0].id = M_DARK, colours[0].icon = -1, colours[0].flags = light ? 0 : UI_MENU_CHECK;
	colours[1].label = "Light", colours[1].id = M_LIGHT, colours[1].icon = -1, colours[1].flags = light ? UI_MENU_CHECK : 0;
	ROW("Copy   Ctrl+Shift+C", M_COPY, sel_on ? 0 : UI_MENU_GREY);
	ROW("Paste   Ctrl+Shift+V", M_PASTE, access(UI_CLIPBOARD, R_OK) == 0 ? 0 : UI_MENU_GREY);
	ROW("Select all", M_ALL, 0);
	ROW("Clear", M_CLEAR, UI_MENU_LINE);
	ROW("Reset", M_RESET, 0);
	ROW("Text size", 0, UI_MENU_LINE);
	menu[n - 1].sub = fonts_menu, menu[n - 1].sub_count = 8;
	ROW("Colours", 0, 0);
	menu[n - 1].sub = colours, menu[n - 1].sub_count = 2;
	ROW("New terminal", M_NEW, UI_MENU_LINE);
	menu[n - 1].icon = ui_icon_of("nxterm");
#undef ROW
	ui_menu_open(menu, n, x, y, 0, keyboard);
}

static void
key(const GR_EVENT_KEYSTROKE *k)
{
	int ch = k->ch, mods = k->modifiers, shift = mods & MWKMOD_SHIFT, ctrl = mods & MWKMOD_CTRL, n;
	char out[40];

	if (ch >= MWKEY_NUMLOCK && ch <= MWKEY_ALTGR)
		return;	/* (a modifier by itself is no key) */
	if (ctrl && shift && ch == 3) {
		sel_copy();
		return;
	}
	if ((ctrl && shift && ch == 22) || (shift && ch == MWKEY_INSERT)) {
		paste();
		return;
	}
	if (shift && (ch == MWKEY_PAGEUP || ch == MWKEY_PAGEDOWN)) {
		set_view(view + (ch == MWKEY_PAGEUP ? rows - 1 : 1 - rows));
		return;
	}
	if (ctrl && shift && (ch == MWKEY_HOME || ch == MWKEY_END)) {
		set_view(ch == MWKEY_HOME ? back_count : 0);
		return;
	}
	if (ctrl && (ch == '=' || ch == '+' || ch == '-')) {
		font_step(ch == '-' ? -1 : 1);
		return;
	}
	if (ch == MWKEY_MENU) {
		int wx, wy;

		ui_window_origin(win, &wx, &wy);
		context_menu(wx + PAD + cx * cw, wy + PAD + (cy + view + 1) * chh, 1);
		return;
	}
	if ((n = key_bytes(ch, mods, out)) <= 0)
		return;
	set_view(0);
	sel_drop();
	pty_send(out, n);
	activity();
}

static void
pointer_cell(int x, int y, int *row, int *col)
{
	*row = clamp((y - PAD) / chh, 0, rows - 1);
	*col = clamp((x - PAD) / cw, 0, cols - 1);
}

static void
button_down(const GR_EVENT_BUTTON *b)
{
	int turn = ui_wheel((const GR_EVENT *)b), row, col, reporting = mouse_mode && !(b->modifiers & MWKMOD_SHIFT), i;
	long now;

	pointer_cell(b->x, b->y, &row, &col);
	if (turn) {
		turn = ui_wheel_sum((const GR_EVENT *)b);
		if (reporting) {
			for (i = 0; i < (turn < 0 ? -turn : turn); i++)
				mouse_report(turn < 0 ? 64 : 65, row, col, 0, 0, b->modifiers);
		} else if (alt) {
			/* (a full-screen program has no lines to look back to: the wheel is its arrows) */
			char arrows[3 * 3 * 8];
			int n = 0;

			for (i = 0; i < 3 * (turn < 0 ? -turn : turn) && n + 4 <= (int)sizeof arrows; i++)
				n += sprintf(arrows + n, "\033%c%c", cursor_keys ? 'O' : '[', turn < 0 ? 'A' : 'B');
			pty_send(arrows, n);
		} else {
			set_view(view - 3 * turn);
		}
		return;
	}
	if (b->x >= win_w - UI_SCROLL_W && (b->changebuttons & GR_BUTTON_L)) {
		bar_held = 1;
		ui_moves(win, EVENTS, 1);
		set_view(back_count - ui_scroll_to(b->y, 0, win_h, alt ? rows : back_count + rows, rows));
		return;
	}
	if (reporting) {
		mouse_report(b->changebuttons & GR_BUTTON_L ? 0 : b->changebuttons & GR_BUTTON_M ? 1 : 2, row, col, 0, 0, b->modifiers);
		mouse_held = 1;
		if (mouse_mode >= 1002)
			ui_moves(win, EVENTS, 1);
		return;
	}
	if (b->changebuttons & GR_BUTTON_R) {
		context_menu(b->rootx, b->rooty, 0);
		return;
	}
	if (b->changebuttons & GR_BUTTON_M) {
		paste();
		return;
	}
	if (!(b->changebuttons & GR_BUTTON_L))
		return;
	/* a selection begins: two clicks in one place select a word, three a line */
	now = ms();
	if (now - click_ms < 450 && view_line(row) == click_line && col == click_col)
		clicks = clicks % 3 + 1;
	else
		clicks = 1;
	click_ms = now, click_line = view_line(row), click_col = col;
	sel_drop();
	sel_kind = clicks - 1;
	sa_line = sb_line = view_line(row), sa_col = sb_col = col;
	sel_on = sel_kind > 0;
	if (sel_on)
		sel_extend();
	sel_held = 1;
	ui_moves(win, EVENTS, 1);
}

static void
pointer_moved(const GR_EVENT_MOUSE *m)
{
	int row, col;

	if (bar_held) {
		set_view(back_count - ui_scroll_to(m->y, 0, win_h, alt ? rows : back_count + rows, rows));
		return;
	}
	pointer_cell(m->x, m->y, &row, &col);
	if (mouse_held) {
		if (mouse_mode >= 1002)
			mouse_report(m->buttons & GR_BUTTON_L ? 0 : m->buttons & GR_BUTTON_M ? 1 : 2, row, col, 0, 1, m->modifiers);
		return;
	}
	if (!sel_held)
		return;
	/* past the top or the bottom it looks further, a line a move */
	if (m->y < PAD && view < back_count && !alt)
		set_view(view + 1);
	else if (m->y >= PAD + rows * chh && view > 0)
		set_view(view - 1);
	if (view_line(row) != sb_line || col != sb_col || !sel_on) {
		sb_line = view_line(row), sb_col = col;
		sel_on = sel_on || sb_line != sa_line || sb_col != sa_col;
		if (sel_on)
			sel_extend();
	}
}

static void
button_up(const GR_EVENT_BUTTON *b)
{
	int row, col;

	if (b->changebuttons & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN))
		return;
	if (mouse_held) {
		pointer_cell(b->x, b->y, &row, &col);
		mouse_report(0, row, col, 1, 0, b->modifiers);
	}
	if (sel_held && sel_on)
		sel_copy();	/* (what is selected is on the clipboard at once, as the old terminal had it) */
	if (!(b->buttons & (GR_BUTTON_L | GR_BUTTON_M | GR_BUTTON_R))) {
		sel_held = bar_held = mouse_held = 0;
		ui_moves(win, EVENTS, 0);
	}
}

/* ---------------- the program's life */

static void
finish(void)
{
	GrClose();
	exit(0);
}

/* For a test: what the screen shows, a line a row after "nxterm|", and where the cursor is. */
static void
dump(void)
{
	char line[COLS_MOST * 3 + 1];
	int r, c, n, end;

	fprintf(stderr, "nxterm: %d x %d, cursor at %d, %d%s%s%s, font %d (%d x %d)%s, %d lines kept\n", cols, rows, cx, cy,
		cursor_shown ? "" : " hidden", focused ? "" : " unfocused", blink_on ? "" : " blinked off",
		font_size, cw, chh, alt ? ", alternate screen" : "", back_count);
	for (r = 0; r < rows; r++) {
		for (end = cols - 1; end >= 0 && scr[r].c[end].ch == ' '; end--)
			;
		for (c = n = 0; c <= end; c++)
			n += utf8(scr[r].c[c].ch, line + n);
		line[n] = 0;
		fprintf(stderr, "nxterm|%s\n", line);
	}
}

static void
on_signal(int sig)
{
	int e = errno;
	char c = sig == SIGCHLD ? 'c' : 'd';

	if (write(wake[1], &c, 1) < 0)
		errno = e;
	errno = e;
}

static void
woken(void)
{
	char got[16];
	int n, i, status;
	pid_t p;

	n = (int)read(wake[0], got, sizeof got);
	for (i = 0; i < n; i++)
		if (got[i] == 'd') {
			dump();
		} else {
			while ((p = waitpid(-1, &status, WNOHANG)) > 0)
				if (p == shell) {
					pty_read();	/* (what it wrote last is shown, then the window goes) */
					finish();
				}
		}
}

static void event(GR_EVENT *ev);

static void
meanwhile(GR_EVENT *ev)
{
	if (ev->type == GR_EVENT_TYPE_FDINPUT || ev->type == GR_EVENT_TYPE_EXPOSURE || ev->type == GR_EVENT_TYPE_UPDATE) {
		event(ev);
		flush();
	}
}

/* The window's close box: asked first while a program other than the shell runs in it. */
static void
close_asked(void)
{
	static const char *const answers[] = { "Close", "Cancel" };
	pid_t running = tcgetpgrp(master);

	if (running > 0 && running != shell) {
		ui_ask_icon = "warning";
		if (ui_ask(TITLE, "A program is still running in this terminal.\nClose it, and end the program?", answers, 2, meanwhile) != 0) {
			GrSetFocus(win);
			return;
		}
	}
	finish();
}

static void
event(GR_EVENT *ev)
{
	int id;

	ui_origin_note(ev);
	switch (ui_menu_event(ev, &id)) {
	case UI_MENU_CHOSEN:
		GrSetFocus(win);
		command(id);
		return;
	case UI_MENU_CLOSED:
		GrSetFocus(win);
		/* fall through */
	case UI_MENU_TOOK:
		return;
	}
	switch (ev->type) {
	case GR_EVENT_TYPE_EXPOSURE:
		if (ev->exposure.wid != win)
			break;
		if (ui_restyle())
			ui_menu_restyle();
		exposed = 1;
		paint_frame();
		break;
	case GR_EVENT_TYPE_UPDATE:
		if (ev->update.wid == win && ev->update.utype == GR_UPDATE_SIZE && ev->update.width && ev->update.height) {
			win_w = ev->update.width, win_h = ev->update.height;
			layout();
		}
		break;
	case GR_EVENT_TYPE_FOCUS_IN:
		if (ev->general.wid == win)
			focused = 1, activity();
		break;
	case GR_EVENT_TYPE_FOCUS_OUT:
		if (ev->general.wid == win)
			focused = 0;
		break;
	case GR_EVENT_TYPE_KEY_DOWN:
		key(&ev->keystroke);
		break;
	case GR_EVENT_TYPE_BUTTON_DOWN:
		button_down(&ev->button);
		break;
	case GR_EVENT_TYPE_MOUSE_MOTION:
		pointer_moved(&ev->mouse);
		break;
	case GR_EVENT_TYPE_BUTTON_UP:
		button_up(&ev->button);
		break;
	case GR_EVENT_TYPE_FDINPUT:
		if (ev->fdinput.fd == master)
			pty_read();
		else
			woken();
		break;
	case GR_EVENT_TYPE_TIMEOUT:
		blink_on = !blink_on;
		break;
	case GR_EVENT_TYPE_CLOSE_REQ:
		if (ev->general.wid == win)
			close_asked();
		break;
	}
}

/* The pseudo-terminal, and the shell (or the command) on its other side. */
static int
start(char **program, const char *line)
{
	static char *args[4];
	char name[64], *path;
	const char *sh = getenv("SHELL");
	sigset_t none;
	int fd, i;

	master = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (master < 0)
		master = open("/dev/pts/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (master < 0 || unlockpt(master) != 0 || ptsname_r(master, name, sizeof name) != 0)
		return -1;
	tell_size();
	if (!sh || access(sh, X_OK) != 0)
		sh = "/bin/sh";
	if (program) {
		path = program[0];
	} else if (line && access(line, X_OK) == 0 && !strchr(line, ' ')) {
		path = args[0] = (char *)line, program = args;
	} else if (line) {
		path = "/bin/sh", args[0] = "sh", args[1] = "-c", args[2] = (char *)line, program = args;
	} else {
		path = args[0] = (char *)sh, program = args;
	}
	setenv("TERM", "xterm-256color", 1);
	unsetenv("COLUMNS");
	unsetenv("LINES");
	sigemptyset(&none);
	shell = real_vfork();
	if (shell == 0) {
		/* (the child is in our memory until its exec: system calls only) */
		setsid();
		if ((fd = open(name, O_RDWR)) < 0)
			_exit(126);
		ioctl(fd, TIOCSCTTY, 0);
		dup2(fd, 0), dup2(fd, 1), dup2(fd, 2);
		for (i = 3; i < 64; i++)
			close(i);
		signal(SIGCHLD, SIG_DFL), signal(SIGINT, SIG_DFL), signal(SIGQUIT, SIG_DFL), signal(SIGPIPE, SIG_DFL);
		signal(SIGHUP, SIG_DFL), signal(SIGTERM, SIG_DFL), signal(SIGTSTP, SIG_DFL), signal(SIGTTIN, SIG_DFL);
		signal(SIGTTOU, SIG_DFL);
		sigprocmask(SIG_SETMASK, &none, NULL);
		execvp(path, program);
		_exit(127);
	}
	return shell < 0 ? -1 : 0;
}

int
main(int argc, char **argv)
{
	static GR_BITMAP beam_fg[7], beam_bg[7];
	struct sigaction action;
	const char *line = NULL;
	char **program = NULL;
	GR_EVENT ev;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-e") && i + 1 < argc) {
			program = argv + i + 1;
			break;
		}
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			fprintf(stderr, "usage: nxterm [COMMAND] | nxterm -e PROGRAM [ARGUMENT ...]\n");
			return 2;
		}
		if (argv[i][0] != '-') {
			line = argv[i];
			break;
		}
	}
	if (GrOpen() < 0)
		return 1;
	ui_init();
	tgc = GrNewGC();
	GrSetGCUseBackground(tgc, GR_FALSE);
	light = !strcmp(reg_get("terminal.colours", "dark"), "light");
	set_scheme();
	set_font(atoi(reg_get("terminal.size", "13")));
	modes_default();
	for (i = 0; i < COLS_MOST; i++)
		tabs[i] = i % 8 == 0;
	saved[0].pen = saved[1].pen = pen;
	cols = fit_cols(cols), rows = fit_rows(rows);
	if (!ensure(cols, rows))
		return 1;
	win_w = cols * cw + 2 * PAD + UI_SCROLL_W, win_h = rows * chh + 2 * PAD;
	win = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, TITLE, GR_ROOT_WINDOW_ID, -1, -1, win_w, win_h, default_bg());
	GrSelectEvents(win, EVENTS);
	/* an I-beam for the pointer over the text */
	for (i = 0; i < 7; i++) {
		beam_fg[i] = (GR_BITMAP)((i == 0 || i == 6 ? 0x14 : 0x08) << 9);
		beam_bg[i] = (GR_BITMAP)((i == 0 || i == 6 ? 0x3e : 0x1c) << 9);
	}
	GrSetWindowCursor(win, GrNewCursor(7, 7, 3, 3, WHITE, BLACK, beam_fg, beam_bg));
	if (pipe2(wake, O_CLOEXEC | O_NONBLOCK) != 0)
		return 1;
	memset(&action, 0, sizeof action);
	action.sa_handler = on_signal;
	action.sa_flags = SA_RESTART;
	sigemptyset(&action.sa_mask);
	sigaction(SIGCHLD, &action, NULL);
	sigaction(SIGUSR1, &action, NULL);
	if (start(program, line) < 0) {
		fprintf(stderr, "nxterm: no pseudo-terminal for the shell\n");
		return 1;
	}
	GrRegisterInput(master);
	GrRegisterInput(wake[0]);
	GrMapWindow(win);
	GrSetFocus(win);
	activity();
	for (;;) {
		int blinking;

		flush();
		blinking = focused && cursor_blinks && cursor_shown && !view && ms() - last_active < BLINK_FOR;
		if (!blinking && !blink_on) {
			blink_on = 1;
			flush();
		}
		if (blinking)
			GrGetNextEventTimeout(&ev, BLINK_MS);
		else
			GrGetNextEvent(&ev);
		event(&ev);
	}
}
