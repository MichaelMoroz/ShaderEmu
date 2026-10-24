/*
 * nxedit: a text editor, a file a tab. nxedit FILE... opens or creates them; without one the
 * text has no file until Save asks for a name. Its menus (File, Edit, Search, View, and the
 * right button's) say the keys. A text with changes is asked about before it is let go.
 */
#define _GNU_SOURCE
#include "ui.h"
#include "ui_files.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include "nxedit_syntax.h"
#include "nxedit_dialog.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <time.h>
#include <sys/stat.h>

#define MENU_H 20
#define TABS_H 22
#define STATUS 20
#define MARGIN 3
#define DOCS 16
#define UNDO_MOST 40000		/* changes kept; past it the older half go */
#define BLOCK (256 * 1024)	/* a file is read in blocks this big, its lines left where they lie */
#define COLOUR_MOST (1 << 20)	/* a C file larger than this is shown without colours */
#define LEX_MOST 4096		/* nor is a line longer than this */
#define WIDEST 1024		/* columns a row can show */
#define RECENT 6
#define FAR (INT_MAX / 2)
#define CONT(c) (((unsigned char)(c) & 0xc0) == 0x80)	/* a byte inside a UTF-8 character */
/* (the pointer's moves are asked for while a button is held: ui_moves) */
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_BUTTON_DOWN | \
	GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_CLOSE_REQ)

struct line { char *text; int length, room; };	/* room 0: the text is not its own (a block's, or "") */
/* What was done, to undo: text put in from (x0, y0) to (x1, y1), or text taken out at (x0, y0).
 * The changes of one command share a group, which is undone whole. */
struct change { int inserted, joined, sealed, x0, y0, x1, y1, length, group; char *text; };

struct doc {
	struct line *lines;
	int count, room;
	char **blocks;
	int block_count, block_room;
	int cx, cy;		/* the cursor: byte, line */
	int ax, ay, marked;	/* where a selection began, when there is one: it ends at the cursor */
	int want;		/* the column Up and Down keep to, or -1 */
	int top, sub, left;	/* the first line shown, its first row (wrapped), the first column (not) */
	int named, crlf, final_newline, readonly, c_code;
	char path[512];
	struct change *undo, *redo;
	int undo_count, undo_room, redo_count, redo_room, saved_group;
	unsigned char *carry;	/* carry[y]: line y begins inside a comment; known up to carry_known */
	int carry_known, carry_room;
	int reading, read_y;	/* shown as Markdown read (nxedit_read.h), from the page's row read_y */
	char **back;		/* the texts read in this tab before, for Back */
	int back_count;
};

static struct doc docs[DOCS], *d;
static int doc_count, current;
static char empty[1];

/* what View says, kept in the registry */
static int wrap, numbers = 1, tab_width = 4, whitespace, colour = 1, line_lit = 1, overwrite;

static GR_WINDOW_ID window;
static int width, height, rows, columns, gutter, text_top;
static char notice[200];
static int scroll_drawn;	/* NXEDIT_SCROLL=draw: a view that moved is drawn whole */
static int group_next = 1, group_now, turning;	/* turning: 1 a change is being undone, 2 redone */

static void
say(const char *text)
{
	snprintf(notice, sizeof notice, "%s", text);
}

static void *
grow(void *array, int *room, int need, int size)
{
	int n = *room ? *room : 16;
	void *to;

	if (need <= *room)
		return array;
	while (n < need)
		n *= 2;
	if ((to = realloc(array, (size_t)n * size)) == NULL) {
		fprintf(stderr, "nxedit: out of memory\n");
		exit(1);
	}
	*room = n;
	return to;
}

/* ---- what is to be drawn again ---- */

static int dirty_from = FAR, dirty_to = -1, dirty_all, status_dirty, tabs_dirty;

static void
dirty(int a, int b)
{
	if (a > b) {
		int t = a;

		a = b, b = t;
	}
	if (a < dirty_from)
		dirty_from = a;
	if (b > dirty_to)
		dirty_to = b;
	status_dirty = 1;
}

/* ---- the lines ---- */

static unsigned lex_gen;	/* changes at every edit: a line coloured before may be another */

static void
edited(int y)
{
	if (d->carry_known > y)
		d->carry_known = y;
	lex_gen++;
}

/* Line y's text in memory of its own, with room for `need` bytes (its first ones kept). */
static char *
own(int y, int need)
{
	struct line *l = &d->lines[y];
	int room = need + need / 2 + 16;
	char *text;

	if (l->room > need)
		return l->text;
	if (l->room)
		text = realloc(l->text, room);
	else if ((text = malloc(room)) != NULL)
		memcpy(text, l->text, l->length < need ? l->length : need);
	if (!text) {
		fprintf(stderr, "nxedit: out of memory\n");
		exit(1);
	}
	l->text = text, l->room = room;
	return text;
}

static void
drop_line(struct line *l)
{
	if (l->room)
		free(l->text);
}

/* n empty lines in at `at`. */
static void
open_lines(int at, int n)
{
	int i;

	d->lines = grow(d->lines, &d->room, d->count + n, sizeof *d->lines);
	memmove(d->lines + at + n, d->lines + at, (d->count - at) * sizeof *d->lines);
	for (i = 0; i < n; i++)
		d->lines[at + i].text = empty, d->lines[at + i].length = d->lines[at + i].room = 0;
	d->count += n;
}

static void
add_line(char *text, int length, int room)
{
	if (d->count == 0 && length && text[length - 1] == '\r')
		d->crlf = 1;
	if (d->crlf && length && text[length - 1] == '\r')
		length--;
	d->lines = grow(d->lines, &d->room, d->count + 1, sizeof *d->lines);
	d->lines[d->count].text = text, d->lines[d->count].length = length, d->lines[d->count].room = room;
	d->count++;
}

/* ---- the changes, which everything else is made of ---- */

static void
push(struct change **stack, int *n, int *room, struct change c)
{
	int half = *n / 2, i;

	if (*n >= UNDO_MOST) {
		for (i = 0; i < half; i++)
			free((*stack)[i].text);
		memmove(*stack, *stack + half, (*n - half) * sizeof **stack);
		*n -= half;
	}
	*stack = grow(*stack, room, *n + 1, sizeof **stack);
	(*stack)[(*n)++] = c;
}

static void
forget(struct change *stack, int *n)
{
	while (*n)
		free(stack[--*n].text);
}

static void
begin(void)
{
	group_now = group_next++;
}

/* The last change is not grown by the next letter typed. */
static void
seal(void)
{
	if (d->undo_count)
		d->undo[d->undo_count - 1].sealed = 1;
}

static int
changed(const struct doc *e)
{
	return (e->undo_count ? e->undo[e->undo_count - 1].group : 0) != e->saved_group;
}

static void
remember(int inserted, int x0, int y0, int x1, int y1, char *text, int length)
{
	struct change c = { inserted, 0, 0, x0, y0, x1, y1, length, group_now, text }, *last;

	if (!turning) {
		forget(d->redo, &d->redo_count);	/* a new change: what was undone cannot be made again */
		c.joined = inserted && length == 1 && y0 == y1;
		last = d->undo_count ? &d->undo[d->undo_count - 1] : NULL;
		/* a letter typed after a letter is one change with it */
		if (c.joined && last && last->joined && !last->sealed && last->y1 == y0 && last->x1 == x0) {
			last->x1 = x1, last->length++;
			return;
		}
	}
	if (turning == 1)
		push(&d->redo, &d->redo_count, &d->redo_room, c);
	else
		push(&d->undo, &d->undo_count, &d->undo_room, c);
}

/* The text from (x0, y0) to (x1, y1), lines parted by a newline; the caller frees it. NULL
 * when there is no memory for it. */
static char *
text_of(int x0, int y0, int x1, int y1, int *length)
{
	long n = 0;
	int y;
	char *text;

	for (y = y0; y <= y1; y++)
		n += (y == y1 ? x1 : d->lines[y].length + 1) - (y == y0 ? x0 : 0);
	if ((text = malloc(n + 1)) == NULL)
		return NULL;
	n = 0;
	for (y = y0; y <= y1; y++) {
		int from = y == y0 ? x0 : 0, to = y == y1 ? x1 : d->lines[y].length;

		memcpy(text + n, d->lines[y].text + from, to - from);
		n += to - from;
		if (y < y1)
			text[n++] = '\n';
	}
	text[n] = 0;
	*length = (int)n;
	return text;
}

/* Puts text in at (x, y) and leaves the cursor after it. */
static void
text_insert(int x, int y, const char *text, int length)
{
	const char *end = text + length, *p;
	int x0 = x, y0 = y, breaks = 0, i, n, tail_length;
	struct line *l;
	char *tail;

	if (length <= 0)
		return;
	for (p = text; (p = memchr(p, '\n', end - p)) != NULL; p++)
		breaks++;
	if (!breaks) {
		l = &d->lines[y];
		own(y, l->length + length);
		memmove(l->text + x + length, l->text + x, l->length - x);
		memcpy(l->text + x, text, length);
		l->length += length;
		x += length;
		dirty(y, y);
	} else {
		/* the tail of line y goes after the last piece; the pieces between are lines of their own */
		l = &d->lines[y];
		tail_length = l->length - x;
		if ((tail = malloc(tail_length + 1)) == NULL) {
			fprintf(stderr, "nxedit: out of memory\n");
			exit(1);
		}
		memcpy(tail, l->text + x, tail_length);
		open_lines(y + 1, breaks);
		l = &d->lines[y];
		l->length = x;
		p = memchr(text, '\n', length);
		n = (int)(p - text);
		own(y, x + n);
		memcpy(l->text + x, text, n);
		l->length = x + n;
		text = p + 1;
		for (i = 1; i < breaks; i++) {
			p = memchr(text, '\n', end - text);
			n = (int)(p - text);
			own(y + i, n);
			memcpy(d->lines[y + i].text, text, n);
			d->lines[y + i].length = n;
			text = p + 1;
		}
		y += breaks;
		n = (int)(end - text);
		own(y, n + tail_length);
		memcpy(d->lines[y].text, text, n);
		memcpy(d->lines[y].text + n, tail, tail_length);
		d->lines[y].length = n + tail_length;
		x = n;
		free(tail);
		dirty(y0, FAR);
	}
	edited(y0);
	d->cx = x, d->cy = y;
	remember(1, x0, y0, x, y, NULL, length);
}

/* Takes the text from (x0, y0) to (x1, y1) out and leaves the cursor there. */
static void
text_delete(int x0, int y0, int x1, int y1)
{
	int length = 0, y, rest;
	char *text;

	if (y0 == y1 && x0 == x1)
		return;
	if ((text = text_of(x0, y0, x1, y1, &length)) == NULL) {
		forget(d->undo, &d->undo_count);
		forget(d->redo, &d->redo_count);
		d->saved_group = -1;
		say("too large to undo: what came before cannot be undone now");
	}
	rest = d->lines[y1].length - x1;
	if (y1 == y0) {
		own(y0, d->lines[y0].length);
		memmove(d->lines[y0].text + x0, d->lines[y0].text + x1, rest);
		d->lines[y0].length -= x1 - x0;
		dirty(y0, y0);
	} else {
		own(y0, x0 + rest);
		memcpy(d->lines[y0].text + x0, d->lines[y1].text + x1, rest);
		d->lines[y0].length = x0 + rest;
		for (y = y0 + 1; y <= y1; y++)
			drop_line(&d->lines[y]);
		memmove(d->lines + y0 + 1, d->lines + y1 + 1, (d->count - y1 - 1) * sizeof *d->lines);
		d->count -= y1 - y0;
		dirty(y0, FAR);
	}
	edited(y0);
	d->cx = x0, d->cy = y0;
	if (text)
		remember(0, x0, y0, x1, y1, text, length);
}

/* Takes the last command back, or (`again`) makes the last one taken back again. */
static void
turn(int again)
{
	struct change *stack = again ? d->redo : d->undo, c;
	int *n = again ? &d->redo_count : &d->undo_count, group;

	if (!*n) {
		say(again ? "nothing to redo" : "nothing to undo");
		return;
	}
	group = stack[*n - 1].group;
	while (*n && (stack = again ? d->redo : d->undo)[*n - 1].group == group) {
		c = stack[--*n];
		turning = again ? 2 : 1;
		group_now = c.group;
		if (c.inserted)
			text_delete(c.x0, c.y0, c.x1, c.y1);
		else
			text_insert(c.x0, c.y0, c.text, c.length);
		turning = 0;
		free(c.text);
	}
	d->marked = 0;
	d->want = -1;
}

/* ---- places in a line ---- */

static int
next_x(int y, int x)
{
	const struct line *l = &d->lines[y];

	if (x < l->length)
		for (x++; x < l->length && CONT(l->text[x]); x++)
			continue;
	return x;
}

static int
prev_x(int y, int x)
{
	const char *t = d->lines[y].text;

	if (x > 0)
		for (x--; x > 0 && CONT(t[x]); x--)
			continue;
	return x;
}

static int
width_of(int c, int col)
{
	return c == '\t' ? tab_width - col % tab_width : CONT(c) ? 0 : 1;
}

/* The columns from byte `from` of line y to byte `to`, counted from 0 at `from`. */
static int
columns_between(int y, int from, int to)
{
	const char *t = d->lines[y].text;
	int col = 0;

	for (; from < to; from++)
		col += width_of(t[from], col);
	return col;
}

/* The byte of line y, from `from` and not past `to`, at column `col` counted from `from`. */
static int
byte_at(int y, int from, int to, int col)
{
	const char *t = d->lines[y].text;
	int c = 0, w;

	while (from < to) {
		w = width_of(t[from], c);
		if (c + w > col && w)
			break;
		c += w;
		from++;
	}
	while (from < to && CONT(t[from]))
		from++;
	return from;
}

static int
in_word(int c)
{
	return isalnum((unsigned char)c) || c == '_' || (c & 0x80);
}

/* ---- rows: a line is one, or with Word wrap as many as it takes ---- */

static int *seg, seg_room;	/* where each row of the line last asked for begins; seg[n] is its end */

static int
segments(int y)
{
	const struct line *l = &d->lines[y];
	const char *t = l->text;
	int n = 0, col = 0, i, space = 0, start = 0, w;

	seg = grow(seg, &seg_room, 2, sizeof *seg);
	seg[0] = 0;
	if (wrap && (l->length > columns || memchr(t, '\t', l->length)))
		for (i = 0; i < l->length; i++) {
			w = width_of(t[i], col);
			if (col + w > columns && i > start) {
				/* a new row, after the row's last space if it has one */
				start = space > start ? space : i;
				col = columns_between(y, start, i);
				if (col + width_of(t[i], col) > columns)
					start = i, col = 0;
				seg = grow(seg, &seg_room, n + 3, sizeof *seg);
				seg[++n] = start;
				w = width_of(t[i], col);
			}
			col += w;
			if (t[i] == ' ' || t[i] == '\t')
				space = i + 1;
		}
	seg[++n] = l->length;
	return n;
}

/* The row of the line (segments(y) just asked) that byte x is on. */
static int
seg_of(int x, int n)
{
	int k = n - 1;

	while (k > 0 && seg[k] > x)
		k--;
	return k;
}

static int
row_next(int *y, int *k)
{
	if (wrap && *k + 1 < segments(*y)) {
		++*k;
		return 1;
	}
	if (*y + 1 >= d->count)
		return 0;
	++*y, *k = 0;
	return 1;
}

static int
row_prev(int *y, int *k)
{
	if (*k > 0) {
		--*k;
		return 1;
	}
	if (*y == 0)
		return 0;
	--*y;
	*k = wrap ? segments(*y) - 1 : 0;
	return 1;
}

/* Brings the cursor into view. */
static void
follow(void)
{
	int col, n, k, y, j, i;

	if (!wrap) {
		d->sub = 0;
		if (d->cy < d->top)
			d->top = d->cy;
		if (d->cy >= d->top + rows)
			d->top = d->cy - rows + 1;
		col = columns_between(d->cy, 0, d->cx);
		if (col < d->left)
			d->left = col > 8 ? col - 8 : 0;
		if (col >= d->left + columns)
			d->left = col - columns + 1 + (columns > 16 ? 8 : 0);
		return;
	}
	d->left = 0;
	n = segments(d->cy);
	k = seg_of(d->cx, n);
	if (d->cy < d->top || (d->cy == d->top && k < d->sub)) {
		d->top = d->cy, d->sub = k;
		return;
	}
	/* counted from the view's first row, a screen at most */
	for (y = d->top, j = d->sub, i = 0; i < rows && (y != d->cy || j != k) && row_next(&y, &j); i++)
		continue;
	if (i >= rows) {
		for (y = d->cy, j = k, i = 1; i < rows && row_prev(&y, &j); i++)
			continue;
		d->top = y, d->sub = j;
	}
}

/* Moves the view by n rows, the cursor staying where it is in the text. */
static void
scroll_rows(int n)
{
	int y = d->top, k = d->sub, most = d->count - rows;

	if (!wrap) {
		y += n;
		d->top = y > most ? (most > 0 ? most : 0) : y < 0 ? 0 : y;
		return;
	}
	for (; n > 0 && row_next(&y, &k); n--)
		continue;
	for (; n < 0 && row_prev(&y, &k); n++)
		continue;
	d->top = y, d->sub = k;
}

/* ---- the selection ---- */

static int
selection(int *x0, int *y0, int *x1, int *y1)
{
	if (!d->marked || (d->ax == d->cx && d->ay == d->cy))
		return 0;
	if (d->ay < d->cy || (d->ay == d->cy && d->ax < d->cx))
		*x0 = d->ax, *y0 = d->ay, *x1 = d->cx, *y1 = d->cy;
	else
		*x0 = d->cx, *y0 = d->cy, *x1 = d->ax, *y1 = d->ay;
	return 1;
}

static void
unmark(void)
{
	int x0, y0, x1, y1;

	if (selection(&x0, &y0, &x1, &y1))
		dirty(y0, y1);
	d->marked = 0;
}

static int
delete_selection(void)
{
	int x0, y0, x1, y1;

	if (!selection(&x0, &y0, &x1, &y1))
		return 0;
	text_delete(x0, y0, x1, y1);
	d->marked = 0;
	return 1;
}

/* The cursor to (x, y): with `extend` the selection follows it, else there is none. */
static void
go(int x, int y, int extend)
{
	if (extend) {
		if (!d->marked)
			d->ax = d->cx, d->ay = d->cy, d->marked = 1;
	} else
		unmark();
	dirty(d->cy, y);
	d->cx = x, d->cy = y;
	seal();
}

static void
select_range(int x0, int y0, int x1, int y1)
{
	unmark();
	dirty(d->cy, y1);
	d->ax = x0, d->ay = y0, d->cx = x1, d->cy = y1, d->marked = 1;
	dirty(y0, y1);
	seal();
}

/* The whole lines a command works on: the selection's, or the cursor's. */
static void
lines_chosen(int *y0, int *y1)
{
	int x0, x1;

	if (!selection(&x0, y0, &x1, y1)) {
		*y0 = *y1 = d->cy;
		return;
	}
	if (*y1 > *y0 && x1 == 0)
		--*y1;
}

/* ---- a file ---- */

static const char *
leaf(const char *path)
{
	return strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
}

static int title_changed = -1;

static void
titled(void)
{
	char title[300];

	title_changed = changed(d);
	snprintf(title, sizeof title, "%s%.200s - Editor", title_changed ? "*" : "", leaf(d->path));
	if (window)
		GrSetWindowTitle(window, title);
}

static int
c_like(const char *path)
{
	static const char *const endings[] = { ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".ino", NULL };
	const char *dot = strrchr(leaf(path), '.');
	int i;

	for (i = 0; dot && endings[i]; i++)
		if (!strcasecmp(dot, endings[i]))
			return 1;
	return 0;
}

static int read_wanted;		/* nxedit --read: every .md file opened is read, not edited */

/* Whether a file opened is shown read: Markdown in the image's documentation (/usr/share/doc), or
 * any Markdown when the editor was started with --read. */
static int
read_first(const char *path)
{
	size_t n = strlen(path);

	return n > 3 && !strcasecmp(path + n - 3, ".md") && (read_wanted || !strncmp(path, "/usr/share/doc/", 15));
}

static void
clear_doc(struct doc *e)
{
	int i;

	for (i = 0; i < e->count; i++)
		drop_line(&e->lines[i]);
	for (i = 0; i < e->block_count; i++)
		free(e->blocks[i]);
	for (i = 0; i < e->back_count; i++)
		free(e->back[i]);
	free(e->back);
	forget(e->undo, &e->undo_count);
	forget(e->redo, &e->redo_count);
	free(e->lines);
	free(e->blocks);
	free(e->undo);
	free(e->redo);
	free(e->carry);
	memset(e, 0, sizeof *e);
	e->want = -1;
	lex_gen++;	/* (the line coloured last may be this slot's next text) */
}

/* An empty text with no file: Save will ask what to call it. */
static void
blank(void)
{
	clear_doc(d);
	add_line(empty, 0, 0);
	snprintf(d->path, sizeof d->path, "/root/untitled.txt");
	d->final_newline = 1;
}

static char recent[RECENT][256];
static int recent_count;

static void
recent_read(void)
{
	const char *list = reg_get("nxedit.recent", "");
	int n;

	for (recent_count = 0; *list && recent_count < RECENT; recent_count++) {
		n = (int)strcspn(list, "|");
		snprintf(recent[recent_count], sizeof recent[0], "%.*s", n, list);
		list += n + (list[n] == '|');
	}
}

/* A file opened or saved goes first in File's Open recent. */
static void
recent_add(const char *path)
{
	char full[512], list[256];
	int i, at = 0;

	if (path[0] != '/') {
		char here[300];

		snprintf(full, sizeof full, "%.300s/%.200s", getcwd(here, sizeof here) ? here : "", path);
	} else
		snprintf(full, sizeof full, "%s", path);
	if (strlen(full) >= sizeof recent[0])
		return;
	for (i = 0; i < recent_count && strcmp(recent[i], full); i++)
		continue;
	if (i == recent_count && recent_count < RECENT)
		recent_count++;
	if (i == recent_count)
		i--;
	memmove(recent[1], recent[0], i * sizeof recent[0]);
	strcpy(recent[0], full);
	list[0] = 0;
	for (i = 0; i < recent_count; i++) {
		int n = (int)strlen(recent[i]);

		if (at + n + 1 >= (int)sizeof list)
			break;
		at += snprintf(list + at, sizeof list - at, "%s%s", i ? "|" : "", recent[i]);
	}
	reg_set("nxedit.recent", list);
}

/* The file's text into the empty current text. A file that is not there is a new one. */
static void
load(const char *file)
{
	int fd = open(file, O_RDONLY), why = errno, got, n;
	char *block, *p, *q, *q2, *end, *pending = NULL;
	long left;
	int pending_length = 0;
	struct stat info;
	struct timespec t0, t1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	snprintf(d->path, sizeof d->path, "%s", file);
	d->named = 1;
	d->final_newline = 1;
	d->c_code = c_like(file);
	d->reading = read_first(file);
	if (fd < 0) {
		if (why == ENOENT)
			say("new file");
		else
			snprintf(notice, sizeof notice, "cannot be read (%s)", strerror(why));
		add_line(empty, 0, 0);
		return;
	}
	left = fstat(fd, &info) == 0 ? (long)info.st_size : 0;
	d->lines = grow(d->lines, &d->room, (int)(left / 40 < 65536 ? left / 40 : 65536) + 16, sizeof *d->lines);
	while (left > 0) {
		int size = left < BLOCK ? (int)left : BLOCK;

		if ((block = malloc(size)) == NULL)
			break;
		for (got = 0; got < size && (n = read(fd, block + got, size - got)) > 0; got += n)
			continue;
		if (got <= 0) {
			free(block);
			break;
		}
		left -= got;
		d->blocks = grow(d->blocks, &d->block_room, d->block_count + 1, sizeof *d->blocks);
		d->blocks[d->block_count++] = block;
		p = block, end = block + got;
		if (pending) {
			/* a line across two blocks has memory of its own */
			q = memchr(p, '\n', end - p);
			n = q ? (int)(q - p) : (int)(end - p);
			if ((q2 = realloc(pending, pending_length + n + 1)) == NULL) {
				left = left > 0 ? left : 1;
				break;
			}
			pending = q2;
			memcpy(pending + pending_length, p, n);
			pending_length += n;
			if (!q)
				continue;
			add_line(pending, pending_length, pending_length + 1);
			pending = NULL;
			p = q + 1;
		}
		while (p < end && (q = memchr(p, '\n', end - p)) != NULL) {
			add_line(p, (int)(q - p), 0);
			p = q + 1;
		}
		if (p < end) {
			pending_length = (int)(end - p);
			if ((pending = malloc(pending_length + 1)) == NULL) {
				left = left > 0 ? left : 1;
				break;
			}
			memcpy(pending, p, pending_length);
		}
	}
	close(fd);
	if (left > 0) {
		/* (a text cut short must not be saved over its file) */
		free(pending);
		clear_doc(d);
		blank();
		snprintf(notice, sizeof notice, "%.100s is too large to open here", leaf(file));
		return;
	}
	if (pending)
		add_line(pending, pending_length, pending_length + 1), d->final_newline = 0;
	if (!d->count)
		add_line(empty, 0, 0);
	d->readonly = access(file, W_OK) != 0;
	if (d->c_code && info.st_size > COLOUR_MOST) {
		d->c_code = 0;
		say("a large file: shown without colours");
	}
	recent_add(file);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	if (getenv("NXEDIT_TIME"))
		fprintf(stderr, "nxedit: %s: %d lines, %ld bytes in %ld ms\n", file, d->count, (long)info.st_size,
			(long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
}

/* Writes the text to a file, which is then the text's own; false, and why, when it cannot. */
static int
write_to(const char *to)
{
	/* written beside the file and renamed over it, so a failed write leaves the old one whole;
	 * in place where no file can be made beside it */
	char *real = realpath(to, NULL), temp[600];
	const char *target = real ? real : to, *ending = d->crlf ? "\r\n" : "\n";
	struct stat info;
	int had = stat(target, &info) == 0, i, fine, why, in_place;
	FILE *file = NULL;

	snprintf(temp, sizeof temp, "%s.nxedit-new", target);
	if (strlen(target) < 580 && (!had || access(target, W_OK) == 0))
		file = fopen(temp, "w");
	if ((in_place = !file))
		file = fopen(target, "w");
	if (file)
		setvbuf(file, NULL, _IOFBF, 65536);
	for (i = 0; file && i < d->count; i++) {
		fwrite(d->lines[i].text, 1, d->lines[i].length, file);
		if (i + 1 < d->count || d->final_newline)
			fputs(ending, file);
	}
	fine = file && !ferror(file);
	why = errno;
	if (file && fclose(file) != 0)
		fine = 0, why = errno;
	if (!in_place) {
		if (fine && had)
			chmod(temp, info.st_mode & 07777);
		if (fine && rename(temp, target) != 0)
			fine = 0, why = errno;
		if (!fine)
			unlink(temp);
	}
	free(real);
	if (!fine) {
		snprintf(notice, sizeof notice, "NOT saved: cannot write %.90s (%s)", to, strerror(why));
		status_dirty = 1;
		return 0;
	}
	if (to != d->path)
		snprintf(d->path, sizeof d->path, "%s", to);
	d->named = 1, d->readonly = 0;
	d->saved_group = d->undo_count ? d->undo[d->undo_count - 1].group : 0;
	seal();
	if (d->c_code != c_like(to))
		d->c_code = c_like(to), dirty_all = 1;
	recent_add(d->path);
	snprintf(notice, sizeof notice, "saved as %.150s", d->path);
	status_dirty = tabs_dirty = 1;
	return 1;
}

/* ---- drawing ---- */

static void read_status(char *right);
static void read_flush(void);

static const char *const bar_names[] = { "File", "Edit", "Search", "View" };
static int bar_x[5], bar_open = -1;
static int tab_x[DOCS + 1];
static int drawn_top = -1, drawn_sub, drawn_left, drawn_count, drawn_layout;
static int brace_x, brace_y = -1, match_x, match_y = -1;

#define LIT_LINE MWRGB(232, 242, 254)
#define BRACE_GROUND MWRGB(190, 230, 190)
#define FAINT MWRGB(175, 175, 175)
static const GR_COLOR kind_colour[ES_KINDS] = { BLACK, MWRGB(0, 0, 230), MWRGB(128, 0, 160), MWRGB(150, 75, 0),
	MWRGB(163, 21, 21), MWRGB(210, 105, 0), MWRGB(0, 128, 0) };

static unsigned char kinds[LEX_MOST + 2];
static char lex_text[LEX_MOST + 2];
static struct doc *lexed_doc;
static int lexed_y = -1;
static unsigned lexed_gen;

/* Whether line y begins inside a comment: worked out from the last line known. */
static int
carry_at(int y)
{
	d->carry = grow(d->carry, &d->carry_room, d->count + 1, 1);
	if (d->carry_known == 0)
		d->carry[0] = 0;
	while (d->carry_known < y) {
		const struct line *l = &d->lines[d->carry_known];

		d->carry[d->carry_known + 1] = (unsigned char)es_carry(l->text, l->length, d->carry[d->carry_known]);
		d->carry_known++;
	}
	return d->carry[y];
}

/* What each byte of line y is, for its colour; NULL for a line shown plain. */
static const unsigned char *
lex(int y)
{
	const struct line *l = &d->lines[y];

	if (!colour || !d->c_code || l->length > LEX_MOST)
		return NULL;
	if (lexed_doc == d && lexed_y == y && lexed_gen == lex_gen)
		return kinds;
	memcpy(lex_text, l->text, l->length);
	lex_text[l->length] = lex_text[l->length + 1] = 0;
	es_line(lex_text, l->length, carry_at(y), kinds);
	lexed_doc = d, lexed_y = y, lexed_gen = lex_gen;
	return kinds;
}

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

/* Screen row r: bytes s to e of line y (its first row, its last); a line past the text is blank. */
static void
draw_row(int r, int y, int s, int e, int first, int last)
{
	static char glyphs[WIDEST + 1];
	static unsigned char look[WIDEST + 1];	/* 0 plain, 1 selected, 2 a brace matched; the kind above */
	static unsigned char kind_at[WIDEST + 1];
	const struct line *l;
	const unsigned char *kind;
	int py = text_top + MARGIN + r * ui_fh, tx = gutter + MARGIN, right = width - UI_SCROLL_W;
	int x0, y0, x1, y1, sb = 0, se = 0, i, j, col = 0, n = 0, from;
	GR_COLOR ground = line_lit && y == d->cy ? LIT_LINE : WHITE, ink;

	if (gutter) {
		ui_fill(window, 0, py, gutter, ui_fh, UI_FACE);
		if (first && y < d->count) {
			char number[12];
			int length = (int)(put_number(number, y + 1) - number);

			ui_text(window, gutter - 4 - length * ui_fw, py, number, length, ui_seven ? MWRGB(43, 145, 175) : MWRGB(110, 110, 110), 1);
		}
	}
	ui_fill(window, gutter, py, right - gutter, ui_fh, y < d->count ? ground : WHITE);
	if (y >= d->count)
		return;
	l = &d->lines[y];
	if (selection(&x0, &y0, &x1, &y1) && y >= y0 && y <= y1) {
		sb = y == y0 ? x0 : 0;
		se = y == y1 ? x1 : l->length + 1;	/* a line's end counts as one more */
	}
	kind = lex(y);
	for (i = s; i < e && n < columns; i++) {
		int c = (unsigned char)l->text[i], w = width_of(c, col), shown;

		shown = c == '\t' ? ' ' : c == ' ' ? (whitespace ? '.' : ' ') : c < 32 || c >= 127 ? '?' : c;
		for (j = 0; j < w && n < columns; j++, col++) {
			if (col < d->left)
				continue;
			glyphs[n] = (char)(j ? ' ' : c == '\t' && whitespace ? '>' : shown);
			kind_at[n] = whitespace && (c == ' ' || c == '\t') ? ES_KINDS : kind ? kind[i] : ES_PLAIN;
			look[n] = i >= sb && i < se ? 1 : y == brace_y && i == brace_x && match_y >= 0 ? 2 :
				y == match_y && i == match_x ? 2 : 0;
			n++;
		}
	}
	if (last && se > l->length && n < columns && col >= d->left)
		glyphs[n] = ' ', kind_at[n] = 0, look[n++] = 1;
	/* the grounds of the selected columns, a run a fill */
	for (i = 0; i < n; i = j) {
		for (j = i + 1; j < n && look[j] == look[i]; j++)
			continue;
		if (look[i])
			ui_fill(window, tx + i * ui_fw, py, (j - i) * ui_fw, ui_fh, look[i] == 1 ? UI_SELECTED : BRACE_GROUND);
	}
	/* the text, a run of one colour a request; spaces at a run's ends are not sent */
	for (i = 0; i < n; i = j) {
		ink = look[i] == 1 ? WHITE : kind_at[i] == ES_KINDS ? FAINT : kind_colour[kind_at[i]];
		for (j = i + 1; j < n && (look[j] == 1 ? WHITE : kind_at[j] == ES_KINDS ? FAINT : kind_colour[kind_at[j]]) == ink; j++)
			continue;
		for (from = i; from < j && glyphs[from] == ' '; from++)
			continue;
		for (col = j; col > from && glyphs[col - 1] == ' '; col--)
			continue;
		if (col > from)
			ui_text(window, tx + from * ui_fw, py, glyphs + from, col - from, ink, 1);
	}
	if (y == d->cy && d->cx >= s && (d->cx < e || last)) {
		col = columns_between(y, s, d->cx) - d->left;
		if (col >= 0 && col <= columns) {
			if (overwrite)
				ui_fill(window, tx + col * ui_fw, py + ui_fh - 2, ui_fw, 2, BLACK);
			else
				ui_fill(window, tx + col * ui_fw, py, 2, ui_fh, BLACK);
		}
	}
}

/* Every row from line y's first to the bottom. */
static void
draw_from(int y)
{
	int r = 0, yy = d->top, k = d->sub, n;

	while (yy < y && yy < d->count && r < rows) {
		n = segments(yy);
		r += n - k;
		k = 0, yy++;
	}
	while (r < rows) {
		if (yy >= d->count) {
			draw_row(r++, yy, 0, 0, 0, 0);
			continue;
		}
		n = segments(yy);
		for (; k < n && r < rows; k++, r++)
			draw_row(r, yy, seg[k], seg[k + 1], k == 0, k == n - 1);
		k = 0, yy++;
	}
}

/* The rows of line y that are in view. */
static void
draw_line(int y)
{
	int r = 0, yy = d->top, k = d->sub, n;

	if (y < d->top)
		return;
	if (!wrap) {
		if (y - d->top < rows)
			draw_row(y - d->top, y, 0, y < d->count ? d->lines[y].length : 0, 1, 1);
		return;
	}
	while (yy < y && r < rows) {
		n = yy < d->count ? segments(yy) : 1;
		r += n - k;
		k = 0, yy++;
	}
	if (r >= rows)
		return;
	if (y >= d->count) {
		draw_row(r, y, 0, 0, 0, 0);
		return;
	}
	n = segments(y);
	for (; k < n && r < rows; k++, r++)
		draw_row(r, y, seg[k], seg[k + 1], k == 0, k == n - 1);
}

static void
draw_bar(void)
{
	int i;

	ui_fill(window, 0, 0, width, MENU_H, UI_FACE);
	for (i = 0; i < 4; i++) {
		if (i == bar_open) {
			if (ui_seven) {
				ui_fill(window, bar_x[i] + 1, 1, bar_x[i + 1] - bar_x[i] - 2, MENU_H - 2, MWRGB(213, 231, 248));
				ui_edges(window, bar_x[i], 0, bar_x[i + 1] - bar_x[i], MENU_H, MWRGB(122, 177, 232), MWRGB(122, 177, 232));
			} else
				ui_bevel(window, bar_x[i], 1, bar_x[i + 1] - bar_x[i], MENU_H - 2, 1);
		}
		ui_text(window, bar_x[i] + 8, 4, bar_names[i], -1, BLACK, 0);
	}
	ui_fill(window, 0, MENU_H - 1, width, 1, UI_SHADOW);
}

static void
draw_tabs(void)
{
	int i, x = 2, y = MENU_H, tw;
	char label[80];

	if (doc_count < 2)
		return;
	ui_fill(window, 0, y, width, TABS_H, UI_FACE);
	for (i = 0; i < doc_count; i++) {
		snprintf(label, sizeof label, "%.60s%s", leaf(docs[i].path), changed(&docs[i]) ? " *" : "");
		tw = ui_width(&ui_var_widths, ui_var, label, -1) + 30;
		tw = tw < 70 ? 70 : tw > 190 ? 190 : tw;
		if (x + tw > width - 2)
			tw = width - 2 - x;
		tab_x[i] = x;
		if (tw > 20) {
			if (ui_seven)
				ui_button_seven(window, x, y + 2, tw - 1, TABS_H - 3, i == current);
			else {
				ui_fill(window, x, y + (i == current ? 1 : 3), tw - 1, TABS_H - (i == current ? 2 : 4), i == current ? WHITE : UI_FACE);
				ui_bevel(window, x, y + (i == current ? 1 : 3), tw - 1, TABS_H - (i == current ? 2 : 4), 0);
			}
			ui_text_fit(window, x + 7, y + 5, tw - 28, label, BLACK);
			/* a cross: the tab's own Close */
			GrSetGCForeground(ui_gc, UI_SHADOW);
			GrLine(window, ui_gc, x + tw - 15, y + 8, x + tw - 9, y + 14);
			GrLine(window, ui_gc, x + tw - 9, y + 8, x + tw - 15, y + 14);
		}
		x += tw;
	}
	tab_x[doc_count] = x;
}

static void
draw_status(void)
{
	char right[160], *p = right, left[700];
	int x0, y0, x1, y1, tw, y = height - STATUS;

	p = put(put_number(put(put_number(put(p, "Ln "), d->cy + 1), ", Col "), columns_between(d->cy, 0, d->cx) + 1), "");
	if (selection(&x0, &y0, &x1, &y1)) {
		long bytes = 0;
		int k;

		for (k = y0; k < y1; k++)
			bytes += d->lines[k].length + 1;
		bytes += x1 - x0;
		p = put(put_number(put(put_number(put(p, "    Sel "), (int)bytes), " | "), y1 - y0 + 1), "");
	}
	p = put(put_number(put(p, "    "), d->count), d->count == 1 ? " line" : " lines");
	p = put(p, d->crlf ? "    Windows (CR LF)" : "    Unix (LF)");
	put(p, overwrite ? "    OVR" : "    INS");
	if (d->reading)
		read_status(right);
	tw = ui_width(&ui_var_widths, ui_var, right, -1);
	ui_fill(window, 0, y, width, STATUS, UI_FACE);
	ui_fill(window, 0, y, width, 1, UI_SHADOW);
	ui_bevel(window, 2, y + 2, width - tw - 22, STATUS - 3, 1);
	ui_bevel(window, width - tw - 18, y + 2, tw + 16, STATUS - 3, 1);
	if (notice[0])
		ui_text_fit(window, 6, y + 4, width - tw - 30, notice, BLACK);
	else {
		put(put(put(left, d->path), changed(d) ? " *" : ""), d->readonly ? "   (read only)" : "");
		ui_text_fit(window, 6, y + 4, width - tw - 30, left, BLACK);
	}
	ui_text(window, width - tw - 10, y + 4, right, -1, BLACK, 0);
	status_dirty = 0;
}

static void
draw_scrollbar(void)
{
	ui_scrollbar(window, width - UI_SCROLL_W, text_top, height - text_top - STATUS, d->count, rows, d->top);
}

/* Columns, rows and the gutter for the window's size; true when they changed. */
static int
layout(void)
{
	int digits = 1, n, was = drawn_layout;

	for (n = d->count; n >= 10; n /= 10)
		digits++;
	if (digits < 3)
		digits = 3;
	gutter = numbers ? (digits + 1) * ui_fw + 4 : 0;
	text_top = MENU_H + (doc_count > 1 ? TABS_H : 0);
	rows = (height - text_top - STATUS - MARGIN) / ui_fh;
	columns = (width - gutter - 2 * MARGIN - UI_SCROLL_W) / ui_fw;
	rows = rows < 1 ? 1 : rows;
	columns = columns < 1 ? 1 : columns > WIDEST ? WIDEST : columns;
	drawn_layout = gutter * 7 + text_top * 131 + rows * 1009 + columns * 50021 + wrap * 3 + tab_width * 17;
	return drawn_layout != was;
}

/* What the commands since the last call changed, drawn. */
static void
flush(void)
{
	int moved, by, y, last;

	if (layout())
		dirty_all = 1;
	if (d->top >= d->count)
		d->top = d->count - 1;
	if (d->top < 0)
		d->top = 0;
	if (wrap && d->sub >= segments(d->top))
		d->sub = segments(d->top) - 1;
	if (!wrap)
		d->sub = 0;
	if (title_changed != changed(d)) {
		titled();
		tabs_dirty = 1;
	}
	if (d->reading) {
		read_flush();
		return;
	}
	moved = d->top != drawn_top || d->sub != drawn_sub || d->left != drawn_left;
	if (dirty_all) {
		draw_bar();
		draw_tabs();
		draw_from(d->top);
		draw_scrollbar();
		draw_status();
	} else {
		if (tabs_dirty)
			draw_tabs();
		by = d->top - drawn_top;
		if (moved && !wrap && d->left == drawn_left && abs(by) < rows && !scroll_drawn) {
			/* the rows that stay in view are copied to their new places, the others drawn */
			int stay = rows - abs(by), ty = text_top + MARGIN;

			GrCopyArea(window, ui_gc, 0, ty + (by < 0 ? -by * ui_fh : 0), width - UI_SCROLL_W, stay * ui_fh, window, 0,
				   ty + (by > 0 ? by * ui_fh : 0), MWROP_COPY);
			dirty(by > 0 ? d->top + stay : d->top, by > 0 ? d->top + rows - 1 : d->top - by - 1);
		} else if (moved) {
			dirty(d->top, FAR);
		}
		if (dirty_to >= FAR)
			draw_from(dirty_from > d->top ? dirty_from : d->top);
		else if (dirty_to >= d->top) {
			last = dirty_to < d->top + rows ? dirty_to : d->top + rows - 1;
			for (y = dirty_from > d->top ? dirty_from : d->top; y <= last; y++)
				draw_line(y);
		}
		if (moved || drawn_count != d->count)
			draw_scrollbar();
		if (status_dirty)
			draw_status();
	}
	drawn_top = d->top, drawn_sub = d->sub, drawn_left = d->left, drawn_count = d->count;
	dirty_from = FAR, dirty_to = -1, dirty_all = tabs_dirty = 0;
}

/* The brace at the cursor and the one it pairs with, picked out. */
static void
find_brace(void)
{
	static const char pairs[] = "()[]{}";
	const struct line *l = &d->lines[d->cy];
	const char *at = NULL;
	int x = -1, y = d->cy, dir, depth = 0, budget = 400000, old_y = brace_y, old_match = match_y;
	char c = 0, other;

	brace_y = match_y = -1;
	if (d->cx < l->length && (at = strchr(pairs, l->text[d->cx])) != NULL && *at)
		x = d->cx;
	else if (d->cx > 0 && (at = strchr(pairs, l->text[d->cx - 1])) != NULL && *at)
		x = d->cx - 1;
	if (x >= 0) {
		c = *at;
		dir = (at - pairs) % 2 ? -1 : 1;
		other = pairs[(at - pairs) + dir];
		brace_x = x, brace_y = y;
		for (; budget > 0 && y >= 0 && y < d->count; y += dir, x = dir > 0 ? -1 : (y >= 0 && y < d->count ? d->lines[y].length : 0)) {
			l = &d->lines[y];
			for (x += dir; x >= 0 && x < l->length; x += dir, budget--) {
				if (l->text[x] == c)
					depth++;
				else if (l->text[x] == other && --depth < 0) {
					match_x = x, match_y = y;
					goto found;
				}
			}
		}
	}
found:
	if (old_y >= 0)
		dirty(old_y, old_y);
	if (old_match >= 0)
		dirty(old_match, old_match);
	if (brace_y >= 0 && match_y >= 0)
		dirty(brace_y, brace_y), dirty(match_y, match_y);
}

/* ---- Find and Replace ---- */

static char find_text[128], replace_text[128];
static int match_case, whole_word, wrap_around = 1;
static unsigned char fold[256];

enum { F_NEXT = 1, F_PREVIOUS, F_REPLACE, F_ALL, F_COUNT, F_CLOSE };
static struct dlg_control find_controls[] = {
	{ .kind = DLG_LABEL, .x = 10, .y = 10, .w = 84, .label = "Find what:" },
	{ .kind = DLG_FIELD, .x = 100, .y = 10, .w = 210, .text = find_text, .size = sizeof find_text },
	{ .kind = DLG_LABEL, .x = 10, .y = 38, .w = 84, .label = "Replace with:" },
	{ .kind = DLG_FIELD, .x = 100, .y = 38, .w = 210, .text = replace_text, .size = sizeof replace_text },
	{ .kind = DLG_CHECK, .x = 10, .y = 72, .w = 130, .label = "Match case", .on = &match_case },
	{ .kind = DLG_CHECK, .x = 10, .y = 94, .w = 130, .label = "Whole word", .on = &whole_word },
	{ .kind = DLG_CHECK, .x = 10, .y = 116, .w = 130, .label = "Wrap around", .on = &wrap_around },
	{ .kind = DLG_BUTTON, .x = 322, .y = 9, .w = 104, .id = F_NEXT, .label = "Find next" },
	{ .kind = DLG_BUTTON, .x = 322, .y = 37, .w = 104, .id = F_PREVIOUS, .label = "Find previous" },
	{ .kind = DLG_BUTTON, .x = 322, .y = 65, .w = 104, .id = F_REPLACE, .label = "Replace" },
	{ .kind = DLG_BUTTON, .x = 322, .y = 93, .w = 104, .id = F_ALL, .label = "Replace all" },
	{ .kind = DLG_BUTTON, .x = 322, .y = 121, .w = 104, .id = F_COUNT, .label = "Count" },
	{ .kind = DLG_BUTTON, .x = 322, .y = 149, .w = 104, .id = F_CLOSE, .label = "Close" },
};
static struct dlg finder = { 0, "Find", 436, 200, sizeof find_controls / sizeof find_controls[0], 1, F_NEXT, F_CLOSE, find_controls };

/* Whether what is sought is at byte x of line l. */
static int
match_at(const struct line *l, int x, const char *s, int n)
{
	int i;

	if (x < 0 || x + n > l->length)
		return 0;
	if (match_case) {
		if (memcmp(l->text + x, s, n))
			return 0;
	} else
		for (i = 0; i < n; i++)
			if (fold[(unsigned char)l->text[x + i]] != fold[(unsigned char)s[i]])
				return 0;
	return !whole_word || ((x == 0 || !in_word(l->text[x - 1])) && (x + n == l->length || !in_word(l->text[x + n])));
}

/* The first place in line y, from byte `from` on and before `to`, where it is; -1 for none. */
static int
match_in(int y, int from, int to, const char *s, int n)
{
	const struct line *l = &d->lines[y];
	const char *t = l->text, *p;
	int x, first = fold[(unsigned char)s[0]];

	if (to > l->length - n + 1)
		to = l->length - n + 1;
	for (x = from; x < to; x++) {
		if (match_case) {
			if ((p = memchr(t + x, s[0], to - x)) == NULL)
				return -1;
			x = (int)(p - t);
		} else if (fold[(unsigned char)t[x]] != first)
			continue;
		if (match_at(l, x, s, n))
			return x;
	}
	return -1;
}

/* The last place in line y that begins before byte `before`; -1 for none. */
static int
match_before(int y, int before, const char *s, int n)
{
	int x = -1, next;

	while ((next = match_in(y, x + 1, before, s, n)) >= 0)
		x = next;
	return x;
}

/* Looks for what is sought from the selection on (dir 1) or back (-1); selects it. */
static int
find(int dir)
{
	int n = (int)strlen(find_text), x0, y0, x1, y1, sx, sy, pass, y, at;
	char text[120];

	if (!n) {
		say("say what to find first: Ctrl+F");
		return 0;
	}
	if (selection(&x0, &y0, &x1, &y1))
		sx = dir > 0 ? x1 : x0, sy = dir > 0 ? y1 : y0;
	else
		sx = d->cx, sy = d->cy;
	for (pass = 0; pass <= d->count; pass++) {
		y = sy + pass * dir;
		if (y >= d->count || y < 0) {
			if (!wrap_around)
				break;
			y += y < 0 ? d->count : -d->count;
		}
		if (dir > 0)
			at = match_in(y, pass == 0 ? sx : 0, pass == d->count ? sx : INT_MAX, find_text, n);
		else
			at = match_before(y, pass == 0 ? sx : INT_MAX, find_text, n);
		if (at >= 0 && !(pass == d->count && dir < 0 && at < sx)) {
			select_range(at, y, at + n, y);
			d->want = -1;
			follow();
			snprintf(text, sizeof text, "found on line %d%s", y + 1, (dir > 0 ? y < sy : y > sy) || pass == d->count ? " (from the other end)" : "");
			say(text);
			dlg_say(&finder, text);
			return 1;
		}
	}
	snprintf(text, sizeof text, "\"%.80s\" is not found", find_text);
	say(text);
	dlg_say(&finder, text);
	status_dirty = 1;
	return 0;
}

static int
count_matches(void)
{
	int n = (int)strlen(find_text), y, x, total = 0;

	for (y = 0; n && y < d->count; y++)
		for (x = 0; (x = match_in(y, x, INT_MAX, find_text, n)) >= 0; x += n)
			total++;
	return total;
}

/* The selection, when it is what is sought, replaced; then the next found. */
static void
replace_one(void)
{
	int n = (int)strlen(find_text), x0, y0, x1, y1;

	if (n && selection(&x0, &y0, &x1, &y1) && y0 == y1 && x1 - x0 == n && match_at(&d->lines[y0], x0, find_text, n)) {
		begin();
		text_delete(x0, y0, x1, y1);
		text_insert(x0, y0, replace_text, (int)strlen(replace_text));
		d->marked = 0;
	}
	find(1);
}

static int
replace_all(void)
{
	static char *line;
	static int room;
	int n = (int)strlen(find_text), r = (int)strlen(replace_text), y, x, from, at, total = 0, cx = d->cx, cy = d->cy;

	if (!n)
		return 0;
	begin();
	seal();
	for (y = 0; y < d->count; y++) {
		if ((x = match_in(y, 0, INT_MAX, find_text, n)) < 0)
			continue;
		/* the line made anew, then put in place of the old one: two changes a line */
		at = 0, from = 0;
		do {
			line = grow(line, &room, at + (x - from) + r + 1, 1);
			memcpy(line + at, d->lines[y].text + from, x - from);
			at += x - from;
			memcpy(line + at, replace_text, r);
			at += r;
			from = x + n;
			total++;
		} while ((x = match_in(y, from, INT_MAX, find_text, n)) >= 0);
		line = grow(line, &room, at + d->lines[y].length - from + 1, 1);
		memcpy(line + at, d->lines[y].text + from, d->lines[y].length - from);
		at += d->lines[y].length - from;
		text_delete(0, y, d->lines[y].length, y);
		text_insert(0, y, line, at);
	}
	d->marked = 0;
	d->cy = cy < d->count ? cy : d->count - 1;
	d->cx = cx < d->lines[d->cy].length ? cx : d->lines[d->cy].length;
	while (d->cx > 0 && CONT(d->lines[d->cy].text[d->cx]))
		d->cx--;
	dirty(0, FAR);
	return total;
}

static void
find_open(int replacing)
{
	int x0, y0, x1, y1, wx, wy;
	const GR_SCREEN_INFO *screen = ui_screen();

	if (selection(&x0, &y0, &x1, &y1) && y0 == y1 && x1 - x0 < (int)sizeof find_text)
		snprintf(find_text, sizeof find_text, "%.*s", x1 - x0, d->lines[y0].text + x0);
	finder.title = replacing ? "Replace" : "Find";
	finder.note[0] = 0;
	dlg_offer(&find_controls[1]);
	if (finder.window) {
		GrSetWindowTitle(finder.window, (char *)finder.title);
		GrRaiseWindow(finder.window);
		GrSetFocus(finder.window);
		finder.focus = replacing && find_text[0] ? 3 : 1;
		dlg_offer(&find_controls[finder.focus]);
		dlg_draw(&finder);
		return;
	}
	ui_window_origin(window, &wx, &wy);
	wx += width - finder.width - 30, wy += 50;
	wx = wx + finder.width > screen->cols ? screen->cols - finder.width - 10 : wx < 0 ? 0 : wx;
	wy = wy + finder.height > screen->rows - 40 ? screen->rows - finder.height - 40 : wy < 0 ? 0 : wy;
	dlg_open(&finder, wx, wy, replacing && find_text[0] ? 3 : 1);
	dlg_offer(&find_controls[finder.focus]);
}

static void
find_event(GR_EVENT *event)
{
	char text[80];
	int id, total;

	if (event->type == GR_EVENT_TYPE_KEY_DOWN && (event->keystroke.ch == MWKEY_F3 ||
	    (event->keystroke.ch == MWKEY_ENTER && (event->keystroke.modifiers & MWKMOD_SHIFT))))
		id = (event->keystroke.modifiers & MWKMOD_SHIFT) ? F_PREVIOUS : F_NEXT;
	else
		id = dlg_event(&finder, event);
	notice[0] = 0;
	switch (id) {
	case F_NEXT:
	case F_PREVIOUS:
		find(id == F_NEXT ? 1 : -1);
		break;
	case F_REPLACE:
		replace_one();
		break;
	case F_ALL:
		total = replace_all();
		snprintf(text, sizeof text, total == 1 ? "1 replaced" : "%d replaced", total);
		dlg_say(&finder, text);
		say(text);
		break;
	case F_COUNT:
		total = count_matches();
		snprintf(text, sizeof text, total == 1 ? "found once" : "found %d times", total);
		dlg_say(&finder, text);
		break;
	case F_CLOSE:
		dlg_close(&finder);
		GrSetFocus(window);
		break;
	}
	if (id)
		find_brace();
}

/* ---- the other windows' events while a question is up ---- */

static void
resized(int w, int h)
{
	width = w, height = h;
	layout();
	follow();
	dirty_all = 1;
}

static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_UPDATE && event->update.wid == window && event->update.utype == GR_UPDATE_SIZE) {
		resized(event->update.width, event->update.height);
		flush();
	} else if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window) {
		dirty_all = 1;
		flush();
	} else if (event->type == GR_EVENT_TYPE_EXPOSURE && finder.window && event->exposure.wid == finder.window)
		dlg_draw(&finder);
}

/* Go to line: a window with a field for the number. */
static void
go_to_line(void)
{
	static char number[16], label[48];
	static struct dlg_control controls[] = {
		{ .kind = DLG_LABEL, .x = 10, .y = 10, .w = 200, .label = label },
		{ .kind = DLG_FIELD, .x = 10, .y = 34, .w = 150, .text = number, .size = sizeof number },
		{ .kind = DLG_BUTTON, .x = 172, .y = 32, .w = 80, .id = 1, .label = "Go" },
		{ .kind = DLG_BUTTON, .x = 172, .y = 60, .w = 80, .id = 2, .label = "Cancel" },
	};
	struct dlg g = { 0, "Go to line", 262, 112, 4, 1, 1, 2, controls };
	GR_EVENT event;
	int id = 0, line;

	snprintf(label, sizeof label, "Line number (1 to %d):", d->count);
	put_number(number, d->cy + 1);
	dlg_open(&g, -1, -1, 1);
	dlg_offer(&controls[1]);
	while (!id) {
		GrGetNextEvent(&event);
		if (event.general.wid != g.window) {
			if (event.type != GR_EVENT_TYPE_NONE)
				others(&event);
			continue;
		}
		id = dlg_event(&g, &event);
	}
	dlg_close(&g);
	GrSetFocus(window);
	if (id == 1 && (line = atoi(number)) > 0) {
		go(0, (line > d->count ? d->count : line) - 1, 0);
		d->want = -1;
		follow();
	}
}

/* ---- files ---- */

static void
switch_to(int i)
{
	current = i;
	d = &docs[i];
	dirty_all = 1;
	drawn_top = -1;
	titled();
	find_brace();
}

/* A new tab, empty; false when there is no room for one. */
static int
new_doc(void)
{
	if (doc_count == DOCS) {
		say("no more than 16 texts are open at once");
		return 0;
	}
	memset(&docs[doc_count], 0, sizeof docs[0]);
	d = &docs[doc_count];
	blank();
	switch_to(doc_count++);
	return 1;
}

static int
save_as(void)
{
	char where[512], chosen[512], *slash;

	snprintf(where, sizeof where, "%s", d->path);
	if ((slash = strrchr(where, '/')) != NULL)
		*slash = 0;
	return ui_save_file("Save the text as", !slash ? "/root" : slash == where ? "/" : where, NULL, slash ? slash + 1 : where,
			    chosen, sizeof chosen, others) && write_to(chosen);
}

static int
save(void)
{
	return d->named ? write_to(d->path) : save_as();
}

/* True if text i may be let go: it has no changes, or the answer was to save them or not to. */
static int
may_drop(int i)
{
	static const char *const answers[] = { "Save", "Don't save", "Cancel" };
	char text[400];

	if (!changed(&docs[i]))
		return 1;
	if (i != current)
		switch_to(i);
	flush();
	snprintf(text, sizeof text, "%.200s has changes that are not saved.\nSave them?", leaf(d->path));
	switch (ui_ask("Editor", text, answers, 3, others)) {
	case 0: return save();
	case 1: return 1;
	}
	return 0;
}

static void
close_doc(int i)
{
	if (!may_drop(i))
		return;
	clear_doc(&docs[i]);
	memmove(docs + i, docs + i + 1, (doc_count - i - 1) * sizeof docs[0]);
	doc_count--;
	memset(&docs[doc_count], 0, sizeof docs[0]);
	if (!doc_count) {
		new_doc();
		return;
	}
	switch_to(current >= doc_count ? doc_count - 1 : current > i ? current - 1 : current);
}

static void
quit(void)
{
	int i;

	for (i = 0; i < doc_count; i++)
		if (!may_drop(i))
			return;
	GrClose();
	exit(0);
}

/* A file in a tab: its own if it is open, the empty one there is, or a new one. */
static void
open_file(const char *file)
{
	int i;

	for (i = 0; i < doc_count; i++)
		if (docs[i].named && !strcmp(docs[i].path, file)) {
			switch_to(i);
			return;
		}
	if (!d || d->named || changed(d) || d->count > 1 || d->lines[0].length) {
		if (!new_doc())
			return;
	}
	clear_doc(d);	/* (a new tab has blank()'s line, which is not the file's) */
	load(file);
	switch_to(current);
	tabs_dirty = 1;
}

static void
reload(void)
{
	static const char *const answers[] = { "Reload", "Cancel" };
	char file[512];
	int cy = d->cy, reading, read_y;

	if (!d->named) {
		say("this text has no file yet");
		return;
	}
	if (changed(d) && ui_ask("Editor", "Reading the file again loses the changes made here.", answers, 2, others) != 0)
		return;
	snprintf(file, sizeof file, "%s", d->path);
	reading = d->reading, read_y = d->read_y;
	clear_doc(d);
	load(file);
	d->cy = cy < d->count ? cy : d->count - 1;
	d->reading = reading, d->read_y = read_y;
	switch_to(current);
}

#include "nxedit_read.h"

/* ---- commands ---- */

enum { C_NEW = 1, C_OPEN, C_RELOAD, C_SAVE, C_SAVE_AS, C_SAVE_ALL, C_CLOSE, C_CLOSE_ALL, C_EXIT,
	C_UNDO, C_REDO, C_CUT, C_COPY, C_PASTE, C_DELETE, C_ALL,
	C_DUPLICATE, C_DELETE_LINE, C_LINE_UP, C_LINE_DOWN, C_SORT, C_TRIM, C_INDENT, C_OUTDENT, C_UPPER, C_LOWER, C_LF, C_CRLF,
	C_OVERWRITE, C_FIND, C_NEXT, C_PREVIOUS, C_REPLACE, C_FIND_WORD, C_LINE, C_BRACE,
	C_WRAP, C_NUMBERS, C_WHITESPACE, C_LIT, C_COLOUR, C_TAB2, C_TAB4, C_TAB8, C_NEXT_TAB, C_PREVIOUS_TAB, C_READ, C_BACK,
	C_RECENT = 100 };

static struct ui_menu_item recent_items[RECENT];
static char recent_labels[RECENT][64];
static struct ui_menu_item file_items[] = {
	{ "New   Ctrl+N", -1, C_NEW }, { "Open...   Ctrl+O", -1, C_OPEN }, { "Open recent", -1, 0, recent_items, 0 },
	{ "Reload from the file", -1, C_RELOAD }, { "Save   Ctrl+S", -1, C_SAVE, NULL, 0, UI_MENU_LINE }, { "Save as...", -1, C_SAVE_AS },
	{ "Save all   Ctrl+Shift+S", -1, C_SAVE_ALL }, { "Close   Ctrl+W", -1, C_CLOSE, NULL, 0, UI_MENU_LINE },
	{ "Close all", -1, C_CLOSE_ALL }, { "Exit   Ctrl+Q", -1, C_EXIT, NULL, 0, UI_MENU_LINE },
};
static struct ui_menu_item lines_items[] = {
	{ "Duplicate   Ctrl+D", -1, C_DUPLICATE }, { "Delete   Ctrl+L", -1, C_DELETE_LINE },
	{ "Move up   Ctrl+Shift+Up", -1, C_LINE_UP }, { "Move down   Ctrl+Shift+Down", -1, C_LINE_DOWN },
	{ "Indent   Tab", -1, C_INDENT, NULL, 0, UI_MENU_LINE }, { "Outdent   Shift+Tab", -1, C_OUTDENT },
	{ "Sort", -1, C_SORT, NULL, 0, UI_MENU_LINE }, { "Trim trailing spaces", -1, C_TRIM },
};
static struct ui_menu_item case_items[] = {
	{ "UPPER CASE   Ctrl+Shift+U", -1, C_UPPER }, { "lower case   Ctrl+U", -1, C_LOWER },
};
static struct ui_menu_item ending_items[] = {
	{ "Unix (LF)", -1, C_LF }, { "Windows (CR LF)", -1, C_CRLF },
};
static struct ui_menu_item edit_items[] = {
	{ "Undo   Ctrl+Z", -1, C_UNDO }, { "Redo   Ctrl+Y", -1, C_REDO }, { "Cut   Ctrl+X", -1, C_CUT, NULL, 0, UI_MENU_LINE },
	{ "Copy   Ctrl+C", -1, C_COPY }, { "Paste   Ctrl+V", -1, C_PASTE }, { "Delete   Del", -1, C_DELETE },
	{ "Select all   Ctrl+A", -1, C_ALL }, { "Lines", -1, 0, lines_items, 8, UI_MENU_LINE }, { "Case", -1, 0, case_items, 2 },
	{ "Line endings", -1, 0, ending_items, 2 }, { "Overwrite   Insert", -1, C_OVERWRITE, NULL, 0, UI_MENU_LINE },
};
static struct ui_menu_item search_items[] = {
	{ "Find...   Ctrl+F", -1, C_FIND }, { "Find next   F3", -1, C_NEXT }, { "Find previous   Shift+F3", -1, C_PREVIOUS },
	{ "Find the word here   Ctrl+F3", -1, C_FIND_WORD }, { "Replace...   Ctrl+H", -1, C_REPLACE, NULL, 0, UI_MENU_LINE },
	{ "Go to line...   Ctrl+G", -1, C_LINE, NULL, 0, UI_MENU_LINE }, { "Go to the matching brace   Ctrl+B", -1, C_BRACE },
};
static struct ui_menu_item tab_items[] = {
	{ "2", -1, C_TAB2 }, { "4", -1, C_TAB4 }, { "8", -1, C_TAB8 },
};
static struct ui_menu_item view_items[] = {
	{ "Read (Markdown)   Ctrl+R", -1, C_READ }, { "Word wrap", -1, C_WRAP, NULL, 0, UI_MENU_LINE }, { "Line numbers", -1, C_NUMBERS }, { "Spaces and tabs", -1, C_WHITESPACE },
	{ "The cursor's line lit", -1, C_LIT }, { "Colours for C", -1, C_COLOUR }, { "Tab width", -1, 0, tab_items, 3 },
	{ "Next tab   Ctrl+PgDn", -1, C_NEXT_TAB, NULL, 0, UI_MENU_LINE }, { "Previous tab   Ctrl+PgUp", -1, C_PREVIOUS_TAB },
};
static struct ui_menu_item pointer_items[] = {
	{ "Undo", -1, C_UNDO }, { "Redo", -1, C_REDO }, { "Cut", -1, C_CUT, NULL, 0, UI_MENU_LINE }, { "Copy", -1, C_COPY },
	{ "Paste", -1, C_PASTE }, { "Delete", -1, C_DELETE }, { "Select all", -1, C_ALL, NULL, 0, UI_MENU_LINE },
};
/* the right button's while reading */
static struct ui_menu_item read_items[] = {
	{ "Back   Backspace", -1, C_BACK }, { "Edit the text   Ctrl+R", -1, C_READ, NULL, 0, UI_MENU_LINE },
};
static const struct { struct ui_menu_item *items; int count; } bar_menus[4] = {
	{ file_items, 10 }, { edit_items, 11 }, { search_items, 7 }, { view_items, 9 },
};

static void
flag(struct ui_menu_item *items, int count, int id, int bit, int on)
{
	int i;

	for (i = 0; i < count; i++)
		if (items[i].id == id)
			items[i].flags = on ? items[i].flags | bit : items[i].flags & ~bit;
}

/* The menus' ticks and grey rows as things are now. */
static void
menu_flags(void)
{
	int x0, y0, x1, y1, some = selection(&x0, &y0, &x1, &y1), i;

	for (i = 0; i < recent_count; i++) {
		int n = (int)strlen(recent[i]);

		snprintf(recent_labels[i], sizeof recent_labels[0], n > 50 ? "...%s" : "%s", recent[i] + (n > 50 ? n - 50 : 0));
		recent_items[i].label = recent_labels[i];
		recent_items[i].icon = -1;
		recent_items[i].id = C_RECENT + i;
	}
	file_items[2].sub_count = recent_count;
	file_items[2].flags = recent_count ? 0 : UI_MENU_GREY;
	flag(edit_items, 11, C_UNDO, UI_MENU_GREY, !d->undo_count);
	flag(edit_items, 11, C_REDO, UI_MENU_GREY, !d->redo_count);
	flag(pointer_items, 7, C_UNDO, UI_MENU_GREY, !d->undo_count);
	flag(pointer_items, 7, C_REDO, UI_MENU_GREY, !d->redo_count);
	for (i = C_CUT; i <= C_DELETE; i++)
		if (i != C_PASTE) {
			flag(edit_items, 11, i, UI_MENU_GREY, !some);
			flag(pointer_items, 7, i, UI_MENU_GREY, !some);
		}
	flag(case_items, 2, C_UPPER, UI_MENU_GREY, !some);
	flag(case_items, 2, C_LOWER, UI_MENU_GREY, !some);
	flag(file_items, 10, C_RELOAD, UI_MENU_GREY, !d->named);
	flag(ending_items, 2, C_LF, UI_MENU_CHECK, !d->crlf);
	flag(ending_items, 2, C_CRLF, UI_MENU_CHECK, d->crlf);
	flag(edit_items, 11, C_OVERWRITE, UI_MENU_CHECK, overwrite);
	flag(view_items, 9, C_WRAP, UI_MENU_CHECK, wrap);
	flag(view_items, 9, C_NUMBERS, UI_MENU_CHECK, numbers);
	flag(view_items, 9, C_WHITESPACE, UI_MENU_CHECK, whitespace);
	flag(view_items, 9, C_LIT, UI_MENU_CHECK, line_lit);
	flag(view_items, 9, C_COLOUR, UI_MENU_CHECK, colour);
	flag(view_items, 9, C_NEXT_TAB, UI_MENU_GREY, doc_count < 2);
	flag(view_items, 9, C_PREVIOUS_TAB, UI_MENU_GREY, doc_count < 2);
	flag(view_items, 9, C_READ, UI_MENU_CHECK, d->reading);
	flag(read_items, 2, C_BACK, UI_MENU_GREY, !d->back_count);
	flag(tab_items, 3, C_TAB2, UI_MENU_CHECK, tab_width == 2);
	flag(tab_items, 3, C_TAB4, UI_MENU_CHECK, tab_width == 4);
	flag(tab_items, 3, C_TAB8, UI_MENU_CHECK, tab_width == 8);
}

static void
open_bar_menu(int i, int keyboard)
{
	int wx, wy;

	menu_flags();
	ui_window_origin(window, &wx, &wy);
	bar_open = i;
	draw_bar();
	ui_menu_open(bar_menus[i].items, bar_menus[i].count, wx + bar_x[i], wy + MENU_H, 0, keyboard);
}

/* True when the selection is on the clipboard. */
static int
copy_selection(void)
{
	int x0, y0, x1, y1, length;
	char *text;

	if (!selection(&x0, &y0, &x1, &y1)) {
		say("nothing is selected");
		return 0;
	}
	if ((text = text_of(x0, y0, x1, y1, &length)) == NULL) {
		say("too large to copy");
		return 0;
	}
	ui_clip_set(text, length);
	free(text);
	say("copied");
	return 1;
}

static void
paste(void)
{
	char *text;
	int n, i, j;

	if ((text = ui_clip_get(&n)) == NULL) {
		say("the clipboard is empty");
		return;
	}
	for (i = j = 0; i < n; i++)	/* a CR before a newline is the newline's */
		if (text[i] != '\r' || i + 1 >= n || text[i + 1] != '\n')
			text[j++] = text[i];
	delete_selection();
	d->marked = 0;
	text_insert(d->cx, d->cy, text, j);
	free(text);
}

/* Each chosen line given a tab, or (`out`) a tab or a tab's width of spaces taken from it. */
static void
indent(int out)
{
	int x0, y0, x1, y1, some = selection(&x0, &y0, &x1, &y1), first, last, y, n, cx = d->cx;
	const char *t;

	lines_chosen(&first, &last);
	for (y = first; y <= last; y++) {
		t = d->lines[y].text;
		if (!out) {
			if (d->lines[y].length || !some)
				text_insert(0, y, "\t", 1), n = 1;
			else
				n = 0;
		} else {
			n = 0;
			if (d->lines[y].length && t[0] == '\t')
				n = 1;
			else
				while (n < tab_width && n < d->lines[y].length && t[n] == ' ')
					n++;
			if (n)
				text_delete(0, y, n, y);
			n = -n;
		}
		if (y == d->cy || !some)
			cx += n;
	}
	if (some) {
		d->ax = 0, d->ay = first, d->marked = 1;
		d->cy = y1 > last ? y1 : last;
		d->cx = y1 > last ? 0 : d->lines[last].length;
	} else {
		d->cy = first;
		d->cx = cx < 0 ? 0 : cx > d->lines[first].length ? d->lines[first].length : cx;
	}
	dirty(first, last);
}

static void
duplicate(void)
{
	int x0, y0, x1, y1, length, cx = d->cx, cy = d->cy;
	char *text;

	if (selection(&x0, &y0, &x1, &y1)) {
		if ((text = text_of(x0, y0, x1, y1, &length)) == NULL)
			return;
		text_insert(x1, y1, text, length);
		free(text);
		select_range(x1, y1, d->cx, d->cy);
		return;
	}
	if ((text = malloc(d->lines[cy].length + 1)) == NULL)
		return;
	text[0] = '\n';
	memcpy(text + 1, d->lines[cy].text, d->lines[cy].length);
	text_insert(d->lines[cy].length, cy, text, d->lines[cy].length + 1);
	free(text);
	d->cx = cx, d->cy = cy;
}

static void
delete_lines(void)
{
	int first, last;

	lines_chosen(&first, &last);
	d->marked = 0;
	if (last + 1 < d->count)
		text_delete(0, first, 0, last + 1);
	else if (first > 0)
		text_delete(d->lines[first - 1].length, first - 1, d->lines[last].length, last);
	else
		text_delete(0, 0, d->lines[last].length, last);
	d->cx = 0;
	if (d->cy >= d->count)
		d->cy = d->count - 1;
}

/* The chosen lines moved past the line above them (dir -1) or below (1). */
static void
move_lines(int dir)
{
	int first, last, length, ax = d->ax, cx = d->cx, marked = d->marked;
	char *text, *more;

	lines_chosen(&first, &last);
	if (dir < 0 ? first == 0 : last + 1 >= d->count)
		return;
	/* the line passed over, with room for its newline */
	text = dir < 0 ? text_of(0, first - 1, d->lines[first - 1].length, first - 1, &length) :
		text_of(0, last + 1, d->lines[last + 1].length, last + 1, &length);
	if (!text || (more = realloc(text, length + 2)) == NULL) {
		free(text);
		say("too large to move");
		return;
	}
	text = more;
	if (dir < 0) {
		text_delete(0, first - 1, 0, first);
		memmove(text + 1, text, length);
		text[0] = '\n';
		text_insert(d->lines[last - 1].length, last - 1, text, length + 1);
	} else {
		text_delete(d->lines[last].length, last, d->lines[last + 1].length, last + 1);
		text[length] = '\n';
		text_insert(0, first, text, length + 1);
	}
	free(text);
	d->ay += dir, d->cy += dir, d->ax = ax, d->cx = cx, d->marked = marked;
	dirty(first - 1, last + 1);
}

static int
line_order(const void *a, const void *b)
{
	const struct line *x = a, *y = b;
	int n = x->length < y->length ? x->length : y->length, c = memcmp(x->text, y->text, n);

	return c ? c : x->length - y->length;
}

static void
sort_lines(void)
{
	int first, last, y, n = 0, at = 0;
	struct line *order;
	char *text;
	long size = 0;

	lines_chosen(&first, &last);
	if (first == last)
		first = 0, last = d->count - 1;
	if ((order = malloc((last - first + 1) * sizeof *order)) == NULL)
		return;
	for (y = first; y <= last; y++)
		order[n++] = d->lines[y], size += d->lines[y].length + 1;
	qsort(order, n, sizeof *order, line_order);
	if ((text = malloc(size)) != NULL) {
		for (y = 0; y < n; y++) {
			memcpy(text + at, order[y].text, order[y].length);
			at += order[y].length;
			if (y + 1 < n)
				text[at++] = '\n';
		}
		d->marked = 0;
		text_delete(0, first, d->lines[last].length, last);
		text_insert(0, first, text, at);
		select_range(0, first, d->lines[last].length, last);
		free(text);
	}
	free(order);
}

static void
trim(void)
{
	int first, last, y, n;
	const struct line *l;

	lines_chosen(&first, &last);
	if (first == last)
		first = 0, last = d->count - 1;
	for (y = first; y <= last; y++) {
		l = &d->lines[y];
		for (n = l->length; n > 0 && (l->text[n - 1] == ' ' || l->text[n - 1] == '\t'); n--)
			continue;
		if (n < l->length)
			text_delete(n, y, l->length, y);
	}
	d->marked = 0;
	d->cx = d->cx > d->lines[d->cy].length ? d->lines[d->cy].length : d->cx;
}

static void
change_case(int upper)
{
	int x0, y0, x1, y1, length, i;
	char *text;

	if (!selection(&x0, &y0, &x1, &y1) || (text = text_of(x0, y0, x1, y1, &length)) == NULL) {
		say("nothing is selected");
		return;
	}
	for (i = 0; i < length; i++)
		text[i] = (char)(upper ? toupper((unsigned char)text[i]) : tolower((unsigned char)text[i]));
	text_delete(x0, y0, x1, y1);
	text_insert(x0, y0, text, length);
	free(text);
	select_range(x0, y0, x1, y1);
}

/* The word at the cursor sought, and the next one found. */
static void
find_word(void)
{
	const struct line *l = &d->lines[d->cy];
	int a = d->cx, b = d->cx, x0, y0, x1, y1;

	if (selection(&x0, &y0, &x1, &y1) && y0 == y1) {
		a = x0, b = x1;
	} else {
		while (a > 0 && in_word(l->text[a - 1]))
			a--;
		while (b < l->length && in_word(l->text[b]))
			b++;
	}
	if (b == a || b - a >= (int)sizeof find_text) {
		say("no word here");
		return;
	}
	snprintf(find_text, sizeof find_text, "%.*s", b - a, l->text + a);
	go(a, d->cy, 0);
	find(1);
}

static void
setting(const char *key, int *value, int to)
{
	*value = to;
	reg_set_int(key, to);
	dirty_all = 1;
	lex_gen++;
}

static void command(int id);

static void
command(int id)
{
	char chosen[512], where[512], *slash;
	int i;

	notice[0] = 0;
	status_dirty = 1;
	begin();
	if (d->reading && id >= C_UNDO && id <= C_BRACE && id != C_COPY) {
		/* (what works on the text, or shows a place of it, is done where it can be seen) */
		rows_from_read();
		d->reading = 0;
		dirty_all = 1;
	}
	switch (id) {
	case C_NEW:
		new_doc();
		break;
	case C_OPEN:
		snprintf(where, sizeof where, "%s", d->path);
		if ((slash = strrchr(where, '/')) != NULL)
			slash[slash == where ? 1 : 0] = 0;
		if (ui_choose_file("Open a text", slash ? where : "/root", NULL, chosen, sizeof chosen, others))
			open_file(chosen);
		break;
	case C_RELOAD:
		reload();
		break;
	case C_SAVE:
		save();
		break;
	case C_SAVE_AS:
		save_as();
		break;
	case C_SAVE_ALL:
		for (i = 0, id = current; i < doc_count; i++)
			if (changed(&docs[i])) {
				switch_to(i);
				if (!save())
					break;
			}
		if (i == doc_count)
			switch_to(id);
		break;
	case C_CLOSE:
		close_doc(current);
		break;
	case C_CLOSE_ALL:
		while (doc_count > 1 || changed(d) || d->named) {
			int was = doc_count;

			close_doc(doc_count - 1);
			if (doc_count == was || (doc_count == 1 && !d->named && !changed(d)))
				break;
		}
		break;
	case C_EXIT:
		quit();
		break;
	case C_UNDO:
	case C_REDO:
		turn(id == C_REDO);
		break;
	case C_COPY:
		copy_selection();
		return;
	case C_CUT:
		if (copy_selection())	/* (what could not be copied is not cut) */
			delete_selection();
		break;
	case C_PASTE:
		paste();
		break;
	case C_DELETE:
		if (!delete_selection())
			say("nothing is selected");
		break;
	case C_ALL:
		select_range(0, 0, d->lines[d->count - 1].length, d->count - 1);
		return;
	case C_DUPLICATE:
		duplicate();
		break;
	case C_DELETE_LINE:
		delete_lines();
		break;
	case C_LINE_UP:
	case C_LINE_DOWN:
		move_lines(id == C_LINE_UP ? -1 : 1);
		break;
	case C_INDENT:
	case C_OUTDENT:
		indent(id == C_OUTDENT);
		break;
	case C_SORT:
		sort_lines();
		break;
	case C_TRIM:
		trim();
		break;
	case C_UPPER:
	case C_LOWER:
		change_case(id == C_UPPER);
		break;
	case C_LF:
	case C_CRLF:
		if (d->crlf != (id == C_CRLF))
			d->crlf = id == C_CRLF, d->saved_group = -1;
		break;
	case C_OVERWRITE:
		overwrite = !overwrite;
		dirty(d->cy, d->cy);
		break;
	case C_FIND:
	case C_REPLACE:
		find_open(id == C_REPLACE);
		return;
	case C_NEXT:
	case C_PREVIOUS:
		find(id == C_NEXT ? 1 : -1);
		break;
	case C_FIND_WORD:
		find_word();
		break;
	case C_LINE:
		go_to_line();
		break;
	case C_BRACE:
		if (match_y >= 0)
			go(match_x, match_y, 0);
		else
			say("no brace here, or none it pairs with");
		break;
	case C_WRAP: setting("nxedit.wrap", &wrap, !wrap); break;
	case C_NUMBERS: setting("nxedit.numbers", &numbers, !numbers); break;
	case C_WHITESPACE: setting("nxedit.whitespace", &whitespace, !whitespace); break;
	case C_LIT: setting("nxedit.lit", &line_lit, !line_lit); break;
	case C_COLOUR: setting("nxedit.colour", &colour, !colour); break;
	case C_TAB2: setting("nxedit.tabs", &tab_width, 2); break;
	case C_TAB4: setting("nxedit.tabs", &tab_width, 4); break;
	case C_TAB8: setting("nxedit.tabs", &tab_width, 8); break;
	case C_READ:
		if (d->reading)
			rows_from_read();
		else
			read_from_rows();
		d->reading = !d->reading;
		dirty_all = 1;
		if (!d->reading) {
			layout();
			follow();
			find_brace();
		}
		return;
	case C_BACK:
		read_back();
		return;
	case C_NEXT_TAB:
	case C_PREVIOUS_TAB:
		if (doc_count > 1)
			switch_to((current + (id == C_NEXT_TAB ? 1 : doc_count - 1)) % doc_count);
		break;
	default:
		if (id >= C_RECENT && id < C_RECENT + recent_count) {
			snprintf(chosen, sizeof chosen, "%s", recent[id - C_RECENT]);
			open_file(chosen);
		}
	}
	if (d->cy >= d->count)
		d->cy = d->count - 1;
	if (d->cx > d->lines[d->cy].length)
		d->cx = d->lines[d->cy].length;
	d->want = id == C_UNDO || id == C_REDO ? -1 : d->want;
	layout();
	follow();
	find_brace();
}

/* The command a letter with Ctrl is (with Shift: `shifted`), or 0. */
static int
command_of(int letter, int shift)
{
	static const char letters[] = "nosqzyxcvafghdlbuwr";
	static const int ids[] = { C_NEW, C_OPEN, C_SAVE, C_EXIT, C_UNDO, C_REDO, C_CUT, C_COPY, C_PASTE, C_ALL, C_FIND, C_LINE,
		C_REPLACE, C_DUPLICATE, C_DELETE_LINE, C_BRACE, C_LOWER, C_CLOSE, C_READ };
	const char *at = strchr(letters, letter);

	if (!at)
		return 0;
	if (shift && letter == 's')
		return C_SAVE_ALL;
	if (shift && letter == 'z')
		return C_REDO;
	if (shift && letter == 'u')
		return C_UPPER;
	return ids[at - letters];
}

/* ---- moving through the text ---- */

static int
kind_of(int c)
{
	return c == ' ' || c == '\t' ? 0 : in_word(c) ? 1 : 2;
}

static void
word_left(int *x, int *y)
{
	const char *t;
	int k;

	if (*x == 0) {
		if (*y > 0)
			--*y, *x = d->lines[*y].length;
		return;
	}
	t = d->lines[*y].text;
	while (*x > 0 && kind_of(t[*x - 1]) == 0)
		--*x;
	k = *x > 0 ? kind_of(t[*x - 1]) : 0;
	while (*x > 0 && kind_of(t[*x - 1]) == k)
		--*x;
}

static void
word_right(int *x, int *y)
{
	const struct line *l = &d->lines[*y];
	int k;

	if (*x >= l->length) {
		if (*y + 1 < d->count)
			++*y, *x = 0;
		return;
	}
	k = kind_of(l->text[*x]);
	while (*x < l->length && k && kind_of(l->text[*x]) == k)
		++*x;
	while (*x < l->length && kind_of(l->text[*x]) == 0)
		++*x;
}

/* One row up or down, keeping to the column wanted. */
static void
vertical(int *x, int *y, int dir)
{
	int n = segments(*y), k = seg_of(*x, n), yy = *y, kk = k, ok;

	if (d->want < 0)
		d->want = columns_between(*y, seg[k], *x);
	ok = dir > 0 ? row_next(&yy, &kk) : row_prev(&yy, &kk);
	if (!ok) {
		*x = dir > 0 ? d->lines[*y].length : 0;
		return;
	}
	n = segments(yy);
	*x = byte_at(yy, seg[kk], seg[kk + 1], d->want);
	if (kk < n - 1 && *x == seg[kk + 1])
		*x = prev_x(yy, *x);
	*y = yy;
}

static void
move(int ch, int shift, int ctrl)
{
	int x = d->cx, y = d->cy, x0, y0, x1, y1, want = d->want, i, t;
	const struct line *l = &d->lines[y];

	switch (ch) {
	case MWKEY_LEFT:
		if (!shift && !ctrl && selection(&x0, &y0, &x1, &y1))
			x = x0, y = y0;
		else if (ctrl)
			word_left(&x, &y);
		else if (x > 0)
			x = prev_x(y, x);
		else if (y > 0)
			x = d->lines[--y].length;
		want = -1;
		break;
	case MWKEY_RIGHT:
		if (!shift && !ctrl && selection(&x0, &y0, &x1, &y1))
			x = x1, y = y1;
		else if (ctrl)
			word_right(&x, &y);
		else if (x < l->length)
			x = next_x(y, x);
		else if (y + 1 < d->count)
			y++, x = 0;
		want = -1;
		break;
	case MWKEY_UP:
	case MWKEY_DOWN:
		if (ctrl) {
			scroll_rows(ch == MWKEY_UP ? -1 : 1);
			return;
		}
		vertical(&x, &y, ch == MWKEY_UP ? -1 : 1);
		want = d->want;
		break;
	case MWKEY_PAGEUP:
	case MWKEY_PAGEDOWN:
		for (i = 1; i < rows; i++)
			vertical(&x, &y, ch == MWKEY_PAGEUP ? -1 : 1);
		scroll_rows(ch == MWKEY_PAGEUP ? 1 - rows : rows - 1);
		want = d->want;
		break;
	case MWKEY_HOME:
		if (ctrl) {
			x = y = 0;
		} else {
			/* first where the line's text begins; from there, its very start */
			for (t = 0; t < l->length && (l->text[t] == ' ' || l->text[t] == '\t'); t++)
				continue;
			x = x == t ? 0 : t;
		}
		want = -1;
		break;
	case MWKEY_END:
		if (ctrl)
			y = d->count - 1;
		x = d->lines[y].length;
		want = -1;
		break;
	}
	go(x, y, shift);
	d->want = want;
	follow();
}

static int
moving(int ch)
{
	return ch == MWKEY_LEFT || ch == MWKEY_RIGHT || ch == MWKEY_UP || ch == MWKEY_DOWN || ch == MWKEY_PAGEUP ||
		ch == MWKEY_PAGEDOWN || ch == MWKEY_HOME || ch == MWKEY_END;
}

static void
key(const GR_EVENT_KEYSTROKE *k)
{
	int ch = k->ch, shift = (k->modifiers & MWKMOD_SHIFT) != 0, ctrl = (k->modifiers & MWKMOD_CTRL) != 0;
	int alt = (k->modifiers & MWKMOD_ALT) != 0, letter = 0, i, x, y, x0, y0, x1, y1, keep;
	char text[260];

	/* a letter with Ctrl comes as its control code (Backspace, Tab and Enter are told by their keys) */
	if (ctrl && ch >= 1 && ch <= 26 && k->scancode != 14 && k->scancode != 15 && k->scancode != 28)
		letter = 'a' + ch - 1;
	else if (ctrl && ch < 128 && isalpha(ch))
		letter = tolower(ch);
	if (letter) {
		if ((i = command_of(letter, shift)) != 0)
			command(i);
		return;
	}
	if (alt && ch < 128 && isalpha(ch)) {
		for (i = 0; i < 4; i++)
			if (tolower(bar_names[i][0]) == tolower(ch))
				open_bar_menu(i, 1);
		return;
	}
	switch (ch) {
	case MWKEY_F10:
		open_bar_menu(0, 1);
		return;
	case MWKEY_F3:
		command(ctrl ? C_FIND_WORD : shift ? C_PREVIOUS : C_NEXT);
		return;
	case MWKEY_INSERT:
		command(shift ? C_PASTE : ctrl ? C_COPY : C_OVERWRITE);
		return;
	case MWKEY_DELETE:
		if (shift && !ctrl) {
			command(C_CUT);
			return;
		}
		break;
	case MWKEY_ESCAPE:
		unmark();
		return;
	}
	/* (the window manager keeps Ctrl+Tab for itself: it never comes here on this desktop) */
	if (ctrl && (ch == MWKEY_TAB || ch == MWKEY_PAGEUP || ch == MWKEY_PAGEDOWN)) {
		command(ch == MWKEY_PAGEUP || (ch == MWKEY_TAB && shift) ? C_PREVIOUS_TAB : C_NEXT_TAB);
		return;
	}
	if (ctrl && shift && (ch == MWKEY_UP || ch == MWKEY_DOWN)) {
		command(ch == MWKEY_UP ? C_LINE_UP : C_LINE_DOWN);
		return;
	}
	if (d->reading) {
		notice[0] = 0;
		read_key(k);
		return;
	}
	notice[0] = 0;
	status_dirty = 1;
	if (moving(ch)) {
		move(ch, shift, ctrl);
		find_brace();
		return;
	}
	begin();
	x = d->cx, y = d->cy;
	keep = 0;
	switch (ch) {
	case MWKEY_ENTER:
	case '\n': {
		/* the new line indented as this one */
		const struct line *l;
		int n = 0;

		delete_selection();
		l = &d->lines[d->cy];
		while (n < d->cx && n < 255 && (l->text[n] == ' ' || l->text[n] == '\t'))
			n++;
		text[0] = '\n';
		memcpy(text + 1, l->text, n);
		text_insert(d->cx, d->cy, text, n + 1);
		break;
	}
	case MWKEY_BACKSPACE:
	case 127:
		if (delete_selection())
			break;
		if (ctrl)
			word_left(&x, &y);
		else if (x > 0)
			x = prev_x(y, x);
		else if (y > 0)
			x = d->lines[--y].length;
		text_delete(x, y, d->cx, d->cy);
		break;
	case MWKEY_DELETE:
		if (delete_selection())
			break;
		if (ctrl)
			word_right(&x, &y);
		else if (x < d->lines[y].length)
			x = next_x(y, x);
		else if (y + 1 < d->count)
			y++, x = 0;
		text_delete(d->cx, d->cy, x, y);
		break;
	case MWKEY_TAB:
		if (shift || (selection(&x0, &y0, &x1, &y1) && y1 > y0)) {
			indent(shift);
			keep = 1;
			break;
		}
		/* fall through */
	default:
		if (ch != MWKEY_TAB && (ch < 32 || ch > 126 || alt))
			return;
		text[0] = (char)ch;
		if (!delete_selection() && overwrite && d->cx < d->lines[d->cy].length)
			text_delete(d->cx, d->cy, next_x(d->cy, d->cx), d->cy);
		text_insert(d->cx, d->cy, text, 1);
	}
	d->marked = keep && d->marked;
	d->want = -1;
	follow();
	find_brace();
}

/* ---- the pointer ---- */

/* The place in the text under a point of the window. */
static void
place(int px, int py, int *x, int *y)
{
	int r = (py - text_top - MARGIN) / ui_fh, yy = d->top, k = d->sub, n, col;

	if (py < text_top + MARGIN)
		r = 0;
	if (r >= rows)
		r = rows - 1;
	while (r > 0 && row_next(&yy, &k))
		r--;
	n = segments(yy);
	col = (px - gutter - MARGIN + ui_fw / 2) / ui_fw + d->left;
	*x = byte_at(yy, seg[k], seg[k + 1], col < 0 ? 0 : col);
	if (k < n - 1 && *x == seg[k + 1])
		*x = prev_x(yy, *x);
	*y = yy;
}

static int selecting;		/* 1: the pointer is held in the text, 2: in the gutter (whole lines) */
static int dragging;		/* the scroll bar's thumb is held */
static int held_x, held_y;	/* where the pointer is while held */
static int word_ax, word_cx;	/* a double click's word: the selection keeps it whole */

static void
select_word(int x, int y)
{
	const struct line *l = &d->lines[y];
	int a = x, b = x, k = x < l->length ? kind_of(l->text[x]) : x > 0 ? kind_of(l->text[x - 1]) : 0;

	while (a > 0 && kind_of(l->text[a - 1]) == k)
		a--;
	while (b < l->length && kind_of(l->text[b]) == k)
		b++;
	select_range(a, y, b, y);
}

static void
select_lines(int from, int to)
{
	if (to >= from)
		select_range(0, from, to + 1 < d->count ? 0 : d->lines[to].length, to + 1 < d->count ? to + 1 : to);
	else
		select_range(from + 1 < d->count ? 0 : d->lines[from].length, from + 1 < d->count ? from + 1 : from, 0, to);
}

static void
click(const GR_EVENT *event)
{
	static unsigned last_time;
	static int last_px = -100, last_py, clicks;
	int x, y, px = event->button.x, py = event->button.y;

	place(px, py, &x, &y);
	if (abs(px - last_px) < 4 && abs(py - last_py) < 4 && event->button.time - last_time < 500)
		clicks = clicks % 3 + 1;
	else
		clicks = 1;
	last_px = px, last_py = py, last_time = event->button.time;
	notice[0] = 0;
	d->want = -1;
	if (px < gutter) {
		selecting = 2;
		word_ax = y;
		select_lines(y, y);
		return;
	}
	selecting = 1;
	if (clicks == 2) {
		select_word(x, y);
		word_ax = d->ax, word_cx = d->cx;
	} else if (clicks == 3) {
		select_lines(y, y);
		selecting = 2, word_ax = y;
	} else if (event->button.modifiers & MWKMOD_SHIFT) {
		go(x, y, 1);
	} else
		go(x, y, 0);
	if (clicks != 2)
		word_ax = -1;
}

/* The pointer moved with the button held, or it is held outside the text: the selection follows. */
static void
drag(void)
{
	int x, y, py = held_y;

	if (py < text_top + MARGIN)
		scroll_rows(-1);
	else if (py >= height - STATUS)
		scroll_rows(1), py = height - STATUS - 1;
	place(held_x, py, &x, &y);
	if (selecting == 2) {
		select_lines(word_ax, y);
		return;
	}
	if (word_ax >= 0 && y == d->ay) {
		/* a double click held: whole words */
		int a = word_ax, b = word_cx;

		if (x < a)
			select_range(b, y, x, y);
		else
			select_range(a, y, x > b ? x : b, y);
		return;
	}
	if (x != d->cx || y != d->cy)
		go(x, y, 1);
}

static void
main_event(GR_EVENT *event)
{
	int i, text_bottom = height - STATUS, x, y, x0, y0, x1, y1;

	switch (event->type) {
	case GR_EVENT_TYPE_EXPOSURE:
		dirty_all = 1;
		break;
	case GR_EVENT_TYPE_UPDATE:
		if (event->update.utype == GR_UPDATE_SIZE && event->update.wid == window)
			resized(event->update.width, event->update.height);
		break;
	case GR_EVENT_TYPE_BUTTON_DOWN:
		if ((i = ui_wheel(event)) != 0) {
			if (d->reading)
				read_scroll(2 * READ_LINE * ui_wheel_sum(event));
			else
				scroll_rows(3 * ui_wheel_sum(event));
			break;
		}
		if (event->button.y < MENU_H) {
			for (i = 0; i < 4; i++)
				if (event->button.x >= bar_x[i] && event->button.x < bar_x[i + 1])
					open_bar_menu(i, 0);
			break;
		}
		if (event->button.y < text_top) {
			for (i = 0; i < doc_count; i++)
				if (event->button.x >= tab_x[i] && event->button.x < tab_x[i + 1]) {
					if ((event->button.changebuttons & GR_BUTTON_M) || event->button.x >= tab_x[i + 1] - 18)
						close_doc(i);
					else if (i != current)
						switch_to(i);
				}
			break;
		}
		if (event->button.y >= text_bottom)
			break;
		if (d->reading) {
			notice[0] = 0;
			status_dirty = 1;
			if (event->button.x >= width - UI_SCROLL_W) {
				ui_moves(window, EVENTS, 1);
				dragging = 1;
				read_to(ui_scroll_to(event->button.y, text_top, text_bottom - text_top, rp_height_guess(), read_view_h()));
			} else if (event->button.changebuttons & GR_BUTTON_R) {
				menu_flags();
				ui_menu_open(read_items, 2, event->button.rootx, event->button.rooty, 0, 0);
			} else if (event->button.changebuttons & GR_BUTTON_L)
				read_link(event->button.x, event->button.y);
			break;
		}
		if (event->button.changebuttons & GR_BUTTON_R) {
			place(event->button.x, event->button.y, &x, &y);
			if (!selection(&x0, &y0, &x1, &y1) || y < y0 || y > y1 || (y == y0 && x < x0) || (y == y1 && x > x1))
				go(x, y, 0);
			menu_flags();
			ui_menu_open(pointer_items, 7, event->button.rootx, event->button.rooty, 0, 0);
			break;
		}
		ui_moves(window, EVENTS, 1);
		held_x = event->button.x, held_y = event->button.y;
		if (event->button.x >= width - UI_SCROLL_W) {
			dragging = 1;
			d->top = ui_scroll_to(event->button.y, text_top, text_bottom - text_top, d->count, rows), d->sub = 0;
		} else if (event->button.changebuttons & GR_BUTTON_L) {
			click(event);
			follow();
			find_brace();
		}
		break;
	case GR_EVENT_TYPE_BUTTON_UP:
		dragging = selecting = 0;
		if (!(event->button.buttons & (GR_BUTTON_L | GR_BUTTON_R | GR_BUTTON_M)))
			ui_moves(window, EVENTS, 0);
		break;
	case GR_EVENT_TYPE_MOUSE_MOTION:
		held_x = event->mouse.x, held_y = event->mouse.y;
		if (dragging && (event->mouse.buttons & GR_BUTTON_L) && d->reading) {
			read_to(ui_scroll_to(event->mouse.y, text_top, text_bottom - text_top, rp_height_guess(), read_view_h()));
		} else if (dragging && (event->mouse.buttons & GR_BUTTON_L)) {
			d->top = ui_scroll_to(event->mouse.y, text_top, text_bottom - text_top, d->count, rows), d->sub = 0;
		} else if (selecting && (event->mouse.buttons & GR_BUTTON_L)) {
			drag();
			find_brace();
		} else {
			dragging = selecting = 0;
			if (!event->mouse.buttons)
				ui_moves(window, EVENTS, 0);
		}
		break;
	case GR_EVENT_TYPE_KEY_DOWN:
		key(&event->keystroke);
		break;
	case GR_EVENT_TYPE_CLOSE_REQ:
		if (event->general.wid == window)	/* (a menu's may come after the menu went) */
			command(C_EXIT);
		break;
	}
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	int i, x, id, wide, high;
	const GR_SCREEN_INFO *screen;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	for (i = 0; i < 256; i++)
		fold[i] = (unsigned char)tolower(i);
	scroll_drawn = getenv("NXEDIT_SCROLL") && !strcmp(getenv("NXEDIT_SCROLL"), "draw");
	wrap = reg_int("nxedit.wrap", 0);
	numbers = reg_int("nxedit.numbers", 1);
	whitespace = reg_int("nxedit.whitespace", 0);
	line_lit = reg_int("nxedit.lit", 1);
	colour = reg_int("nxedit.colour", 1);
	tab_width = reg_int("nxedit.tabs", 4);
	tab_width = tab_width < 1 || tab_width > 16 ? 4 : tab_width;
	recent_read();
	read_dump = getenv("NXEDIT_DUMP") != NULL;
	if (argc > 1 && !strcmp(argv[1], "--read"))
		read_wanted = 1, argc--, argv++;
	for (i = 0, x = 2; i < 4; i++) {
		bar_x[i] = x;
		x += ui_width(&ui_var_widths, ui_var, bar_names[i], -1) + 16;
	}
	bar_x[4] = x;
	if (argc > 1)
		for (i = 1; i < argc; i++)
			open_file(argv[i]);
	else
		new_doc();
	if (doc_count > 1)
		switch_to(0);
	screen = ui_screen();
	wide = 80 * ui_fw + 6 * ui_fw + 4 + 2 * MARGIN + UI_SCROLL_W;
	high = 26 * ui_fh + MARGIN + MENU_H + STATUS + (doc_count > 1 ? TABS_H : 0);
	wide = wide > screen->cols - 20 ? screen->cols - 20 : wide;
	high = high > screen->rows - 60 ? screen->rows - 60 : high;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Editor", GR_ROOT_WINDOW_ID, -1, -1, wide, high, WHITE);
	titled();
	GrSelectEvents(window, EVENTS);
	resized(wide, high);
	GrMapWindow(window);
	GrSetFocus(window);
	for (;;) {
		if (read_pending() && !GrPeekEvent(&event)) {
			/* the rest of a page being read is laid out while nothing waits */
			read_step();
			flush();
			continue;
		}
		if (selecting && (held_y < text_top + MARGIN || held_y >= height - STATUS)) {
			/* held above or below the text: it scrolls while the pointer stays */
			GrGetNextEventTimeout(&event, 60);
			if (event.type == GR_EVENT_TYPE_TIMEOUT) {
				drag();
				follow();
				flush();
				continue;
			}
		} else
			GrGetNextEvent(&event);
		ui_origin_note(&event);
		if (event.type == GR_EVENT_TYPE_KEY_DOWN && bar_open >= 0 && ui_menu_depth == 1 &&
		    (event.keystroke.ch == MWKEY_LEFT || event.keystroke.ch == MWKEY_RIGHT)) {
			/* Left and Right go along the bar */
			i = (bar_open + (event.keystroke.ch == MWKEY_LEFT ? 3 : 1)) % 4;
			if (event.keystroke.ch == MWKEY_LEFT || ui_menu_level[0].hover < 0 || !ui_menu_level[0].items[ui_menu_level[0].hover].sub) {
				ui_menu_close();
				open_bar_menu(i, 1);
				continue;
			}
		}
		switch (ui_menu_event(&event, &id)) {
		case UI_MENU_CHOSEN:
			if (bar_open >= 0)
				bar_open = -1, draw_bar();
			GrSetFocus(window);
			command(id);
			flush();
			continue;
		case UI_MENU_CLOSED:
			if (bar_open >= 0)
				bar_open = -1, draw_bar();
			GrSetFocus(window);
			/* fall through */
		case UI_MENU_TOOK:
			continue;
		}
		if (finder.window && event.general.wid == finder.window)
			find_event(&event);
		else
			main_event(&event);
		flush();
	}
}
