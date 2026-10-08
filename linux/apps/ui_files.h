/*
 * Choosing a file, for any of the desktop's programs: a window of its own with the folders to
 * walk through and the files of the kinds asked for, until the user opens one or gives up.
 * ui_choose_file() at the end is all a program calls.
 */
#ifndef SHADEREMU_APPS_UI_FILES_H
#define SHADEREMU_APPS_UI_FILES_H

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include "ui.h"

#define UI_FILES_MOST 600
#define UI_FILES_W 380
#define UI_FILES_H 300

static struct ui_files_entry { char name[64]; char folder; } ui_files_entries[UI_FILES_MOST];
static struct ui_list ui_files_list;
static char ui_files_where[512];

static int
ui_files_wanted(const char *name, const char *kinds)
{
	const char *dot = strrchr(name, '.'), *found;
	size_t n;

	if (!kinds)
		return 1;
	if (!dot)
		return 0;
	n = strlen(dot);
	/* the ending must stand in the list as a whole word */
	for (found = kinds; (found = strcasestr(found, dot)) != NULL; found += n)
		if ((found == kinds || found[-1] == ' ') && (found[n] == 0 || found[n] == ' '))
			return 1;
	return 0;
}

static int
ui_files_order(const void *a, const void *b)
{
	const struct ui_files_entry *x = a, *y = b;

	return x->folder != y->folder ? y->folder - x->folder : strcasecmp(x->name, y->name);
}

/* The folder's entries: its folders first, then the files of the kinds wanted. */
static void
ui_files_read(const char *kinds)
{
	DIR *folder = opendir(ui_files_where);
	struct dirent *item;
	struct stat info;
	char path[600];
	int count = 0;

	while (folder && (item = readdir(folder)) != NULL && count < UI_FILES_MOST) {
		int is_folder = item->d_type == DT_DIR;

		if (item->d_name[0] == '.' || strlen(item->d_name) >= sizeof ui_files_entries[0].name)
			continue;
		/* only what the listing does not say is asked of the file system: that is slow */
		if (item->d_type == DT_UNKNOWN || item->d_type == DT_LNK) {
			snprintf(path, sizeof path, "%s/%s", ui_files_where, item->d_name);
			is_folder = stat(path, &info) == 0 && S_ISDIR(info.st_mode);
		}
		if (!is_folder && !ui_files_wanted(item->d_name, kinds))
			continue;
		strcpy(ui_files_entries[count].name, item->d_name);
		ui_files_entries[count++].folder = (char)is_folder;
	}
	if (folder)
		closedir(folder);
	qsort(ui_files_entries, count, sizeof ui_files_entries[0], ui_files_order);
	ui_files_list.count = count;
	ui_files_list.top = 0;
	ui_files_list.selected = count ? 0 : -1;
}

static const char *
ui_files_label(int row)
{
	static char text[80];

	snprintf(text, sizeof text, ui_files_entries[row].folder ? "[%s]" : "%s", ui_files_entries[row].name);
	return text;
}

static void
ui_files_draw(GR_WINDOW_ID w)
{
	ui_fill(w, 0, 0, UI_FILES_W, UI_FILES_H, UI_FACE);
	ui_text_fit(w, 8, 8, UI_FILES_W - 16, ui_files_where, BLACK);
	ui_list_draw(w, &ui_files_list, ui_files_label);
	ui_button(w, 8, UI_FILES_H - 30, 70, 22, "Up", 0);
	ui_button(w, UI_FILES_W - 166, UI_FILES_H - 30, 76, 22, "Open", 0);
	ui_button(w, UI_FILES_W - 84, UI_FILES_H - 30, 76, 22, "Cancel", 0);
}

/* Into the selected folder, or out of this one (row -1). True when the selected row is a file. */
static int
ui_files_open(int row, const char *kinds)
{
	size_t n = strlen(ui_files_where);
	char *slash;

	if (row < 0) {
		if ((slash = strrchr(ui_files_where, '/')) != NULL && n > 1)
			slash[slash == ui_files_where ? 1 : 0] = 0;
	} else if (!ui_files_entries[row].folder) {
		return 1;
	} else if (n + strlen(ui_files_entries[row].name) + 2 < sizeof ui_files_where) {
		snprintf(ui_files_where + n, sizeof ui_files_where - n, "%s%s", n > 1 ? "/" : "", ui_files_entries[row].name);
	}
	ui_files_read(kinds);
	return 0;
}

/*
 * Asks for a file, starting in the folder `start`; true with its path in `path`. `kinds` is the
 * endings wanted, spaces between (".ppm .bmp"; NULL: any). `others` gets the events of the
 * program's other windows meanwhile, so that they can draw themselves.
 */
static int
ui_choose_file(const char *title, const char *start, const char *kinds, char *path, int size, void (*others)(GR_EVENT *))
{
	GR_WINDOW_ID w = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, (char *)title, GR_ROOT_WINDOW_ID, -1, -1, UI_FILES_W, UI_FILES_H, UI_FACE);
	GR_EVENT event;
	int chosen = 0, done = 0;

	snprintf(ui_files_where, sizeof ui_files_where, "%s", start && start[0] == '/' ? start : "/");
	ui_files_list.x = 8;
	ui_files_list.y = 28;
	ui_files_list.width = UI_FILES_W - 16;
	ui_files_list.height = UI_FILES_H - 28 - 38;
	ui_files_read(kinds);
	GrSelectEvents(w, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_MOUSE_MOTION |
			  GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(w);
	while (!done) {
		int open = 0, how;

		GrGetNextEvent(&event);
		if (event.general.wid != w) {
			if (others && event.type != GR_EVENT_TYPE_NONE)
				others(&event);
			continue;
		}
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			ui_files_draw(w);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			done = 1;
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			if (event.keystroke.ch == MWKEY_ENTER)
				open = 1;
			else if (event.keystroke.ch == MWKEY_ESCAPE)
				done = 1;
			else if (event.keystroke.ch == MWKEY_BACKSPACE)
				open = -1;
			else if ((event.keystroke.ch == MWKEY_UP && ui_files_list.selected > 0) ||
				 (event.keystroke.ch == MWKEY_DOWN && ui_files_list.selected + 1 < ui_files_list.count)) {
				ui_files_list.selected += event.keystroke.ch == MWKEY_UP ? -1 : 1;
				ui_list_show(&ui_files_list);
				ui_files_draw(w);
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_MOUSE_MOTION:
			how = ui_list_event(&ui_files_list, &event);
			if (how == 2)
				open = 1;
			else if (how == 1)
				ui_files_draw(w);
			else if (event.type == GR_EVENT_TYPE_BUTTON_DOWN && !ui_wheel(&event) && event.button.y >= UI_FILES_H - 30) {
				if (event.button.x < 78)
					open = -1;
				else if (event.button.x >= UI_FILES_W - 84)
					done = 1;
				else if (event.button.x >= UI_FILES_W - 166)
					open = 1;
			}
			break;
		}
		if (open && (open < 0 || ui_files_list.selected >= 0)) {
			if (ui_files_open(open < 0 ? -1 : ui_files_list.selected, kinds)) {
				snprintf(path, size, "%s%s%s", ui_files_where, ui_files_where[1] ? "/" : "", ui_files_entries[ui_files_list.selected].name);
				chosen = done = 1;
			} else {
				ui_files_draw(w);
			}
		}
	}
	GrDestroyWindow(w);
	return chosen;
}

#endif
