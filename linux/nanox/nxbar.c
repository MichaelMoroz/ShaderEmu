/*
 * nxbar: a bar along the bottom of the screen with a Start button and its menu of programs,
 * a button for every open window, and a clock.
 *
 * The menu is whatever the files /usr/share/nxapps.* list, one "Label=command" a line: a
 * program's build adds itself by putting such a file in the image (linux/nanox/build.sh,
 * doom.sh). Clicking a window's button brings the window to the front.
 */
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
#define MAX_ITEMS 20
#define MAX_TASKS 12
#define REGISTRY "/usr/share"
#define FACE MWRGB(192, 192, 192)
#define SHADOW MWRGB(128, 128, 128)
#define SELECTED MWRGB(0, 0, 128)

static struct { char label[32], command[64]; } items[MAX_ITEMS];
static int item_count;
static struct { GR_WINDOW_ID frame, client; char title[20]; int hidden; } tasks[MAX_TASKS];
static int task_count;

static GR_WINDOW_ID bar, menu;
static GR_GC_ID gc;
static GR_SCREEN_INFO screen;
static int menu_open, hover = -1;

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
	GrFillRect(menu, gc, 0, 0, MENU_WIDTH, item_count * ITEM_HEIGHT + 4);
	bevel(menu, 0, 0, MENU_WIDTH, item_count * ITEM_HEIGHT + 4, 0);
	for (i = 0; i < item_count; i++) {
		if (i == hover) {
			GrSetGCForeground(gc, SELECTED);
			GrFillRect(menu, gc, 2, 2 + i * ITEM_HEIGHT, MENU_WIDTH - 4, ITEM_HEIGHT);
		}
		GrSetGCForeground(gc, i == hover ? WHITE : BLACK);
		GrText(menu, gc, 10, 5 + i * ITEM_HEIGHT, items[i].label, strlen(items[i].label), GR_TFASCII | GR_TFTOP);
	}
}

static void
show_menu(int open)
{
	menu_open = open;
	hover = -1;
	if (open) {
		GrMapWindow(menu);
		GrRaiseWindow(menu);
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
	menu = GrNewWindowEx(GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOMOVE | GR_WM_PROPS_NOAUTOMOVE | GR_WM_PROPS_NOFOCUS,
		"nxbar menu", GR_ROOT_WINDOW_ID, 0, screen.rows - BAR_HEIGHT - item_count * ITEM_HEIGHT - 4,
		MENU_WIDTH, item_count * ITEM_HEIGHT + 4, FACE);
	GrSelectEvents(bar, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN);
	GrSelectEvents(menu, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_MOUSE_MOTION |
		GR_EVENT_MASK_MOUSE_EXIT);
	GrMapWindow(bar);

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
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.wid == bar) {
				if (event.button.x < START_WIDTH + 6) {
					show_menu(!menu_open);
					break;
				}
				if (menu_open)
					show_menu(0);
				for (i = 0; i < task_count; i++)
					if (event.button.x >= task_x(i) && event.button.x < task_x(i) + task_width()) {
						if (tasks[i].hidden)
							GrMapWindow(tasks[i].frame);
						GrRaiseWindow(tasks[i].frame);
						GrSetFocus(tasks[i].client);
					}
			} else {
				i = (event.button.y - 2) / ITEM_HEIGHT;
				show_menu(0);
				if (i >= 0 && i < item_count)
					launch(items[i].command);
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
				show_menu(0);
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
		if (find_tasks() || localtime(&now)->tm_min != minute) {
			minute = localtime(&now)->tm_min;
			draw_bar();
		}
	}
}
