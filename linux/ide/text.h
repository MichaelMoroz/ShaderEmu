/* text.h: the IDE's text: lines, a cursor and a selection, and every change kept so that it
 * can be taken back and made again (docs/apps/ide.md). Nothing here draws. */
#ifndef IDE_TEXT_H
#define IDE_TEXT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TX_HISTORY 16384	/* changes; a command's many are one Undo, and the oldest drop out */
#define TX_TAB 4

static char **tx_lines;		/* each allocated, no newline, no tabs */
static int tx_count, tx_room;
static int tx_x, tx_y;		/* the cursor: column, line */
static int tx_ax, tx_ay, tx_marked;	/* where a selection began, when there is one: it ends at the cursor */
static int tx_modified;
static int tx_touched = 1 << 30;	/* the first line changed since whoever draws last set this */
static int tx_shifted;		/* lines were added or taken out since then */
static int tx_touched_last = -1;	/* the last line changed since then, while none was added or taken out */

/* What was done: text put in from (x0, y0) to (x1, y1), or `text` taken out at (x0, y0).
 * Changes with one `step` were one command, and go back together. */
struct tx_change { int inserted, x0, y0, x1, y1, length, typed, step; char *text; };
static struct tx_change *tx_done, *tx_undone;	/* grown as needed, up to TX_HISTORY each */
static int tx_done_count, tx_undone_count, tx_done_room, tx_undone_room, tx_step;
static int tx_replaying;	/* 0: a new change; 1: one being taken back; 2: one being made again */

static void
tx_line_in(int at, const char *text, int length)
{
	if (tx_count == tx_room) {
		tx_room = tx_room ? tx_room * 2 : 64;
		tx_lines = realloc(tx_lines, (size_t)tx_room * sizeof *tx_lines);
	}
	memmove(tx_lines + at + 1, tx_lines + at, (size_t)(tx_count - at) * sizeof *tx_lines);
	tx_lines[at] = malloc((size_t)length + 1);
	memcpy(tx_lines[at], text, (size_t)length);
	tx_lines[at][length] = 0;
	tx_count++;
	tx_shifted = 1;
}

static void
tx_push(struct tx_change **stack, int *count, int *room, struct tx_change c)
{
	if (*count == TX_HISTORY) {
		free((*stack)[0].text);
		memmove(*stack, *stack + 1, (TX_HISTORY - 1) * sizeof (*stack)[0]);
		(*count)--;
	}
	if (*count == *room) {
		*room = *room ? *room * 2 : 32;
		*stack = realloc(*stack, (size_t)*room * sizeof (*stack)[0]);
	}
	(*stack)[(*count)++] = c;
}

static void
tx_remember(int inserted, int x0, int y0, int x1, int y1, char *text, int length)
{
	struct tx_change c = { inserted, x0, y0, x1, y1, length, 0, tx_step, text }, *last;

	if (y0 < tx_touched)
		tx_touched = y0;
	if ((inserted ? y1 : y0) > tx_touched_last)
		tx_touched_last = inserted ? y1 : y0;
	if (tx_replaying == 1) {
		tx_push(&tx_undone, &tx_undone_count, &tx_undone_room, c);
		return;
	}
	if (tx_replaying == 0) {
		while (tx_undone_count)
			free(tx_undone[--tx_undone_count].text);
		/* a letter typed after a letter is one change with it */
		c.typed = inserted && y0 == y1 && x1 == x0 + 1;
		last = tx_done_count ? &tx_done[tx_done_count - 1] : NULL;
		if (c.typed && last && last->typed && last->y1 == y0 && last->x1 == x0) {
			last->x1 = x1;
			return;
		}
	}
	tx_push(&tx_done, &tx_done_count, &tx_done_room, c);
}

/* The text from (x0, y0) to (x1, y1), lines parted by a newline; the caller frees it. */
static char *
tx_text(int x0, int y0, int x1, int y1, int *length)
{
	int n = 0, y;
	char *text;

	for (y = y0; y <= y1; y++)
		n += (y == y1 ? x1 : (int)strlen(tx_lines[y]) + 1) - (y == y0 ? x0 : 0);
	text = malloc((size_t)n + 1);
	n = 0;
	for (y = y0; y <= y1; y++) {
		int from = y == y0 ? x0 : 0, to = y == y1 ? x1 : (int)strlen(tx_lines[y]);

		memcpy(text + n, tx_lines[y] + from, (size_t)(to - from));
		n += to - from;
		if (y < y1)
			text[n++] = '\n';
	}
	text[n] = 0;
	*length = n;
	return text;
}

/* Puts text in at (x, y) and leaves the cursor after it. A tab becomes spaces to its stop. */
static void
tx_insert(int x, int y, const char *text, int length)
{
	int x0 = x, y0 = y, tail_length = (int)strlen(tx_lines[y]) - x, i, k, n, from, last;
	char *tail = NULL, *line;

	/* a line of the text at a time: its width first, so that its line is made longer once */
	for (from = 0; ; from = i + 1) {
		for (i = from, n = 0; i < length && text[i] != '\n'; i++)
			n += text[i] == '\r' ? 0 : text[i] == '\t' ? TX_TAB - (x + n) % TX_TAB : 1;
		last = i >= length;
		if (!last && !from) {
			/* what was after the cursor goes to the end of the last line put in */
			tail = malloc((size_t)tail_length + 1);
			memcpy(tail, tx_lines[y] + x, (size_t)tail_length + 1);
		}
		line = tx_lines[y] = realloc(tx_lines[y], (size_t)(x + n + (last ? tail_length : 0) + 1));
		if (last && tail)
			memcpy(line + x + n, tail, (size_t)tail_length + 1);
		else if (last)
			memmove(line + x + n, line + x, (size_t)tail_length + 1);
		for (k = from; k < i; k++) {
			int ch = (unsigned char)text[k], w = ch == '\t' ? TX_TAB - x % TX_TAB : ch != '\r';

			while (w-- > 0)
				line[x++] = ch < 32 || ch > 126 ? ' ' : (char)ch;
		}
		if (last)
			break;
		line[x] = 0;
		tx_line_in(++y, "", 0);
		x = 0;
	}
	free(tail);
	tx_x = x, tx_y = y;
	tx_modified = 1;
	tx_remember(1, x0, y0, x, y, NULL, 0);
}

/* Takes the text from (x0, y0) to (x1, y1) out and leaves the cursor there. */
static void
tx_delete(int x0, int y0, int x1, int y1)
{
	int length, y, rest;
	char *text;

	if (y0 == y1 && x0 == x1)
		return;
	text = tx_text(x0, y0, x1, y1, &length);
	rest = (int)strlen(tx_lines[y1]) - x1;
	if (y1 == y0) {
		memmove(tx_lines[y0] + x0, tx_lines[y0] + x1, (size_t)rest + 1);
	} else {
		tx_lines[y0] = realloc(tx_lines[y0], (size_t)(x0 + rest + 1));
		memcpy(tx_lines[y0] + x0, tx_lines[y1] + x1, (size_t)rest + 1);
		for (y = y0 + 1; y <= y1; y++)
			free(tx_lines[y]);
		memmove(tx_lines + y0 + 1, tx_lines + y1 + 1, (size_t)(tx_count - y1 - 1) * sizeof *tx_lines);
		tx_count -= y1 - y0;
		tx_shifted = 1;
	}
	tx_x = x0, tx_y = y0;
	tx_modified = 1;
	tx_remember(0, x0, y0, x1, y1, text, length);
}

/* Takes the last command back (again: 0), or makes the last one taken back again (1).
 * False when there is none. */
static int
tx_replay(int again)
{
	struct tx_change **stack = again ? &tx_undone : &tx_done, c;
	int *count = again ? &tx_undone_count : &tx_done_count, step;

	if (!*count)
		return 0;
	step = (*stack)[*count - 1].step;
	tx_step++;
	tx_replaying = again ? 2 : 1;
	while (*count && (*stack)[*count - 1].step == step) {
		c = (*stack)[--*count];
		if (c.inserted)
			tx_delete(c.x0, c.y0, c.x1, c.y1);
		else
			tx_insert(c.x0, c.y0, c.text, c.length);
		free(c.text);
	}
	tx_replaying = 0;
	tx_marked = 0;
	return 1;
}

/* The selection's two ends in the text's order; false when there is none. */
static int
tx_selection(int *x0, int *y0, int *x1, int *y1)
{
	if (!tx_marked || (tx_ax == tx_x && tx_ay == tx_y))
		return 0;
	if (tx_ay < tx_y || (tx_ay == tx_y && tx_ax < tx_x))
		*x0 = tx_ax, *y0 = tx_ay, *x1 = tx_x, *y1 = tx_y;
	else
		*x0 = tx_x, *y0 = tx_y, *x1 = tx_ax, *y1 = tx_ay;
	return 1;
}

static int
tx_delete_selection(void)
{
	int x0, y0, x1, y1;

	if (!tx_selection(&x0, &y0, &x1, &y1))
		return 0;
	tx_delete(x0, y0, x1, y1);
	tx_marked = 0;
	return 1;
}

/* Forgets the text and its history (the arrays stay for the next text). */
static void
tx_forget(void)
{
	int i;

	for (i = 0; i < tx_count; i++)
		free(tx_lines[i]);
	tx_count = 0;
	while (tx_done_count)
		free(tx_done[--tx_done_count].text);
	while (tx_undone_count)
		free(tx_undone[--tx_undone_count].text);
}

/* Forgets the text and its history, and takes a file's (or one empty line). */
static int
tx_load(const char *path)
{
	FILE *file = path ? fopen(path, "r") : NULL;
	char text[2048], *line = NULL;
	int i, x = 0, room = 0;

	tx_forget();
	/* the file's lines as tx_insert would leave them, each made once (not a change to take back) */
	while (file && fgets(text, sizeof text, file))
		for (i = 0; text[i]; i++) {
			int ch = (unsigned char)text[i], n = ch == '\t' ? TX_TAB - x % TX_TAB : ch != '\r';

			if (ch == '\n') {
				tx_line_in(tx_count, line ? line : "", x);
				x = 0;
				continue;
			}
			if (x + n > room)
				line = realloc(line, (size_t)(room = room * 2 + 256));
			while (n-- > 0)
				line[x++] = ch < 32 || ch > 126 ? ' ' : (char)ch;
		}
	tx_line_in(tx_count, line ? line : "", x);
	free(line);
	if (file)
		fclose(file);
	/* a file that ends in a newline has no empty last line of its own */
	if (tx_count > 1 && !tx_lines[tx_count - 1][0])
		free(tx_lines[--tx_count]);
	tx_x = tx_y = tx_marked = tx_modified = 0;
	tx_touched = 0, tx_shifted = 1, tx_touched_last = -1;
	return file != NULL;
}

static int
tx_save(const char *path)
{
	FILE *file = fopen(path, "w");
	int i;

	if (!file)
		return 0;
	for (i = 0; i < tx_count; i++)
		fprintf(file, "%s\n", tx_lines[i]);
	/* (a write that failed is not a save: the text stays changed) */
	i = !ferror(file);
	if (fclose(file) != 0 || !i)
		return 0;
	tx_modified = 0;
	return 1;
}

/* Looks for `sought` from just after the cursor, round the end to the start; selects it. */
static int
tx_find(const char *sought)
{
	int n = (int)strlen(sought), y, pass, x0, y0, x1, y1, start_x = tx_x, start_y = tx_y;

	if (!n)
		return 0;
	if (tx_selection(&x0, &y0, &x1, &y1))
		start_x = x0 + 1, start_y = y0;		/* past the one found last */
	for (pass = 0; pass <= tx_count; pass++) {
		const char *line, *at;
		int from = 0;

		y = (start_y + pass) % tx_count;
		line = tx_lines[y];
		if (pass == 0)
			from = start_x < (int)strlen(line) ? start_x : (int)strlen(line);
		if ((at = strstr(line + from, sought)) != NULL) {
			tx_ax = (int)(at - line), tx_ay = y;
			tx_x = tx_ax + n, tx_y = y;
			tx_marked = 1;
			return 1;
		}
	}
	return 0;
}
/* The same backwards, from just before the cursor or the selection, round the start to the end. */
static int
tx_find_back(const char *sought)
{
	int n = (int)strlen(sought), pass, x0, y0, x1, y1, start_x = tx_x, start_y = tx_y;

	if (!n)
		return 0;
	if (tx_selection(&x0, &y0, &x1, &y1))
		start_x = x0, start_y = y0;
	for (pass = 0; pass <= tx_count; pass++) {
		int y = ((start_y - pass) % tx_count + tx_count) % tx_count, limit = pass ? 1 << 30 : start_x;
		const char *line = tx_lines[y], *at, *best = NULL;

		for (at = strstr(line, sought); at && at - line < limit; at = strstr(at + 1, sought))
			best = at;
		if (best) {
			tx_ax = (int)(best - line), tx_ay = y;
			tx_x = tx_ax + n, tx_y = y;
			tx_marked = 1;
			return 1;
		}
	}
	return 0;
}

/* Every `sought` made `with` (one line of text each), as one change; how many there were. */
static int
tx_replace_all(const char *sought, const char *with)
{
	int n = (int)strlen(sought), m = (int)strlen(with), y, count = 0, x = tx_x, cy = tx_y;
	char *at;

	if (!n)
		return 0;
	for (y = 0; y < tx_count; y++) {
		int from = 0;

		while ((at = strstr(tx_lines[y] + from, sought)) != NULL) {
			int k = (int)(at - tx_lines[y]);

			tx_delete(k, y, k + n, y);
			if (m)
				tx_insert(k, y, with, m);
			from = k + m;
			count++;
		}
	}
	tx_y = cy < tx_count ? cy : tx_count - 1;
	tx_x = x < (int)strlen(tx_lines[tx_y]) ? x : (int)strlen(tx_lines[tx_y]);
	tx_marked = 0;
	return count;
}
#endif
