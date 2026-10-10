/*
 * nxbar: a bar along the bottom of the screen with a Start button and its menu of programs,
 * a button for every open window, and a clock.
 *
 * The menu is whatever the files /usr/share/nxapps.* list, one "Label=command" a line, or
 * "Folder/Label=command" for a program in a folder: a program's build adds itself by putting
 * such a file in the image. Clicking a window's button brings the window to the front.
 */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#define MWINCLUDECOLORS
#include "nano-X.h"

#define BAR_HEIGHT 26
#define START_WIDTH 58
#define ITEM_HEIGHT 20
#define MENU_WIDTH 150
#define CLOCK_WIDTH 52
#define TASK_WIDTH 96
#define MAX_ITEMS 48
#define MAX_FOLDERS 8
#define MAX_TASKS 12
#define REGISTRY "/usr/share"
#define FACE MWRGB(192, 192, 192)
#define SHADOW MWRGB(128, 128, 128)
#define SELECTED MWRGB(0, 0, 128)

static struct { char label[32], command[64]; int folder; } items[MAX_ITEMS];	/* folder: -1 for none */
static int item_count;
static char folders[MAX_FOLDERS][16];
static int folder_count;
/* What the menu shows now: the folders and the programs in none, or one folder's programs. */
static struct { const char *label; int folder, item; } rows[MAX_ITEMS + MAX_FOLDERS + 1];
static int row_count, shown = -1;
static struct { GR_WINDOW_ID frame, client; char title[20]; int hidden; } tasks[MAX_TASKS];
static int task_count;

static GR_WINDOW_ID bar, menu;
static GR_GC_ID gc;
static GR_SCREEN_INFO screen;
static int menu_open, hover = -1;

/* The folder a line names before a slash (made if it is new), or -1. */
static int
folder_of(const char *line)
{
	const char *slash = strchr(line, '/');
	int length = slash ? (int)(slash - line) : 0, i;

	if (length < 1 || length >= (int)sizeof folders[0])
		return -1;
	for (i = 0; i < folder_count; i++)
		if ((int)strlen(folders[i]) == length && strncmp(folders[i], line, length) == 0)
			return i;
	if (folder_count == MAX_FOLDERS)
		return -1;
	memcpy(folders[folder_count], line, length);
	return folder_count++;
}

/* The menu: every line of every registry file, files in name order. */
static void
load_items(void)
{
	struct dirent **names;
	int n = scandir(REGISTRY, &names, NULL, alphasort), i;

	for (i = 0; i < n; i++) {
		char path[300], line[128];
		FILE *file;

		if (strncmp(names[i]->d_name, "nxapps.", 7) != 0)
			continue;
		snprintf(path, sizeof path, REGISTRY "/%s", names[i]->d_name);
		file = fopen(path, "r");
		while (file && item_count < MAX_ITEMS && fgets(line, sizeof line, file)) {
			char *command = strchr(line, '=');

			line[strcspn(line, "\r\n")] = 0;
			if (!command || command == line || !command[1])
				continue;
			*command++ = 0;
			items[item_count].folder = folder_of(line);
			if (items[item_count].folder >= 0)
				memmove(line, strchr(line, '/') + 1, strlen(strchr(line, '/')));
			snprintf(items[item_count].label, sizeof items[0].label, "%s", line);
			snprintf(items[item_count].command, sizeof items[0].command, "%s", command);
			item_count++;
		}
		if (file)
			fclose(file);
	}
}

/* A rectangle with a lit top-left and a shaded bottom-right edge, or the reverse. */
static void
bevel(GR_WINDOW_ID w, int x, int y, int width, int height, int pressed)
{
	GrSetGCForeground(gc, pressed ? SHADOW : WHITE);
	GrLine(w, gc, x, y, x + width - 1, y);
	GrLine(w, gc, x, y, x, y + height - 1);
	GrSetGCForeground(gc, pressed ? WHITE : SHADOW);
	GrLine(w, gc, x, y + height - 1, x + width - 1, y + height - 1);
	GrLine(w, gc, x + width - 1, y, x + width - 1, y + height - 1);
}

/* A task button's width: TASK_WIDTH, or less when that many would not fit before the clock. */
static int
task_width(void)
{
	int room = screen.cols - CLOCK_WIDTH - 6 - (START_WIDTH + 12);
	int width = task_count ? room / task_count - 4 : TASK_WIDTH;

	return width > TASK_WIDTH ? TASK_WIDTH : width < 8 ? 8 : width;
}

static int
task_x(int i)
{
	return START_WIDTH + 12 + i * (task_width() + 4);
}

static void
draw_bar(void)
{
	char text[16];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	int i, width = task_width();

	GrSetGCForeground(gc, FACE);
	GrFillRect(bar, gc, 0, 0, screen.cols, BAR_HEIGHT);
	GrSetGCForeground(gc, WHITE);
	GrLine(bar, gc, 0, 0, screen.cols - 1, 0);
	bevel(bar, 3, 3, START_WIDTH, BAR_HEIGHT - 6, menu_open);
	GrSetGCForeground(gc, BLACK);
	GrText(bar, gc, 16, 7, (void *)"Start", 5, GR_TFASCII | GR_TFTOP);
	for (i = 0; i < task_count; i++) {
		int count = strlen(tasks[i].title), w, h, b;

		/* as much of the title as the button has room for */
		for (; count > 0; count--) {
			GrGetGCTextSize(gc, tasks[i].title, count, GR_TFASCII, &w, &h, &b);
			if (w <= width - 10)
				break;
		}
		bevel(bar, task_x(i), 3, width, BAR_HEIGHT - 6, 0);
		GrSetGCForeground(gc, tasks[i].hidden ? SHADOW : BLACK);	/* minimised: greyed */
		GrText(bar, gc, task_x(i) + 6, 7, tasks[i].title, count, GR_TFASCII | GR_TFTOP);
	}
	bevel(bar, screen.cols - CLOCK_WIDTH - 3, 3, CLOCK_WIDTH, BAR_HEIGHT - 6, 1);
	snprintf(text, sizeof(text), "%02d:%02d", tm->tm_hour, tm->tm_min);
	GrSetGCForeground(gc, BLACK);
	GrText(bar, gc, screen.cols - CLOCK_WIDTH + 8, 7, text, strlen(text), GR_TFASCII | GR_TFTOP);
}

static int
by_frame(const void *a, const void *b)
{
	return (int)*(const GR_WINDOW_ID *)a - (int)*(const GR_WINDOW_ID *)b;
}

/*
 * The open windows: the root's children but our own two, once they have been seen on the
 * screen (one that is not shown now is minimised). A decorated window is a frame the window
 * manager made around the program's window, which is the one with the title. Kept in the
 * order the windows were made, so buttons stay where they are. True if anything changed.
 */
static int
find_tasks(void)
{
	static GR_WINDOW_ID before[MAX_TASKS], seen[64];
	static int before_hidden[MAX_TASKS], before_count = -1, seen_count;
	GR_WINDOW_ID still[64];
	int still_count = 0, k;
	GR_WINDOW_ID parent, *children = NULL, *inside;
	GR_COUNT n = 0, m, i;
	int changed;

	task_count = 0;
	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &children, &n);
	if (n > 1)
		qsort(children, n, sizeof *children, by_frame);
	for (i = 0; i < n && task_count < MAX_TASKS; i++) {
		GR_WINDOW_INFO info;
		GR_WM_PROPERTIES props;
		GR_WINDOW_ID client = children[i];

		if (children[i] == bar || children[i] == menu)
			continue;
		GrGetWindowInfo(children[i], &info);
		if (info.props & GR_WM_PROPS_NODECORATE)
			continue;
		for (k = 0; k < seen_count && seen[k] != children[i]; k++)
			;
		if (!info.mapped && k == seen_count)
			continue;	/* never shown yet */
		if (still_count < 64)
			still[still_count++] = children[i];
		inside = NULL;
		m = 0;
		GrQueryTree(children[i], &parent, &inside, &m);
		if (m > 0)
			client = inside[0];
		free(inside);
		props.title = NULL;
		GrGetWMProperties(client, &props);
		tasks[task_count].frame = children[i];
		tasks[task_count].client = client;
		tasks[task_count].hidden = !info.mapped;
		snprintf(tasks[task_count].title, sizeof tasks[0].title, "%.13s", props.title ? (char *)props.title : "window");
		free(props.title);
		task_count++;
	}
	free(children);
	memcpy(seen, still, sizeof still);
	seen_count = still_count;
	changed = task_count != before_count;
	for (i = 0; i < task_count && !changed; i++)
		changed = before[i] != tasks[i].frame || before_hidden[i] != tasks[i].hidden;
	for (i = 0; i < task_count; i++) {
		before[i] = tasks[i].frame;
		before_hidden[i] = tasks[i].hidden;
	}
	before_count = task_count;
	return changed;
}

static void
draw_menu(void)
{
	int i;

	GrSetGCForeground(gc, FACE);
	GrFillRect(menu, gc, 0, 0, MENU_WIDTH, row_count * ITEM_HEIGHT + 4);
	bevel(menu, 0, 0, MENU_WIDTH, row_count * ITEM_HEIGHT + 4, 0);
	for (i = 0; i < row_count; i++) {
		if (i == hover) {
			GrSetGCForeground(gc, SELECTED);
			GrFillRect(menu, gc, 2, 2 + i * ITEM_HEIGHT, MENU_WIDTH - 4, ITEM_HEIGHT);
		}
		GrSetGCForeground(gc, i == hover ? WHITE : BLACK);
		GrText(menu, gc, 10, 5 + i * ITEM_HEIGHT, (void *)rows[i].label, strlen(rows[i].label), GR_TFASCII | GR_TFTOP);
		if (rows[i].folder >= 0 && rows[i].item < 0)	/* a folder */
			GrText(menu, gc, MENU_WIDTH - 16, 5 + i * ITEM_HEIGHT, (void *)">", 1, GR_TFASCII | GR_TFTOP);
	}
}

/* Shows the folders and the programs in none (-1), or a folder's programs and a way back,
 * with row `chosen` picked out (-1: none, as when the pointer is what chooses). */
static void
show_folder(int folder, int chosen)
{
	int i;

	row_count = 0;
	shown = folder;
	for (i = 0; i < folder_count && folder < 0; i++)
		rows[row_count].label = folders[i], rows[row_count].folder = i, rows[row_count++].item = -1;
	for (i = 0; i < item_count; i++)
		if (items[i].folder == folder)
			rows[row_count].label = items[i].label, rows[row_count].folder = folder, rows[row_count++].item = i;
	/* the way back is the bottom row: the menu's bottom stays where it is, and the pointer in it */
	if (folder >= 0)
		rows[row_count].label = "< Back", rows[row_count].folder = -1, rows[row_count++].item = -1;
	hover = chosen < row_count ? chosen : -1;
	GrMoveWindow(menu, 0, screen.rows - BAR_HEIGHT - row_count * ITEM_HEIGHT - 4);
	GrResizeWindow(menu, MENU_WIDTH, row_count * ITEM_HEIGHT + 4);
	draw_menu();
}

/* Opens the menu with a row picked out (-1: none), or closes it. Open, it has the keyboard;
 * closed, the window system hands that back to the window in front. */
static void
show_menu(int open, int chosen)
{
	menu_open = open;
	hover = -1;
	if (open) {
		show_folder(-1, chosen);
		GrMapWindow(menu);
		GrRaiseWindow(menu);
		GrSetFocus(menu);
	} else
		GrUnmapWindow(menu);
	draw_bar();
}

/* Starts a command: words separated by spaces, no shell (which would cost half a second). */
static void
launch(const char *command)
{
	char text[64], *words[8];
	int n = 0;

	if (fork() != 0)
		return;
	snprintf(text, sizeof text, "%s", command);
	for (words[n] = strtok(text, " "); words[n] && n < 6; words[n] = strtok(NULL, " "))
		n++;
	words[n] = NULL;
	if (n)
		execvp(words[0], words);
	_exit(127);
}

/* "Open this" from the host (docs/open.md): a count among the machine's control words, and an
 * address in two runs of them. The last count acted on is kept in a file, for a bar started
 * again, and each new one starts `open` with the address. */
#define OPEN_COUNT 144		/* word indices: 0x87000240 */
#define OPEN_FIRST 148		/* 176 bytes of the address */
#define OPEN_MORE 196		/* and 80 more */
#define OPEN_SEEN "/tmp/nxopen.count"

static void
watch_open(void)
{
	static volatile uint32_t *regs;
	static uint32_t seen;
	static int tried;
	char address[257];
	uint32_t count, length;
	FILE *file;

	if (!tried) {
		int fd = open("/dev/gpu", O_RDWR);

		tried = 1;
		if (fd >= 0)
			regs = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x01000000);
		if (regs == MAP_FAILED)
			regs = NULL;
		if ((file = fopen(OPEN_SEEN, "r")) != NULL) {
			if (fscanf(file, "%u", &seen) != 1)
				seen = 0;
			fclose(file);
		}
	}
	if (!regs || (count = regs[OPEN_COUNT]) == seen)
		return;
	seen = count;
	if ((file = fopen(OPEN_SEEN, "w")) != NULL) {
		fprintf(file, "%u\n", seen);
		fclose(file);
	}
	length = regs[OPEN_COUNT + 1];
	if (length == 0 || length > 255)
		return;
	memcpy(address, (const void *)&regs[OPEN_FIRST], 176);
	memcpy(address + 176, (const void *)&regs[OPEN_MORE], 80);
	address[length] = 0;
	if (fork() == 0) {
		execlp("open", "open", address, (char *)NULL);
		_exit(127);
	}
}

/* Opens a row: a folder (or the way back, to the row of the folder left), or a program. */
static void
choose(int row, int keyboard)
{
	if (row < 0 || row >= row_count) {
		show_menu(0, -1);
	} else if (rows[row].item < 0) {
		show_folder(rows[row].folder, !keyboard ? -1 : rows[row].folder < 0 ? shown : 0);
	} else {
		show_menu(0, -1);
		launch(items[rows[row].item].command);
	}
}

/* The open menu's keys: arrows, Enter, Escape, and a letter for the next row it starts. */
static void
menu_key(int ch)
{
	int i, to = hover;

	switch (ch) {
	case MWKEY_UP: to = hover <= 0 ? row_count - 1 : hover - 1; break;
	case MWKEY_DOWN: to = hover + 1 >= row_count ? 0 : hover + 1; break;
	case MWKEY_HOME: case MWKEY_PAGEUP: to = 0; break;
	case MWKEY_END: case MWKEY_PAGEDOWN: to = row_count - 1; break;
	case MWKEY_ENTER: case ' ':
		if (hover >= 0)
			choose(hover, 1);
		return;
	case MWKEY_RIGHT:
		if (hover >= 0 && rows[hover].item < 0 && rows[hover].folder >= 0)
			choose(hover, 1);
		return;
	case MWKEY_LEFT: case MWKEY_BACKSPACE: case MWKEY_ESCAPE:
		if (shown >= 0)
			show_folder(-1, shown);
		else if (ch == MWKEY_ESCAPE)
			show_menu(0, -1);
		return;
	default:
		/* a letter: the next row that starts with it */
		for (i = 1; i <= row_count && ch > ' ' && ch < 127; i++)
			if (tolower(rows[(hover + i) % row_count].label[0]) == tolower(ch)) {
				to = (hover + i) % row_count;
				break;
			}
	}
	if (to != hover && row_count) {
		hover = to;
		draw_menu();
	}
}

int
main(void)
{
	GR_EVENT event;
	int minute = -1;

	if (GrOpen() < 0) {
		fprintf(stderr, "nxbar: cannot reach the Nano-X server\n");
		return 1;
	}
	signal(SIGCHLD, SIG_IGN);	/* started programs are not ours to wait for */
	load_items();
	GrGetScreenInfo(&screen);
	gc = GrNewGC();
	GrSetGCUseBackground(gc, GR_FALSE);

	bar = GrNewWindowEx(GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOMOVE | GR_WM_PROPS_NOAUTOMOVE | GR_WM_PROPS_NOFOCUS,
		"nxbar", GR_ROOT_WINDOW_ID, 0, screen.rows - BAR_HEIGHT, screen.cols, BAR_HEIGHT, FACE);
	menu = GrNewWindowEx(GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOMOVE | GR_WM_PROPS_NOAUTOMOVE,
		"nxbar menu", GR_ROOT_WINDOW_ID, 0, screen.rows - BAR_HEIGHT - ITEM_HEIGHT - 4, MENU_WIDTH, ITEM_HEIGHT + 4, FACE);
	GrSelectEvents(bar, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN);
	GrSelectEvents(menu, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_MOUSE_MOTION |
		GR_EVENT_MASK_MOUSE_EXIT | GR_EVENT_MASK_KEY_DOWN);
	GrMapWindow(bar);
	/* the Windows key, whatever window has the keyboard */
	GrGrabKey(bar, MWKEY_LMETA, GR_GRAB_HOTKEY);
	GrGrabKey(bar, MWKEY_RMETA, GR_GRAB_HOTKEY);
	/* NXBAR_SHOW=1 starts with the menu open, NXBAR_SHOW=Folder with that folder: for a test without a pointer */
	if (getenv("NXBAR_SHOW")) {
		int i;

		show_menu(1, -1);
		for (i = 0; i < folder_count; i++)
			if (strcmp(folders[i], getenv("NXBAR_SHOW")) == 0)
				show_folder(i, -1);
	}

	for (;;) {
		static time_t looked;
		time_t now;
		int i;

		GrGetNextEventTimeout(&event, 1000);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (event.exposure.wid == bar)
				draw_bar();
			else
				draw_menu();
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			if (event.keystroke.hotkey)
				show_menu(!menu_open, 0);	/* opened by a key: the first row is picked out */
			else if (menu_open && event.keystroke.wid == menu)
				menu_key(event.keystroke.ch);
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.buttons & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN))
				break;
			if (event.button.wid == bar) {
				if (event.button.x < START_WIDTH + 6) {
					show_menu(!menu_open, -1);
					break;
				}
				if (menu_open)
					show_menu(0, -1);
				for (i = 0; i < task_count; i++)
					if (event.button.x >= task_x(i) && event.button.x < task_x(i) + task_width()) {
						if (tasks[i].hidden)
							GrMapWindow(tasks[i].frame);
						GrRaiseWindow(tasks[i].frame);
						GrSetFocus(tasks[i].client);
					}
			} else {
				choose((event.button.y - 2) / ITEM_HEIGHT, 0);
			}
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (event.mouse.wid == menu && hover != (event.mouse.y - 2) / ITEM_HEIGHT) {
				hover = (event.mouse.y - 2) / ITEM_HEIGHT;
				draw_menu();
			}
			break;
		case GR_EVENT_TYPE_MOUSE_EXIT:
			if (menu_open && event.general.wid == menu)
				show_menu(0, -1);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
		/* once a second: windows that came or went, and the minute */
		now = time(NULL);
		if (now == looked)
			continue;
		looked = now;
		watch_open();
		if (find_tasks() || localtime(&now)->tm_min != minute) {
			minute = localtime(&now)->tm_min;
			draw_bar();
		}
	}
}
