/*
 * nxfiles: a file manager, a tile for every entry. A click selects, a click on the selected
 * tile (or Enter, or Open) opens it: a folder is entered, a picture goes to the viewer, a page
 * to the browser, a program is started, anything else goes to the editor. nxfiles DIRECTORY.
 */
#include <dirent.h>
#include <signal.h>
#include <strings.h>
#include <sys/stat.h>
#include "ui.h"

#define BAR 26
#define STATUS 18
#define TILE_W 78
#define TILE_H 60
#define WIDTH 420
#define HEIGHT 330
#define MAX_ENTRIES 512

enum { FILE_UNKNOWN, FILE_OTHER, FILE_FOLDER, FILE_PROGRAM, FILE_PICTURE, FILE_PAGE };

static struct entry { char name[64]; long size; int kind; } entries[MAX_ENTRIES];
static int count, selected = -1, top, rows, columns, width = WIDTH, height = HEIGHT;
static int dragging;	/* the scroll bar's thumb is held */
static int focus;	/* what the keyboard works: 0 the tiles, 1 + n button n */
static char where[512];
static GR_WINDOW_ID window;
static const char *buttons[] = { "Up", "Open", "Edit", "New file", "Delete", "Refresh" };
#define BUTTONS 6
#define BUTTON_W 62

static int
ends_with(const char *name, const char *end)
{
	size_t n = strlen(name), m = strlen(end);

	return n >= m && !strcasecmp(name + n - m, end);
}

static int
order(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;
	int fx = x->kind == FILE_FOLDER, fy = y->kind == FILE_FOLDER;

	return fx != fy ? fy - fx : strcmp(x->name, y->name);
}

/* What an entry is, asked of the file system the first time it matters: in a folder of
 * hundreds of names, asking for all of them takes seconds. */
static int
kind_of(int i)
{
	struct entry *e = &entries[i];
	struct stat info;

	if (e->kind != FILE_UNKNOWN)
		return e->kind;
	e->kind = FILE_OTHER;
	if (stat(e->name, &info) != 0)
		return e->kind;
	e->size = info.st_size;
	if (S_ISDIR(info.st_mode))
		e->kind = FILE_FOLDER;
	else if (ends_with(e->name, ".ppm") || ends_with(e->name, ".pgm") || ends_with(e->name, ".bmp") ||
		 ends_with(e->name, ".gif") || ends_with(e->name, ".xpm"))
		e->kind = FILE_PICTURE;
	else if (ends_with(e->name, ".html") || ends_with(e->name, ".htm"))
		e->kind = FILE_PAGE;
	else if (info.st_mode & 0111)
		e->kind = FILE_PROGRAM;
	return e->kind;
}

static void
read_folder(void)
{
	DIR *folder;
	struct dirent *item;

	if (!getcwd(where, sizeof where))
		strcpy(where, "/");
	count = 0;
	folder = opendir(".");
	while (folder && count < MAX_ENTRIES && (item = readdir(folder)) != NULL) {
		struct entry *e = &entries[count];

		if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
			continue;
		snprintf(e->name, sizeof e->name, "%s", item->d_name);
		e->size = 0;
		e->kind = item->d_type == DT_DIR ? FILE_FOLDER : FILE_UNKNOWN;
		count++;
	}
	if (folder)
		closedir(folder);
	qsort(entries, count, sizeof entries[0], order);
	selected = -1;
	top = 0;
}

static int
tile_rows(void)
{
	return (count + columns - 1) / columns;
}

/* A 32 x 26 picture of what the entry is, its top left corner at x, y. */
static void
draw_icon(int x, int y, int kind)
{
	int i;

	switch (kind) {
	case FILE_FOLDER:
		ui_fill(window, x + 2, y + 3, 12, 4, MWRGB(214, 166, 44));
		ui_fill(window, x + 2, y + 6, 28, 18, MWRGB(244, 200, 80));
		ui_fill(window, x + 2, y + 22, 28, 2, MWRGB(190, 146, 36));
		break;
	case FILE_PROGRAM:
		ui_fill(window, x + 2, y + 3, 28, 21, UI_SHADOW);
		ui_fill(window, x + 3, y + 4, 26, 19, UI_FACE);
		ui_fill(window, x + 3, y + 4, 26, 5, UI_SELECTED);
		ui_fill(window, x + 6, y + 12, 12, 2, BLACK);
		ui_fill(window, x + 6, y + 16, 18, 2, BLACK);
		break;
	case FILE_PICTURE:
		ui_fill(window, x + 2, y + 3, 28, 21, UI_SHADOW);
		ui_fill(window, x + 3, y + 4, 26, 19, MWRGB(140, 196, 240));
		ui_fill(window, x + 22, y + 6, 4, 4, MWRGB(250, 230, 90));
		for (i = 0; i < 8; i++)
			ui_fill(window, x + 10 - i, y + 14 + i, 2 + 2 * i, 1, MWRGB(70, 150, 80));
		ui_fill(window, x + 3, y + 21, 26, 2, MWRGB(70, 150, 80));
		break;
	default:
		ui_fill(window, x + 6, y + 1, 20, 24, UI_SHADOW);
		ui_fill(window, x + 7, y + 2, 18, 22, WHITE);
		for (i = 0; i < 5; i++)
			ui_fill(window, x + 10, y + 6 + i * 4, i == 4 ? 7 : 12, 1, kind == FILE_PAGE ? MWRGB(0, 0, 200) : UI_SHADOW);
		break;
	}
}

static void
draw_tile(int i)
{
	int row = i / columns - top, x = 3 + (i % columns) * TILE_W, y = BAR + 3 + row * TILE_H;
	int chosen = i == selected, full, n, w, h, b;
	char name[20];

	if (i < 0 || i >= count || row < 0 || row >= rows)
		return;
	ui_fill(window, x, y, TILE_W - 2, TILE_H - 2, chosen ? UI_SELECTED : WHITE);
	draw_icon(x + (TILE_W - 34) / 2, y + 3, kind_of(i));
	/* as much of the name as the tile has room for */
	full = strlen(entries[i].name);
	GrSetGCFont(ui_gc, ui_var);
	for (n = full > 17 ? 17 : full; ; n--) {
		snprintf(name, sizeof name, n < full ? "%.*s.." : "%.*s", n, entries[i].name);
		GrGetGCTextSize(ui_gc, name, strlen(name), GR_TFASCII, &w, &h, &b);
		if (w <= TILE_W - 6 || n <= 1)
			break;
	}
	ui_text(window, x + (TILE_W - 2 - w) / 2, y + 36, name, -1, chosen ? WHITE : BLACK, 0);
}

static void
draw_status(void)
{
	char text[640];

	if (selected >= 0 && kind_of(selected) != FILE_FOLDER)
		snprintf(text, sizeof text, "%s   %ld bytes", entries[selected].name, entries[selected].size);
	else
		snprintf(text, sizeof text, "%s   %d items", where, count);
	ui_fill(window, 0, height - STATUS, width, STATUS, UI_FACE);
	ui_text(window, 6, height - STATUS + 3, text, -1, BLACK, 0);
}

static void
draw_all(void)
{
	int i, area = height - BAR - STATUS;

	ui_fill(window, 0, 0, width, BAR, UI_FACE);
	for (i = 0; i < BUTTONS; i++)
		ui_button(window, 3 + i * (BUTTON_W + 3), 3, BUTTON_W, BAR - 6, buttons[i], 0);
	if (focus)
		ui_focus(window, 3 + (focus - 1) * (BUTTON_W + 3) + 4, 3 + 4, BUTTON_W - 8, BAR - 6 - 8);
	ui_fill(window, 0, BAR, width - UI_SCROLL_W, area, WHITE);
	ui_bevel(window, 0, BAR, width - UI_SCROLL_W, area, 1);
	for (i = top * columns; i < (top + rows) * columns; i++)
		draw_tile(i);
	ui_scrollbar(window, width - UI_SCROLL_W, BAR, area, tile_rows(), rows, top);
	draw_status();
}

static void
layout(void)
{
	columns = (width - UI_SCROLL_W - 6) / TILE_W;
	if (columns < 1)
		columns = 1;
	rows = (height - BAR - STATUS - 6) / TILE_H;
	if (rows < 1)
		rows = 1;
}

static void
scroll_to(int row)
{
	int most = tile_rows() - rows;

	if (row > most)
		row = most;
	if (row < 0)
		row = 0;
	if (row != top) {
		top = row;
		draw_all();
	}
}

static void
open_entry(int i, int edit)
{
	struct entry *e = &entries[i];
	char path[600];

	if (i < 0 || i >= count)
		return;
	snprintf(path, sizeof path, "%s/%s", strcmp(where, "/") ? where : "", e->name);
	if (kind_of(i) == FILE_FOLDER) {
		if (chdir(e->name) == 0)
			read_folder();
		draw_all();
	} else if (edit) {
		ui_run("nxedit", path);
	} else if (e->kind == FILE_PICTURE) {
		ui_run("nxview", path);
	} else if (e->kind == FILE_PAGE) {
		ui_run("nxweb", path);
	} else if (e->kind == FILE_PROGRAM) {
		ui_run(path, NULL);
	} else {
		ui_run("nxedit", path);
	}
}

static void
select_tile(int i)
{
	int was = selected, row;

	if (count == 0)
		return;
	i = i < 0 ? 0 : i >= count ? count - 1 : i;
	selected = i;
	row = i / columns;
	if (row < top || row >= top + rows) {
		top = row < top ? row : row - rows + 1;
		draw_all();
	} else {
		draw_tile(was);
		draw_tile(i);
		draw_status();
	}
}

static void
press(int button)
{
	char name[80];
	FILE *file;
	int n;

	switch (button) {
	case 0:
		if (chdir("..") == 0)
			read_folder();
		break;
	case 1:
	case 2:
		open_entry(selected, button == 2);
		return;
	case 3:
		/* the first of new.txt, new1.txt, ... that is not there yet */
		for (n = 0; n < 100; n++) {
			snprintf(name, sizeof name, n ? "new%d.txt" : "new.txt", n);
			if (access(name, F_OK) != 0)
				break;
		}
		if ((file = fopen(name, "w")) != NULL)
			fclose(file);
		read_folder();
		break;
	case 4:
		if (selected >= 0 && (kind_of(selected) == FILE_FOLDER ? rmdir(entries[selected].name)
									   : unlink(entries[selected].name)) == 0)
			read_folder();
		break;
	case 5:
		read_folder();
		break;
	}
	draw_all();
}

/* Arrows and the paging keys choose a tile, a letter the next name it starts, Backspace the
 * folder above, Delete, F5; Tab goes to the buttons (arrows, Enter) and back to the tiles. */
static void
key(const GR_EVENT *event)
{
	int ch = event->keystroke.ch, i;

	if (ui_tab(event, &focus, 1 + BUTTONS)) {
		draw_all();
		return;
	}
	if (focus) {
		/* on the buttons */
		if (ch == MWKEY_LEFT || ch == MWKEY_RIGHT)
			focus = 1 + (focus - 1 + (ch == MWKEY_LEFT ? BUTTONS - 1 : 1)) % BUTTONS;
		else if (ch == MWKEY_ESCAPE || ch == MWKEY_DOWN)
			focus = 0;
		else if (ui_press_key(ch)) {
			press(focus - 1);
			return;
		} else
			return;
		draw_all();
		return;
	}
	switch (ch) {
	case MWKEY_LEFT: select_tile(selected - 1); break;
	case MWKEY_RIGHT: select_tile(selected + 1); break;
	case MWKEY_UP: select_tile(selected < 0 ? 0 : selected >= columns ? selected - columns : selected); break;
	case MWKEY_DOWN: select_tile(selected < 0 ? 0 : selected + columns < count ? selected + columns : selected); break;
	case MWKEY_PAGEUP: select_tile(selected - rows * columns); break;
	case MWKEY_PAGEDOWN: select_tile(selected + rows * columns); break;
	case MWKEY_HOME: select_tile(0); break;
	case MWKEY_END: select_tile(count - 1); break;
	case MWKEY_ENTER: open_entry(selected, 0); break;
	case MWKEY_BACKSPACE: press(0); break;
	case MWKEY_DELETE: press(4); break;
	case MWKEY_F5: press(5); break;
	default:
		/* a letter: the next entry that starts with it */
		for (i = 1; i <= count; i++)
			if (ui_starts(entries[(selected + i) % count].name, ch)) {
				select_tile((selected + i) % count);
				break;
			}
	}
}

int
main(int argc, char **argv)
{
	GR_EVENT event;

	if (GrOpen() < 0)
		return 1;
	signal(SIGCHLD, SIG_IGN);
	ui_init();
	if (chdir(argc > 1 ? argv[1] : "/root") != 0)
		chdir("/");
	read_folder();
	layout();
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Files", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP |
		GR_EVENT_MASK_MOUSE_MOTION | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(window);
	GrSetFocus(window);
	for (;;) {
		int i, area;

		GrGetNextEvent(&event);
		area = height - BAR - STATUS;
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw_all();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.utype == GR_UPDATE_SIZE) {
				width = event.update.width;
				height = event.update.height;
				layout();
				scroll_to(top);
				draw_all();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (ui_wheel(&event)) {
				scroll_to(top + ui_wheel_sum(&event));
			} else if (event.button.y < BAR) {
				for (i = 0; i < BUTTONS; i++)
					if (ui_inside(event.button.x, event.button.y, 3 + i * (BUTTON_W + 3), 3, BUTTON_W, BAR - 6))
						press(i);
			} else if (event.button.y >= height - STATUS) {
				break;
			} else if (event.button.x >= width - UI_SCROLL_W) {
				dragging = 1;
				scroll_to(ui_scroll_to(event.button.y, BAR, area, tile_rows(), rows));
			} else if (event.button.x >= 3 && event.button.x < 3 + columns * TILE_W) {
				i = (top + (event.button.y - BAR - 3) / TILE_H) * columns + (event.button.x - 3) / TILE_W;
				if (i >= count)
					break;
				if (i == selected)
					open_entry(i, 0);
				else
					select_tile(i);
			}
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			dragging = 0;
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (dragging && (event.mouse.buttons & GR_BUTTON_L))
				scroll_to(ui_scroll_to(event.mouse.y, BAR, area, tile_rows(), rows));
			else
				dragging = 0;
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(&event);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
	}
}
