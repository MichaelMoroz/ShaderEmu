/*
 * nxfiles: a file manager. A click selects, a click on the selected entry (or Enter, or Open)
 * opens it: a folder is entered, a picture goes to the viewer, a program is started, anything
 * else goes to the editor. nxfiles DIRECTORY starts there.
 */
#include <dirent.h>
#include <signal.h>
#include <strings.h>
#include <sys/stat.h>
#include "ui.h"

#define BAR 26
#define ROW 15
#define WIDTH 420
#define HEIGHT 330
#define MAX_ENTRIES 512

static struct entry { char name[64]; long size; int folder, program; } entries[MAX_ENTRIES];
static int count, selected = -1, top, rows, width = WIDTH, height = HEIGHT;
static char where[512];
static GR_WINDOW_ID window;
static const char *buttons[] = { "Up", "Open", "Edit", "New file", "Delete", "Refresh" };
#define BUTTONS 6
#define BUTTON_W 62

static int
order(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;

	return x->folder != y->folder ? y->folder - x->folder : strcmp(x->name, y->name);
}

static void
read_folder(void)
{
	DIR *folder;
	struct dirent *item;
	struct stat info;
	char path[600];

	if (!getcwd(where, sizeof where))
		strcpy(where, "/");
	count = 0;
	folder = opendir(".");
	while (folder && count < MAX_ENTRIES && (item = readdir(folder)) != NULL) {
		if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
			continue;
		snprintf(entries[count].name, sizeof entries[0].name, "%s", item->d_name);
		snprintf(path, sizeof path, "%s", item->d_name);
		entries[count].size = entries[count].folder = entries[count].program = 0;
		if (stat(path, &info) == 0) {
			entries[count].size = info.st_size;
			entries[count].folder = S_ISDIR(info.st_mode);
			entries[count].program = !S_ISDIR(info.st_mode) && (info.st_mode & 0111);
		}
		count++;
	}
	if (folder)
		closedir(folder);
	qsort(entries, count, sizeof entries[0], order);
	selected = -1;
	top = 0;
}

static void
draw_row(int i)
{
	int y = BAR + 4 + (i - top) * ROW, chosen = i == selected;
	char size[24];

	if (i < top || i >= top + rows || i >= count)
		return;
	ui_fill(window, 2, y, width - 4, ROW, chosen ? UI_SELECTED : WHITE);
	ui_text(window, 8, y + 1, entries[i].name, -1, chosen ? WHITE : entries[i].folder ? MWRGB(0, 0, 160) : BLACK, 0);
	if (entries[i].folder)
		strcpy(size, "folder");
	else
		snprintf(size, sizeof size, "%ld", entries[i].size);
	ui_text(window, width - 80, y + 1, size, -1, chosen ? WHITE : UI_SHADOW, 0);
}

static void
draw_all(void)
{
	int i;

	ui_fill(window, 0, 0, width, BAR, UI_FACE);
	for (i = 0; i < BUTTONS; i++)
		ui_button(window, 3 + i * (BUTTON_W + 3), 3, BUTTON_W, BAR - 6, buttons[i], 0);
	ui_fill(window, 0, BAR, width, height - BAR - 18, WHITE);
	ui_bevel(window, 0, BAR, width, height - BAR - 18, 1);
	for (i = top; i < top + rows; i++)
		draw_row(i);
	ui_fill(window, 0, height - 18, width, 18, UI_FACE);
	ui_text(window, 6, height - 15, where, -1, BLACK, 0);
}

static int
ends_with(const char *name, const char *end)
{
	size_t n = strlen(name), m = strlen(end);

	return n >= m && !strcasecmp(name + n - m, end);
}

static void
open_entry(int i, int edit)
{
	struct entry *e = &entries[i];
	char path[600];

	if (i < 0 || i >= count)
		return;
	snprintf(path, sizeof path, "%s/%s", strcmp(where, "/") ? where : "", e->name);
	if (e->folder) {
		if (chdir(e->name) == 0)
			read_folder();
		draw_all();
	} else if (edit) {
		ui_run("nxedit", path);
	} else if (ends_with(e->name, ".ppm") || ends_with(e->name, ".pgm") || ends_with(e->name, ".bmp") ||
		   ends_with(e->name, ".gif") || ends_with(e->name, ".xpm")) {
		ui_run("nxview", path);
	} else if (e->program) {
		ui_run(path, NULL);
	} else {
		ui_run("nxedit", path);
	}
}

static void
select_row(int i)
{
	int was = selected;

	if (i < 0 || i >= count)
		return;
	selected = i;
	if (i < top || i >= top + rows) {
		top = i < top ? i : i - rows + 1;
		draw_all();
	} else {
		draw_row(was);
		draw_row(i);
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
		if (selected >= 0 && (entries[selected].folder ? rmdir(entries[selected].name) : unlink(entries[selected].name)) == 0)
			read_folder();
		break;
	case 5:
		read_folder();
		break;
	}
	draw_all();
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
	rows = (HEIGHT - BAR - 18 - 8) / ROW;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Files", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN |
		GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(window);
	GrSetFocus(window);
	for (;;) {
		int i;

		GrGetNextEvent(&event);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw_all();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.utype == GR_UPDATE_SIZE) {
				width = event.update.width;
				height = event.update.height;
				rows = (height - BAR - 18 - 8) / ROW;
				draw_all();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.y < BAR) {
				for (i = 0; i < BUTTONS; i++)
					if (ui_inside(event.button.x, event.button.y, 3 + i * (BUTTON_W + 3), 3, BUTTON_W, BAR - 6))
						press(i);
			} else if (event.button.y < height - 18) {
				i = top + (event.button.y - BAR - 4) / ROW;
				if (i == selected)
					open_entry(i, 0);
				else
					select_row(i);
			}
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			switch (event.keystroke.ch) {
			case MWKEY_UP: select_row(selected > 0 ? selected - 1 : 0); break;
			case MWKEY_DOWN: select_row(selected + 1 < count ? selected + 1 : count - 1); break;
			case MWKEY_PAGEUP: select_row(selected > rows ? selected - rows : 0); break;
			case MWKEY_PAGEDOWN: select_row(selected + rows < count ? selected + rows : count - 1); break;
			case MWKEY_ENTER: open_entry(selected, 0); break;
			case MWKEY_BACKSPACE: press(0); break;
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
	}
}
