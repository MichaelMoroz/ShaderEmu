/*
 * nxedit: a text editor. nxedit FILE opens or creates the file. Arrow keys, Home, End, Page
 * Up and Down move, with Shift they select, and so does the pointer held down; Ctrl+C, Ctrl+X
 * and Ctrl+V copy, cut and paste through the desktop's clipboard (ui.h), Ctrl+A selects all,
 * Ctrl+Z undoes, Ctrl+F finds and F3 finds the next; Ctrl+S saves, Ctrl+Q quits.
 */
#include "ui.h"

#define STATUS 18
#define MARGIN 3
#define UNDO_MOST 512

static char **lines;		/* each allocated, no newline */
static int count, room;
static int cx, cy;		/* the cursor: column, line */
static int ax, ay, marked;	/* where a selection began, when there is one: it ends at the cursor */
static int top, left;		/* first line and column shown */
static int rows, columns;	/* that fit the window */
static int dragging;		/* the scroll bar's thumb is held */
static int selecting;		/* the pointer is held in the text */
static int width, height, modified, leaving;
static char path[256] = "/root/untitled.txt";
static const char *notice = "";
static GR_WINDOW_ID window;
static char sought[64];		/* what Ctrl+F looks for */
static int finding;		/* it is being typed, in the status line */

/* What was done, to undo: text put in from (x0, y0) to (x1, y1), or text taken out at (x0, y0). */
static struct change { int inserted, x0, y0, x1, y1, length, joined; char *text; } undo[UNDO_MOST];
static int undo_count, undoing;

static void
insert_line(int at, const char *text, int length)
{
	if (count == room) {
		room = room ? room * 2 : 64;
		lines = realloc(lines, room * sizeof *lines);
	}
	memmove(lines + at + 1, lines + at, (count - at) * sizeof *lines);
	lines[at] = malloc(length + 1);
	memcpy(lines[at], text, length);
	lines[at][length] = 0;
	count++;
}

/* ---- the text's two changes, which everything else is made of ---- */

static void
remember(int inserted, int x0, int y0, int x1, int y1, char *text, int length)
{
	struct change *c;

	if (undoing) {
		free(text);
		return;
	}
	/* a letter typed after a letter is one change with it */
	if (inserted && undo_count && length == 1 && text[0] != '\n') {
		c = &undo[undo_count - 1];
		if (c->inserted && c->joined && c->y1 == y0 && c->x1 == x0) {
			c->x1 = x1;
			free(text);
			return;
		}
	}
	if (undo_count == UNDO_MOST) {
		free(undo[0].text);
		memmove(undo, undo + 1, (UNDO_MOST - 1) * sizeof undo[0]);
		undo_count--;
	}
	c = &undo[undo_count++];
	c->inserted = inserted, c->x0 = x0, c->y0 = y0, c->x1 = x1, c->y1 = y1;
	c->length = length;
	c->joined = inserted && length == 1 && text[0] != '\n';
	if (inserted) {
		free(text);
		c->text = NULL;
	} else {
		c->text = text;
	}
}

/* The text from (x0, y0) to (x1, y1), lines parted by a newline; the caller frees it. */
static char *
text_of(int x0, int y0, int x1, int y1, int *length)
{
	int n = 0, y;
	char *text;

	for (y = y0; y <= y1; y++)
		n += (y == y1 ? x1 : (int)strlen(lines[y]) + 1) - (y == y0 ? x0 : 0);
	text = malloc(n + 1);
	n = 0;
	for (y = y0; y <= y1; y++) {
		int from = y == y0 ? x0 : 0, to = y == y1 ? x1 : (int)strlen(lines[y]);

		memcpy(text + n, lines[y] + from, to - from);
		n += to - from;
		if (y < y1)
			text[n++] = '\n';
	}
	text[n] = 0;
	*length = n;
	return text;
}

/* Puts text in at (x, y) and leaves the cursor after it. */
static void
text_insert(int x, int y, const char *text, int length)
{
	int x0 = x, y0 = y, tail_length = strlen(lines[y]) - x;
	char *tail = malloc(tail_length + 1), *copy = malloc(length + 1);

	memcpy(copy, text, length);
	memcpy(tail, lines[y] + x, tail_length + 1);
	lines[y][x] = 0;
	while (length > 0) {
		const char *end = memchr(text, '\n', length);
		int n = end ? (int)(end - text) : length, i;

		lines[y] = realloc(lines[y], x + n + tail_length + 1);
		for (i = 0; i < n; i++)
			lines[y][x + i] = text[i] == '\t' || (unsigned char)text[i] < 32 || (unsigned char)text[i] > 126 ? ' ' : text[i];
		x += n;
		lines[y][x] = 0;
		text += n, length -= n;
		if (end) {
			insert_line(++y, "", 0);
			x = 0;
			text++, length--;
		}
	}
	lines[y] = realloc(lines[y], x + tail_length + 1);
	memcpy(lines[y] + x, tail, tail_length + 1);
	free(tail);
	cx = x, cy = y;
	modified = 1;
	remember(1, x0, y0, x, y, copy, (int)strlen(copy));
}

/* Takes the text from (x0, y0) to (x1, y1) out and leaves the cursor there. */
static void
text_delete(int x0, int y0, int x1, int y1)
{
	int length, y, rest;
	char *text;

	if (y0 == y1 && x0 == x1)
		return;
	text = text_of(x0, y0, x1, y1, &length);
	rest = strlen(lines[y1]) - x1;
	if (y1 == y0) {
		memmove(lines[y0] + x0, lines[y0] + x1, rest + 1);
	} else {
		lines[y0] = realloc(lines[y0], x0 + rest + 1);
		memcpy(lines[y0] + x0, lines[y1] + x1, rest + 1);
	}
	for (y = y0 + 1; y <= y1; y++)
		free(lines[y]);
	memmove(lines + y0 + 1, lines + y1 + 1, (count - y1 - 1) * sizeof *lines);
	count -= y1 - y0;
	cx = x0, cy = y0;
	modified = 1;
	remember(0, x0, y0, x1, y1, text, length);
}

static void
undo_last(void)
{
	struct change *c;

	if (!undo_count) {
		notice = "nothing to undo";
		return;
	}
	c = &undo[--undo_count];
	undoing = 1;
	if (c->inserted)
		text_delete(c->x0, c->y0, c->x1, c->y1);
	else
		text_insert(c->x0, c->y0, c->text, c->length);
	undoing = 0;
	free(c->text);
	marked = 0;
}

/* ---- the selection ---- */

/* The selection's two ends in the text's order; false when there is none. */
static int
selection(int *x0, int *y0, int *x1, int *y1)
{
	if (!marked || (ax == cx && ay == cy))
		return 0;
	if (ay < cy || (ay == cy && ax < cx))
		*x0 = ax, *y0 = ay, *x1 = cx, *y1 = cy;
	else
		*x0 = cx, *y0 = cy, *x1 = ax, *y1 = ay;
	return 1;
}

static int
delete_selection(void)
{
	int x0, y0, x1, y1;

	if (!selection(&x0, &y0, &x1, &y1))
		return 0;
	text_delete(x0, y0, x1, y1);
	marked = 0;
	return 1;
}

static void
copy_selection(void)
{
	int x0, y0, x1, y1, length;
	char *text;

	if (!selection(&x0, &y0, &x1, &y1)) {
		notice = "nothing is selected";
		return;
	}
	text = text_of(x0, y0, x1, y1, &length);
	ui_clip_set(text, length);
	free(text);
	notice = "copied";
}

static void
load(void)
{
	FILE *file = fopen(path, "r");
	char text[1024];

	while (file && fgets(text, sizeof text, file)) {
		int length = strcspn(text, "\r\n"), i;

		for (i = 0; i < length; i++)
			if (text[i] == '\t')
				text[i] = ' ';
		insert_line(count, text, length);
	}
	if (file)
		fclose(file);
	else
		notice = "new file";
	if (count == 0)
		insert_line(0, "", 0);
}

static void
save(void)
{
	FILE *file = fopen(path, "w");
	int i;

	if (!file) {
		notice = "cannot write the file";
		return;
	}
	for (i = 0; i < count; i++)
		fprintf(file, "%s\n", lines[i]);
	fclose(file);
	modified = 0;
	notice = "saved";
}

static void
draw_line(int line)
{
	int y = MARGIN + (line - top) * ui_fh, length, x0, y0, x1, y1, from = 0, to = 0, shown;

	if (line < top || line >= top + rows)
		return;
	ui_fill(window, 0, y, width - UI_SCROLL_W, ui_fh, WHITE);
	if (line >= count)
		return;
	length = strlen(lines[line]);
	shown = length - left > columns ? columns : length - left;
	if (selection(&x0, &y0, &x1, &y1) && line >= y0 && line <= y1) {
		/* the selected columns of this line; a line's end counts as one more */
		from = (line == y0 ? x0 : 0) - left;
		to = (line == y1 ? x1 : length + 1) - left;
		from = from < 0 ? 0 : from > columns ? columns : from;
		to = to < 0 ? 0 : to > columns ? columns : to;
		if (to > from)
			ui_fill(window, MARGIN + from * ui_fw, y, (to - from) * ui_fw, ui_fh, UI_SELECTED);
	}
	if (shown > 0) {
		const char *text = lines[line] + left;

		if (to > from) {
			int a = from > shown ? shown : from, b = to > shown ? shown : to;

			if (a > 0)
				ui_text(window, MARGIN, y, text, a, BLACK, 1);
			if (b > a)
				ui_text(window, MARGIN + a * ui_fw, y, text + a, b - a, WHITE, 1);
			if (shown > b)
				ui_text(window, MARGIN + b * ui_fw, y, text + b, shown - b, BLACK, 1);
		} else {
			ui_text(window, MARGIN, y, text, shown, BLACK, 1);
		}
	}
	if (line == cy)
		ui_fill(window, MARGIN + (cx - left) * ui_fw, y, 2, ui_fh, BLACK);
}

static void
draw_status(void)
{
	char text[200];

	if (finding)
		snprintf(text, sizeof text, "Find: %s_   (Enter finds, Escape leaves)", sought);
	else
		snprintf(text, sizeof text, "%s%s   line %d, column %d   %s", path, modified ? " *" : "", cy + 1, cx + 1, notice);
	ui_fill(window, 0, height - STATUS, width, STATUS, UI_FACE);
	ui_bevel(window, 0, height - STATUS, width, STATUS, 1);
	ui_text(window, 6, height - STATUS + 3, text, -1, BLACK, 0);
	if (!finding && width > 560)
		ui_text(window, width - 236, height - STATUS + 3, "Ctrl+S save  F find  Z undo  Q quit", -1, UI_SHADOW, 0);
}

static void
draw_all(void)
{
	int line;

	ui_fill(window, 0, 0, width - UI_SCROLL_W, height - STATUS, WHITE);
	for (line = top; line < top + rows; line++)
		draw_line(line);
	ui_scrollbar(window, width - UI_SCROLL_W, 0, height - STATUS, count, rows, top);
	draw_status();
}

/* Shows the text from another line on, leaving the cursor where it is in the text. */
static void
scroll_to(int line)
{
	if (line > count - rows)
		line = count - rows;
	if (line < 0)
		line = 0;
	if (line != top) {
		top = line;
		draw_all();
	}
}

/* Brings the cursor into view; true if everything had to move. */
static int
follow(void)
{
	int old_top = top, old_left = left;

	if (cy < top)
		top = cy;
	if (cy >= top + rows)
		top = cy - rows + 1;
	if (cx < left)
		left = cx;
	if (cx >= left + columns)
		left = cx - columns + 1;
	return top != old_top || left != old_left;
}

/* Looks for the sought text from just after the cursor, round the end to the start; selects it. */
static void
find_next(void)
{
	int n = strlen(sought), y, pass, x0, y0, x1, y1, start_x = cx, start_y = cy;

	if (!n) {
		notice = "Ctrl+F says what to find";
		return;
	}
	if (selection(&x0, &y0, &x1, &y1))
		start_x = x0 + 1, start_y = y0;	/* past the one found last */
	for (pass = 0; pass <= count; pass++) {
		const char *line, *at;
		int from = 0;

		y = (start_y + pass) % count;
		line = lines[y];
		if (pass == 0)
			from = start_x < (int)strlen(line) ? start_x : (int)strlen(line);
		at = strstr(line + from, sought);
		if (at) {
			ax = (int)(at - line), ay = y;
			cx = ax + n, cy = y;
			marked = 1;
			notice = "found";
			return;
		}
	}
	notice = "not found";
}

static void
leave(void)
{
	if (modified && !leaving) {
		leaving = 1;
		notice = "not saved: Ctrl+S saves, Ctrl+Q once more leaves without";
		draw_status();
		return;
	}
	GrClose();
	exit(0);
}

/* A key of the line Ctrl+F opens in the status bar. */
static void
find_key(int ch)
{
	int n = strlen(sought);

	if (ch == MWKEY_ESCAPE) {
		finding = 0;
	} else if (ch == MWKEY_ENTER || ch == '\n') {
		finding = 0;
		find_next();
		follow();
		draw_all();
		return;
	} else if (ch == MWKEY_BACKSPACE || ch == 127) {
		if (n)
			sought[n - 1] = 0;
	} else if (ch >= 32 && ch < 127 && n < (int)sizeof sought - 1) {
		sought[n] = ch, sought[n + 1] = 0;
	}
	draw_status();
}

static void
key(int ch, int modifiers)
{
	int was = cy, length = strlen(lines[cy]), whole = 0, had = marked, moves = 0, command = 0;
	int shift = (modifiers & MWKMOD_SHIFT) != 0, edits, cut = 0;

	if (finding) {
		find_key(ch);
		return;
	}
	notice = "";
	/* a letter with Ctrl comes as the letter with the modifier, or as its control code */
	if ((modifiers & MWKMOD_CTRL) && ch < 128 && isalpha(ch))
		command = tolower(ch);
	else if (ch == 1 || ch == 3 || ch == 6 || ch == 17 || ch == 19 || ch == 22 || ch == 24 || ch == 26)
		command = 'a' + ch - 1;
	if (command != 'q')
		leaving = 0;
	if (command) {
		switch (command) {
		case 's':
			save();
			draw_status();
			return;
		case 'q':
			leave();
			return;
		case 'c':
			copy_selection();
			draw_status();
			return;
		case 'x':
			copy_selection();
			delete_selection();
			break;
		case 'v': {
			int n;
			char *text = ui_clip_get(&n);

			if (!text) {
				notice = "the clipboard is empty";
				draw_status();
				return;
			}
			delete_selection();
			text_insert(cx, cy, text, n);
			free(text);
			break;
		}
		case 'a':
			ax = ay = 0;
			cy = count - 1, cx = strlen(lines[cy]);
			marked = 1;
			break;
		case 'z':
			undo_last();
			break;
		case 'f':
			finding = 1;
			draw_status();
			return;
		default:
			return;
		}
		follow();
		draw_all();
		return;
	}
	if (ch == MWKEY_F3) {
		find_next();
		follow();
		draw_all();
		return;
	}
	switch (ch) {
	case MWKEY_LEFT:
	case MWKEY_RIGHT:
	case MWKEY_UP:
	case MWKEY_DOWN:
	case MWKEY_PAGEUP:
	case MWKEY_PAGEDOWN:
	case MWKEY_HOME:
	case MWKEY_END:
		moves = 1;
		if (shift && !marked)
			ax = cx, ay = cy, marked = 1;
		if (!shift)
			marked = 0;
		break;
	}
	edits = ch == MWKEY_ENTER || ch == '\n' || ch == MWKEY_BACKSPACE || ch == 127 || ch == MWKEY_DELETE || ch == '\t' ||
		(ch >= 32 && ch <= 126);
	if (edits) {
		cut = delete_selection();	/* what is selected goes, whatever is typed */
		marked = 0;
		whole = cut;
	}
	switch (ch) {
	case MWKEY_LEFT:
		if (cx > 0)
			cx--;
		else if (cy > 0)
			cx = strlen(lines[--cy]);
		break;
	case MWKEY_RIGHT:
		if (cx < length)
			cx++;
		else if (cy + 1 < count)
			cy++, cx = 0;
		break;
	case MWKEY_UP:
		if (cy > 0)
			cy--;
		break;
	case MWKEY_DOWN:
		if (cy + 1 < count)
			cy++;
		break;
	case MWKEY_PAGEUP:
		cy = cy > rows ? cy - rows : 0;
		break;
	case MWKEY_PAGEDOWN:
		cy = cy + rows < count ? cy + rows : count - 1;
		break;
	case MWKEY_HOME:
		cx = 0;
		break;
	case MWKEY_END:
		cx = length;
		break;
	case MWKEY_ENTER:
	case '\n':
		text_insert(cx, cy, "\n", 1);
		whole = 1;
		break;
	case MWKEY_BACKSPACE:
	case 127:
		if (cut) {
		} else if (cx > 0) {
			text_delete(cx - 1, cy, cx, cy);
		} else if (cy > 0) {
			text_delete(strlen(lines[cy - 1]), cy - 1, 0, cy);
			whole = 1;
		}
		break;
	case MWKEY_DELETE:
		if (cut) {
		} else if (cx < length) {
			text_delete(cx, cy, cx + 1, cy);
		} else if (cy + 1 < count) {
			text_delete(cx, cy, 0, cy + 1);
			whole = 1;
		}
		break;
	default: {
		char letter;

		if (moves)
			break;
		if (ch == '\t')
			ch = ' ';
		if (ch < 32 || ch > 126)
			return;
		letter = (char)ch;
		text_insert(cx, cy, &letter, 1);
	}
	}
	if (cx > (int)strlen(lines[cy]))
		cx = strlen(lines[cy]);
	if (marked && ax == cx && ay == cy && !shift)
		marked = 0;
	if (follow() || whole || had || marked)
		draw_all();
	else {
		draw_line(was);
		draw_line(cy);
		draw_status();
	}
}

/* The place in the text under a point of the window. */
static void
place(int px, int py, int *x, int *y)
{
	int line = top + (py - MARGIN) / ui_fh, column;

	if (py < MARGIN)
		line = top;
	if (line >= count)
		line = count - 1;
	column = left + (px - MARGIN + ui_fw / 2) / ui_fw;
	if (column < 0)
		column = 0;
	if (column > (int)strlen(lines[line]))
		column = strlen(lines[line]);
	*x = column, *y = line;
}

static void
resized(int w, int h)
{
	width = w;
	height = h;
	rows = (height - STATUS - MARGIN) / ui_fh;
	columns = (width - 2 * MARGIN - UI_SCROLL_W) / ui_fw;
	if (rows < 1)
		rows = 1;
	if (columns < 1)
		columns = 1;
	follow();
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	char title[300];

	if (GrOpen() < 0)
		return 1;
	ui_init();
	if (argc > 1)
		snprintf(path, sizeof path, "%s", argv[1]);
	load();
	snprintf(title, sizeof title, "%s - Editor", path);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, title, GR_ROOT_WINDOW_ID, -1, -1,
		80 * ui_fw + 2 * MARGIN, 24 * ui_fh + MARGIN + STATUS, WHITE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_UPDATE |
		GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_MOTION | GR_EVENT_MASK_CLOSE_REQ);
	resized(80 * ui_fw + 2 * MARGIN, 24 * ui_fh + MARGIN + STATUS);
	GrMapWindow(window);
	GrSetFocus(window);
	for (;;) {
		GrGetNextEvent(&event);
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
			if (ui_wheel(&event)) {
				scroll_to(top + 3 * ui_wheel_sum(&event));
			} else if (event.button.x >= width - UI_SCROLL_W && event.button.y < height - STATUS) {
				dragging = 1;
				scroll_to(ui_scroll_to(event.button.y, 0, height - STATUS, count, rows));
			} else if (event.button.y < height - STATUS && (event.button.changebuttons & GR_BUTTON_L)) {
				/* a click puts the cursor there, and a selection begins with it */
				int had = marked;

				place(event.button.x, event.button.y, &cx, &cy);
				ax = cx, ay = cy;
				marked = 1;
				selecting = 1;
				leaving = 0;
				notice = "";
				if (had)
					draw_all();
				else {
					int line;

					for (line = top; line < top + rows; line++)
						draw_line(line);	/* (the cursor was on one of them) */
					draw_status();
				}
			}
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			dragging = selecting = 0;
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (dragging && (event.mouse.buttons & GR_BUTTON_L)) {
				scroll_to(ui_scroll_to(event.mouse.y, 0, height - STATUS, count, rows));
			} else if (selecting && (event.mouse.buttons & GR_BUTTON_L)) {
				int x, y, from, to, line;

				place(event.mouse.x, event.mouse.y < height - STATUS ? event.mouse.y : height - STATUS - 1, &x, &y);
				if (x != cx || y != cy) {
					from = y < cy ? y : cy, to = y < cy ? cy : y;
					cx = x, cy = y;
					for (line = from; line <= to; line++)
						draw_line(line);
					draw_status();
				}
			} else {
				dragging = selecting = 0;
			}
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(event.keystroke.ch, event.keystroke.modifiers);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			leave();
			break;
		}
	}
}
