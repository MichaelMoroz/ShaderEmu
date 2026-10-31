/*
 * Choosing a file, for any of the desktop's programs: a window of its own with the folders to
 * walk through and the files of the kinds asked for, until the user opens one or gives up.
 * ui_choose_file() at the end is all a program calls to open one, ui_save_file() to name one.
 */
#ifndef SHADEREMU_APPS_UI_FILES_H
#define SHADEREMU_APPS_UI_FILES_H

#include <dirent.h>
#include <limits.h>
#include <strings.h>
#include <sys/stat.h>
#include "ui.h"

char *strcasestr(const char *, const char *);	/* (musl declares it only under _GNU_SOURCE) */

#define UI_FILES_MOST 600
#define UI_FILES_W 380
#define UI_FILES_H 300

static struct ui_files_entry { char name[64]; char folder; } ui_files_entries[UI_FILES_MOST];
static struct ui_list ui_files_list;
static char ui_files_where[512];
static int ui_files_saving;		/* the window asks for a name to save as */
static int ui_files_stop;		/* set by `others`: the window goes with nothing chosen (its program is closing) */
static int ui_files_offered;		/* the name is still the one offered: a typed character replaces it */
static int ui_files_folders;		/* a folder is asked for: Open (Choose) takes the selected one, or this */
static char ui_files_name[128], ui_files_note[160], ui_files_sure[600];

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

/* Selects the next entry whose name starts with the letter typed; true if there is one. */
static int
ui_files_find(int ch)
{
	int i, n;

	for (i = 1; i <= ui_files_list.count; i++) {
		n = (ui_files_list.selected + i) % ui_files_list.count;
		if (ui_starts(ui_files_entries[n].name, ch)) {
			ui_files_list.selected = n;
			ui_list_show(&ui_files_list);
			return 1;
		}
	}
	return 0;
}

static void
ui_files_draw(GR_WINDOW_ID w)
{
	char text[140];

	ui_fill(w, 0, 0, UI_FILES_W, UI_FILES_H, UI_FACE);
	ui_text_fit(w, 8, 8, UI_FILES_W - 16, ui_files_note[0] ? ui_files_note : ui_files_where, BLACK);
	ui_list_draw(w, &ui_files_list, ui_files_label);
	if (ui_files_saving) {
		ui_text(w, 8, UI_FILES_H - 55, "Name", -1, BLACK, 0);
		ui_fill(w, 48, UI_FILES_H - 58, UI_FILES_W - 56, 20, WHITE);
		ui_bevel(w, 48, UI_FILES_H - 58, UI_FILES_W - 56, 20, 1);
		snprintf(text, sizeof text, ui_files_offered ? "%s" : "%s_", ui_files_name);
		if (ui_files_offered)
			ui_fill(w, 50, UI_FILES_H - 56, UI_FILES_W - 60, 16, UI_SELECTED);
		ui_text_fit(w, 53, UI_FILES_H - 55, UI_FILES_W - 66, text, ui_files_offered ? WHITE : BLACK);
	}
	ui_button(w, 8, UI_FILES_H - 30, 70, 22, "Up", 0);
	ui_button(w, UI_FILES_W - 166, UI_FILES_H - 30, 76, 22, ui_files_saving ? "Save" : ui_files_folders ? "Choose" : "Open", 0);
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

/* The list's selection moved: draws, and a save takes a file's name. True on a file. */
static int
ui_files_picked(GR_WINDOW_ID w)
{
	int n = ui_files_list.selected, file = n >= 0 && !ui_files_entries[n].folder;

	if (ui_files_saving && file) {
		snprintf(ui_files_name, sizeof ui_files_name, "%s", ui_files_entries[n].name);
		ui_files_note[0] = ui_files_sure[0] = 0;
		ui_files_offered = 0;
	}
	ui_files_draw(w);
	return file;
}

/* A key in a save's window: true if it was the name's (draw again), or sets *open for a folder. */
static int
ui_files_key(int ch, int *open)
{
	size_t n = strlen(ui_files_name);

	if (ch == MWKEY_LEFT || (ch == MWKEY_BACKSPACE && !n)) {
		*open = -1;
	} else if (ch == MWKEY_RIGHT) {
		*open = ui_files_list.selected >= 0 && ui_files_entries[ui_files_list.selected].folder ? 2 : 0;
	} else if (ch == MWKEY_BACKSPACE) {
		ui_files_name[n - 1] = 0;
		ui_files_offered = 0;
	} else if (ch >= 32 && ch < 127 && n < sizeof ui_files_name - 1) {
		n = ui_files_offered ? 0 : n;
		ui_files_name[n] = (char)ch, ui_files_name[n + 1] = 0;
		ui_files_offered = 0;
	} else {
		return 0;
	}
	ui_files_note[0] = ui_files_sure[0] = 0;
	return !*open;
}

/* Gives a path the first ending of `kinds`: in place of another of them, after anything else.
 * Only the last part of the path is looked at. True if the path changed. */
static int
ui_files_ending(char *path, int size, const char *kinds)
{
	char *base = strrchr(path, '/'), *dot, ending[16];

	base = base ? base + 1 : path;
	dot = strrchr(base, '.');
	snprintf(ending, sizeof ending, "%.*s", (int)strcspn(kinds, " "), kinds);
	if (dot && !strcasecmp(dot, ending))
		return 0;
	if (dot && dot != base && ui_files_wanted(base, kinds))
		*dot = 0;
	strncat(path, ending, size - strlen(path) - 1);
	return 1;
}

/* The name in the field is taken: a folder is gone into, a file that is there is asked about
 * once, and anything else is the answer (true). The file asked about is the one the caller
 * will write: the name with the first ending of `kinds` (ui_files_ending). */
static int
ui_files_named(const char *kinds, char *path, int size)
{
	char full[700], real[PATH_MAX];
	struct stat info;
	const char *name = ui_files_name;

	snprintf(full, sizeof full, "%s%s%s", name[0] == '/' ? "" : ui_files_where, name[0] == '/' || !ui_files_where[1] ? "" : "/", name);
	ui_files_note[0] = 0;
	if (stat(full, &info) == 0 && S_ISDIR(info.st_mode)) {
		if (realpath(full, real))
			snprintf(ui_files_where, sizeof ui_files_where, "%.*s", (int)sizeof ui_files_where - 1, real);
		ui_files_name[0] = 0;
		ui_files_read(kinds);
		return 0;
	}
	snprintf(path, size, "%.*s", size - 1, full);
	/* the name it will write: as typed if it is of a kind asked for (photo.jpg among .png .jpg), else with the first ending */
	if (kinds && !ui_files_wanted(strrchr(full, '/') ? strrchr(full, '/') + 1 : full, kinds))
		ui_files_ending(full, sizeof full, kinds);
	if (access(full, F_OK) == 0 && strcmp(full, ui_files_sure)) {
		snprintf(ui_files_sure, sizeof ui_files_sure, "%.*s", (int)sizeof ui_files_sure - 1, full);
		snprintf(ui_files_note, sizeof ui_files_note, "%.90s is there already: Save again replaces it", strrchr(full, '/') + 1);
		return 0;
	}
	return 1;
}

/* The window of both questions: `name` is NULL to open a file, or what a save starts as. */
static int
ui_files_ask(const char *title, const char *start, const char *kinds, const char *name, char *path, int size, void (*others)(GR_EVENT *))
{
	GR_WINDOW_ID w = GrNewWindowEx(UI_FIXED, (char *)title, GR_ROOT_WINDOW_ID, -1, -1, UI_FILES_W, UI_FILES_H, UI_FACE);
	GR_EVENT event;
	int chosen = 0, done = 0, n;

	snprintf(ui_files_where, sizeof ui_files_where, "%s", start && start[0] == '/' ? start : "/");
	ui_files_saving = ui_files_offered = name != NULL;
	snprintf(ui_files_name, sizeof ui_files_name, "%s", name ? name : "");
	ui_files_note[0] = ui_files_sure[0] = 0;
	ui_files_list.x = 8;
	ui_files_list.y = 28;
	ui_files_list.width = UI_FILES_W - 16;
	ui_files_list.height = UI_FILES_H - 28 - 38 - (name ? 28 : 0);
	ui_files_read(kinds);
	/* (the release too: without it the moves of a held button do not come, and the bar is not dragged) */
	GrSelectEvents(w, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_MOTION |
			  GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(w);
	ui_files_stop = 0;
	while (!done && !ui_files_stop) {
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
			else if (name && ui_files_key(event.keystroke.ch, &open))
				ui_files_draw(w);
			else if (event.keystroke.ch == MWKEY_BACKSPACE)
				open = -1;
			else if (ui_list_key(&ui_files_list, event.keystroke.ch) || (!name && ui_files_find(event.keystroke.ch)))
				ui_files_picked(w);
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
		case GR_EVENT_TYPE_MOUSE_MOTION:
			how = ui_list_event(&ui_files_list, &event);
			if (how == 2)
				open = ui_files_picked(w) ? 1 : 2;
			else if (how == 1)
				ui_files_picked(w);
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
		n = ui_files_list.selected;
		if (name && open == 1 && (ui_files_name[0] || n < 0 || !ui_files_entries[n].folder)) {
			/* a save's Enter and its Save button are about the name, when there is one */
			if (!ui_files_name[0])
				snprintf(ui_files_note, sizeof ui_files_note, "Type a name to save as");
			chosen = done = ui_files_name[0] && ui_files_named(kinds, path, size);
			ui_files_draw(w);
		} else if (ui_files_folders && open == 1) {
			snprintf(path, size, "%s%s%s", ui_files_where, n >= 0 && ui_files_where[1] ? "/" : "", n >= 0 ? ui_files_entries[n].name : "");
			chosen = done = 1;
		} else if (open && (open < 0 || n >= 0)) {
			ui_files_note[0] = 0;
			if (ui_files_open(open < 0 ? -1 : n, kinds)) {
				snprintf(path, size, "%s%s%s", ui_files_where, ui_files_where[1] ? "/" : "", ui_files_entries[n].name);
				chosen = done = 1;
			} else {
				ui_files_draw(w);
			}
		}
	}
	GrDestroyWindow(w);
	return chosen;
}

/*
 * Asks for a file, starting in the folder `start`; true with its path in `path`. `kinds` is the
 * endings wanted, spaces between (".ppm .bmp"; NULL: any). `others` gets the events of the
 * program's other windows meanwhile, so that they can draw themselves.
 */
static int
ui_choose_file(const char *title, const char *start, const char *kinds, char *path, int size, void (*others)(GR_EVENT *))
{
	return ui_files_ask(title, start, kinds, NULL, path, size, others);
}

/* Asks for a folder: only folders are listed, a double click goes into one, Choose takes the
 * selected one (or, with none, the folder shown). */
static int
ui_choose_folder(const char *title, const char *start, char *path, int size, void (*others)(GR_EVENT *))
{
	int chosen;

	ui_files_folders = 1;
	chosen = ui_files_ask(title, start, "/", NULL, path, size, others);	/* ("/" is no file's ending) */
	ui_files_folders = 0;
	return chosen;
}

/*
 * Asks what to save as, with `name` in the field: typed keys are the name's, a click on a file
 * takes its name, Right goes into the selected folder and Left out of this one. The path comes
 * back as named: the caller gives it the first ending of `kinds` (ui_files_ending) and writes.
 */
static int
ui_save_file(const char *title, const char *start, const char *kinds, const char *name, char *path, int size, void (*others)(GR_EVENT *))
{
	return ui_files_ask(title, start, kinds, name, path, size, others);
}

#endif
