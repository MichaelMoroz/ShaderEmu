/*
 * nxedit: a text editor. nxedit FILE opens or creates the file; arrow keys, Home, End, Page
 * Up and Down move; Ctrl+S saves, Ctrl+Q quits.
 */
#include "ui.h"

#define STATUS 18
#define MARGIN 3

static char **lines;		/* each allocated, no newline */
static int count, room;
static int cx, cy;		/* the cursor: column, line */
static int top, left;		/* first line and column shown */
static int rows, columns;	/* that fit the window */
static int width, height, modified;
static char path[256] = "/root/untitled.txt";
static const char *notice = "";
static GR_WINDOW_ID window;

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
	int y = MARGIN + (line - top) * ui_fh, length;

	if (line < top || line >= top + rows)
		return;
	ui_fill(window, 0, y, width, ui_fh, WHITE);
	if (line >= count)
		return;
	length = strlen(lines[line]);
	if (length > left)
		ui_text(window, MARGIN, y, lines[line] + left, length - left > columns ? columns : length - left, BLACK, 1);
	if (line == cy)
		ui_fill(window, MARGIN + (cx - left) * ui_fw, y, 2, ui_fh, BLACK);
}

static void
draw_status(void)
{
	char text[160];

	snprintf(text, sizeof text, "%s%s   line %d, column %d   %s", path, modified ? " *" : "", cy + 1, cx + 1, notice);
	ui_fill(window, 0, height - STATUS, width, STATUS, UI_FACE);
	ui_bevel(window, 0, height - STATUS, width, STATUS, 1);
	ui_text(window, 6, height - STATUS + 3, text, -1, BLACK, 0);
	ui_text(window, width - 150, height - STATUS + 3, "Ctrl+S save  Ctrl+Q quit", -1, UI_SHADOW, 0);
}

static void
draw_all(void)
{
	int line;

	ui_fill(window, 0, 0, width, height - STATUS, WHITE);
	for (line = top; line < top + rows; line++)
		draw_line(line);
	draw_status();
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

static void
key(int ch, int modifiers)
{
	int was = cy, length = strlen(lines[cy]), whole = 0;
	char *line;

	notice = "";
	if ((modifiers & MWKMOD_CTRL) || ch == 19 || ch == 17) {
		if (ch == 's' || ch == 19)
			save();
		else if (ch == 'q' || ch == 17) {
			GrClose();
			exit(0);
		}
		draw_status();
		return;
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
		insert_line(cy + 1, lines[cy] + cx, length - cx);
		lines[cy][cx] = 0;
		cy++;
		cx = 0;
		modified = whole = 1;
		break;
	case MWKEY_BACKSPACE:
	case 127:
		if (cx > 0) {
			memmove(lines[cy] + cx - 1, lines[cy] + cx, length - cx + 1);
			cx--;
		} else if (cy > 0) {
			int before = strlen(lines[cy - 1]);

			lines[cy - 1] = realloc(lines[cy - 1], before + length + 1);
			memcpy(lines[cy - 1] + before, lines[cy], length + 1);
			free(lines[cy]);
			memmove(lines + cy, lines + cy + 1, (count - cy - 1) * sizeof *lines);
			count--;
			cy--;
			cx = before;
			whole = 1;
		}
		modified = 1;
		break;
	case MWKEY_DELETE:
		if (cx < length)
			memmove(lines[cy] + cx, lines[cy] + cx + 1, length - cx);
		modified = 1;
		break;
	default:
		if (ch == '\t')
			ch = ' ';
		if (ch < 32 || ch > 126)
			return;
		line = lines[cy] = realloc(lines[cy], length + 2);
		memmove(line + cx + 1, line + cx, length - cx + 1);
		line[cx++] = ch;
		modified = 1;
	}
	if (cx > (int)strlen(lines[cy]))
		cx = strlen(lines[cy]);
	if (follow() || whole)
		draw_all();
	else {
		draw_line(was);
		draw_line(cy);
		draw_status();
	}
}

static void
resized(int w, int h)
{
	width = w;
	height = h;
	rows = (height - STATUS - MARGIN) / ui_fh;
	columns = (width - 2 * MARGIN) / ui_fw;
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
		GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_CLOSE_REQ);
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
			/* a click puts the cursor there */
			if (event.button.y < height - STATUS) {
				int was = cy;

				cy = top + (event.button.y - MARGIN) / ui_fh;
				if (cy >= count)
					cy = count - 1;
				cx = left + (event.button.x - MARGIN + ui_fw / 2) / ui_fw;
				if (cx > (int)strlen(lines[cy]))
					cx = strlen(lines[cy]);
				draw_line(was);
				draw_line(cy);
				draw_status();
			}
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(event.keystroke.ch, event.keystroke.modifiers);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
	}
}
