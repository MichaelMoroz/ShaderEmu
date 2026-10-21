/* nxide: a small place to write C, JavaScript, Python and HLSL in, build it and run it (docs/apps/ide.md):
 * the text coloured by a lexer (syntax.h), every change kept (text.h), the machine's own compiler (or
 * qjsc, MicroPython, hlslc) with its complaints marked at their lines, and the program run below or
 * under the debugger (debug.h; ide-pydbg.py for Python; a shader runs in hlslview). */
#define _GNU_SOURCE
#include "ui.h"
#include "ui_files.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include "ui_glyph.h"
#include "registry.h"
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "text.h"
#include "syntax.h"
#include "debug.h"

#define MENU_H 20
#define TOOL_H 28
#define STATUS 18
#define TAB_H 18
#define PANEL_H 140
#define DOC_TAB_H 22		/* the row of the open files' tabs */
#define TREE_W 168		/* the folder's tree at the left */
#define DOCS 12
#define ED_GUTTER (ui_seven ? MWRGB(244, 244, 244) : MWRGB(232, 232, 232))
#define OUT_MOST 600
/* what MicroPython is told to look at a file with (its name is the next word) */
#define PY_COMPILE "import sys;compile(open(sys.argv[1]).read(),sys.argv[1],'exec')"
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_BUTTON_DOWN | \
	GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_TIMEOUT)

enum {
	C_NONE, C_NEW, C_OPEN, C_SAVE, C_SAVE_AS, C_EXIT, C_UNDO, C_REDO, C_CUT, C_COPY, C_PASTE, C_DELETE, C_FIND, C_FIND_NEXT, C_GOTO,
	C_ALL, C_LINE, C_WORD, C_INDENT, C_UNINDENT, C_COMMENT, C_PANEL, C_NUMBERS, C_CHECKING, C_BUILD, C_RUN, C_DEBUG, C_STOP,
	C_OVER, C_INTO, C_OUT, C_MARK, C_UNMARK_ALL, C_KEYS, C_ABOUT, C_CLOSE, C_NEXT_TAB, C_PREV_TAB, C_TREE, C_TREE_HERE,
	C_REPLACE, C_FIND_PREV, C_SAVE_ALL, C_PROJECT_OPEN, C_PROJECT_CLOSE,
	C_EXAMPLE	/* C_EXAMPLE + n: the nth example */
};
enum { P_OUTPUT, P_PROBLEMS, P_VARIABLES, P_STACK, PANELS };
/* J_FLAT: the ELF a build linked made a flat program; J_PYDBG: a Python program under ide-pydbg.py */
enum { J_NONE, J_BUILD, J_RUN, J_CHECK, J_FLAT, J_PYDBG };
enum { ASK_NONE, ASK_FIND, ASK_GOTO, ASK_SAVE, ASK_REPLACE, ASK_WITH };

static GR_WINDOW_ID window;
static GR_GC_ID text_gc, ground_gc, bar_gc;	/* the editor's text, its white ground, the cursor's bar */
static int width = 780, height = 520;
static char path[300], program[320], notice[160];
static int top, left, rows, columns, gutter;	/* gutter: where the gutter ends, from the window's left */
static int ex, tree_on;	/* ex: where the editor begins, right of the tree; the tree is shown */
static int tree_wanted = 1;	/* Ctrl+B: whether a project's tree is shown */
static char project[128];	/* the project's folder, whose tree is at the left; none: no tree */
static int numbers_on = 1, panel_on = 1, panel = P_OUTPUT, checking_on = 1;
static int dragging, selecting, leaving;
static unsigned click_at;
static int click_x, click_y;

/* the lines' colours: whether each begins inside a comment, known up to sx_known */
static signed char *sx_starts;
static int sx_known, sx_room;
static unsigned char *sx_kinds;
static int sx_kinds_room;
static const GR_COLOR sx_colours[SX_KINDS] = { MWRGB(0, 0, 0), MWRGB(0, 0, 200), MWRGB(0, 110, 130), MWRGB(130, 0, 130),
					      MWRGB(160, 40, 20), MWRGB(0, 110, 60), MWRGB(0, 128, 0), MWRGB(110, 80, 0) };

/* the status line when it asks for something */
static int asking;
static char answer[200], sought[64], replacement[64];

/* what runs beside: a build, the program, or a quiet check of what is being typed */
static struct { pid_t pid; int fd, kind, heard, lang; } job;	/* heard: its output wakes the loop (GrRegisterInput) */
static int job_unseen;		/* its end was not seen (no status): nothing is concluded from it */
static int then_do;		/* what a build that works goes on to: C_RUN or C_DEBUG */
static char job_text[8192];	/* a build's or a check's own lines, for the problems */
static int job_length;
static int debug_fd = -1;	/* the debugged program's output */
static int debug_heard;		/* and whether it wakes the loop */
static unsigned edited_at;	/* when the text last changed, and whether it was checked since */
static int unchecked;
static int edited_now;		/* it changed since the loop last slept: edited_at is still to be read */
/* texts the compiler found nothing in: their two sums. The image's examples are there from the
 * build (linux/ide/build.sh writes /usr/share/ide-clean), the others from this run's checks. */
#define CLEAN_MOST 96
static unsigned clean[CLEAN_MOST][2], check_sum[2];
static int clean_count, clean_next, check_closed;	/* check_closed: the text checked names no file of the user's */

static char *out_lines[OUT_MOST];
static int out_count, out_top, out_fresh, worker_marked;
static char out_part[200];
static struct problem { int line, error; char text[150]; } problems[64];
static int problem_count, problem_top;
/* The open files: the one shown lives in the globals (text.h's, path, the problems, the
 * marks), the others here until they are shown again (doc_keep, doc_take). */
struct doc {
	char path[300];
	char **lines;
	int count, room, x, y, ax, ay, marked, modified, top, left;
	struct tx_change *done, *undone;
	int done_count, undone_count, done_room, undone_room;
	struct problem *problems;
	int problem_count, marks[256], mark_count;
};
static struct doc docs[DOCS];
static int doc_count, doc_now = -1, run_doc = -1;	/* run_doc: the file whose build, run or debugging it is */
static int flat_machine;	/* no MMU: a program is a flat file, made from an ELF the debugger reads */
static void doc_show(int i);
static char variables[48][160], stack[16][160];
static int variable_count, variable_top, stack_count;

static char examples[12][40], example_ends[12][6], example_labels[12][56];	/* a name, its ending, its menu's line */
static int example_count;
/* the Python debugger: its state as debug.h's, the line it stopped at, and its two pipes */
static int py_state = DBG_GONE, py_line, py_tell = -1, py_hear = -1, py_heard;
static char py_part[200];
static int print_on;		/* NXIDE_PRINT: what happens, on the console, for a test */
static char script[2000];	/* NXIDE_SCRIPT: commands done one after another, when nothing runs */
static int scripted_run;	/* there was a script: nothing asks in a window, even after its last word */

static void command(int id);
static void draw_all(void);
static void py_end(void);

static unsigned
milliseconds(void)
{
	struct timeval now;

	gettimeofday(&now, NULL);
	return (unsigned)(now.tv_sec * 1000 + now.tv_usec / 1000);
}

static void
say(const char *text)
{
	snprintf(notice, sizeof notice, "%s", text);
	if (print_on) {
		printf("nxide: %s\n", text);
		fflush(stdout);
	}
}

/* ---- the layout ---- */

static int
side_y(void)
{
	return MENU_H + TOOL_H;
}

static int
editor_y(void)
{
	return MENU_H + TOOL_H + DOC_TAB_H;
}

/* The program's stop is on this line of the file shown. */
static int
stop_on(int line)
{
	return doc_now == run_doc && ((dbg_state == DBG_STOPPED && dbg_line_now == line + 1) || (py_state == DBG_STOPPED && py_line == line + 1));
}

/* A program is held at a line, by either debugger. */
static int
held(void)
{
	return dbg_state == DBG_STOPPED || py_state == DBG_STOPPED;
}

/* The language of a file, by its ending. */
static int
lang_of(const char *file)
{
	const char *dot = strrchr(file, '.');

	return !dot || strchr(dot, '/') ? SX_C : !strcmp(dot, ".js") || !strcmp(dot, ".mjs") ? SX_JS : !strcmp(dot, ".py") ? SX_PY :
	       !strcmp(dot, ".hlsl") || !strcmp(dot, ".hlsli") || !strcmp(dot, ".fx") ? SX_HLSL : SX_C;
}

/* A text cut to `room` pixels of the look's font, with "..." at its end when it was cut. */
static const char *
fitted(const char *text, int room)
{
	static char out[2][160];
	static int which;
	int n = (int)strlen(text), dots = ui_width(&ui_var_widths, ui_var, "...", 3);
	char *to = out[which ^= 1];

	if (ui_width(&ui_var_widths, ui_var, text, n) <= room)
		return text;
	for (n = n < 150 ? n : 150; n > 0 && ui_width(&ui_var_widths, ui_var, text, n) + dots > room; n--)
		continue;
	snprintf(to, sizeof out[0], "%.*s...", n, text);
	return n || dots <= room ? to : "";
}

static int
panel_y(void)
{
	return height - STATUS - (panel_on ? PANEL_H : 0);
}

static void
resized(int w, int h)
{
	width = w, height = h;
	tree_on = project[0] && tree_wanted;	/* (a tree is a project's, or there is none) */
	ex = tree_on ? TREE_W + 3 : 0;
	gutter = ex + (numbers_on ? 5 * ui_fw + 16 : 16);
	rows = (panel_y() - editor_y()) / ui_fh;
	columns = (width - UI_SCROLL_W - gutter - 4) / ui_fw;
	if (rows < 1)
		rows = 1;
	if (columns < 8)
		columns = 8;
}

/* ---- colours: a line's kinds need to know whether it begins inside a comment ---- */

/* A line as it is drawn, kept until it changes: a piece a colour (its kind, its first column
 * in view and its length, two bytes each, then its characters), from the colour's first
 * character to its last with spaces where the others' are (a space draws nothing). */
#define SHOWN_MOST 512		/* columns of a line that are drawn */
#define PIECE_HEAD 5
struct shown {
	unsigned short room, bytes;	/* of data, and how much of it the pieces are */
	signed char starts, ends;	/* inside a comment where the line begins, and where it ends */
	unsigned char valid;
	char data[];
};
static struct shown **shown;		/* by line; those from shown_upto on are not the lines' any more */
static int shown_room, shown_upto, shown_left = -1, shown_columns;
static union { struct shown head; char room[sizeof(struct shown) + (SX_KINDS + 1) * (PIECE_HEAD + SHOWN_MOST)]; } made, picked;
#define ROWS_KEPT 128
static int row_line[ROWS_KEPT];	/* the line a row shows just as it is kept (no selection, no stop), or -1 */
static int bar_line = -1, bar_x;	/* where the cursor's bar is drawn: a line, a column of the view */

static int
piece_first(const char *piece)
{
	return (unsigned char)piece[1] | (unsigned char)piece[2] << 8;
}

static int
piece_count(const char *piece)
{
	return (unsigned char)piece[3] | (unsigned char)piece[4] << 8;
}

/* A line's kinds, in sx_kinds; what comes back is whether the next line begins inside a comment. */
static int
kinds_of(int line, int in_comment)
{
	int n = (int)strlen(tx_lines[line]) + 4;

	if (n > sx_kinds_room)
		sx_kinds = realloc(sx_kinds, (size_t)(sx_kinds_room = n * 2));
	return sx_line(tx_lines[line], in_comment, sx_kinds);
}

/* The kept line, if it is still what the line is. */
static struct shown *
shown_kept(int line)
{
	struct shown *e = line < shown_upto ? shown[line] : NULL;

	return e && e->valid && e->starts == sx_starts[line] ? e : NULL;
}

/* Makes sx_starts known up to `line`: a kept line says how it ends, another is read for it. */
static void
starts_upto(int line)
{
	if (tx_count + 2 > sx_room) {
		sx_room = tx_count * 2 + 64;
		sx_starts = realloc(sx_starts, (size_t)sx_room);
	}
	if (shown_left != left || shown_columns != columns)
		shown_upto = 0, shown_left = left, shown_columns = columns;
	if (sx_known < 1)
		sx_starts[0] = 0, sx_known = 1;
	for (; sx_known <= line; sx_known++) {
		struct shown *e = shown_kept(sx_known - 1);

		sx_starts[sx_known] = (signed char)(e ? e->ends : kinds_of(sx_known - 1, sx_starts[sx_known - 1]));
	}
}

/* Makes `to` of a line whose kinds are in sx_kinds; the view's columns `from` up to `upto` are selected. */
static void
shown_make(struct shown *to, int line, int from, int upto)
{
	static char pieces[SX_KINDS + 1][SHOWN_MOST];
	short first[SX_KINDS + 1], last[SX_KINDS + 1];
	const char *text = tx_lines[line];
	int length = (int)strlen(text), end = length < left + columns ? length : left + columns, a, b, k, n = 0;

	end = end > left + SHOWN_MOST ? left + SHOWN_MOST : end;
	for (b = 0; b <= SX_KINDS; b++)
		first[b] = -1;
	for (a = left; a < end; a++) {
		if (text[a] == ' ')
			continue;
		b = a - left >= from && a - left < upto ? SX_KINDS : sx_kinds[a];
		if (first[b] < 0)
			first[b] = (short)a;
		else
			for (k = last[b] + 1; k < a; k++)
				pieces[b][k - first[b]] = ' ';
		pieces[b][a - first[b]] = text[a];
		last[b] = (short)a;
	}
	for (b = 0; b <= SX_KINDS; b++) {
		if (first[b] < 0)
			continue;
		k = last[b] - first[b] + 1;
		to->data[n] = (char)b;
		to->data[n + 1] = (char)(first[b] - left), to->data[n + 2] = (char)((first[b] - left) >> 8);
		to->data[n + 3] = (char)k, to->data[n + 4] = (char)(k >> 8);
		memcpy(to->data + n + PIECE_HEAD, pieces[b], (size_t)k);
		n += PIECE_HEAD + k;
	}
	to->bytes = (unsigned short)n;
}

/* The line read anew into `made`: its colours, and how it ends. */
static struct shown *
shown_read(int line)
{
	starts_upto(line);
	made.head.starts = sx_starts[line];
	made.head.ends = (signed char)kinds_of(line, sx_starts[line]);
	shown_make(&made.head, line, 0, 0);
	if (sx_known == line + 1)
		sx_starts[sx_known++] = made.head.ends;	/* (the next line's beginning, for when it is asked for) */
	return &made.head;
}

/* Keeps `made` as the line's. */
static struct shown *
shown_keep(int line)
{
	struct shown *e;
	int i;

	if (line >= shown_room) {
		int room = line * 2 + 64;

		shown = realloc(shown, (size_t)room * sizeof shown[0]);
		memset(shown + shown_room, 0, (size_t)(room - shown_room) * sizeof shown[0]);
		shown_room = room;
	}
	for (i = shown_upto; i < line; i++)
		if (shown[i])
			shown[i]->valid = 0;
	if (line >= shown_upto)
		shown_upto = line + 1;
	e = shown[line];
	if (!e || e->room < made.head.bytes) {
		e = shown[line] = realloc(e, sizeof *e + made.head.bytes + 24u);
		e->room = (unsigned short)(made.head.bytes + 24);
	}
	e->bytes = made.head.bytes, e->starts = made.head.starts, e->ends = made.head.ends, e->valid = 1;
	memcpy(e->data, made.head.data, made.head.bytes);
	return e;
}

/* A line's pieces as they are drawn with nothing selected: kept, or read now and kept. */
static struct shown *
shown_of(int line)
{
	struct shown *e;

	starts_upto(line);
	if ((e = shown_kept(line)) != NULL)
		return e;
	shown_read(line);
	return shown_keep(line);
}

/* The first column of the view at which two lines' pieces look different, or 1 << 20. */
static int
shown_differ(const struct shown *a, const struct shown *b)
{
	const char *of[2][SX_KINDS + 1] = { { NULL } };
	int kind, at = 1 << 20, n, i;

	for (n = 0; n < a->bytes; n += PIECE_HEAD + piece_count(a->data + n))
		of[0][(unsigned char)a->data[n]] = a->data + n;
	for (n = 0; n < b->bytes; n += PIECE_HEAD + piece_count(b->data + n))
		of[1][(unsigned char)b->data[n]] = b->data + n;
	for (kind = 0; kind <= SX_KINDS; kind++) {
		const char *p = of[0][kind], *q = of[1][kind];
		int here = 1 << 20, least;

		if (!p && !q)
			continue;
		if (!p || !q)
			here = piece_first(p ? p : q);
		else if (piece_first(p) != piece_first(q))
			here = piece_first(p) < piece_first(q) ? piece_first(p) : piece_first(q);
		else {
			if (piece_count(p) < piece_count(q))
				p = of[1][kind], q = of[0][kind];	/* (p: the longer) */
			least = piece_count(q);
			for (i = 0; i < least && p[PIECE_HEAD + i] == q[PIECE_HEAD + i]; i++)
				continue;
			/* past the shorter one's end the longer differs at its next character that is not a space */
			for (n = piece_count(p); i >= least && i < n && p[PIECE_HEAD + i] == ' '; i++)
				continue;
			if (i < n)
				here = piece_first(p) + i;
		}
		at = here < at ? here : at;
	}
	return at;
}

/* ---- drawing ---- */

static int
problem_at(int line)
{
	int i, found = 0;

	for (i = 0; i < problem_count; i++)
		if (problems[i].line == line + 1)
			found |= problems[i].error ? 2 : 1;
	return found;
}

/* Text of the editor's font, through a context of its own: the other one keeps the panels'. */
static void
ink(GR_COLOR colour)
{
	static GR_COLOR now;
	static int known;

	if (!known || now != colour)
		(GrSetGCForeground)(text_gc, colour);
	now = colour, known = 1;
}

static void
write_at(int x, int y, const char *text, int count)
{
	GrText(window, text_gc, x, y, (void *)text, count, GR_TFASCII | GR_TFTOP);
}

/* A line's number at the right of four places, or of as many as it has. */
static void
draw_number(int line, int y)
{
	char number[12];
	int n = line + 1, at = sizeof number - 1;

	for (number[at] = 0; n || at == (int)sizeof number - 1; n /= 10)
		number[--at] = (char)('0' + n % 10);
	while (at > (int)sizeof number - 5)
		number[--at] = ' ';
	ink(UI_SHADOW);
	write_at(ex + 14, y, number + at, (int)sizeof number - 1 - at);
}

/* A line's pieces of one kind (all of them: -1), from column `from` of the view on. */
static void
draw_pieces(const struct shown *e, int y, int kind, int from)
{
	int n, x = gutter + 3;

	for (n = 0; n < e->bytes; n += PIECE_HEAD + piece_count(e->data + n)) {
		const char *piece = e->data + n;
		int first = piece_first(piece), count = piece_count(piece), skip = 0;

		if ((kind >= 0 && piece[0] != kind) || first + count <= from)
			continue;
		if (first < from)
			for (skip = from - first; piece[PIECE_HEAD + skip] == ' '; skip++)
				continue;
		ink(piece[0] == SX_KINDS ? WHITE : sx_colours[(unsigned char)piece[0]]);
		write_at(x + (first + skip) * ui_fw, y, piece + PIECE_HEAD + skip, count - skip);
	}
}

/* The cursor's bar, if it is on this line. */
static void
draw_bar(int line, int y)
{
	if (bar_line == line)
		bar_line = -1;
	if (line != tx_y || tx_x < left || tx_x > left + columns)
		return;
	GrFillRect(window, bar_gc, gutter + 3 + (tx_x - left) * ui_fw, y, 2, ui_fh);
	bar_line = line, bar_x = tx_x - left;
}

/* ---- the bracket at the cursor and its pair, boxed ---- */

static int match_line[2] = { -1, -1 }, match_col[2];

static int
in_words(int column)
{
	return sx_kinds[column] != SX_STRING && sx_kinds[column] != SX_COMMENT;
}

/* The bracket just after the cursor (or else just before it) and the one that pairs with it,
 * strings and comments left out; lines -1 when there is none. */
static void
match_find(int lines[2], int cols[2])
{
	static const char opens[] = "([{", closes[] = ")]}";
	const char *text = tx_lines[tx_y], *o;
	int x = tx_x, dir, depth = 1, y, c, steps = 0;
	char mine, want, ch;

	lines[0] = lines[1] = -1;
	if (!text[x] || !strchr("()[]{}", text[x])) {
		if (x > 0 && strchr("()[]{}", text[x - 1]))
			x--;
		else
			return;
	}
	mine = text[x];
	if ((o = strchr(opens, mine)) != NULL)
		dir = 1, want = closes[o - opens];
	else
		dir = -1, want = opens[strchr(closes, mine) - closes];
	starts_upto(tx_y);
	kinds_of(tx_y, sx_starts[tx_y]);
	if (!in_words(x))
		return;
	for (y = tx_y, c = x; ; ) {
		c += dir;
		while (c < 0 || c >= (int)strlen(tx_lines[y])) {
			y += dir;
			if (y < 0 || y >= tx_count || abs(y - tx_y) > 400)
				return;
			starts_upto(y);
			kinds_of(y, sx_starts[y]);
			c = dir > 0 ? 0 : (int)strlen(tx_lines[y]) - 1;
		}
		ch = tx_lines[y][c];
		if (++steps > 20000)
			return;
		if (!in_words(c))
			continue;
		if (ch == mine)
			depth++;
		else if (ch == want && !--depth)
			break;
	}
	lines[0] = tx_y, cols[0] = x, lines[1] = y, cols[1] = c;
}

static void
match_box(int k)
{
	int line = match_line[k], c = match_col[k];

	if (line < top || line >= top + rows || c < left || c >= left + columns)
		return;
	GrSetGCForeground(ui_gc, MWRGB(140, 140, 140));
	GrRect(window, ui_gc, gutter + 2 + (c - left) * ui_fw, editor_y() + (line - top) * ui_fh, ui_fw + 2, ui_fh);
}

static void
match_boxes_on(int line)
{
	int k;

	for (k = 0; k < 2; k++)
		if (match_line[k] == line && line >= 0)
			match_box(k);
}

static void draw_line(int line);

/* After the cursor moved or the text changed: the old boxes taken away, the new ones drawn. */
static void
match_update(void)
{
	int lines[2], cols[2] = { 0, 0 }, old_lines[2] = { match_line[0], match_line[1] }, old_cols[2] = { match_col[0], match_col[1] }, k, j, kept;

	match_find(lines, cols);
	for (k = 0; k < 2; k++)
		match_line[k] = lines[k], match_col[k] = cols[k];
	for (k = 0; k < 2; k++) {
		if (old_lines[k] < 0 || old_lines[k] >= tx_count)
			continue;
		for (j = kept = 0; j < 2; j++)
			kept |= lines[j] == old_lines[k] && cols[j] == old_cols[k];
		if (!kept)
			draw_line(old_lines[k]);
	}
	for (k = 0; k < 2; k++)
		if (match_line[k] >= 0)
			match_box(k);
}

/* True, with the view's columns, if a line has selected columns in view. */
static int
selected_in(int line, int *from, int *to)
{
	int x0, y0, x1, y1;

	if (!tx_selection(&x0, &y0, &x1, &y1) || line < y0 || line > y1)
		return 0;
	/* the selected columns of this line; a line's end counts as one more */
	*from = (line == y0 ? x0 : 0) - left;
	*to = (line == y1 ? x1 : (int)strlen(tx_lines[line]) + 1) - left;
	*from = *from < 0 ? 0 : *from > columns ? columns : *from;
	*to = *to < 0 ? 0 : *to > columns ? columns : *to;
	return *to > *from;
}

/* What stands before a line's text, but its number: its mark, the stop's sign, a problem's bar. */
static void
draw_signs(int line, int y, int here)
{
	int wrong;

	if (dbg_marked(line + 1)) {
		GrSetGCForeground(ui_gc, MWRGB(200, 20, 20));
		GrFillEllipse(window, ui_gc, ex + 7, y + ui_fh / 2, 4, 4);
	}
	if (here) {
		ink(BLACK);
		write_at(ex + 2, y, ">", 1);
	}
	if ((wrong = problem_at(line)) != 0)
		ui_fill(window, gutter - 3, y, 3, ui_fh, wrong & 2 ? MWRGB(220, 0, 0) : MWRGB(230, 150, 0));
}

/* A line of the text; `cleared`: its ground is drawn already (draw_editor_from). */
static void
draw_line_on(int line, int cleared)
{
	int y = editor_y() + (line - top) * ui_fh, from = 0, to = 0, picking;
	int here = stop_on(line);
	struct shown *e;

	if (line < top || line >= top + rows)
		return;
	if (line - top < ROWS_KEPT)
		row_line[line - top] = -1;
	if (!cleared)
		ui_fill(window, ex, y, gutter - ex, ui_fh, ED_GUTTER);
	if (!cleared || here)
		ui_fill(window, gutter, y, width - UI_SCROLL_W - gutter, ui_fh, here ? MWRGB(255, 255, 150) : WHITE);
	if (line >= tx_count) {
		draw_bar(line, y);
		return;
	}
	if (numbers_on)
		draw_number(line, y);
	draw_signs(line, y, here);
	picking = selected_in(line, &from, &to);
	if (picking) {
		ui_fill(window, gutter + 3 + from * ui_fw, y, (to - from) * ui_fw, ui_fh, UI_SELECTED);
		starts_upto(line);
		kinds_of(line, sx_starts[line]);
		shown_make(e = &picked.head, line, from, to);
	} else
		e = shown_of(line);
	draw_pieces(e, y, -1, 0);
	match_boxes_on(line);
	draw_bar(line, y);
	if (!picking && !here && line - top < ROWS_KEPT)
		row_line[line - top] = line;
}

static void
draw_line(int line)
{
	draw_line_on(line, 0);
}

/* A line's row from column `from` of the view on, and the cursor's bar taken from where it was
 * and put where it is: when the row shows the line as it is kept; the whole line otherwise. */
static void
draw_line_tail(int line, int from)
{
	int y = editor_y() + (line - top) * ui_fh, x, a, b;

	if (line < top || line >= top + rows)
		return;
	if (line < tx_count)
		starts_upto(line);
	if (line >= tx_count || line - top >= ROWS_KEPT || row_line[line - top] != line || !shown_kept(line) ||
	    selected_in(line, &a, &b) || stop_on(line)) {
		draw_line(line);
		return;
	}
	if (bar_line == line && (line != tx_y || bar_x != tx_x - left) && bar_x < from)
		from = bar_x;
	x = gutter + 3 + from * ui_fw;
	if (from < 1 << 20 && x < width - UI_SCROLL_W) {
		GrFillRect(window, ground_gc, x, y, width - UI_SCROLL_W - x, ui_fh);
		draw_pieces(shown_kept(line), y, -1, from);
	}
	match_boxes_on(line);
	draw_bar(line, y);
}

/* The text's scroll bar. The view may begin after the last whole page (Go to line near the
 * end, lines taken out): the thumb is then at the bar's end, where it ran past it and over
 * the panel and the status line. */
static void
draw_scroll_bar(void)
{
	ui_scrollbar(window, width - UI_SCROLL_W, editor_y(), panel_y() - editor_y(), tx_count, rows,
		     tx_count > rows && top > tx_count - rows ? tx_count - rows : top);
}

static void
draw_editor_from(int line)
{
	static const unsigned char order[SX_KINDS] = { SX_KEYWORD, SX_TYPE, SX_DIRECTIVE, SX_STRING, SX_NUMBER, SX_COMMENT, SX_CALL, SX_PLAIN };
	struct shown *plain[ROWS_KEPT];
	int y, i, k, a, b, first;

	/* the lines' ground in two pieces, not two a line */
	first = line = line < top ? top : line;
	y = editor_y() + (line - top) * ui_fh;
	if (line < top + rows) {
		ui_fill(window, ex, y, gutter - ex, (top + rows - line) * ui_fh, ED_GUTTER);
		ui_fill(window, gutter, y, width - UI_SCROLL_W - gutter, (top + rows - line) * ui_fh, WHITE);
	}
	/* a line with something selected or the stop on it is drawn by itself; the others together,
	 * a colour after a colour: a colour changed is a request too, and was three a line */
	for (; line < top + rows; line++) {
		i = line - top;
		if (i >= ROWS_KEPT || line >= tx_count || selected_in(line, &a, &b) || stop_on(line)) {
			draw_line_on(line, 1);
			continue;
		}
		plain[i] = shown_of(line);
		row_line[i] = line;
		if (bar_line == line)
			bar_line = -1;
	}
#define EACH_PLAIN for (line = first, y = editor_y() + (line - top) * ui_fh; line < top + rows; line++, y += ui_fh) \
	if (line - top < ROWS_KEPT && row_line[line - top] == line)
	if (numbers_on)
		EACH_PLAIN
			draw_number(line, y);
	EACH_PLAIN
		draw_signs(line, y, 0);
	for (k = 0; k < SX_KINDS; k++)
		EACH_PLAIN
			draw_pieces(plain[line - top], y, order[k], 0);
	EACH_PLAIN
		match_boxes_on(line);
	EACH_PLAIN
		draw_bar(line, y);
#undef EACH_PLAIN
	ui_fill(window, ex, editor_y() + rows * ui_fh, width - UI_SCROLL_W - ex, panel_y() - editor_y() - rows * ui_fh, WHITE);
	draw_scroll_bar();
}

static const char *const menu_names[] = { "File", "Edit", "Selection", "View", "Run", "Help" };
static int menu_x[7];

static void
draw_menu_bar(void)
{
	int i;

	ui_fill(window, 0, 0, width, MENU_H, UI_FACE);
	for (i = 0; i < 6; i++)
		ui_text(window, menu_x[i] + 8, 3, menu_names[i], -1, BLACK, 0);
	ui_fill(window, 0, MENU_H - 1, width, 1, UI_SHADOW);
}

static const struct { const char *label; int id, gap; } tools[] = {
	{ "New", C_NEW, 0 }, { "Open", C_OPEN, 0 }, { "Save", C_SAVE, 0 }, { "Undo", C_UNDO, 8 }, { "Redo", C_REDO, 0 },
	{ "Build", C_BUILD, 8 }, { "Run", C_RUN, 0 }, { "Debug", C_DEBUG, 0 }, { "Stop", C_STOP, 0 },
	{ "Over", C_OVER, 8 }, { "Into", C_INTO, 0 }, { "Out", C_OUT, 0 },
};
#define TOOLS ((int)(sizeof tools / sizeof tools[0]))
static int tool_x[TOOLS + 1], tool_w[TOOLS];
static int tools_shown = -1;	/* tools_now() as the row was last drawn */

/* Whether a command can be done as things stand. */
static int
can(int id)
{
	int idle = job.kind != J_BUILD && job.kind != J_RUN && job.kind != J_FLAT && job.kind != J_PYDBG && dbg_state == DBG_GONE;

	switch (id) {
	case C_UNDO: return tx_done_count > 0;
	case C_REDO: return tx_undone_count > 0;
	case C_BUILD: case C_RUN: return idle;
	case C_DEBUG: return (idle && sx_lang != SX_JS && sx_lang != SX_HLSL) || held();	/* (QuickJS cannot stop at a line, nor a lane) */
	case C_STOP: return !idle;
	case C_OVER: case C_INTO: case C_OUT: return held();
	case C_PROJECT_CLOSE: return project[0] != 0;
	}
	return 1;
}

/* The keys' looks as bits: which can be pressed, and whether Debug reads "Go on". */
static int
tools_now(void)
{
	int i, bits = held();

	for (i = 0; i < TOOLS; i++)
		bits = bits << 1 | can(tools[i].id);
	return bits;
}

/* A triangle pointing down, its widest row at y. */
static void
point_down(int cx, int y, int size)
{
	int k;

	for (k = 0; k <= size; k++)
		GrLine(window, ui_gc, cx - (size - k), y + k, cx + (size - k), y + k);
}

/* A key's sign, its middle at cx, cy: in colour, or grey when it cannot be pressed. */
static void
tool_sign(int id, int cx, int cy, int on)
{
	int k;

#define INK(colour) GrSetGCForeground(ui_gc, on ? (colour) : UI_SHADOW)
	switch (id) {
	case C_NEW:	/* a page with lines on it */
		ui_fill(window, cx - 4, cy - 6, 9, 13, on ? WHITE : UI_FACE);
		INK(MWRGB(90, 90, 90));
		GrRect(window, ui_gc, cx - 4, cy - 6, 9, 13);
		for (k = -2; k <= 4; k += 3)
			GrLine(window, ui_gc, cx - 2, cy + k, cx + 2, cy + k);
		break;
	case C_OPEN:
		ui_glyph(window, cx, cy, UI_G_FOLDER, on ? MWRGB(214, 160, 40) : UI_SHADOW);
		break;
	case C_SAVE:	/* a disk: its label and its shutter */
		INK(MWRGB(40, 80, 170));
		GrFillRect(window, ui_gc, cx - 5, cy - 5, 11, 11);
		GrSetGCForeground(ui_gc, on ? WHITE : UI_FACE);
		GrFillRect(window, ui_gc, cx - 3, cy - 5, 7, 4);
		GrFillRect(window, ui_gc, cx - 2, cy + 2, 5, 3);
		break;
	case C_UNDO:
	case C_REDO:
		ui_glyph(window, cx, cy, id == C_UNDO ? UI_G_TURN_LEFT : UI_G_TURN_RIGHT, on ? MWRGB(30, 90, 200) : UI_SHADOW);
		break;
	case C_BUILD:	/* bricks */
		INK(MWRGB(180, 80, 30));
		for (k = 0; k < 3; k++) {
			GrFillRect(window, ui_gc, cx - 6 + (k & 1) * 3, cy - 5 + k * 4, 5, 3);
			GrFillRect(window, ui_gc, cx + (k & 1) * 3, cy - 5 + k * 4, 5 - (k & 1) * 3, 3);
		}
		break;
	case C_RUN:
		ui_glyph(window, cx, cy, UI_G_PLAY, on ? MWRGB(0, 150, 40) : UI_SHADOW);
		break;
	case C_DEBUG:	/* a play sign with a bug at its foot, or a bar before it: go on */
		if (held()) {
			INK(MWRGB(0, 150, 40));
			GrFillRect(window, ui_gc, cx - 6, cy - 5, 2, 11);
			ui_glyph_point(window, cx - 2, cy, 5, 1);
			break;
		}
		ui_glyph(window, cx - 1, cy, UI_G_PLAY, on ? MWRGB(0, 150, 40) : UI_SHADOW);
		INK(MWRGB(200, 30, 30));
		GrFillEllipse(window, ui_gc, cx + 4, cy + 3, 2, 2);
		break;
	case C_STOP:
		ui_glyph(window, cx, cy, UI_G_STOP, on ? MWRGB(200, 30, 30) : UI_SHADOW);
		break;
	case C_OVER:	/* an arch over a dot */
		INK(MWRGB(30, 90, 200));
		GrLine(window, ui_gc, cx - 5, cy + 1, cx - 5, cy - 4);
		GrLine(window, ui_gc, cx - 5, cy - 4, cx + 4, cy - 4);
		GrLine(window, ui_gc, cx + 4, cy - 4, cx + 4, cy - 2);
		point_down(cx + 4, cy - 1, 3);
		GrFillRect(window, ui_gc, cx - 1, cy + 4, 3, 3);
		break;
	case C_INTO:	/* down onto a dot */
		INK(MWRGB(30, 90, 200));
		GrFillRect(window, ui_gc, cx - 1, cy - 6, 3, 4);
		point_down(cx, cy - 2, 3);
		GrFillRect(window, ui_gc, cx - 1, cy + 4, 3, 3);
		break;
	case C_OUT:	/* up from a dot */
		INK(MWRGB(30, 90, 200));
		ui_glyph_point_up(window, cx, cy - 2, 3);
		GrFillRect(window, ui_gc, cx - 1, cy - 2, 3, 4);
		GrFillRect(window, ui_gc, cx - 1, cy + 4, 3, 3);
		break;
	}
#undef INK
}

/* The row of keys; `all`: with its ground, else only the keys that look different now. */
static void
draw_tools_some(int all)
{
	int i, now = tools_now(), differ = all ? -1 : now ^ tools_shown;

	/* (a key's bit is TOOLS - 1 - i; the bit above them all says what Debug reads) */
	if (differ >> TOOLS & 1)
		differ = -1, all = 1;
	tools_shown = now;
	if (all)
		ui_fill(window, 0, MENU_H, width, TOOL_H, UI_FACE);
	for (i = 0; i < TOOLS; i++) {
		const char *label = tools[i].id == C_DEBUG && held() ? "Go on" : tools[i].label;

		if (!(differ >> (TOOLS - 1 - i) & 1))
			continue;
		if (!all)
			ui_fill(window, tool_x[i], MENU_H + 3, tool_w[i], 22, UI_FACE);
		ui_bevel(window, tool_x[i], MENU_H + 3, tool_w[i], 22, 0);
		tool_sign(tools[i].id, tool_x[i] + 12, MENU_H + 14, can(tools[i].id));
		ui_text(window, tool_x[i] + 22, MENU_H + 7, label, -1, can(tools[i].id) ? BLACK : UI_SHADOW, 0);
	}
	if (all)
		ui_fill(window, 0, MENU_H + TOOL_H - 1, width, 1, UI_SHADOW);
}

static void
draw_tools(void)
{
	draw_tools_some(1);
}

/* ---- the open files' tabs ---- */

static int doc_tab_x[DOCS], doc_tab_w[DOCS];
static int tabs_drawn = -1;	/* doc_now, doc_count and tx_modified as the row was last drawn */

static const char *
leaf_of(const char *file)
{
	const char *slash = strrchr(file, '/');

	return slash ? slash + 1 : file;
}

/* A tab: the chosen one white and joined to what is under it. */
static void
draw_tab(int x, int y, int w, int h, int chosen)
{
	if (chosen) {
		ui_fill(window, x, y, w, h, WHITE);
		GrSetGCForeground(ui_gc, UI_SHADOW);
		GrLine(window, ui_gc, x, y, x + w - 1, y);
		GrLine(window, ui_gc, x, y, x, y + h - 1);
		GrLine(window, ui_gc, x + w - 1, y, x + w - 1, y + h - 1);
	} else if (ui_seven)
		ui_button_seven(window, x, y, w, h, 0);
	else {
		ui_fill(window, x, y, w, h, UI_FACE);
		ui_bevel(window, x, y, w, h, 0);
	}
}

static void
draw_doc_tabs(void)
{
	int y = side_y(), x = ex + 2, i, w, most = doc_count ? (width - ex - 6) / doc_count - 2 : 0, chosen_x = 0, chosen_w = 0;

	most = most > 220 ? 220 : most;
	char label[80];

	tabs_drawn = doc_now | doc_count << 8 | tx_modified << 16;
	ui_fill(window, ex, y, width - ex, DOC_TAB_H, UI_FACE);
	for (i = 0; i < doc_count; i++) {
		int chosen = i == doc_now, ty = y + (chosen ? 2 : 4), th = DOC_TAB_H - (chosen ? 2 : 5);

		snprintf(label, sizeof label, "%.60s%s", leaf_of(chosen ? path : docs[i].path), (chosen ? tx_modified : docs[i].modified) ? " *" : "");
		w = ui_width(&ui_var_widths, ui_var, label, -1) + 32;
		w = w > most ? most : w;
		doc_tab_x[i] = x, doc_tab_w[i] = w;
		draw_tab(x, ty, w, th, chosen);
		/* a name too long for its tab ends in "...", and its close box stays clear */
		ui_text(window, x + 8, ty + (th - 12) / 2, fitted(label, w - 26), -1, BLACK, 0);
		/* its close box */
		GrSetGCForeground(ui_gc, chosen ? MWRGB(80, 80, 80) : UI_SHADOW);
		GrLine(window, ui_gc, x + w - 14, ty + th / 2 - 3, x + w - 8, ty + th / 2 + 3);
		GrLine(window, ui_gc, x + w - 14, ty + th / 2 + 3, x + w - 8, ty + th / 2 - 3);
		if (chosen)
			chosen_x = x, chosen_w = w;
		x += w + 2;
	}
	GrSetGCForeground(ui_gc, UI_SHADOW);
	if (chosen_w) {
		GrLine(window, ui_gc, ex, y + DOC_TAB_H - 1, chosen_x, y + DOC_TAB_H - 1);
		GrLine(window, ui_gc, chosen_x + chosen_w - 1, y + DOC_TAB_H - 1, width - 1, y + DOC_TAB_H - 1);
	} else
		GrLine(window, ui_gc, ex, y + DOC_TAB_H - 1, width - 1, y + DOC_TAB_H - 1);
}

/* The tab at x, or -1; *closing when x is on its close box. */
static int
doc_tab_at(int x, int *closing)
{
	int i;

	for (i = 0; i < doc_count; i++)
		if (x >= doc_tab_x[i] && x < doc_tab_x[i] + doc_tab_w[i]) {
			*closing = x >= doc_tab_x[i] + doc_tab_w[i] - 18;
			return i;
		}
	return -1;
}

/* ---- the folder's tree ---- */

#define TREE_ROWS 240
#define TREE_ROW 18
#define TREE_HEAD 22
static struct { char path[128]; unsigned char depth, folder, open; } tree[TREE_ROWS];
static int tree_count, tree_top;
static char tree_root[128] = "/root", tree_opened[24][128];
static int tree_opened_count;

static int
tree_is_open(const char *dir)
{
	int i;

	for (i = 0; i < tree_opened_count; i++)
		if (!strcmp(tree_opened[i], dir))
			return 1;
	return 0;
}

struct tree_entry { char name[64]; int folder; };

static int
tree_order(const void *a, const void *b)
{
	const struct tree_entry *p = a, *q = b;

	return p->folder != q->folder ? q->folder - p->folder : strcmp(p->name, q->name);
}

/* A folder's names in the tree, folders first, and an open folder's own under it. */
static void
tree_add(const char *dir, int depth)
{
	DIR *folder = opendir(dir);
	struct tree_entry *list = folder ? malloc(160 * sizeof *list) : NULL;
	struct dirent *entry;
	struct stat is;
	char full[300];
	int n = 0, i;

	while (list && n < 160 && (entry = readdir(folder)) != NULL) {
		if (entry->d_name[0] == '.' || strlen(entry->d_name) >= sizeof list[0].name)
			continue;
		snprintf(full, sizeof full, "%s/%s", dir, entry->d_name);
		list[n].folder = entry->d_type == DT_DIR ||
				 ((entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK) && !stat(full, &is) && S_ISDIR(is.st_mode));
		strcpy(list[n++].name, entry->d_name);
	}
	if (folder)
		closedir(folder);
	if (list)
		qsort(list, (size_t)n, sizeof *list, tree_order);
	for (i = 0; i < n && tree_count < TREE_ROWS; i++) {
		/* (a path too long for the tree is left out, not cut to another file's) */
		if (snprintf(tree[tree_count].path, sizeof tree[0].path, "%s%s%s", dir, dir[1] ? "/" : "", list[i].name) >= (int)sizeof tree[0].path)
			continue;
		tree[tree_count].depth = (unsigned char)depth;
		tree[tree_count].folder = (unsigned char)list[i].folder;
		tree[tree_count].open = list[i].folder && depth < 6 && tree_is_open(tree[tree_count].path);
		if (tree[tree_count++].open)
			tree_add(tree[tree_count - 1].path, depth + 1);
	}
	free(list);
}

static int
tree_shown(void)
{
	return (panel_y() - side_y() - TREE_HEAD - 6) / TREE_ROW;
}

static void
tree_read(void)
{
	tree_count = 0;
	if (!project[0])
		return;
	tree_add(tree_root, 0);
	if (tree_top > tree_count - tree_shown())
		tree_top = tree_count - tree_shown();
	if (tree_top < 0)
		tree_top = 0;
}

static void
draw_tree(void)
{
	static int cells[7] = { -2 }, cells_seven = -1;	/* (each look's sheets have an index of their own) */
	int y0 = side_y(), ly = y0 + TREE_HEAD, lh = panel_y() - ly - 2, shown = tree_shown(), bar = tree_count > shown ? UI_SCROLL_W : 0, i;

	if (!tree_on)
		return;
	if (cells[0] == -2 || cells_seven != ui_seven) {
		cells_seven = ui_seven;
		static const char *const names[7] = { "folder", "folder-open", "c-source", "c-header", "file", "script", "code" };

		for (i = 0; i < 7; i++)
			cells[i] = ui_icon_of(names[i]);
	}
	ui_fill(window, 0, y0, TREE_W + 3, panel_y() - y0, UI_FACE);
	ui_icon(window, ui_gc, 4, y0 + 3, cells[1], 16);
	ui_text(window, 24, y0 + 5, fitted(tree_root[1] ? leaf_of(tree_root) : "/", TREE_W - 50), -1, BLACK, 0);
	/* the project's close box */
	ui_bevel(window, TREE_W - 22, y0 + 2, 20, 18, 0);
	GrSetGCForeground(ui_gc, BLACK);
	GrLine(window, ui_gc, TREE_W - 15, y0 + 8, TREE_W - 9, y0 + 14);
	GrLine(window, ui_gc, TREE_W - 15, y0 + 14, TREE_W - 9, y0 + 8);
	GrLine(window, ui_gc, TREE_W - 14, y0 + 8, TREE_W - 8, y0 + 14);
	GrLine(window, ui_gc, TREE_W - 14, y0 + 14, TREE_W - 8, y0 + 8);
	ui_fill(window, 2, ly, TREE_W - 2, lh, WHITE);
	for (i = 0; i < shown && tree_top + i < tree_count; i++) {
		int k = tree_top + i, ry = ly + 2 + i * TREE_ROW, x = 6 + tree[k].depth * 12, chosen = !strcmp(tree[k].path, path), cell;
		const char *name = leaf_of(tree[k].path), *dot = strrchr(name, '.');

		if (chosen)
			ui_fill(window, 4, ry, TREE_W - 6 - bar, TREE_ROW, UI_SELECTED);
		GrSetGCForeground(ui_gc, chosen ? WHITE : MWRGB(100, 100, 100));
		if (tree[k].folder && tree[k].open)
			point_down(x + 3, ry + 7, 3);
		else if (tree[k].folder)
			ui_glyph_point(window, x + 1, ry + 9, 3, 1);
		cell = tree[k].folder ? cells[tree[k].open] : dot && !strcmp(dot, ".c") ? cells[2] : dot && !strcmp(dot, ".h") ? cells[3] :
		       dot && !strcmp(dot, ".js") ? cells[5] : dot && !strcmp(dot, ".py") ? cells[6] : dot && !strcmp(dot, ".hlsl") ? cells[2] : cells[4];
		ui_icon(window, ui_gc, x + 9, ry + 1, cell, 16);
		ui_text(window, x + 28, ry + 3, fitted(name, TREE_W - 8 - bar - x - 28), -1, chosen ? WHITE : BLACK, 0);
	}
	if (bar)
		ui_scrollbar(window, TREE_W - 2 - UI_SCROLL_W, ly + 2, lh - 4, tree_count, shown, tree_top);
	ui_bevel(window, 2, ly, TREE_W - 2, lh, 1);
}

/* Puts a text, or a number, at the end of another; where that ends is returned. */
static char *
put(char *to, const char *text)
{
	while (*text)
		*to++ = *text++;
	*to = 0;
	return to;
}

static char *
put_number(char *to, int n)
{
	char digits[12];
	int at = sizeof digits - 1;

	for (digits[at] = 0; n > 0 || at == (int)sizeof digits - 1; n /= 10)
		digits[--at] = (char)('0' + n % 10);
	return put(to, digits + at);
}

static int status_whole = 1;	/* its ground and edge are to be drawn too (draw_all) */

/* The status line: of its two texts, the one that changed (a key changes where the cursor is). */
static void
draw_status(void)
{
	static const char *const asks[] = { "", "Find: ", "Go to line: ", "Save as: ", "Replace: ", "Replace it with: " };
	static char left_shown[400], right_shown[48];
	static int left_wide;
	char text[400], where[48];
	int y = height - STATUS, n, both, k;

	if ((doc_now | doc_count << 8 | tx_modified << 16) != tabs_drawn)
		draw_doc_tabs();
	if (asking)
		snprintf(text, sizeof text, "%s%s_   (Enter, or Escape to leave)", asks[asking], answer);
	else {
		n = (int)(put(put(put(text, path), tx_modified ? " *" : ""), "   ") - text);
		strncat(text, notice, sizeof text - 1 - (size_t)n);
		/* nothing to say: what the compiler said of the cursor's line, if anything */
		for (k = 0; !notice[0] && k < problem_count; k++)
			if (problems[k].line == tx_y + 1) {
				snprintf(text + n, sizeof text - (size_t)n, "%s", problems[k].text);
				break;
			}
	}
	put_number(put(put_number(put(where, "line "), tx_y + 1), ", column "), tx_x + 1);
	/* (a text too long for its room runs under the other, which is then drawn over it again) */
	both = status_whole || strcmp(text, left_shown) || (left_wide && strcmp(where, right_shown));
	if (status_whole) {
		ui_fill(window, 0, y, width, STATUS, UI_FACE);
		ui_bevel(window, 0, y, width, STATUS, 1);
	} else if (both)
		ui_fill(window, 1, y + 1, width - 2, STATUS - 2, UI_FACE);
	else if (strcmp(where, right_shown))
		ui_fill(window, width - 133, y + 1, 132, STATUS - 2, UI_FACE);
	if (both) {
		ui_text_fit(window, 6, y + 3, width - 150, text, BLACK);
		left_wide = 6 + ui_width(&ui_var_widths, ui_var, text, -1) > width - 133;
		strcpy(left_shown, text);
	}
	if (both || strcmp(where, right_shown)) {
		ui_text(window, width - 132, y + 3, where, -1, BLACK, 0);
		strcpy(right_shown, where);
	}
	status_whole = 0;
}

static void
draw_panel(void)
{
	static const char *const names[PANELS] = { "Output", "Problems", "Variables", "Call stack" };
	int y = panel_y(), i, shown = (PANEL_H - TAB_H - 2) / 15, count, first;
	char label[40];

	if (!panel_on)
		return;
	ui_fill(window, 0, y, width, TAB_H, UI_FACE);
	ui_fill(window, 0, y, width, 1, UI_SHADOW);
	for (i = 0; i < PANELS; i++) {
		char *end = put(label, names[i]);

		if (i == P_PROBLEMS && problem_count)
			put(put_number(put(end, " ("), problem_count), ")");
		draw_tab(4 + i * 96, y + (i == panel ? 1 : 2), 94, TAB_H - (i == panel ? 1 : 2), i == panel);
		ui_text(window, 10 + i * 96, y + 3, label, -1, BLACK, 0);
	}
	ui_fill(window, 0, y + TAB_H, width, PANEL_H - TAB_H, WHITE);
	count = panel == P_OUTPUT ? out_count : panel == P_PROBLEMS ? problem_count : panel == P_VARIABLES ? variable_count : stack_count;
	first = panel == P_OUTPUT ? out_top : panel == P_PROBLEMS ? problem_top : panel == P_VARIABLES ? variable_top : 0;
	for (i = 0; i < shown && first + i < count; i++) {
		int k = first + i, py = y + TAB_H + 2 + i * 15;

		if (panel == P_OUTPUT)	/* the compiler's errors red, its warnings orange: a click goes there */
			ui_text_fit(window, 6, py, width - 12, out_lines[k], strstr(out_lines[k], ": error") ? MWRGB(200, 0, 0) :
				    strstr(out_lines[k], ": warning") ? MWRGB(170, 100, 0) : BLACK);
		else if (panel == P_PROBLEMS) {
			if (problems[k].line)
				put_number(put(label, "line "), problems[k].line);
			else
				put(label, "elsewhere");
			ui_text(window, 6, py, label, -1, problems[k].error ? MWRGB(200, 0, 0) : MWRGB(170, 100, 0), 0);
			ui_text_fit(window, 76, py, width - 82, problems[k].text, BLACK);
		} else
			ui_text_fit(window, 6, py, width - 12, panel == P_VARIABLES ? variables[k] : stack[k], BLACK);
	}
	out_fresh = 0;
}

static void layout(void);

static void
draw_all(void)
{
	if (ui_restyle()) {	/* (Settings changed the look: a word of memory read) */
		ui_menu_restyle();
		layout();
	}
	draw_menu_bar();
	draw_tools();
	draw_doc_tabs();
	draw_tree();
	if (tree_on)
		ui_fill(window, TREE_W, editor_y(), 3, panel_y() - editor_y(), UI_FACE);
	match_find(match_line, match_col);	/* (the cursor may have been put elsewhere) */
	draw_editor_from(top);
	draw_panel();
	status_whole = 1;
	draw_status();
}

static void
show_panel(int which)
{
	panel = which;
	if (!panel_on) {
		panel_on = 1;
		resized(width, height);
		draw_all();
	} else
		draw_panel();
}

/* ---- the view follows the cursor ---- */

static int top_was;	/* the view's first line before the last follow() */

static int
follow(void)
{
	int old_top = top, old_left = left;

	top_was = top;
	if (tx_y < top)
		top = tx_y;
	if (tx_y >= top + rows)
		top = tx_y - rows + 1;
	if (tx_x < left)
		left = tx_x;
	if (tx_x >= left + columns)
		left = tx_x - columns + 1;
	return top != old_top || left != old_left;
}

static int scroll_drawn;	/* NXIDE_SCROLL=draw: a view that moved is drawn whole, as it was before */

/* The view moved from line `was` to top and no text changed: the rows that stay in view are
 * copied to where they now are (one request, where each line was three), and the lines that
 * came into view are drawn. From too far, or with something selected, all of it is drawn. */
static void
view_moved(int was)
{
	int by = top - was, stay = rows - abs(by), y = editor_y(), line, x0, y0, x1, y1;

	if (stay < 1 || rows > ROWS_KEPT || scroll_drawn || tx_selection(&x0, &y0, &x1, &y1)) {
		draw_editor_from(top);
		return;
	}
	GrCopyArea(window, ui_gc, ex, y + (by < 0 ? -by * ui_fh : 0), width - UI_SCROLL_W - ex, stay * ui_fh, window, ex, y + (by > 0 ? by * ui_fh : 0),
		   MWROP_COPY);
	memmove(row_line + (by < 0 ? -by : 0), row_line + (by > 0 ? by : 0), (size_t)stay * sizeof row_line[0]);
	/* (the bar the cursor left behind goes first: a line drawn with the cursor on it forgets it) */
	if (bar_line >= 0 && bar_line != tx_y)
		draw_line_tail(bar_line, 1 << 20);
	for (line = by > 0 ? top + stay : top; line < (by > 0 ? top + rows : top - by); line++)
		draw_line(line);
	draw_scroll_bar();
}

static void
scroll_to(int line)
{
	int was = top;

	if (line > tx_count - rows)
		line = tx_count - rows;
	if (line < 0)
		line = 0;
	if (line != top) {
		top = line;
		view_moved(was);
	}
}

static void
go_to_line(int line)
{
	if (line < 1)
		line = 1;
	if (line > tx_count)
		line = tx_count;
	tx_y = line - 1, tx_x = 0, tx_marked = 0;
	if (tx_y < top || tx_y >= top + rows)
		top = tx_y > rows / 3 ? tx_y - rows / 3 : 0;
	left = 0;
	match_find(match_line, match_col);
	draw_editor_from(top);
	draw_status();
}

/* After anything that may have changed the text: draws what it has to, and no more (`all`:
 * the view moved or a selection is shown, and every line in view is drawn). */
static void
changed(int was_y, int all)
{
	int first = tx_touched, last = tx_touched_last, whole = tx_shifted, before, known = sx_known, from = 0, i;

	if (first == 1 << 30) {
		if (all == 2)
			view_moved(top_was);	/* (the cursor took the view up or down with it) */
		else if (all) {
			draw_editor_from(top);
			return;
		}
		/* the cursor went: its bar is taken from its old row and put on its new one */
		draw_line_tail(was_y, 1 << 20);
		if (tx_y != was_y)
			draw_line_tail(tx_y, 1 << 20);
		return;
	}
	/* the line after: if it now begins inside a comment and did not, all below it changed colour */
	before = first + 1 < sx_known ? sx_starts[first + 1] : -1;
	if (sx_known > first + 1)
		sx_known = first + 1;
	if (whole || first >= tx_count) {
		shown_upto = shown_upto < first ? shown_upto : first;
	} else if (last > first) {
		for (i = first; i <= last && i < shown_upto; i++)
			if (shown[i])
				shown[i]->valid = 0;
		if (first + 1 < tx_count) {
			starts_upto(first + 1);
			whole = before >= 0 && sx_starts[first + 1] != before;
		}
	} else {
		/* one line changed: read once, and compared with what its row shows */
		struct shown *old;

		starts_upto(first);
		old = first >= top && first - top < ROWS_KEPT && row_line[first - top] == first ? shown_kept(first) : NULL;
		shown_read(first);
		from = old ? shown_differ(old, &made.head) : 0;
		shown_keep(first);
		whole = before >= 0 && made.head.ends != before;
		if (before >= 0 && !whole)
			sx_known = known;	/* (the lines below begin as they did) */
	}
	tx_touched = 1 << 30, tx_shifted = 0, tx_touched_last = -1;
	unchecked = edited_now = 1;
	if (all)
		draw_editor_from(top);
	else if (whole)
		draw_editor_from(first);
	else {
		if (was_y != first)
			draw_line_tail(was_y, 1 << 20);	/* (first: where the cursor's bar was) */
		draw_line_tail(first, from);
		if (tx_y != first && tx_y != was_y)
			draw_line_tail(tx_y, 1 << 20);
	}
}

/* ---- the panels' text ---- */

static void
out_line(const char *text)
{
	if (out_count == OUT_MOST) {
		int i;

		for (i = 0; i < OUT_MOST / 2; i++)
			free(out_lines[i]);
		memmove(out_lines, out_lines + OUT_MOST / 2, (OUT_MOST - OUT_MOST / 2) * sizeof out_lines[0]);
		out_count -= OUT_MOST / 2;
	}
	/* the workers' library says so when one of them is stopped by a mark (debug_changed) */
	if (strstr(text, "a worker core stopped, cause 00000002"))
		worker_marked = 1;
	out_lines[out_count++] = strdup(text);
	out_top = out_count > (PANEL_H - TAB_H - 2) / 15 ? out_count - (PANEL_H - TAB_H - 2) / 15 : 0;
	out_fresh = 1;
	if (print_on)
		printf("nxide| %s\n", text);
}

/* What a program or the compiler wrote: whole lines to the output, the rest kept for later. */
static void
out_bytes(const char *bytes, int count)
{
	int n = (int)strlen(out_part), i;

	for (i = 0; i < count; i++) {
		if (bytes[i] == '\n' || n == (int)sizeof out_part - 1) {
			out_part[n] = 0;
			out_line(out_part);
			n = 0;
		}
		if (bytes[i] != '\n' && bytes[i] != '\r')
			out_part[n++] = bytes[i] == '\t' ? ' ' : bytes[i];
	}
	out_part[n] = 0;
}

/* Whether a file named in a complaint is the one shown or its check's copy. */
static int
ours(const char *name, int length)
{
	const char *check = sx_lang == SX_JS ? "/tmp/ide-check.js" : sx_lang == SX_PY ? "/tmp/ide-check.py" :
			    sx_lang == SX_HLSL ? "/tmp/ide-check.hlsl" : "/tmp/ide-check.c";

	return (length == (int)strlen(path) && !strncmp(name, path, (size_t)length)) ||
	       (length == (int)strlen(check) && !strncmp(name, check, (size_t)length));
}

/* An interpreter's complaint as one problem. QuickJS: "TypeError: what", then "    at f (FILE:LINE:COLUMN)"
 * lines, the innermost first; MicroPython: "Traceback", '  File "FILE", line LINE' lines, the
 * innermost last, and "TypeError: what". The line is the innermost in this file. */
static void
script_problems(const char *text)
{
	struct problem *p = &problems[0];
	char last[150] = "";
	int line = 0, found = 0, traced = 0;

	p->text[0] = 0;
	while (*text) {
		const char *end = strchr(text, '\n'), *at, *colon;
		int length = end ? (int)(end - text) : (int)strlen(text);

		if (sx_lang == SX_JS && !strncmp(text, "    at ", 7)) {
			if (!traced)
				snprintf(p->text, sizeof p->text, "%s", last), traced = 1;
			/* FILE:LINE:COLUMN, after a "(" when the frame has a function's name */
			at = memchr(text, '(', (size_t)length) ? (const char *)memchr(text, '(', (size_t)length) + 1 : text + 7;
			colon = memrchr(at, ':', (size_t)(text + length - at));
			while (colon && colon > at && colon[-1] != ':' && isdigit((unsigned char)colon[-1]))
				colon--;
			if (colon && colon > at && colon[-1] == ':' && !found && ours(at, (int)(colon - 1 - at)))
				line = atoi(colon), found = 1;
		} else if (sx_lang == SX_PY && !strncmp(text, "  File \"", 8)) {
			at = text + 8;
			colon = memchr(at, '"', (size_t)(text + length - at));
			if (colon && ours(at, (int)(colon - at)) && strstr(colon, ", line "))
				line = atoi(strstr(colon, ", line ") + 7);
			traced = 1;
		} else if (length) {
			snprintf(last, sizeof last, "%.*s", length < 149 ? length : 149, text);
			if (sx_lang == SX_PY && traced && text[0] != ' ' && strncmp(text, "Traceback", 9))
				snprintf(p->text, sizeof p->text, "%s", last);
		}
		text += length + (end != NULL);
	}
	problem_count = problem_top = 0;
	if (!traced)
		return;
	p->line = line, p->error = 1;
	memset(p->text + strlen(p->text), 0, sizeof p->text - strlen(p->text));	/* (whole: two lists of them are compared) */
	problem_count = 1;
}

/* The compiler's lines ("file:LINE: error: what", hlslc's too) as problems at their lines. */
static void
problems_from(const char *text)
{
	if (sx_lang == SX_JS || sx_lang == SX_PY) {
		script_problems(text);
		return;
	}
	problem_count = problem_top = 0;
	while (*text && problem_count < (int)(sizeof problems / sizeof problems[0])) {
		const char *end = strchr(text, '\n'), *at;
		int length = end ? (int)(end - text) : (int)strlen(text), n;

		for (at = text; at < text + length; at++)
			if (at[0] == ':' && isdigit((unsigned char)at[1]) && at > text) {
				struct problem *p = &problems[problem_count];
				const char *what = at + 1;
				int name = (int)(at - text);

				p->line = (int)strtol(at + 1, (char **)&what, 10);
				if (*what != ':')
					continue;
				what += what[1] == ' ' ? 2 : 1;
				p->error = !!memmem(what, (size_t)(text + length - what), "error", 5);
				if (!p->error && !memmem(what, (size_t)(text + length - what), "warning", 7))
					break;
				/* another file's (a header's): at no line of this one, its whole line said */
				if (!ours(text, name))
					p->line = 0, what = text;
				n = (int)(text + length - what) < (int)sizeof p->text - 1 ? (int)(text + length - what) : (int)sizeof p->text - 1;
				memcpy(p->text, what, (size_t)n);
				memset(p->text + n, 0, sizeof p->text - (size_t)n);	/* (whole: two lists of them are compared) */
				problem_count++;
				break;
			}
		text += length + (end != NULL);
	}
}

/* The problems of a check, and what they change drawn: the bars before the lines whose
 * problems are others now, and the panel if anything in it is. */
static void
problems_set(const char *text)
{
	static struct problem before[sizeof problems / sizeof problems[0]];
	unsigned char was[ROWS_KEPT];
	int count = problem_count, line;

	memcpy(before, problems, (size_t)count * sizeof problems[0]);
	for (line = top; line < top + rows && line - top < ROWS_KEPT; line++)
		was[line - top] = (unsigned char)problem_at(line);
	problems_from(text);
	if (count == problem_count && !memcmp(before, problems, (size_t)count * sizeof problems[0]))
		return;
	if (rows > ROWS_KEPT)
		draw_editor_from(top);
	for (line = top; line < top + rows && rows <= ROWS_KEPT; line++)
		if (was[line - top] != problem_at(line))
			draw_line(line);
	draw_panel();
}

/* The text's two sums, as a check's file has it; false if it names a file by "...", which
 * may be another tomorrow. */
static int
text_sums(unsigned sums[2])
{
	unsigned a = 2166136261u ^ (unsigned)sx_lang, b = 5381;	/* (C's sums are clean.py's) */
	int i, closed = 1;

	for (i = 0; i < tx_count; i++) {
		const unsigned char *at = (const unsigned char *)tx_lines[i];

		if (*at == '#' || *at == ' ')
			closed &= !(strstr(tx_lines[i], "include") && strchr(tx_lines[i], '"'));
		for (; *at; at++) {
			a = (a ^ *at) * 16777619u;
			b = b * 33 + *at;
		}
		a = (a ^ '\n') * 16777619u;
		b = b * 33 + '\n';
	}
	sums[0] = a, sums[1] = b;
	return closed;
}

static void
clean_add(const unsigned sums[2])
{
	clean[clean_next][0] = sums[0], clean[clean_next][1] = sums[1];
	clean_next = (clean_next + 1) % CLEAN_MOST;
	clean_count += clean_count < CLEAN_MOST;
}

static int
clean_has(const unsigned sums[2])
{
	int i;

	for (i = 0; i < clean_count; i++)
		if (clean[i][0] == sums[0] && clean[i][1] == sums[1])
			return 1;
	return 0;
}

/* The sums the build found clean: a line each, two numbers in hexadecimal. */
static void
clean_load(void)
{
	FILE *file = fopen("/usr/share/ide-clean", "r");
	unsigned sums[2];

	while (file && fscanf(file, "%x %x", &sums[0], &sums[1]) == 2)
		clean_add(sums);
	if (file)
		fclose(file);
}

/* ---- what runs beside ---- */

/* Starts a program beside, its output and its complaints into one pipe. No shell between:
 * starting one is as much as a small program's whole run. */
static void
job_start(int kind, char *const words[])
{
	int ends[2];

	if (pipe(ends) < 0)
		return;
	/* (the kernel's vfork: the child is on this program's memory until it is another program,
	 * and does nothing there but system calls; a fork was a copy of every page written after) */
	job.pid = real_vfork();
	if (job.pid < 0) {
		close(ends[0]), close(ends[1]);
		job.pid = 0;
		say("cannot start it");
		return;
	}
	if (job.pid == 0) {
		setpgid(0, 0);
		dup2(ends[1], 1), dup2(ends[1], 2);
		close(ends[0]), close(ends[1]);
		execvp(words[0], words);
		_exit(127);
	}
	close(ends[1]);
	fcntl(ends[0], F_SETFL, O_NONBLOCK);
	job.fd = ends[0], job.kind = kind, job.lang = sx_lang;
	GrRegisterInput(job.fd);
	job.heard = 1;
	job_length = 0;
	job_text[0] = 0;
}

/* The pipe is no longer waited on: it is at its end (and would wake the loop for ever), or going. */
static void
job_unheard(void)
{
	if (job.heard)
		GrUnregisterInput(job.fd);
	job.heard = 0;
}

static void
job_stop(void)
{
	if (job.kind == J_NONE)
		return;
	kill(-job.pid, SIGKILL);
	kill(job.pid, SIGKILL);
	waitpid(job.pid, NULL, 0);
	job_unheard();
	close(job.fd);
	if (job.kind == J_PYDBG)
		py_end();
	job.kind = J_NONE;
}

static void
debug_shown(void)
{
	int i;

	variable_count = dbg_variables(variables, 48, dbg_function_now, dbg_regs[8]);
	stack_count = dbg_stack(stack, 16);
	variable_top = 0;
	if (print_on) {
		for (i = 0; i < variable_count; i++)
			printf("nxide: variable: %s\n", variables[i]);
		for (i = 0; i < stack_count; i++)
			printf("nxide: in %s\n", stack[i]);
	}
}

/* The debugged program stopped, or is gone. */
static void
debug_changed(void)
{
	char text[160];

	if (dbg_state == DBG_GONE) {
		if (dbg_exit < 0)
			snprintf(text, sizeof text, "the program was ended by signal %d", -dbg_exit);
		else
			snprintf(text, sizeof text, "the program ended, with %d", dbg_exit);
		if (worker_marked)
			snprintf(text, sizeof text, "a worker core came to a marked line and the program ended: only the first core's lines can be marked");
		worker_marked = 0;
		say(text);
		out_line(text);
		variable_count = stack_count = 0;
		draw_all();
		return;
	}
	if (run_doc >= 0 && run_doc != doc_now)
		doc_show(run_doc);	/* (the stop is in the file being debugged) */
	if (dbg_signal)
		snprintf(text, sizeof text, "signal %d (%s) at line %d: Go on passes it to the program", dbg_signal, strsignal(dbg_signal), dbg_line_now);
	else
		snprintf(text, sizeof text, "stopped at line %d, in %s", dbg_line_now, dbg_function_now >= 0 ? dbg_functions[dbg_function_now].name : "?");
	say(text);
	debug_shown();
	panel = panel == P_STACK ? P_STACK : P_VARIABLES;
	if (dbg_line_now) {
		tx_y = dbg_line_now - 1 < tx_count ? dbg_line_now - 1 : tx_count - 1;
		tx_x = 0, tx_marked = 0;
		if (tx_y < top || tx_y >= top + rows)
			top = tx_y > rows / 3 ? tx_y - rows / 3 : 0;
	}
	draw_all();
}

/* The file the debugger reads: the program, or with no MMU the ELF it was made from. */
static const char *
program_elf(void)
{
	static char elf[340];

	snprintf(elf, sizeof elf, "%s%s", program, flat_machine ? ".elf" : "");
	return elf;
}

static void
debug_begin(void)
{
	int ends[2];

	if (!dbg_load(program_elf())) {
		say("this program has nothing for a debugger in it");
		return;
	}
	if (pipe(ends) < 0)
		return;
	if (!dbg_start(program, ends[1])) {
		close(ends[0]), close(ends[1]);
		say("the program did not start");
		return;
	}
	close(ends[1]);
	fcntl(ends[0], F_SETFL, O_NONBLOCK);
	debug_fd = ends[0];
	GrRegisterInput(debug_fd);
	debug_heard = 1;
	/* a mark is an instruction changed in memory every core runs: a worker that comes to one
	 * stops for good, and nothing here can hold or release it */
	say(dbg_shared && dbg_mark_count ? "debugging. This program has worker cores: a mark on a line they run ends it (only the first core can be held)"
					 : "debugging");
	/* to the first marked line, or with none to the first line of main */
	if (dbg_mark_count)
		dbg_go(DBG_RUN);
	else
		dbg_step(DBG_INTO);
	draw_tools();
	draw_status();
}

/* ---- the Python debugger: ide-pydbg.py runs the program under sys.settrace, is told what to do
 * through one pipe and says where it stopped through another (docs/apps/ide.md) ---- */

#define PY_TELL "/tmp/ide-dbg.in"
#define PY_HEAR "/tmp/ide-dbg.out"

/* The marks, then what to do: go, into, over, out. */
static void
py_send(const char *what)
{
	char line[1200];
	int i, n = snprintf(line, sizeof line, "marks");

	for (i = 0; i < dbg_mark_count && n < (int)sizeof line - 40; i++)
		n += snprintf(line + n, sizeof line - (size_t)n, " %d", dbg_marks[i]);
	n += snprintf(line + n, sizeof line - (size_t)n, "\n%s%s", what, what[0] ? "\n" : "");
	if (py_tell >= 0 && write(py_tell, line, (size_t)n) == n && what[0])
		py_state = DBG_RUNNING;
}

static void
py_end(void)
{
	if (py_heard)
		GrUnregisterInput(py_hear);
	if (py_hear >= 0)
		close(py_hear);
	if (py_tell >= 0)
		close(py_tell);
	py_hear = py_tell = -1;
	py_heard = 0;
	py_state = DBG_GONE;
	py_part[0] = 0;
	unlink(PY_TELL), unlink(PY_HEAR);
}

static void
py_begin(void)
{
	char *const words[] = { "micropython", "/usr/share/ide-pydbg.py", path, PY_TELL, PY_HEAR, NULL };

	/* (both ends of each pipe are opened here, read and write: neither open waits for the other side) */
	unlink(PY_TELL), unlink(PY_HEAR);
	if (mkfifo(PY_TELL, 0600) || mkfifo(PY_HEAR, 0600) || (py_tell = open(PY_TELL, O_RDWR | O_CLOEXEC)) < 0 ||
	    (py_hear = open(PY_HEAR, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0) {
		py_end();
		say("the debugger's pipes could not be made in /tmp");
		return;
	}
	job_start(J_PYDBG, words);
	if (job.kind != J_PYDBG) {
		py_end();
		return;
	}
	py_send("");	/* (the marks it starts with: it goes to the first, or with none stops at the first line) */
	py_state = DBG_RUNNING;
	GrRegisterInput(py_hear);
	py_heard = 1;
	variable_count = stack_count = 0;
	say("debugging (Variables has the module's names: MicroPython keeps none of a function's own)");
	draw_tools();
	draw_status();
}

/* What the debugger said: "stop LINE", "var NAME = VALUE", "frame NAME, line N", "end". */
static void
py_poll(void)
{
	char bytes[512], text[160];
	int n, i, k = (int)strlen(py_part), stopped = 0;

	while (py_hear >= 0 && (n = (int)read(py_hear, bytes, sizeof bytes)) > 0)
		for (i = 0; i < n; i++) {
			if (bytes[i] != '\n') {
				if (k < (int)sizeof py_part - 1)
					py_part[k++] = bytes[i];
				continue;
			}
			py_part[k] = 0, k = 0;
			if (!strncmp(py_part, "stop ", 5))
				py_line = atoi(py_part + 5), variable_count = stack_count = variable_top = 0;
			else if (!strncmp(py_part, "var ", 4) && variable_count < 48)
				snprintf(variables[variable_count++], sizeof variables[0], "%.159s", py_part + 4);
			else if (!strncmp(py_part, "frame ", 6) && stack_count < 16)
				snprintf(stack[stack_count++], sizeof stack[0], "%.159s", py_part + 6);
			else if (!strcmp(py_part, "end"))
				stopped = 1;
		}
	py_part[k] = 0;
	if (!stopped)
		return;
	py_state = DBG_STOPPED;
	if (run_doc >= 0 && run_doc != doc_now)
		doc_show(run_doc);
	snprintf(text, sizeof text, "stopped at line %d, in %.120s", py_line, stack_count ? stack[0] : "?");
	if (strstr(text, ", line "))
		*strstr(text, ", line ") = 0;
	say(text);
	if (print_on) {
		for (i = 0; i < variable_count; i++)
			printf("nxide: variable: %s\n", variables[i]);
		for (i = 0; i < stack_count; i++)
			printf("nxide: in %s\n", stack[i]);
	}
	panel = panel == P_STACK ? P_STACK : P_VARIABLES;
	if (py_line > 0) {
		tx_y = py_line - 1 < tx_count ? py_line - 1 : tx_count - 1;
		tx_x = 0, tx_marked = 0;
		if (tx_y < top || tx_y >= top + rows)
			top = tx_y > rows / 3 ? tx_y - rows / 3 : 0;
	}
	draw_all();
}

/* A build that worked, and what it goes on to. */
static void
built_ok(void)
{
	char text[160];

	if (sx_lang != SX_C)
		snprintf(text, sizeof text, "%.100s: nothing wrong found", leaf_of(path));
	else
		snprintf(text, sizeof text, "built: %.100s%s", program, problem_count ? ", with warnings" : "");
	say(text);
	if (then_do == C_RUN) {
		char *const words[] = { program, NULL }, *const js[] = { "qjs", "--std", path, NULL }, *const py[] = { "micropython", path, NULL };
		char *const shader[] = { "hlslview", path, NULL };

		if (sx_lang != SX_C) {
			snprintf(text, sizeof text, "%s '%.120s'", sx_lang == SX_JS ? "qjs --std" : sx_lang == SX_PY ? "micropython" : "hlslview", path);
			out_line(text);
		}
		job_start(J_RUN, sx_lang == SX_JS ? js : sx_lang == SX_PY ? py : sx_lang == SX_HLSL ? shader : words);
		say("running");
		panel = P_OUTPUT;
	} else if (then_do == C_DEBUG && sx_lang == SX_PY)
		py_begin();
	else if (then_do == C_DEBUG)
		debug_begin();
	then_do = 0;
	draw_all();
}

static void
job_ended(int kind, int status)
{
	char text[160];
	int errors = 0, i;

	if (kind == J_FLAT) {
		if (!WIFEXITED(status) || WEXITSTATUS(status)) {
			say("not built: elf2flt did not make the program");
			then_do = 0;
			draw_all();
		} else
			built_ok();
		return;
	}
	if (kind == J_RUN || kind == J_PYDBG) {
		if (kind == J_PYDBG)
			py_end();
		if (WIFSIGNALED(status))
			snprintf(text, sizeof text, "the program was ended by signal %d (%s)", WTERMSIG(status), strsignal(WTERMSIG(status)));
		else
			snprintf(text, sizeof text, "the program ended, with %d", WEXITSTATUS(status));
		say(text);
		out_line(text);
		/* a script that failed: its interpreter's complaint marked at its line */
		if (job.lang != SX_C && WIFEXITED(status) && WEXITSTATUS(status) && run_doc >= 0) {
			doc_show(run_doc);
			problems_from(job_text);
			if (problem_count && problems[0].line)
				go_to_line(problems[0].line);
		}
		draw_all();
		return;
	}
	if (kind == J_CHECK) {
		problems_set(job_text);
		/* a text the compiler said nothing of need not be shown to it again */
		if (!problem_count && check_closed && !job_unseen && WIFEXITED(status) && !WEXITSTATUS(status))
			clean_add(check_sum);
		return;
	}
	if (run_doc >= 0 && run_doc != doc_now)
		doc_show(run_doc);	/* (its problems are that file's) */
	problems_from(job_text);
	for (i = 0; i < problem_count; i++)
		errors += problems[i].error;
	if (errors || !WIFEXITED(status) || WEXITSTATUS(status)) {
		if (sx_lang != SX_C)
			snprintf(text, sizeof text, "%s", problem_count ? problems[0].text : "it did not look right to its interpreter");
		else
			snprintf(text, sizeof text, "not built: %d error%s", errors, errors == 1 ? "" : "s");
		say(text);
		panel = problem_count ? P_PROBLEMS : P_OUTPUT;
		if (problem_count && problems[0].line)
			go_to_line(problems[0].line);
		then_do = 0;
		draw_all();
	} else if (flat_machine && sx_lang == SX_C) {
		char *const words[] = { "elf2flt", (char *)program_elf(), program, NULL };

		job_start(J_FLAT, words);
	} else
		built_ok();
}

/* Looks at what runs beside; true when something was drawn. */
static void
poll_all(void)
{
	char bytes[512];
	int n, status;

	if (job.kind != J_NONE) {
		pid_t ended = waitpid(job.pid, &status, WNOHANG);

		while ((n = (int)read(job.fd, bytes, sizeof bytes)) > 0) {
			/* a script's run keeps its last 8 KB, for the complaint at its end */
			if (job_length + n >= (int)sizeof job_text && (job.kind == J_PYDBG || (job.kind == J_RUN && job.lang != SX_C))) {
				memmove(job_text, job_text + job_length / 2, (size_t)(job_length - job_length / 2));
				job_length -= job_length / 2;
			}
			if ((job.kind != J_RUN || job.lang != SX_C) && job_length + n < (int)sizeof job_text) {
				memcpy(job_text + job_length, bytes, (size_t)n);
				job_text[job_length += n] = 0;
			}
			if (job.kind != J_CHECK)
				out_bytes(bytes, n);
		}
		if (n == 0)
			job_unheard();
		if (ended == job.pid || (ended < 0 && errno == ECHILD)) {
			int kind = job.kind;

			job_unseen = ended < 0;
			if (ended < 0)
				status = 0;	/* (it ended unseen: what it wrote says how) */

			job_unheard();
			close(job.fd);
			job.kind = J_NONE;
			job_ended(kind, status);
		}
	}
	if (debug_fd >= 0) {
		while ((n = (int)read(debug_fd, bytes, sizeof bytes)) > 0)
			out_bytes(bytes, n);
		if ((n == 0 || dbg_state == DBG_GONE) && debug_heard) {
			GrUnregisterInput(debug_fd);
			debug_heard = 0;
		}
		if (dbg_state == DBG_GONE) {
			close(debug_fd);
			debug_fd = -1;
		}
	}
	if (dbg_poll())
		debug_changed();
	py_poll();
	if (out_fresh && panel == P_OUTPUT)
		draw_panel();
	/* nothing typed for a while: the compiler looks at the text as it is, quietly */
	if (checking_on && unchecked && !edited_now && job.kind == J_NONE && dbg_state == DBG_GONE && milliseconds() - edited_at > 1500) {
		static char folder[300];	/* (a file it includes by "..." is beside the text's own) */
		static char *const check[] = { "tcc", "-static", "-mfpu", "-I", folder, "-c", "/tmp/ide-check.c", "-o", "/tmp/ide-check.o", NULL };
		static char *const check_js[] = { "qjsc", "-c", "-o", "/tmp/ide-check.out", "/tmp/ide-check.js", NULL };
		static char *const check_py[] = { "micropython", "-c", PY_COMPILE, "/tmp/ide-check.py", NULL };
		static char *const check_hlsl[] = { "hlslc", "/tmp/ide-check.hlsl", NULL };
		static const char *const checked[SX_LANGS] = { "/tmp/ide-check.c", "/tmp/ide-check.js", "/tmp/ide-check.py", "/tmp/ide-check.hlsl" };
		static int files[SX_LANGS] = { -1, -1, -1, -1 }, room;
		int *file = &files[sx_lang];
		static char *text;
		struct stat is;
		int i, n = 0;

		unchecked = 0;
		snprintf(folder, sizeof folder, "%.*s", strrchr(path, '/') ? (int)(strrchr(path, '/') - path) + 1 : 1, strrchr(path, '/') ? path : ".");
		check_closed = text_sums(check_sum);
		if (check_closed && clean_has(check_sum)) {
			problems_set("");	/* (it said nothing of this text before) */
			return;
		}
		/* the text as one piece, into a file kept open: made and closed anew each time it was
		 * 30,000 instructions */
		for (i = 0; i < tx_count; i++)
			n += (int)strlen(tx_lines[i]) + 1;
		if (n > room)
			text = realloc(text, (size_t)(room = n + 4096));
		for (i = n = 0; i < tx_count; i++) {
			int length = (int)strlen(tx_lines[i]);

			memcpy(text + n, tx_lines[i], (size_t)length);
			text[n + length] = '\n';
			n += length + 1;
		}
		if (*file >= 0 && (fstat(*file, &is) != 0 || !is.st_nlink)) {
			close(*file);	/* (somebody took the file away) */
			*file = -1;
		}
		if (*file < 0)
			*file = open(checked[sx_lang], O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
		if (*file < 0 || pwrite(*file, text, (size_t)n, 0) != n || ftruncate(*file, n) != 0)
			return;
		job_start(J_CHECK, sx_lang == SX_JS ? check_js : sx_lang == SX_PY ? check_py : sx_lang == SX_HLSL ? check_hlsl : check);
	}
}

/* ---- files ---- */

static void
named(const char *file)
{
	const char *leaf;
	char title[340];
	int n;

	snprintf(path, sizeof path, "%.299s", file);
	leaf = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	n = (int)strcspn(leaf, ".");
	snprintf(program, sizeof program, "/tmp/ide-%.*s", n, leaf);
	/* (a long name is cut for the caption, which has the window's width and its three boxes) */
	snprintf(title, sizeof title, "%s - IDE", fitted(leaf, width - 150));
	if (lang_of(path) != sx_lang) {
		sx_lang = lang_of(path);	/* (another language: every line is lexed again) */
		sx_known = 0, shown_upto = 0;
	}
	if (window)
		GrSetWindowTitle(window, title);
	if (print_on)
		printf("nxide: the file is %s\n", path);
}

static void
opened(void)
{
	sx_known = 0, shown_upto = 0;
	top = left = 0;
	problem_count = 0;
	dbg_mark_count = 0;
	tx_touched = 1 << 30, tx_shifted = 0, tx_touched_last = -1;
	unchecked = edited_now = 1;
}

/* ---- the open files: the one shown is in the globals ---- */

static void
doc_keep(void)
{
	struct doc *d;

	if (doc_now < 0)
		return;
	d = &docs[doc_now];
	snprintf(d->path, sizeof d->path, "%s", path);
	d->lines = tx_lines, d->count = tx_count, d->room = tx_room;
	d->x = tx_x, d->y = tx_y, d->ax = tx_ax, d->ay = tx_ay, d->marked = tx_marked, d->modified = tx_modified;
	d->top = top, d->left = left;
	d->done = tx_done, d->undone = tx_undone, d->done_count = tx_done_count, d->undone_count = tx_undone_count;
	d->done_room = tx_done_room, d->undone_room = tx_undone_room;
	if (!d->problems)
		d->problems = malloc(sizeof problems);
	if (d->problems)
		memcpy(d->problems, problems, sizeof problems);
	d->problem_count = d->problems ? problem_count : 0;
	memcpy(d->marks, dbg_marks, sizeof d->marks);
	d->mark_count = dbg_mark_count;
}

static void
doc_take(int i)
{
	struct doc *d = &docs[i];

	if (job.kind == J_CHECK)
		job_stop();
	doc_now = i;
	tx_lines = d->lines, tx_count = d->count, tx_room = d->room;
	tx_x = d->x, tx_y = d->y, tx_ax = d->ax, tx_ay = d->ay, tx_marked = d->marked, tx_modified = d->modified;
	top = d->top, left = d->left;
	tx_done = d->done, tx_undone = d->undone, tx_done_count = d->done_count, tx_undone_count = d->undone_count;
	tx_done_room = d->done_room, tx_undone_room = d->undone_room;
	problem_count = d->problems ? d->problem_count : 0, problem_top = 0;
	if (d->problems)
		memcpy(problems, d->problems, sizeof problems);
	memcpy(dbg_marks, d->marks, sizeof dbg_marks);
	dbg_mark_count = d->mark_count;
	named(d->path);
	sx_known = 0, shown_upto = 0;
	tx_touched = 1 << 30, tx_shifted = 0, tx_touched_last = -1;
	unchecked = edited_now = 1;
	match_line[0] = match_line[1] = -1;
	leaving = 0;
}

/* Shows another open file; the caller draws. */
static void
doc_show(int i)
{
	if (i == doc_now || i < 0 || i >= doc_count)
		return;
	doc_keep();
	doc_take(i);
}

/* A new empty file in the globals, as the last tab. */
static void
doc_add(void)
{
	doc_keep();
	memset(&docs[doc_count], 0, sizeof docs[0]);
	tx_lines = NULL, tx_count = tx_room = 0;
	tx_done = tx_undone = NULL;
	tx_done_count = tx_undone_count = tx_done_room = tx_undone_room = 0;
	tx_x = tx_y = tx_marked = tx_modified = 0;
	problem_count = dbg_mark_count = 0;
	match_line[0] = match_line[1] = -1;
	doc_now = doc_count++;
}

static int
doc_find(const char *file)
{
	int i;

	for (i = 0; i < doc_count; i++)
		if (!strcmp(i == doc_now ? path : docs[i].path, file))
			return i;
	return -1;
}

static int may_drop(void);

/* A file in a tab of its own, or the tab it is in already; with every tab taken, in the shown
 * one's place if it may go. False if it was not opened. */
static int
open_file(const char *file)
{
	int i = doc_find(file);

	if (i >= 0) {
		doc_show(i);
		return 1;
	}
	if (doc_count < DOCS)
		doc_add();
	else if ((doc_now == run_doc && !can(C_BUILD)) || !may_drop()) {
		say("every tab is taken: close one first");
		return 0;
	} else if (doc_now == run_doc)
		run_doc = -1;
	if (job.kind == J_CHECK)
		job_stop();
	named(file);
	say(tx_load(path) ? "" : "a new file");
	opened();
	return 1;
}

/* A new file to write in: untitled.c, or untitled2.c and on when that is taken. */
static void
fresh_doc(void)
{
	static const char *const fresh = "#include <stdio.h>\n\nint main(void)\n{\n    printf(\"hello\\n\");\n    return 0;\n}\n";
	char name[160];
	int n;

	for (n = 1; n < 100; n++) {
		/* (in the project's folder, when there is a project) */
		if (n == 1)
			snprintf(name, sizeof name, "%.120s/untitled.c", project[0] ? project : "/root");
		else
			snprintf(name, sizeof name, "%.120s/untitled%d.c", project[0] ? project : "/root", n);
		if (access(name, F_OK) != 0 && doc_find(name) < 0)
			break;
	}
	if (!open_file(name))
		return;
	if (tx_count == 1 && !tx_lines[0][0]) {
		tx_insert(0, 0, fresh, (int)strlen(fresh));
		tx_x = tx_y = 0;
	}
	opened();
}

/* Takes an open file away (the caller asked about its changes); the last gives way to a new one. */
static void
doc_close(int i)
{
	doc_show(i);
	tx_forget();
	free(tx_lines), free(tx_done), free(tx_undone), free(docs[i].problems);
	memmove(docs + i, docs + i + 1, (size_t)(doc_count - i - 1) * sizeof docs[0]);
	doc_count--;
	run_doc = run_doc == i ? -1 : run_doc > i ? run_doc - 1 : run_doc;
	doc_now = -1;
	if (doc_count)
		doc_take(i < doc_count ? i : doc_count - 1);
	else
		fresh_doc();
}

/* An example is the user's copy of it once there is one: /root/NAME.c (.js, .py, .hlsl). */
static void
open_example(int n)
{
	char own[120], source[120];

	snprintf(own, sizeof own, "/root/%s.%s", examples[n], example_ends[n]);
	snprintf(source, sizeof source, "/usr/share/example-%s.%s", examples[n], example_ends[n]);
	if (access(own, R_OK) == 0 || doc_find(own) >= 0) {
		open_file(own);
		return;
	}
	if (!open_file(source))
		return;
	named(own);
	tx_modified = 1;
	say("an example: Save keeps your copy of it");
}

static void
save(void)
{
	say(tx_save(path) ? "saved" : "cannot write the file");
	tree_read();	/* (it may be a new file of the tree's) */
}

static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window)
		draw_all();
}

/* ---- the menus ---- */

static struct ui_menu_item example_items[12];
#define EXAMPLES_ITEM 7
#define CLOSE_PROJECT_ITEM 9
static const struct ui_menu_item file_items[] = {
	{ "New   Ctrl+N", -1, C_NEW }, { "Open...   Ctrl+O", -1, C_OPEN }, { "Open a folder as the project...", -1, C_PROJECT_OPEN },
	{ "Save   Ctrl+S", -1, C_SAVE, NULL, 0, UI_MENU_LINE }, { "Save as...", -1, C_SAVE_AS },
	{ "Save all   Ctrl+Shift+S", -1, C_SAVE_ALL }, { "Close the tab   Ctrl+W", -1, C_CLOSE },
	{ "Examples", -1, 0, example_items, 0, UI_MENU_LINE }, { "Its folder as the project", -1, C_TREE_HERE },
	{ "Close the project", -1, C_PROJECT_CLOSE }, { "Exit   Ctrl+Q", -1, C_EXIT, NULL, 0, UI_MENU_LINE },
};
static const struct ui_menu_item edit_items[] = {
	{ "Undo   Ctrl+Z", -1, C_UNDO }, { "Redo   Ctrl+Y", -1, C_REDO }, { "Cut   Ctrl+X", -1, C_CUT, NULL, 0, UI_MENU_LINE },
	{ "Copy   Ctrl+C", -1, C_COPY }, { "Paste   Ctrl+V", -1, C_PASTE }, { "Delete", -1, C_DELETE },
	{ "Find...   Ctrl+F", -1, C_FIND, NULL, 0, UI_MENU_LINE }, { "Find next   F3", -1, C_FIND_NEXT },
	{ "Find the one before   Shift+F3", -1, C_FIND_PREV }, { "Replace...   Ctrl+H", -1, C_REPLACE },
	{ "Go to line...   Ctrl+G", -1, C_GOTO, NULL, 0, UI_MENU_LINE },
};
static const struct ui_menu_item selection_items[] = {
	{ "Select all   Ctrl+A", -1, C_ALL }, { "Select the line   Ctrl+L", -1, C_LINE }, { "Select the word   Ctrl+D", -1, C_WORD },
	{ "Indent   Tab", -1, C_INDENT, NULL, 0, UI_MENU_LINE }, { "Unindent   Shift+Tab", -1, C_UNINDENT }, { "Comment or uncomment   Ctrl+/", -1, C_COMMENT },
};
#define VIEW_ITEMS 6
static const struct ui_menu_item view_items[VIEW_ITEMS] = {
	{ "The project's tree   Ctrl+B", -1, C_TREE }, { "The panel below", -1, C_PANEL }, { "Line numbers", -1, C_NUMBERS },
	{ "Check while typing", -1, C_CHECKING }, { "Next tab   Ctrl+Tab", -1, C_NEXT_TAB, NULL, 0, UI_MENU_LINE },
	{ "Tab before   Ctrl+Shift+Tab", -1, C_PREV_TAB },
};
static const struct ui_menu_item run_items[] = {
	{ "Build   F7", -1, C_BUILD }, { "Run   Ctrl+F5 or F6", -1, C_RUN }, { "Debug, or go on   F5", -1, C_DEBUG }, { "Stop   Shift+F5", -1, C_STOP },
	{ "Step over   F10", -1, C_OVER, NULL, 0, UI_MENU_LINE }, { "Step into   F11", -1, C_INTO }, { "Step out   Shift+F11", -1, C_OUT },
	{ "Mark or unmark the line   F9", -1, C_MARK, NULL, 0, UI_MENU_LINE }, { "Unmark every line", -1, C_UNMARK_ALL },
};
static const struct ui_menu_item help_items[] = { { "Keys", -1, C_KEYS }, { "About", -1, C_ABOUT } };
#define FILE_ITEMS ((int)(sizeof file_items / sizeof file_items[0]))
static struct { const struct ui_menu_item *items; int count; } menus[6] = {
	{ file_items, FILE_ITEMS }, { edit_items, 11 }, { selection_items, 6 }, { view_items, VIEW_ITEMS }, { run_items, 9 }, { help_items, 2 },
};

static void
menu_open(int which, int keyboard)
{
	static struct ui_menu_item files[FILE_ITEMS], views[VIEW_ITEMS];
	struct { int x, y; } info;
	int i;

	ui_window_origin(window, &info.x, &info.y);
	if (which == 0) {	/* (the examples are found when the program starts: their count is not constant) */
		memcpy(files, file_items, sizeof files);
		files[EXAMPLES_ITEM].sub_count = example_count;
		if (!project[0])
			files[CLOSE_PROJECT_ITEM].flags |= UI_MENU_GREY;
		ui_menu_open(files, FILE_ITEMS, info.x + menu_x[0], info.y + MENU_H, 0, keyboard);
		return;
	}
	if (which == 3) {	/* (a tick by each choice that is on) */
		memcpy(views, view_items, sizeof views);
		for (i = 0; i < 4; i++)
			if (i == 0 ? tree_on : i == 1 ? panel_on : i == 2 ? numbers_on : checking_on)
				views[i].flags |= UI_MENU_CHECK;
		if (!project[0])
			views[0].flags |= UI_MENU_GREY;	/* (no project: no tree) */
		ui_menu_open(views, VIEW_ITEMS, info.x + menu_x[3], info.y + MENU_H, 0, keyboard);
		return;
	}
	ui_menu_open(menus[which].items, menus[which].count, info.x + menu_x[which], info.y + MENU_H, 0, keyboard);
}

/* ---- commands ---- */

static void
copy_selection(void)
{
	int x0, y0, x1, y1, length;
	char *text;

	if (!tx_selection(&x0, &y0, &x1, &y1)) {
		say("nothing is selected");
		return;
	}
	text = tx_text(x0, y0, x1, y1, &length);
	ui_clip_set(text, length);
	free(text);
	say("copied");
}

/* The lines the selection touches (or the cursor's), each given `spaces` more or fewer in front. */
static void
indent(int spaces)
{
	int x0, y0, x1, y1, y, n;

	if (!tx_selection(&x0, &y0, &x1, &y1))
		y0 = y1 = tx_y, x1 = 1;
	if (x1 == 0 && y1 > y0)
		y1--;
	for (y = y0; y <= y1; y++) {
		if (spaces > 0 && tx_lines[y][0])
			tx_insert(0, y, "    ", spaces);
		for (n = 0; spaces < 0 && n < -spaces && tx_lines[y][n] == ' '; n++)
			continue;
		if (spaces < 0 && n)
			tx_delete(0, y, n, y);
	}
	tx_ax = 0, tx_ay = y0, tx_x = (int)strlen(tx_lines[y1]), tx_y = y1;
	tx_marked = y1 > y0;
}

static void
comment(void)
{
	const char *mark = sx_lang == SX_PY ? "# " : "// ";	/* (and its length without the space) */
	int x0, y0, x1, y1, y, n, all = 1, k = (int)strlen(mark) - 1;

	if (!tx_selection(&x0, &y0, &x1, &y1))
		y0 = y1 = tx_y, x1 = 1;
	if (x1 == 0 && y1 > y0)
		y1--;
	for (y = y0; y <= y1; y++) {
		n = (int)strspn(tx_lines[y], " ");
		all &= !tx_lines[y][n] || !strncmp(tx_lines[y] + n, mark, (size_t)k);
	}
	for (y = y0; y <= y1; y++) {
		n = (int)strspn(tx_lines[y], " ");
		if (!tx_lines[y][n])
			continue;
		if (all)
			tx_delete(n, y, n + (tx_lines[y][n + k] == ' ' ? k + 1 : k), y);
		else
			tx_insert(n, y, mark, k + 1);
	}
	tx_ax = 0, tx_ay = y0, tx_x = (int)strlen(tx_lines[y1]), tx_y = y1;
	tx_marked = y1 > y0;
}

static void
select_word(void)
{
	const char *line = tx_lines[tx_y];
	int a = tx_x, b = tx_x;

	while (a > 0 && (isalnum((unsigned char)line[a - 1]) || line[a - 1] == '_'))
		a--;
	while (isalnum((unsigned char)line[b]) || line[b] == '_')
		b++;
	tx_ax = a, tx_ay = tx_y, tx_x = b, tx_marked = b > a;
}

/* Asks what to save the text as, in a window, and writes it there; false if it was not written. */
static int
save_as(void)
{
	char where[300], chosen[600], *slash;

	snprintf(where, sizeof where, "%s", path);
	if ((slash = strrchr(where, '/')) != NULL)
		*slash = 0;
	if (!ui_save_file("Save as", !slash ? "/root" : slash == where ? "/" : where, NULL, slash ? slash + 1 : where, chosen, sizeof chosen, others))
		return 0;
	if (!tx_save(chosen)) {
		say("NOT saved: cannot write the file");
		return 0;
	}
	named(chosen);
	say("saved");
	return 1;
}

/* True if the text may be let go: it has no changes, or the answer was to save them or not to.
 * A test's script is asked nothing: its second "quit" leaves, as it did. */
static int
may_drop(void)
{
	static const char *const answers[] = { "Save", "Don't save", "Cancel" };
	char text[400];
	int answer;

	if (!tx_modified)
		return 1;
	if (scripted_run) {
		if (leaving)
			return 1;
		leaving = 1;
		say("not saved: Ctrl+S saves, Exit once more leaves without");
		return 0;
	}
	snprintf(text, sizeof text, "%.200s has changes that are not saved.\nSave them?", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
	answer = ui_ask("IDE", text, answers, 3, others);
	if (answer == 0)
		save();
	draw_all();
	return answer == 1 || (answer == 0 && !tx_modified);
}

/* The same of every open file, each shown while it is asked about. */
static int
may_drop_all(void)
{
	int i, any = 0;

	for (i = 0; i < doc_count; i++)
		any |= i == doc_now ? tx_modified : docs[i].modified;
	if (!any)
		return 1;
	if (scripted_run)
		return may_drop();
	for (i = 0; i < doc_count; i++)
		if (i == doc_now ? tx_modified : docs[i].modified) {
			doc_show(i);
			draw_all();
			if (!may_drop())
				return 0;
		}
	return 1;
}

/* Closes a tab, once its changes are saved or let go. */
static void
close_tab(int i)
{
	if (i == run_doc && !can(C_BUILD)) {
		say("its program is being built or run: Stop it first");
		draw_status();
		return;
	}
	doc_show(i);
	if (!may_drop()) {
		draw_all();
		return;
	}
	doc_close(i);
	draw_all();
}

/* A text file, as far as its first bytes say: no zeros, and not too large to edit. */
static int
looks_like_text(const char *file)
{
	char head[512];
	struct stat is;
	int fd = open(file, O_RDONLY), n;

	if (fd < 0)
		return 1;
	n = (int)read(fd, head, sizeof head);
	n = fstat(fd, &is) == 0 && is.st_size > (1 << 20) ? -2 : n;
	close(fd);
	return n != -2 && (n <= 0 || !memchr(head, 0, (size_t)n));
}

#define PROJECT_KEY "ide.project"	/* the registry's key for the project open at the last exit */

/* A folder as the project: its tree at the left, kept for the next start (not by a test's run). */
static void
project_open(const char *dir, int keep)
{
	char real[PATH_MAX];
	struct stat is;

	if (!realpath(dir, real) || stat(real, &is) || !S_ISDIR(is.st_mode) || strlen(real) >= sizeof project) {
		say("that is no folder that can be a project");
		return;
	}
	snprintf(project, sizeof project, "%s", real);
	snprintf(tree_root, sizeof tree_root, "%s", project);
	tree_opened_count = tree_top = 0;
	tree_wanted = 1;
	tree_read();
	resized(width, height);
	if (keep && !scripted_run)
		reg_set(PROJECT_KEY, project);
	say("the project's tree is at the left: Ctrl+B hides and shows it");
}

static void
project_close(void)
{
	project[0] = 0;
	tree_count = 0;
	resized(width, height);
	if (!scripted_run)
		reg_set(PROJECT_KEY, NULL);
	say("no project: no tree");
}

/* The folder of the file shown, as the project. */
static void
tree_here(void)
{
	char folder[300];

	snprintf(folder, sizeof folder, "%.*s", strrchr(path, '/') && strrchr(path, '/') != path ? (int)(strrchr(path, '/') - path) : 1,
		 strrchr(path, '/') ? path : "/");
	project_open(folder, 1);
}

static void
build(int then)
{
	char shell[800];
	char *const words[] = { "cc", "-g", path, "-o", program, NULL };

	if (job.kind == J_CHECK)
		job_stop();
	static char elf[340];
	char *const linked[] = { "tcc", "-shared", "-nostdlib", "-mfpu", "/usr/lib/flatcrt.o", "/usr/lib/crti.o", "-g", path, "-lgles", "-lnano-X",
				 "-lhlsl", "-lmcw", "-lc", "/usr/lib/tcc/libtcc1.a", "/usr/lib/crtn.o", "-o", elf, NULL };

	if (tx_modified && !tx_save(path)) {	/* (a file as it was opened is not written again) */
		say("cannot write the file");
		return;
	}
	if (sx_lang != SX_C) {
		/* a script's build is its interpreter's look at it: qjsc compiles it, MicroPython's compile(); a shader's is hlslc */
		char *const js[] = { "qjsc", "-c", "-o", "/tmp/ide-build.out", path, NULL };
		char *const py[] = { "micropython", "-c", (char *)PY_COMPILE, path, NULL };
		char *const shader[] = { "hlslc", path, NULL };

		run_doc = doc_now;
		then_do = then;
		problem_count = 0;
		out_line("");
		snprintf(shell, sizeof shell, sx_lang == SX_JS ? "qjsc -c '%s' (is it JavaScript?)" : sx_lang == SX_PY ? "micropython: compile '%s' (is it Python?)" :
			 "hlslc '%s' (compiled for the lanes, not run)", path);
		out_line(shell);
		job_start(J_BUILD, sx_lang == SX_JS ? js : sx_lang == SX_PY ? py : shader);
		say("looking at it...");
		panel = P_OUTPUT;
		unchecked = 0;
		return;
	}
	/* no MMU: linked as cc links it, and its ELF kept for the debugger (J_FLAT makes the program) */
	snprintf(elf, sizeof elf, "%s", program_elf());
	run_doc = doc_now;
	then_do = then;
	problem_count = 0;
	out_line("");
	snprintf(shell, sizeof shell, "cc -g '%s' -o '%s' 2>&1", path, program);
	out_line(shell);
	job_start(J_BUILD, flat_machine ? linked : words);
	say("building...");
	panel = P_OUTPUT;
	unchecked = 0;
}

static void
help(int about)
{
	static const char *const keys[] = {
		"F7 builds, F6 or Ctrl+F5 builds and runs, F5 builds and runs under the debugger (and goes on from a stop), Shift+F5 stops.",
		"F9 marks the line to stop at (or click before its number). F10 steps over a line, F11 into a call, Shift+F11 out of the function.",
		"Ctrl+Z and Ctrl+Y take a change back and make it again. Ctrl+F finds, F3 and Shift+F3 the next and the one before, Ctrl+H replaces.",
		"Tab and Shift+Tab indent the selected lines, Ctrl+/ comments them. Ctrl+L selects the line, Ctrl+D the word, Ctrl+G goes to a line.",
		"Ctrl+N, Ctrl+O, Ctrl+S, Ctrl+W: a new file, open, save, close the tab. Ctrl+Tab goes round the tabs, Ctrl+B a project's tree.",
		"A red bar before a line is the compiler's error there, an orange one its warning: Problems below says what, a click goes there.",
	};
	static const char *const what[] = {
		"nxide: an editor for C, JavaScript, Python and HLSL, the machine's own C compiler (TinyCC), QuickJS, MicroPython, an HLSL compiler and a debugger.",
		"The colours are a lexer's; the compiler (qjsc, MicroPython, hlslc) checks the text a moment after you stop typing. A .hlsl file runs in hlslview. File > Examples has programs to begin from.",
	};
	int i;

	out_line("");
	for (i = 0; i < (about ? 2 : 6); i++)
		out_line(about ? what[i] : keys[i]);
	show_panel(P_OUTPUT);
}

static void
ask(int what, const char *start)
{
	asking = what;
	snprintf(answer, sizeof answer, "%.199s", start);
	draw_status();
}

static void
command_done(int id)
{
	char chosen[600], where[300], *text;
	int was = tx_y, length, i, x0, y0, x1, y1;

	if (!can(id)) {
		say("not now");
		draw_status();
		return;
	}
	tx_step++;
	notice[0] = 0;
	if (id != C_EXIT)
		leaving = 0;
	switch (id) {
	case C_NEW:
		fresh_doc();
		draw_all();
		return;
	case C_OPEN:
		/* from the project's folder, or else the shown file's */
		snprintf(where, sizeof where, "%.*s", project[0] ? (int)strlen(project) : strrchr(path, '/') && strrchr(path, '/') != path ? (int)(strrchr(path, '/') - path) : 5,
			 project[0] ? project : strrchr(path, '/') && strrchr(path, '/') != path ? path : "/root");
		if (ui_choose_file("Open", where, ".c .h .js .py .hlsl .txt", chosen, sizeof chosen, others))
			open_file(chosen);
		draw_all();
		return;
	case C_SAVE:
		save();
		draw_tree();
		break;
	case C_SAVE_ALL:
		for (i = 0, x0 = doc_now; i < doc_count; i++)
			if (i == doc_now ? tx_modified : docs[i].modified) {
				doc_show(i);
				save();
			}
		doc_show(x0);
		say("every file saved");
		draw_all();
		return;
	case C_CLOSE:
		close_tab(doc_now);
		return;
	case C_NEXT_TAB:
	case C_PREV_TAB:
		doc_show((doc_now + (id == C_NEXT_TAB ? 1 : doc_count - 1)) % doc_count);
		draw_all();
		return;
	case C_TREE:
		if (!project[0]) {
			say("no project is open: File, Open a folder as the project");
			break;
		}
		tree_wanted = !tree_on;
		resized(width, height);
		draw_all();
		return;
	case C_TREE_HERE:
		tree_here();
		draw_all();
		return;
	case C_PROJECT_OPEN:
		snprintf(where, sizeof where, "%s", project[0] ? project : "/root");
		if (ui_choose_folder("Open a folder as the project", where, chosen, sizeof chosen, others))
			project_open(chosen, 1);
		draw_all();
		return;
	case C_PROJECT_CLOSE:
		project_close();
		draw_all();
		return;
	case C_REPLACE:
		if (tx_selection(&x0, &y0, &x1, &y1) && y0 == y1 && x1 - x0 < (int)sizeof sought)
			snprintf(sought, sizeof sought, "%.*s", x1 - x0, tx_lines[y0] + x0);
		ask(ASK_REPLACE, sought);
		return;
	case C_FIND_PREV:
		say(tx_find_back(sought) ? "found" : sought[0] ? "not found" : "Ctrl+F says what to find");
		break;
	case C_SAVE_AS:
		if (script[0]) {	/* (a test's script types the path in the status line) */
			ask(ASK_SAVE, path);
			return;
		}
		save_as();
		draw_all();
		return;
	case C_EXIT:
		if (!may_drop_all()) {
			draw_status();
			return;
		}
		job_stop();
		dbg_end();
		GrClose();
		exit(0);
	case C_UNDO:
	case C_REDO:
		if (!tx_replay(id == C_REDO))
			say(id == C_REDO ? "nothing to make again" : "nothing to take back");
		break;
	case C_CUT:
		copy_selection();
		tx_delete_selection();
		break;
	case C_COPY:
		copy_selection();
		break;
	case C_PASTE:
		if ((text = ui_clip_get(&length)) != NULL) {
			tx_delete_selection();
			tx_insert(tx_x, tx_y, text, length);
			free(text);
		}
		break;
	case C_DELETE:
		tx_delete_selection();
		break;
	case C_FIND:
		if (tx_selection(&x0, &y0, &x1, &y1) && y0 == y1 && x1 - x0 < (int)sizeof sought)
			snprintf(sought, sizeof sought, "%.*s", x1 - x0, tx_lines[y0] + x0);
		ask(ASK_FIND, sought);
		return;
	case C_FIND_NEXT:
		say(tx_find(sought) ? "found" : sought[0] ? "not found" : "Ctrl+F says what to find");
		break;
	case C_GOTO:
		ask(ASK_GOTO, "");
		return;
	case C_ALL:
		tx_ax = tx_ay = 0, tx_y = tx_count - 1, tx_x = (int)strlen(tx_lines[tx_y]), tx_marked = 1;
		break;
	case C_LINE:
		tx_ax = 0, tx_ay = tx_y, tx_marked = 1;
		if (tx_y + 1 < tx_count)
			tx_y++, tx_x = 0;
		else
			tx_x = (int)strlen(tx_lines[tx_y]);
		break;
	case C_WORD:
		select_word();
		break;
	case C_INDENT:
		indent(TX_TAB);
		break;
	case C_UNINDENT:
		indent(-TX_TAB);
		break;
	case C_COMMENT:
		comment();
		break;
	case C_PANEL:
		panel_on = !panel_on;
		resized(width, height);
		draw_all();
		return;
	case C_NUMBERS:
		numbers_on = !numbers_on;
		resized(width, height);
		draw_all();
		return;
	case C_CHECKING:
		checking_on = !checking_on;
		say(checking_on ? "the compiler checks the text while you type" : "the text is checked only when it is built");
		break;
	case C_BUILD:
		build(0);
		draw_all();
		return;
	case C_RUN:
		build(C_RUN);
		draw_all();
		return;
	case C_DEBUG:
		if (py_state == DBG_STOPPED) {
			doc_show(run_doc);
			py_send("go");
			say("running");
		} else if (dbg_state == DBG_STOPPED) {
			doc_show(run_doc);	/* (the marks it goes to are that file's) */
			dbg_go(DBG_RUN);
			say("running");
		} else
			build(C_DEBUG);
		draw_all();
		return;
	case C_STOP:
		if (job.kind == J_BUILD || job.kind == J_RUN || job.kind == J_FLAT || job.kind == J_PYDBG)
			job_stop();
		dbg_end();
		then_do = 0;
		variable_count = stack_count = 0;
		say("stopped");
		draw_all();
		return;
	case C_OVER:
	case C_INTO:
	case C_OUT:
		if (run_doc != doc_now) {
			doc_show(run_doc);
			draw_all();
		}
		if (py_state == DBG_STOPPED)
			py_send(id == C_OVER ? "over" : id == C_INTO ? "into" : "out");
		else
			dbg_step(id == C_OVER ? DBG_OVER : id == C_INTO ? DBG_INTO : DBG_OUT);
		draw_tools();
		return;
	case C_MARK:
		for (i = 0; i < dbg_mark_count && dbg_marks[i] != tx_y + 1; i++)
			continue;
		if (i < dbg_mark_count)
			dbg_marks[i] = dbg_marks[--dbg_mark_count];
		else if (dbg_mark_count < (int)(sizeof dbg_marks / sizeof dbg_marks[0]))
			dbg_marks[dbg_mark_count++] = tx_y + 1;
		draw_line(tx_y);
		return;
	case C_UNMARK_ALL:
		dbg_mark_count = 0;
		draw_editor_from(top);
		return;
	case C_KEYS:
	case C_ABOUT:
		help(id == C_ABOUT);
		return;
	default:
		if (id >= C_EXAMPLE && id < C_EXAMPLE + example_count) {
			open_example(id - C_EXAMPLE);
			draw_all();
		}
		return;
	}
	changed(was, follow() || tx_marked || id == C_UNDO || id == C_REDO);
	match_update();
	if (tools_now() != tools_shown)
		draw_tools_some(0);
	draw_status();
}

/* A command, from a menu, a key of the row, the keyboard or a test: with NXIDE_PRINT, what it left. */
static void
command(int id)
{
	command_done(id);
	if (print_on) {
		printf("nxide: command %d: at %d,%d of %d lines, %s, %d to take back, %d to make again\n", id, tx_x, tx_y, tx_count,
		       tx_modified ? "changed" : "as saved", tx_done_count, tx_undone_count);
		fflush(stdout);
	}
}

/* ---- keys ---- */

/* A key of the line the status bar asks with. */
static void
ask_key(int ch)
{
	int n = (int)strlen(answer), what = asking;

	if (ch == MWKEY_ESCAPE) {
		asking = ASK_NONE;
	} else if (ch == MWKEY_ENTER || ch == '\r' || ch == '\n') {
		asking = ASK_NONE;
		if (what == ASK_FIND) {
			snprintf(sought, sizeof sought, "%.63s", answer);
			command(C_FIND_NEXT);
			return;
		}
		if (what == ASK_REPLACE) {
			snprintf(sought, sizeof sought, "%.63s", answer);
			ask(ASK_WITH, replacement);
			return;
		}
		if (what == ASK_WITH) {
			char text[160];
			int was = tx_y, count;

			snprintf(replacement, sizeof replacement, "%.63s", answer);
			tx_step++;
			count = tx_replace_all(sought, replacement);
			snprintf(text, sizeof text, "%d replaced", count);
			say(text);
			changed(was, follow() || 1);
			match_update();
		}
		if (what == ASK_GOTO)
			go_to_line(atoi(answer));
		if (what == ASK_SAVE && answer[0] == '/') {
			named(answer);
			save();
		}
	} else if (ch == MWKEY_BACKSPACE || ch == 127) {
		if (n)
			answer[n - 1] = 0;
	} else if (ch >= 32 && ch < 127 && n < (int)sizeof answer - 1) {
		answer[n] = (char)ch, answer[n + 1] = 0;
	}
	draw_status();
}

static void
key(int ch, int modifiers)
{
	int was = tx_y, length = (int)strlen(tx_lines[tx_y]), shift = (modifiers & MWKMOD_SHIFT) != 0, moves = 0, n, id = 0;
	int x0, y0, x1, y1, had = tx_selection(&x0, &y0, &x1, &y1), was_marked = tx_marked, was_ax = tx_ax, was_ay = tx_ay, was_left;
	char one;

	if (asking) {
		ask_key(ch);
		return;
	}
	/* (Ctrl+H's control code is Backspace's) */
	if ((modifiers & MWKMOD_CTRL) && (ch == 'h' || ch == 'H')) {
		command(C_REPLACE);
		return;
	}
	if ((modifiers & MWKMOD_CTRL) && (ch == MWKEY_TAB || ch == MWKEY_PAGEUP || ch == MWKEY_PAGEDOWN)) {
		command(ch == MWKEY_PAGEUP || (ch == MWKEY_TAB && shift) ? C_PREV_TAB : C_NEXT_TAB);
		return;
	}
	/* a letter with Ctrl comes as the letter with the modifier, or as its control code */
	if ((modifiers & MWKMOD_CTRL) && ch < 128 && (isalpha(ch) || ch == '/'))
		ch = ch == '/' ? 31 : tolower(ch) - 'a' + 1;
	switch (ch) {
	case 1: id = C_ALL; break;
	case 2: id = C_TREE; break;
	case 3: id = C_COPY; break;
	case 4: id = C_WORD; break;
	case 6: id = C_FIND; break;
	case 7: id = C_GOTO; break;
	case 12: id = C_LINE; break;
	case 14: id = C_NEW; break;
	case 15: id = C_OPEN; break;
	case 17: id = C_EXIT; break;
	case 19: id = shift ? C_SAVE_ALL : C_SAVE; break;
	case 22: id = C_PASTE; break;
	case 23: id = C_CLOSE; break;
	case 24: id = C_CUT; break;
	case 25: id = C_REDO; break;
	case 26: id = C_UNDO; break;
	case 31: id = C_COMMENT; break;
	case MWKEY_F3: id = shift ? C_FIND_PREV : C_FIND_NEXT; break;
	case MWKEY_F5: id = shift ? C_STOP : (modifiers & MWKMOD_CTRL) ? C_RUN : C_DEBUG; break;
	case MWKEY_F6: id = C_RUN; break;
	case MWKEY_F7: id = C_BUILD; break;
	case MWKEY_F9: id = C_MARK; break;
	case MWKEY_F10: id = C_OVER; break;
	case MWKEY_F11: id = shift ? C_OUT : C_INTO; break;
	case MWKEY_F12: id = C_OUT; break;
	}
	if (id) {
		command(id);
		return;
	}
	tx_step++;
	notice[0] = 0;
	switch (ch) {
	case MWKEY_LEFT: case MWKEY_RIGHT: case MWKEY_UP: case MWKEY_DOWN: case MWKEY_HOME: case MWKEY_END: case MWKEY_PAGEUP: case MWKEY_PAGEDOWN:
		moves = 1;
		if (shift && !tx_marked)
			tx_ax = tx_x, tx_ay = tx_y, tx_marked = 1;
		if (!shift)
			tx_marked = 0;
		if (ch == MWKEY_LEFT) {
			if (tx_x > 0)
				tx_x--;
			else if (tx_y > 0)
				tx_y--, tx_x = (int)strlen(tx_lines[tx_y]);
		} else if (ch == MWKEY_RIGHT) {
			if (tx_x < length)
				tx_x++;
			else if (tx_y + 1 < tx_count)
				tx_y++, tx_x = 0;
		} else if (ch == MWKEY_UP || ch == MWKEY_DOWN || ch == MWKEY_PAGEUP || ch == MWKEY_PAGEDOWN) {
			tx_y += ch == MWKEY_UP ? -1 : ch == MWKEY_DOWN ? 1 : ch == MWKEY_PAGEUP ? -rows : rows;
			tx_y = tx_y < 0 ? 0 : tx_y >= tx_count ? tx_count - 1 : tx_y;
			if (tx_x > (int)strlen(tx_lines[tx_y]))
				tx_x = (int)strlen(tx_lines[tx_y]);
		} else if (ch == MWKEY_HOME) {
			/* to the first letter, and from there to the line's start */
			n = (int)strspn(tx_lines[tx_y], " ");
			tx_x = (modifiers & MWKMOD_CTRL) ? 0 : tx_x == n ? 0 : n;
			if (modifiers & MWKMOD_CTRL)
				tx_y = 0;
		} else {
			if (modifiers & MWKMOD_CTRL)
				tx_y = tx_count - 1;
			tx_x = (int)strlen(tx_lines[tx_y]);
		}
		break;
	case MWKEY_ENTER: case '\n': {
		/* the new line begins where this one does, and a step further in after a brace */
		char lead[320] = "\n";

		tx_delete_selection();
		n = (int)strspn(tx_lines[tx_y], " ");
		n = n > tx_x ? tx_x : n;
		n = n > 150 ? 150 : n;
		memset(lead + 1, ' ', (size_t)n);
		if (sx_lang == SX_PY) {
			/* after a line that ends in a colon (spaces and a comment aside), a step further in */
			int k = tx_x;

			if (memchr(tx_lines[tx_y], '#', (size_t)tx_x))
				k = (int)((const char *)memchr(tx_lines[tx_y], '#', (size_t)tx_x) - tx_lines[tx_y]);
			while (k > 0 && tx_lines[tx_y][k - 1] == ' ')
				k--;
			if (k > 0 && tx_lines[tx_y][k - 1] == ':') {
				memset(lead + 1 + n, ' ', TX_TAB);
				n += TX_TAB;
			}
		} else if (tx_x > 0 && tx_lines[tx_y][tx_x - 1] == '{') {
			memset(lead + 1 + n, ' ', TX_TAB);
			/* between a pair of braces: the closing one goes to a line of its own under the opening one's */
			if (tx_lines[tx_y][tx_x] == '}') {
				int y = tx_y;

				lead[1 + n + TX_TAB] = '\n';
				memset(lead + 2 + n + TX_TAB, ' ', (size_t)n);
				tx_insert(tx_x, tx_y, lead, 2 * n + TX_TAB + 2);
				tx_x = n + TX_TAB, tx_y = y + 1;
				break;
			}
			n += TX_TAB;
		}
		tx_insert(tx_x, tx_y, lead, n + 1);
		break;
	}
	case MWKEY_BACKSPACE: case 127:
		if (tx_delete_selection())
			break;
		if (tx_x > 0) {
			/* in the spaces before a line's text, back to the stop before */
			n = (int)strspn(tx_lines[tx_y], " ") >= tx_x && tx_x % TX_TAB == 0 ? TX_TAB : 1;
			tx_delete(tx_x - n, tx_y, tx_x, tx_y);
		} else if (tx_y > 0)
			tx_delete((int)strlen(tx_lines[tx_y - 1]), tx_y - 1, 0, tx_y);
		break;
	case MWKEY_DELETE:
		if (tx_delete_selection())
			break;
		if (tx_x < length)
			tx_delete(tx_x, tx_y, tx_x + 1, tx_y);
		else if (tx_y + 1 < tx_count)
			tx_delete(tx_x, tx_y, 0, tx_y + 1);
		break;
	case MWKEY_TAB:
		if (tx_marked && tx_ay != tx_y) {
			indent(shift ? -TX_TAB : TX_TAB);
			break;
		}
		if (shift) {
			indent(-TX_TAB);
			break;
		}
		tx_delete_selection();
		tx_insert(tx_x, tx_y, "\t", 1);
		break;
	case MWKEY_ESCAPE:
		tx_marked = 0;
		break;
	default:
		if (ch < 32 || ch > 126)
			return;
		tx_delete_selection();
		/* a closing brace alone on its line goes back a step */
		if (ch == '}' && sx_lang != SX_PY && tx_x >= TX_TAB && (int)strspn(tx_lines[tx_y], " ") == length && tx_x == length)
			tx_delete(tx_x - TX_TAB, tx_y, tx_x, tx_y);
		one = (char)ch;
		tx_insert(tx_x, tx_y, &one, 1);
		tx_marked = 0;
	}
	if (!moves && ch != MWKEY_ESCAPE && !(tx_marked && (ch == MWKEY_TAB || ch == '\t')))
		tx_marked = 0;
	was_left = left;
	n = follow();
	if (moves && n && left == was_left && !had && !tx_selection(&x0, &y0, &x1, &y1))
		changed(was, 2);
	else if (moves && shift && !n && (!was_marked || (tx_ax == was_ax && tx_ay == was_ay))) {
		/* the selection grew or shrank at the cursor's end: the lines the cursor passed */
		for (n = was < tx_y ? was : tx_y; n <= (was < tx_y ? tx_y : was); n++)
			draw_line(n);
	} else
		changed(was, n || had || tx_selection(&x0, &y0, &x1, &y1));
	match_update();
	if (tools_now() != tools_shown)
		draw_tools_some(0);
	draw_status();
}

/* ---- the pointer ---- */

static void
place(int px, int py, int *x, int *y)
{
	int line = top + (py - editor_y()) / ui_fh, column = left + (px - gutter - 3 + ui_fw / 2) / ui_fw;

	line = line < 0 ? 0 : line >= tx_count ? tx_count - 1 : line;
	column = column < 0 ? 0 : column > (int)strlen(tx_lines[line]) ? (int)strlen(tx_lines[line]) : column;
	*x = column, *y = line;
}

/* A line of the output the compiler wrote ("FILE.c:LINE: ..."): that file, at that line. */
static void
output_clicked(const char *text)
{
	const char *at, *end;
	char file[300];
	int line = 0;

	/* an interpreter's: MicroPython's '  File "FILE", line N', QuickJS's "    at f (FILE:LINE:COLUMN)" */
	if ((at = strstr(text, "File \"")) != NULL && (end = strchr(at + 6, '"')) != NULL && strstr(end, ", line "))
		snprintf(file, sizeof file, "%.*s", (int)(end - at - 6), at + 6), line = atoi(strstr(end, ", line ") + 7);
	else if (!strncmp(text, "    at ", 7)) {
		at = strchr(text, '(') ? strchr(text, '(') + 1 : text + 7;
		end = strrchr(at, ':');
		while (end && end > at && end[-1] != ':' && isdigit((unsigned char)end[-1]))
			end--;
		if (end && end > at + 1 && end[-1] == ':')
			snprintf(file, sizeof file, "%.*s", (int)(end - 1 - at), at), line = atoi(end);
	}
	if (line > 0 && file[0] == '/') {
		if (strcmp(file, path) && access(file, R_OK) == 0 && !open_file(file))
			return;
		draw_all();
		go_to_line(line);
		return;
	}
	for (at = text; *at; at++)
		if (at[0] == ':' && isdigit((unsigned char)at[1]) && at - text > 2 && at[-2] == '.' && (at[-1] == 'c' || at[-1] == 'h')) {
			snprintf(file, sizeof file, "%.*s", (int)(at - text), text);
			if (strcmp(file, path) && access(file, R_OK) == 0 && !open_file(file))
				return;
			draw_all();
			go_to_line(atoi(at + 1));
			return;
		}
}

/* A click in the tree: a folder opens or closes, a file opens in its tab. */
static void
tree_clicked(int x, int y)
{
	int ly = side_y() + TREE_HEAD, shown = tree_shown(), k, i;

	if (y < ly) {
		if (x >= TREE_W - 22) {	/* (its close box) */
			project_close();
			draw_all();
		}
		return;
	}
	if (tree_count > shown && x >= TREE_W - 2 - UI_SCROLL_W) {
		tree_top = ui_scroll_to(y, ly + 2, panel_y() - ly - 6, tree_count, shown);
		draw_tree();
		return;
	}
	k = tree_top + (y - ly - 2) / TREE_ROW;
	if (k < 0 || k >= tree_count || y < ly + 2 || (y - ly - 2) / TREE_ROW >= shown)
		return;
	if (tree[k].folder) {
		for (i = 0; i < tree_opened_count && strcmp(tree_opened[i], tree[k].path); i++)
			continue;
		if (i < tree_opened_count)
			memmove(tree_opened[i], tree_opened[i + 1], (size_t)(--tree_opened_count - i) * sizeof tree_opened[0]);
		else if (tree_opened_count < (int)(sizeof tree_opened / sizeof tree_opened[0]))
			strcpy(tree_opened[tree_opened_count++], tree[k].path);
		tree_read();
		draw_tree();
		return;
	}
	if (!looks_like_text(tree[k].path)) {
		say("not a text file");
		draw_status();
		return;
	}
	open_file(tree[k].path);
	draw_all();
}

static void
pressed(GR_EVENT *event)
{
	int x = event->button.x, y = event->button.y, i, shown = (PANEL_H - TAB_H - 2) / 15, closing;

	if (ui_wheel(event)) {
		int by = 3 * ui_wheel_sum(event);

		if (tree_on && x < TREE_W && y >= side_y() && y < panel_y()) {
			tree_top += by;
			tree_top = tree_top > tree_count - tree_shown() ? tree_count - tree_shown() : tree_top;
			tree_top = tree_top < 0 ? 0 : tree_top;
			draw_tree();
		} else if (panel_on && y >= panel_y()) {
			int *first = panel == P_OUTPUT ? &out_top : panel == P_PROBLEMS ? &problem_top : &variable_top;
			int count = panel == P_OUTPUT ? out_count : panel == P_PROBLEMS ? problem_count : variable_count;

			*first += by;
			*first = *first > count - shown ? count - shown : *first;
			*first = *first < 0 ? 0 : *first;
			draw_panel();
		} else
			scroll_to(top + by);
		return;
	}
	/* the middle button closes the tab it is pressed on */
	if ((event->button.changebuttons & GR_BUTTON_M) && y >= side_y() && y < editor_y() && x >= ex && (i = doc_tab_at(x, &closing)) >= 0) {
		close_tab(i);
		return;
	}
	if (!(event->button.changebuttons & GR_BUTTON_L))
		return;
	if (y < MENU_H) {
		for (i = 0; i < 6; i++)
			if (x >= menu_x[i] && x < menu_x[i + 1])
				menu_open(i, 0);
	} else if (y < side_y()) {
		for (i = 0; i < TOOLS; i++)
			if (x >= tool_x[i] && x < tool_x[i] + tool_w[i])
				command(tools[i].id);
	} else if (y >= height - STATUS) {
		return;
	} else if (panel_on && y >= panel_y()) {
		if (y < panel_y() + TAB_H) {
			if (x < 4 + PANELS * 96)
				show_panel((x - 4) / 96);
			return;
		}
		i = (y - panel_y() - TAB_H - 2) / 15;
		if (panel == P_PROBLEMS && problem_top + i >= 0 && problem_top + i < problem_count) {
			if (problems[problem_top + i].line)
				go_to_line(problems[problem_top + i].line);
			else
				output_clicked(problems[problem_top + i].text);	/* (the header, at its line) */
		}
		else if (panel == P_OUTPUT && out_top + i >= 0 && out_top + i < out_count)
			output_clicked(out_lines[out_top + i]);
		else if (panel == P_STACK && i >= 0 && i < stack_count && doc_now == run_doc && strstr(stack[i], ", line "))
			go_to_line(atoi(strstr(stack[i], ", line ") + 7));
	} else if (tree_on && x < TREE_W + 3) {
		if (x < TREE_W)
			tree_clicked(x, y);
	} else if (y < editor_y()) {
		if ((i = doc_tab_at(x, &closing)) < 0)
			return;
		if (closing)
			close_tab(i);
		else {
			doc_show(i);
			draw_all();
		}
	} else if (x >= width - UI_SCROLL_W) {
		dragging = 1;
		scroll_to(ui_scroll_to(y, editor_y(), panel_y() - editor_y(), tx_count, rows));
	} else if (x < gutter) {
		/* before a line's number: its mark */
		place(gutter + 3, y, &tx_x, &tx_y);
		command(C_MARK);
		draw_editor_from(top);
	} else {
		unsigned now = milliseconds();
		int x0, y0, x1, y1, had = tx_selection(&x0, &y0, &x1, &y1), was = tx_y;

		place(x, y, &tx_x, &tx_y);
		if (now - click_at < 500 && click_x == tx_x && click_y == tx_y) {
			select_word();
			click_at = 0;
		} else {
			tx_ax = tx_x, tx_ay = tx_y, tx_marked = 1;
			selecting = 1;
			click_at = now, click_x = tx_x, click_y = tx_y;
		}
		if (had || tx_ax != tx_x)
			draw_editor_from(top);
		else
			draw_line(was), draw_line(tx_y);
		match_update();
		draw_status();
	}
}

/* ---- a test's hands: NXIDE_SCRIPT="mark:14 debug over over go quit" ---- */

/* The whole text as one piece, for comparing; the caller frees it. */
static char *
whole_text(void)
{
	int length;

	return tx_text(0, 0, (int)strlen(tx_lines[tx_count - 1]), tx_count - 1, &length);
}

/* What must always hold of the cursor and the selection. */
static void
sound_or_die(const char *after, int step)
{
	int bad = tx_count < 1 || tx_y < 0 || tx_y >= tx_count || tx_x < 0 || tx_x > (int)strlen(tx_lines[tx_y]) ||
		  (tx_marked && (tx_ay < 0 || tx_ay >= tx_count || tx_ax < 0 || tx_ax > (int)strlen(tx_lines[tx_ay]))) || top < 0 || left < 0;

	if (!bad)
		return;
	printf("nxide: storm: WRONG after %s at step %d: cursor %d,%d of %d lines, mark %d at %d,%d\n", after, step, tx_x, tx_y, tx_count,
	       tx_marked, tx_ax, tx_ay);
	fflush(stdout);
	exit(3);
}

/* `steps` keys and commands chosen by a fixed sequence of chances, through the same two
 * functions a hand's go through; then every change taken back must leave the text as it
 * was, and every one made again the text as the storm left it. */
static void
storm(int steps)
{
	static const int moves[] = { MWKEY_LEFT, MWKEY_RIGHT, MWKEY_UP, MWKEY_DOWN, MWKEY_HOME, MWKEY_END, MWKEY_PAGEUP, MWKEY_PAGEDOWN };
	static const int commands[] = { C_COMMENT, C_INDENT, C_UNINDENT, C_CUT, C_COPY, C_PASTE, C_LINE, C_WORD, C_UNDO, C_REDO, C_UNDO, C_FIND_NEXT };
	static const char letters[] = "abc xyz_01 (){};=+*/\"'#<>.,";
	char *before, *after, *now;
	unsigned chance = 12345;
	int i, taken = 0, again = 0, earlier = 0;

	/* "as it was" is the text with the whole history taken back, changes before the storm too */
	while (tx_done_count && earlier < 100000)
		command(C_UNDO), earlier++;
	before = whole_text();
	while (earlier-- > 0)
		command(C_REDO);
	snprintf(sought, sizeof sought, "int");
	for (i = 0; i < steps; i++) {
		unsigned r;

		chance = chance * 1103515245u + 12345u;
		r = chance >> 8;
		if (r % 100 < 45)
			key(letters[r / 100 % (sizeof letters - 1)], 0);
		else if (r % 100 < 55)
			key(MWKEY_ENTER, 0);
		else if (r % 100 < 63)
			key(r / 100 % 2 ? MWKEY_BACKSPACE : MWKEY_DELETE, 0);
		else if (r % 100 < 85)
			key(moves[r / 100 % 8], r / 1000 % 3 == 0 ? MWKMOD_SHIFT : 0);
		else if (r % 100 < 88)
			key(MWKEY_TAB, r / 100 % 2 ? MWKMOD_SHIFT : 0);
		else if (r % 100 < 99)
			command(commands[r / 100 % 12]);
		else
			command(C_ALL);
		sound_or_die("a key", i);
	}
	after = whole_text();
	while (tx_done_count && taken < 100000) {
		command(C_UNDO);
		sound_or_die("undo", taken++);
	}
	now = whole_text();
	printf("nxide: storm: %d steps; %d changes taken back: the text is %s\n", steps, taken, strcmp(now, before) ? "NOT as it was" : "as it was");
	free(now);
	while (tx_undone_count && again < 100000) {
		command(C_REDO);
		sound_or_die("redo", again++);
	}
	now = whole_text();
	printf("nxide: storm: %d made again: the text is %s\n", again, strcmp(now, after) ? "NOT as the storm left it" : "as the storm left it");
	free(now), free(before), free(after);
	fflush(stdout);
}

/* A key by its name, for the script: "up", "s-down" (with Shift), "enter", "tab", "f9"... */
static void
named_key(const char *name)
{
	static const struct { const char *name; int ch; } keys[] = {
		{ "up", MWKEY_UP }, { "down", MWKEY_DOWN }, { "left", MWKEY_LEFT }, { "right", MWKEY_RIGHT }, { "home", MWKEY_HOME },
		{ "end", MWKEY_END }, { "pageup", MWKEY_PAGEUP }, { "pagedown", MWKEY_PAGEDOWN }, { "delete", MWKEY_DELETE },
		{ "backspace", MWKEY_BACKSPACE }, { "tab", MWKEY_TAB }, { "enter", MWKEY_ENTER }, { "escape", MWKEY_ESCAPE },
		{ "f3", MWKEY_F3 }, { "f5", MWKEY_F5 }, { "f6", MWKEY_F6 }, { "f7", MWKEY_F7 }, { "f9", MWKEY_F9 }, { "f10", MWKEY_F10 },
		{ "f11", MWKEY_F11 }, { "f12", MWKEY_F12 },
	};
	int shift = !strncmp(name, "s-", 2), control = !strncmp(name, "c-", 2), i;

	name += shift || control ? 2 : 0;
	if (control && name[0] && !name[1]) {
		key(name[0], MWKMOD_CTRL);
		return;
	}
	for (i = 0; i < (int)(sizeof keys / sizeof keys[0]); i++)
		if (!strcmp(name, keys[i].name))
			key(keys[i].ch, shift ? MWKMOD_SHIFT : control ? MWKMOD_CTRL : 0);
}

static void
scripted(void)
{
	static const struct { const char *word; int id; } words[] = {
		{ "build", C_BUILD }, { "run", C_RUN }, { "debug", C_DEBUG }, { "go", C_DEBUG }, { "over", C_OVER }, { "into", C_INTO },
		{ "out", C_OUT }, { "stop", C_STOP }, { "undo", C_UNDO }, { "redo", C_REDO }, { "save", C_SAVE }, { "quit", C_EXIT },
		{ "comment", C_COMMENT }, { "all", C_ALL }, { "indent", C_INDENT }, { "unindent", C_UNINDENT }, { "new", C_NEW },
		{ "cut", C_CUT }, { "copy", C_COPY }, { "paste", C_PASTE }, { "word", C_WORD }, { "panel", C_PANEL }, { "numbers", C_NUMBERS },
		{ "checking", C_CHECKING }, { "keys", C_KEYS }, { "about", C_ABOUT }, { "unmark", C_UNMARK_ALL }, { "next", C_FIND_NEXT },
		{ "close", C_CLOSE }, { "tab", C_NEXT_TAB }, { "tree", C_TREE }, { "saveall", C_SAVE_ALL }, { "back", C_FIND_PREV },
	};
	char word[256];	/* (open: and project: take a path) */
	int n, i;

	if (!script[0] || job.kind == J_BUILD || job.kind == J_RUN || dbg_state == DBG_RUNNING || then_do || (job.kind == J_PYDBG && py_state != DBG_STOPPED))
		return;
	n = (int)strcspn(script, " ");
	snprintf(word, sizeof word, "%.*s", n, script);
	memmove(script, script + n + (script[n] == ' '), strlen(script + n + (script[n] == ' ')) + 1);
	if (print_on)
		printf("nxide: script: %s\n", word);
	if (!strncmp(word, "mark:", 5)) {
		tx_y = atoi(word + 5) - 1;
		command(C_MARK);
	} else if (!strncmp(word, "type:", 5)) {
		for (i = 5; word[i]; i++)
			key(word[i] == '_' ? ' ' : word[i] == '|' ? MWKEY_ENTER : word[i], 0);
	} else if (!strncmp(word, "line:", 5)) {
		go_to_line(atoi(word + 5));
	} else if (!strncmp(word, "example:", 8)) {
		for (i = 0; i < example_count; i++) {
			char named_as[48];

			snprintf(named_as, sizeof named_as, "%.39s.%.5s", examples[i], example_ends[i]);
			if (!strcmp(examples[i], word + 8) || !strcmp(named_as, word + 8)) {
				command(C_EXAMPLE + i);
				break;
			}
		}
		printf("nxide: opened %s: %d lines, cursor %d,%d\n", path, tx_count, tx_x, tx_y);
	} else if (!strncmp(word, "project:", 8)) {
		project_open(word + 8, 0);
		draw_all();
		printf("nxide: project %s: %d rows in its tree\n", project[0] ? project : "(none)", tree_count);
	} else if (!strcmp(word, "noproject")) {
		command(C_PROJECT_CLOSE);
	} else if (!strncmp(word, "open:", 5)) {
		open_file(word + 5);
		draw_all();
		printf("nxide: opened %s: %d lines\n", path, tx_count);
	} else if (!strncmp(word, "find:", 5)) {
		snprintf(sought, sizeof sought, "%.63s", word + 5);
		command(C_FIND_NEXT);
		printf("nxide: find %s: cursor %d,%d\n", sought, tx_x, tx_y);
	} else if (!strncmp(word, "key:", 4)) {
		named_key(word + 4);
	} else if (!strncmp(word, "menu:", 5)) {
		menu_open(atoi(word + 5), 1);
		ui_menu_close();
	} else if (!strncmp(word, "replace:", 8) && strchr(word + 8, '=')) {
		/* replace:OLD=NEW, as Ctrl+H asks it */
		snprintf(sought, sizeof sought, "%.*s", (int)(strchr(word + 8, '=') - word - 8), word + 8);
		snprintf(answer, sizeof answer, "%s", strchr(word + 8, '=') + 1);
		asking = ASK_WITH;
		ask_key(MWKEY_ENTER);
		printf("nxide: %s\n", notice);
	} else if (!strcmp(word, "tabs")) {
		for (i = 0; i < doc_count; i++)
			printf("nxide: tab %d: %s%s\n", i, i == doc_now ? path : docs[i].path, i == doc_now ? " (shown)" : "");
	} else if (!strncmp(word, "storm:", 6)) {
		storm(atoi(word + 6));
	} else if (!strcmp(word, "where")) {
		printf("nxide: at %d,%d of %d lines, %s, %d to take back, %d to make again, %d problems\n", tx_x, tx_y, tx_count,
		       tx_modified ? "changed" : "as saved", tx_done_count, tx_undone_count, problem_count);
	} else if (!strcmp(word, "text")) {
		for (i = 0; i < tx_count; i++)
			printf("nxide: text %d: %s\n", i + 1, tx_lines[i]);
	} else if (!strcmp(word, "problems")) {
		for (i = 0; i < problem_count; i++)
			printf("nxide: problem: line %d: %s\n", problems[i].line, problems[i].text);
	} else if (!strcmp(word, "wait")) {
		usleep(2500000);
	} else
		for (i = 0; i < (int)(sizeof words / sizeof words[0]); i++)
			if (!strcmp(word, words[i].word))
				command(words[i].id);
	fflush(stdout);
}

static void
child_changed(int signal)
{
	(void)signal;
}

/* How long the loop may sleep (0: until something happens). What runs beside wakes it by its
 * output and by SIGCHLD; a wait is only for a signal that came just before the sleep, for the
 * check that is due, and for a script's next word. */
static GR_TIMEOUT
wait_ms(void)
{
	unsigned since;
	int beside = (job.kind != J_NONE && py_state != DBG_STOPPED) || dbg_state == DBG_RUNNING, fresh = edited_now;

	/* (the clock is asked once a key: a question to the kernel each time) */
	if (edited_now)
		edited_at = milliseconds(), edited_now = 0;
	if (script[0])
		return beside || debug_fd >= 0 ? 40 : 300;
	if (beside)
		return job.kind == J_CHECK ? 2000 : 250;	/* (a check's end is heard at its pipe's) */
	if (!checking_on || !unchecked || dbg_state != DBG_GONE || py_state != DBG_GONE)
		return 0;
	if (fresh)
		return 1502;
	since = milliseconds() - edited_at;
	return since > 1500 ? 1 : 1502 - since;
}

static int
by_name(const void *a, const void *b)
{
	return strcmp(a, b);
}

/* Where the menus' names and the keys are along their rows, in the look's font. */
static void
layout(void)
{
	int i, x;

	for (i = 0, x = 4; i < 6; i++) {
		menu_x[i] = x;
		x += ui_width(&ui_var_widths, ui_var, menu_names[i], -1) + 16;
	}
	menu_x[6] = x;
	for (i = 0, x = 4; i < TOOLS; i++) {
		x += tools[i].gap;
		tool_x[i] = x, tool_w[i] = ui_width(&ui_var_widths, ui_var, tools[i].id == C_DEBUG ? "Go on" : tools[i].label, -1) + 29;
		x += tool_w[i] + 2;
	}
	tools_shown = -1;
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	DIR *share = opendir("/usr/share");
	struct dirent *entry;
	struct sigaction signal_child = { 0 };
	struct stat is;
	int i, chosen;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	text_gc = GrNewGC(), ground_gc = GrNewGC(), bar_gc = GrNewGC();
	GrSetGCUseBackground(text_gc, GR_FALSE);
	(GrSetGCFont)(text_gc, ui_fixed);
	(GrSetGCForeground)(ground_gc, WHITE);
	(GrSetGCForeground)(bar_gc, BLACK);
	clean_load();
	/* the desktop's bar ignores its children's ends, and whatever it starts inherits that: a
	 * build would then end unseen (waitpid finds no child), and the IDE wait for it for ever.
	 * A child's end or stop also ends the loop's wait (wait_ms), so nothing is polled for. */
	signal_child.sa_handler = child_changed;
	signal_child.sa_flags = SA_RESTART;
	sigaction(SIGCHLD, &signal_child, NULL);
	print_on = getenv("NXIDE_PRINT") != NULL;
	scroll_drawn = getenv("NXIDE_SCROLL") && !strcmp(getenv("NXIDE_SCROLL"), "draw");
	if (getenv("NXIDE_SCRIPT"))
		snprintf(script, sizeof script, "%s", getenv("NXIDE_SCRIPT"));
	scripted_run = script[0] != 0;
	/* the examples: example-NAME.c, .js, .py and .hlsl, C's first, each language's by name */
	while (share && (entry = readdir(share)) != NULL && example_count < 12) {
		const char *dot = strrchr(entry->d_name, '.');

		if (!strncmp(entry->d_name, "example-", 8) && strlen(entry->d_name) < 40 && dot && (!strcmp(dot, ".c") || !strcmp(dot, ".js") || !strcmp(dot, ".py") || !strcmp(dot, ".hlsl")))
			snprintf(examples[example_count++], sizeof examples[0], "%d%s", dot[1] == 'c' ? 0 : dot[1] == 'j' ? 1 : dot[1] == 'p' ? 2 : 3,
				 entry->d_name + 8);
	}
	if (share)
		closedir(share);
	qsort(examples, (size_t)example_count, sizeof examples[0], by_name);
	for (i = 0; i < example_count; i++) {
		char *dot = strrchr(examples[i], '.');

		snprintf(example_ends[i], sizeof example_ends[0], "%s", dot + 1);
		*dot = 0;
		memmove(examples[i], examples[i] + 1, strlen(examples[i]));
		snprintf(example_labels[i], sizeof example_labels[0], "%s%s", examples[i],
			 example_ends[i][0] == 'j' ? " (JavaScript)" : example_ends[i][0] == 'p' ? " (Python)" : example_ends[i][0] == 'h' ? " (HLSL)" : "");
		example_items[i].label = example_labels[i];
		example_items[i].icon = -1;
		example_items[i].id = C_EXAMPLE + i;
	}
	layout();
	flat_machine = access("/usr/bin/elf2flt", X_OK) == 0;
	if (width > ui_screen()->cols - 16)
		width = ui_screen()->cols - 16;
	if (height > ui_screen()->rows - UI_MENU_BAR - 40)
		height = ui_screen()->rows - UI_MENU_BAR - 40;
	for (i = 0; i < TOOLS && print_on; i++)
		printf("nxide: key %s at %d\n", tools[i].label, tool_x[i] + tool_w[i] / 2);
	for (i = 0; i < 6 && print_on; i++)
		printf("nxide: menu %s at %d\n", menu_names[i], (menu_x[i] + menu_x[i + 1]) / 2);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "IDE", GR_ROOT_WINDOW_ID, -1, -1, width, height, WHITE);
	GrSelectEvents(window, EVENTS);
	resized(width, height);
	/* a folder is the project, a file is opened; with neither, the project of the last exit (not
	 * for a test's run) and the hello example. A project's own first source is shown, if it has one. */
	if (argc > 1 && stat(argv[1], &is) == 0 && S_ISDIR(is.st_mode))
		project_open(argv[1], 1);
	else if (argc == 1 && !scripted_run && reg_get(PROJECT_KEY, NULL))
		project_open(reg_get(PROJECT_KEY, ""), 0);
	if (argc > 1 && !project[0])
		open_file(argv[1]);
	for (i = 0; project[0] && !doc_count && i < tree_count; i++) {
		const char *dot = strrchr(tree[i].path, '.');

		if (!tree[i].folder && !tree[i].depth && dot && (!strcmp(dot, ".c") || !strcmp(dot, ".js") || !strcmp(dot, ".py") || !strcmp(dot, ".hlsl")))
			open_file(tree[i].path);
	}
	if (!doc_count) {
		for (i = 0; i < example_count && strcmp(examples[i], "hello"); i++)
			continue;
		if (i < example_count)
			open_example(i);
		else
			fresh_doc();
	}
	GrMapWindow(window);
	GrSetFocus(window);
	for (;;) {
		GrGetNextEventTimeout(&event, wait_ms());
		ui_origin_note(&event);
		switch (ui_menu_event(&event, &chosen)) {
		case UI_MENU_CHOSEN:
			command(chosen);
			continue;
		case UI_MENU_TOOK:
		case UI_MENU_CLOSED:
			continue;
		}
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw_all();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.utype == GR_UPDATE_SIZE) {
				resized(event.update.width, event.update.height);
				draw_all();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			pressed(&event);
			ui_moves(window, EVENTS, dragging || selecting);	/* (the pointer's moves, while it selects or scrolls) */
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			dragging = selecting = 0;
			ui_moves(window, EVENTS, 0);
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (dragging && (event.mouse.buttons & GR_BUTTON_L)) {
				scroll_to(ui_scroll_to(event.mouse.y, editor_y(), panel_y() - editor_y(), tx_count, rows));
			} else if (selecting && (event.mouse.buttons & GR_BUTTON_L)) {
				int nx, ny, from, to;

				place(event.mouse.x, event.mouse.y < panel_y() ? event.mouse.y : panel_y() - 1, &nx, &ny);
				if (nx != tx_x || ny != tx_y) {
					from = ny < tx_y ? ny : tx_y, to = ny < tx_y ? tx_y : ny;
					tx_x = nx, tx_y = ny;
					for (i = from; i <= to; i++)
						draw_line(i);
					draw_status();
				}
			} else {
				dragging = selecting = 0;
				ui_moves(window, EVENTS, 0);
			}
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(event.keystroke.ch, event.keystroke.modifiers);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			command(C_EXIT);
			break;
		}
		poll_all();
		scripted();
	}
}
