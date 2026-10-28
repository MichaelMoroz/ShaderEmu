/*
 * nxbar: a bar along the bottom of the screen with a Start button and its menu of programs,
 * the programs pinned to it, a button for every open window, a volume control, the clock (a
 * click shows the month) and, at its right end, a strip that shows the desktop.
 *
 * The menu is whatever the files /usr/share/nxapps.* list, one "Label=command" a line, or
 * "Folder/Label=command" for a program in a folder (a folder may be in a folder): a program's
 * build adds itself by putting such a file in the image. A folder's programs open to its side
 * (../apps/ui_menu.h). Clicking a window's button brings the window to the front, or puts it
 * away when it is in front already; the right button there offers to minimise, close or pin
 * it, the middle one closes it, and the wheel goes from window to window. The bar is drawn
 * in the desktop's look, classic or Windows 7 (../apps/ui_look.h), and follows a change of it.
 * nxbar --look sets the look from the registry and exits (the nx script, before the desktop).
 */
#include <ctype.h>
#include "../userland/real_vfork.h"
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
#include "../apps/gameid.h"
#include "../apps/ui_icons.h"
#include "../apps/ui_menu.h"
#include "../apps/ui_launch.h"
#include "../userland/shaderemu_sound.h"

#define BAR_HEIGHT 26
#define START_WIDTH (ui_seven ? 28 : 64)
#define QUICK_W 24	/* a pinned program's button */
#define MAX_PINNED 8
#define CLOCK_WIDTH 52
#define SPEED_WIDTH 96	/* the machine's speed, before the clock */
#define VOLUME_W 22	/* the volume's button, before the speed */
#define DESK_W (ui_seven ? 12 : 10)	/* the strip that shows the desktop, at the right end */
#define NAME_MOST 120	/* the player's name before that, at most so wide */
#define TASK_WIDTH (ui_seven ? 140 : 110)
#define MAX_ITEMS 96
#define MAX_TASKS 12
#define REGISTRY "/usr/share"
#define FACE MWRGB(192, 192, 192)
#define SHADOW MWRGB(128, 128, 128)
#define TEXT_Y (ui_smooth ? 6 : 7)	/* a line's top in the bar (Selawik's ascent is a pixel more) */
#define INK (ui_seven ? MWRGB(255, 255, 255) : BLACK)
#define LOOK7_SHEET_BAR "/usr/share/look7.ppm"	/* cells of 24: 8 the Start orb, 9 it pressed */
#define PINNED_KEY "bar.pinned"	/* the registry's: commands, parted by commas */
#define PINNED_DEFAULT "nxfiles,nxterm,nxweb"
#define TASK_RESTORE 1000	/* ids of a window's own menu; a program's is its line's number plus one */
#define TASK_MINIMISE 1001
#define TASK_CLOSE 1002
#define TASK_PIN 1003
#define PIN_REMOVE 1100		/* a pinned program's menu: + which */
#define PIN_START 1200

static struct { char path[64], command[64]; } items[MAX_ITEMS];	/* path: Folder/Folder/Label */
static int item_count;
/* The menu as ui_menu.h wants it: a folder's rows are one run of this, its own folders' after it. */
static struct ui_menu_item rows[MAX_ITEMS + 32];
static char row_names[MAX_ITEMS + 32][32];
static struct ui_icon_ref row_icons[MAX_ITEMS + 32];
static int row_count;
static const struct ui_menu_item *top;
static int top_count;
static struct { GR_WINDOW_ID frame, client; char title[20]; int hidden, process; struct ui_icon_ref icon; } tasks[MAX_TASKS];
static int task_count, task_asked = -1, pin_asked = -1;
static GR_WINDOW_ID active_frame;	/* the window with the keyboard's frame, when it has a button */
static struct { char command[64]; struct ui_icon_ref icon; } pinned[MAX_PINNED];
static int pinned_count;
static GR_WINDOW_ID desk_hidden[MAX_TASKS];	/* windows the strip put away, to bring back */
static int desk_hidden_count;

static GR_WINDOW_ID bar, calendar, volume;	/* calendar, volume: the popups, while they are open */
static GR_GC_ID gc;
static GR_SCREEN_INFO screen;

/* The rows of the folder whose path is `prefix` ("" or "Games/"): its folders in the order they
 * were first named, then its programs. Returns how many, and where they are. */
static int
build(const char *prefix, const struct ui_menu_item **out)
{
	int first = row_count, count = 0, length = strlen(prefix), pass, i, j, n;

	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < item_count && row_count < MAX_ITEMS + 32; i++) {
			const char *rest = items[i].path + length, *slash = strchr(rest, '/');
			char lower[32];

			if (strncmp(items[i].path, prefix, length) != 0 || (slash != NULL) != (pass == 0))
				continue;
			n = slash ? (int)(slash - rest) : (int)strlen(rest);
			for (j = first; slash && j < first + count; j++)
				if ((int)strlen(row_names[j]) == n && !strncmp(row_names[j], rest, n))
					break;
			if (slash && j < first + count)
				continue;	/* a folder named before */
			snprintf(row_names[row_count], sizeof row_names[0], "%.*s", n, rest);
			for (j = 0; row_names[row_count][j]; j++)
				lower[j] = tolower((unsigned char)row_names[row_count][j]);
			lower[j] = 0;
			rows[row_count].label = row_names[row_count];
			rows[row_count].id = slash ? 0 : i + 1;
			/* a program's own icon (its file's), a folder's by its name */
			row_icons[row_count].file[0] = 0;
			if (!slash)
				ui_icon_of_command(items[i].command, &row_icons[row_count]);
			else
				row_icons[row_count].cell = ui_icon_of(lower) >= 0 ? ui_icon_of(lower) : ui_icon_of("folder");
			rows[row_count].icon = row_icons[row_count].cell;
			rows[row_count].icon_file = row_icons[row_count].file;
			rows[row_count].sub = NULL;
			rows[row_count].sub_count = 0;
			rows[row_count].flags = 0;
			row_count++;
			count++;
		}
	for (j = first; j < first + count; j++)
		if (rows[j].id == 0) {
			char deeper[80];

			snprintf(deeper, sizeof deeper, "%s%s/", prefix, row_names[j]);
			rows[j].sub_count = build(deeper, &rows[j].sub);
		}
	*out = rows + first;
	return count;
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
			snprintf(items[item_count].path, sizeof items[0].path, "%s", line);
			snprintf(items[item_count].command, sizeof items[0].command, "%s", command);
			item_count++;
		}
		if (file)
			fclose(file);
	}
	top_count = build("", &top);
}

/* The programs pinned to the bar, from the registry. */
static void
load_pinned(void)
{
	char list[MAX_PINNED * 64], *word, *rest;

	snprintf(list, sizeof list, "%s", reg_get(PINNED_KEY, PINNED_DEFAULT));
	pinned_count = 0;
	for (word = strtok_r(list, ",", &rest); word && pinned_count < MAX_PINNED; word = strtok_r(NULL, ",", &rest)) {
		while (*word == ' ')
			word++;
		if (!*word)
			continue;
		snprintf(pinned[pinned_count].command, sizeof pinned[0].command, "%s", word);
		ui_icon_of_command(word, &pinned[pinned_count].icon);
		pinned_count++;
	}
}

static void
save_pinned(void)
{
	char list[MAX_PINNED * 64] = "";
	int i;

	for (i = 0; i < pinned_count; i++) {
		if (i)
			strcat(list, ",");
		strcat(list, pinned[i].command);
	}
	reg_set(PINNED_KEY, list);
}

/* A command's label: the Start menu's for it, else its first word. */
static void
label_of(const char *command, char *out, int room)
{
	const char *slash;
	int i;

	for (i = 0; i < item_count; i++)
		if (!strcmp(items[i].command, command)) {
			slash = strrchr(items[i].path, '/');
			snprintf(out, room, "%s", slash ? slash + 1 : items[i].path);
			return;
		}
	snprintf(out, room, "%.*s", (int)strcspn(command, " "), command);
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

/* Windows 7's button on the bar: a lighter glass box with rounded corners; brighter for the
 * window in front, none for one put away. */
static void
glass(int x, int y, int width, int height, int how)
{
	static const GR_COLOR top[] = { MWRGB(86, 106, 136), MWRGB(118, 150, 192) },
		bottom[] = { MWRGB(60, 78, 106), MWRGB(82, 112, 156) }, edge[] = { MWRGB(118, 138, 166), MWRGB(170, 198, 236) };

	if (how < 0)
		return;
	GrSetGCForeground(gc, top[how]);
	GrFillRect(bar, gc, x + 1, y + 1, width - 2, height / 2 - 1);
	GrSetGCForeground(gc, bottom[how]);
	GrFillRect(bar, gc, x + 1, y + height / 2, width - 2, height - height / 2 - 1);
	GrSetGCForeground(gc, edge[how]);
	GrLine(bar, gc, x + 1, y, x + width - 2, y);
	GrLine(bar, gc, x + 1, y + height - 1, x + width - 2, y + height - 1);
	GrLine(bar, gc, x, y + 1, x, y + height - 2);
	GrLine(bar, gc, x + width - 1, y + 1, x + width - 1, y + height - 2);
}

/* The player's name ($USER, which the desktop's starter sets: docs/system/persist.md) and its box's width, 0 for none. */
static const char *
player_name(void)
{
	const char *name = getenv("USER");

	return name && *name ? name : NULL;
}

static int
name_width(void)
{
	static int width = -1;

	if (width < 0) {
		const char *name = player_name();

		width = name ? ui_width(&ui_menu_widths, ui_menu_font, name, strlen(name)) + 16 : 0;
		if (width > NAME_MOST)
			width = NAME_MOST;
	}
	return width ? width + 3 : 0;
}

/* Where the parts stand, from the right end: the strip, the clock, the speed, the volume. */
#define DESK_X (screen.cols - DESK_W)
#define CLOCK_X (DESK_X - 3 - CLOCK_WIDTH)
#define SPEED_X (CLOCK_X - 3 - SPEED_WIDTH)
#define VOLUME_X (SPEED_X - 2 - VOLUME_W)
#define NAME_X (VOLUME_X - 1 - name_width())

static int
quick_x(int i)
{
	return 3 + START_WIDTH + 6 + i * QUICK_W;
}

static int
tasks_x(void)
{
	return quick_x(pinned_count) + (pinned_count ? 6 : 0);
}

/* A task button's width: TASK_WIDTH, or less when that many would not fit before the tray. */
static int
task_width(void)
{
	int room = NAME_X - 6 - tasks_x();
	int width = task_count ? room / task_count - 4 : TASK_WIDTH;

	return width > TASK_WIDTH ? TASK_WIDTH : width < 8 ? 8 : width;
}

static int
task_x(int i)
{
	return tasks_x() + i * (task_width() + 4);
}

/* The machine's speed in millions of instructions a second: the first core's (its cycle counter
 * counts them), then all cores' (the workers'
 * counts are the control words, as nxmon reads them: docs/machine/multicore.md). True when the
 * text is another than it was. */
static char speed_text[24] = "";

static int
speed_sample(unsigned ms)
{
	static const volatile uint32_t *words, *more;
	static uint32_t before[256], first_before, shown[2];
	static unsigned then;
	static int tried;
	uint32_t first, ran, all = 0;
	unsigned c, cores, span = ms - then;

	if (!tried) {
		int fd = open("/dev/gpu", O_RDWR);
		void *at = fd < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x01000000);

		tried = 1;
		if (at != MAP_FAILED)
			words = at;
		at = fd < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x01000000 + 0x6ba000);
		if (at != MAP_FAILED)
			more = at;
	}
	__asm__ volatile("rdcycle %0" : "=r"(first));

	ran = first - first_before;
	first_before = first;
	cores = !words || words[0x3c0 / 4] < 1 ? 1 : words[0x3c0 / 4] > 256 ? 256 : words[0x3c0 / 4];
	if (cores > 16 && !more)
		cores = 16;
	for (c = 1; c < cores; c++) {
		uint32_t now = c < 16 ? words[0x380 / 4 + c] : more[c];

		all += now < before[c] ? now : now - before[c];	/* (counts begun again: all of it is new) */
		before[c] = now;
	}
	then = ms;
	if (span < 200 || span > 10000)
		return 0;	/* the first sample, or too short a time to say */
	ran = (uint32_t)((unsigned long long)ran * 1000 / span);
	all = ran + (uint32_t)((unsigned long long)all * 1000 / span);
	ran /= 100000, all /= 100000;
	if (ran == shown[0] && all == shown[1] && speed_text[0])
		return 0;	/* (no text is made for a second that reads as the last did) */
	shown[0] = ran, shown[1] = all;
	snprintf(speed_text, sizeof speed_text, "%u.%u / %u.%u M", ran / 10, ran % 10, all / 10, all % 10);
	return 1;
}

static void
draw_speed(void)
{
	int x = SPEED_X;

	if (ui_seven) {
		/* (the bar's own strip behind it again, this part of it) */
		GrDrawImageFromFile(bar, gc, x, 0, SPEED_WIDTH, BAR_HEIGHT, "/usr/share/bar7.ppm", 0);
	} else {
		GrSetGCForeground(gc, FACE);
		GrFillRect(bar, gc, x, 3, SPEED_WIDTH, BAR_HEIGHT - 6);
		bevel(bar, x, 3, SPEED_WIDTH, BAR_HEIGHT - 6, 1);
	}
	GrSetGCForeground(gc, ui_seven ? MWRGB(206, 218, 232) : BLACK);
	GrText(bar, gc, x + (SPEED_WIDTH - ui_width(&ui_menu_widths, ui_menu_font, speed_text, strlen(speed_text))) / 2, TEXT_Y,
		speed_text, strlen(speed_text), GR_TFASCII | GR_TFTOP);
}

static int
active_task(void)
{
	int i;

	for (i = 0; i < task_count; i++)
		if (tasks[i].frame == active_frame && !tasks[i].hidden)
			return i;
	return -1;
}

static void
draw_bar(void)
{
	char text[16];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	int i, width = task_width(), active = active_task(), open = ui_menu_is_open() && task_asked < 0 && pin_asked < 0;

	GrSetGCForeground(gc, ui_seven ? MWRGB(36, 52, 74) : FACE);
	GrFillRect(bar, gc, 0, 0, screen.cols, BAR_HEIGHT);
	if (ui_seven) {
		GrDrawImageFromFile(bar, gc, 0, 0, screen.cols, BAR_HEIGHT, "/usr/share/bar7.ppm", 0);
		GrDrawImageFromFile(bar, gc, 3, 1, 24, 24, LOOK7_SHEET_BAR, GR_IMAGE_CELL(24, open ? 9 : 8));
	} else {
		GrSetGCForeground(gc, WHITE);
		GrLine(bar, gc, 0, 0, screen.cols - 1, 0);
		bevel(bar, 3, 3, START_WIDTH, BAR_HEIGHT - 6, open);
		ui_icon(bar, gc, 8, 5, ui_icon_of("start"), 16);
		GrSetGCForeground(gc, BLACK);
		GrText(bar, gc, 28, TEXT_Y, (void *)"Start", 5, GR_TFASCII | GR_TFTOP);
		GrText(bar, gc, 29, TEXT_Y, (void *)"Start", 5, GR_TFASCII | GR_TFTOP);	/* twice: bold */
	}
	/* the pinned programs, an icon each, and a line after them */
	for (i = 0; i < pinned_count; i++)
		ui_icon_draw(bar, gc, quick_x(i) + (QUICK_W - 16) / 2, 5, &pinned[i].icon, 16);
	if (pinned_count) {
		GrSetGCForeground(gc, ui_seven ? MWRGB(20, 28, 40) : SHADOW);
		GrLine(bar, gc, quick_x(pinned_count) + 2, 4, quick_x(pinned_count) + 2, BAR_HEIGHT - 5);
		GrSetGCForeground(gc, ui_seven ? MWRGB(90, 110, 140) : WHITE);
		GrLine(bar, gc, quick_x(pinned_count) + 3, 4, quick_x(pinned_count) + 3, BAR_HEIGHT - 5);
	}
	for (i = 0; i < task_count; i++) {
		int count = strlen(tasks[i].title);

		/* as much of the title as the button has room for */
		while (count > 0 && ui_width(&ui_menu_widths, ui_menu_font, tasks[i].title, count) > width - 28)
			count--;
		if (ui_seven)
			glass(task_x(i), 2, width, BAR_HEIGHT - 4, tasks[i].hidden ? -1 : i == active);
		else {
			if (i == active) {	/* the window in front: its button stays pressed */
				GrSetGCForeground(gc, MWRGB(224, 224, 224));
				GrFillRect(bar, gc, task_x(i) + 1, 4, width - 2, BAR_HEIGHT - 8);
			}
			bevel(bar, task_x(i), 3, width, BAR_HEIGHT - 6, i == active);
		}
		if (width >= 28)
			ui_icon_draw(bar, gc, task_x(i) + 4 + (i == active && !ui_seven), 5 + (i == active && !ui_seven), &tasks[i].icon, 16);
		/* minimised: greyed */
		GrSetGCForeground(gc, tasks[i].hidden ? (ui_seven ? MWRGB(150, 162, 178) : SHADOW) : INK);
		GrText(bar, gc, task_x(i) + 24, TEXT_Y, tasks[i].title, count, GR_TFASCII | GR_TFTOP);
	}
	/* the volume's button: its icon, muted when the card is silent */
	ui_icon(bar, gc, VOLUME_X + (VOLUME_W - 16) / 2, 5, ui_icon_of(snd_page && SND_MASTER == 0 ? "volume-muted" : "volume"), 16);
	if (!ui_seven)
		bevel(bar, CLOCK_X, 3, CLOCK_WIDTH, BAR_HEIGHT - 6, 1);
	snprintf(text, sizeof(text), "%02d:%02d", tm->tm_hour, tm->tm_min);
	GrSetGCForeground(gc, INK);
	GrText(bar, gc, CLOCK_X + (CLOCK_WIDTH - ui_width(&ui_menu_widths, ui_menu_font, text, strlen(text))) / 2, TEXT_Y,
		text, strlen(text), GR_TFASCII | GR_TFTOP);
	draw_speed();
	if (name_width()) {
		const char *name = player_name();
		int x = NAME_X, count = strlen(name);

		if (!ui_seven)
			bevel(bar, x, 3, name_width() - 3, BAR_HEIGHT - 6, 1);
		while (count > 0 && ui_width(&ui_menu_widths, ui_menu_font, name, count) > name_width() - 19)
			count--;
		GrSetGCForeground(gc, ui_seven ? MWRGB(206, 218, 232) : BLACK);
		GrText(bar, gc, x + 8, TEXT_Y, (void *)name, count, GR_TFASCII | GR_TFTOP);
	}
	/* the strip that shows the desktop */
	if (ui_seven) {
		GrSetGCForeground(gc, MWRGB(20, 28, 40));
		GrLine(bar, gc, DESK_X, 0, DESK_X, BAR_HEIGHT - 1);
		GrSetGCForeground(gc, MWRGB(104, 124, 152));
		GrLine(bar, gc, DESK_X + 1, 0, DESK_X + 1, BAR_HEIGHT - 1);
	} else
		bevel(bar, DESK_X, 3, DESK_W - 2, BAR_HEIGHT - 6, desk_hidden_count > 0);
}

/* The icon of the program a window is of: its file's own, or its name's in the sheets. It is
 * looked for once a window. */
static const struct ui_icon_ref *
icon_of_frame(GR_WINDOW_ID frame, int process)
{
	static struct { GR_WINDOW_ID frame; struct ui_icon_ref icon; } known[MAX_TASKS];
	static int next, seven = -1;
	char path[40], program[96];
	int i, n;

	if (seven != ui_seven)	/* (another look: other sheets, other cells) */
		memset(known, 0, sizeof known);
	seven = ui_seven;
	for (i = 0; i < MAX_TASKS; i++)
		if (known[i].frame == frame)
			return &known[i].icon;
	snprintf(path, sizeof path, "/proc/%d/exe", process);
	n = (int)readlink(path, program, sizeof program - 1);
	program[n > 0 ? n : 0] = 0;
	i = next++ % MAX_TASKS;
	known[i].frame = frame;
	ui_icon_of_program(program, "default", &known[i].icon);
	return &known[i].icon;
}

static int
by_frame(const void *a, const void *b)
{
	return (int)*(const GR_WINDOW_ID *)a - (int)*(const GR_WINDOW_ID *)b;
}

/*
 * The open windows: the root's children but our own, once they have been seen on the screen
 * (one that is not shown now is minimised). A decorated window is a frame the window manager
 * made around the program's window, which is the one with the title. Kept in the order the
 * windows were made, so buttons stay where they are. True if anything changed.
 * (Three questions a window: asked when it is new or an update named it, or of all when `all`.)
 */
static GR_WINDOW_ID noted[8];
static int noted_count;

/* An update named this window: what the bar knows of it is asked again. */
static void
note_window(GR_WINDOW_ID id)
{
	if (noted_count < 8)
		noted[noted_count] = id;
	noted_count++;		/* (more than there is room for: every window is asked about) */
}

static int
find_tasks(int all)
{
	static GR_WINDOW_ID before[MAX_TASKS], seen[64];
	static struct { GR_WINDOW_ID frame, client; int plain, mapped, process; char title[sizeof tasks[0].title]; } known[64];
	static int known_count;
	static int before_hidden[MAX_TASKS], before_count = -1, seen_count;
	static char before_title[MAX_TASKS][sizeof tasks[0].title];
	GR_WINDOW_ID still[64];
	int still_count = 0, k;
	GR_WINDOW_ID parent, *children = NULL, *inside;
	GR_COUNT n = 0, m, i;
	int changed;

	task_count = 0;
	if (noted_count > 8)
		all = 1;
	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &children, &n);
	if (n > 1)
		qsort(children, n, sizeof *children, by_frame);
	for (i = 0; i < n && task_count < MAX_TASKS; i++) {
		GR_WINDOW_INFO info;
		GR_WM_PROPERTIES props;
		int at, ask = all, j;

		if (children[i] == bar || children[i] == calendar || children[i] == volume)
			continue;
		for (at = 0; at < known_count && known[at].frame != children[i]; at++)
			;
		for (j = 0; j < noted_count && j < 8 && !ask; j++)
			ask = noted[j] == children[i];
		if (at == known_count) {
			/* a new window: one with no frame is no task, and is not asked about again */
			at = known_count < 64 ? known_count++ : (int)(children[i] & 63);
			known[at].frame = children[i];
			known[at].client = 0;
			known[at].plain = 0;
			ask = 1;
		}
		if (ask) {
			GrGetWindowInfo(children[i], &info);
			known[at].plain = (info.props & GR_WM_PROPS_NODECORATE) != 0;
			known[at].mapped = info.mapped;
			known[at].process = info.processid;	/* (a frame is made as its program: the same process) */
		}
		if (known[at].plain)
			continue;
		for (k = 0; k < seen_count && seen[k] != children[i]; k++)
			;
		if (!known[at].mapped && k == seen_count)
			continue;	/* never shown yet */
		if (still_count < 64)
			still[still_count++] = children[i];
		/* a frame's program window stays the one it was made for: asked once */
		if (!known[at].client) {
			inside = NULL;
			m = 0;
			GrQueryTree(children[i], &parent, &inside, &m);
			known[at].client = m > 0 ? inside[0] : 0;
			free(inside);
			ask = 1;
		}
		if (ask) {
			props.title = NULL;
			GrGetWMProperties(known[at].client ? known[at].client : children[i], &props);
			snprintf(known[at].title, sizeof known[at].title, "%.13s", props.title ? (char *)props.title : "window");
			free(props.title);
		}
		tasks[task_count].frame = children[i];
		tasks[task_count].client = known[at].client ? known[at].client : children[i];
		tasks[task_count].hidden = !known[at].mapped;
		tasks[task_count].process = known[at].process;
		tasks[task_count].icon = *icon_of_frame(children[i], known[at].process);
		strcpy(tasks[task_count].title, known[at].title);
		task_count++;
	}
	free(children);
	noted_count = 0;
	memcpy(seen, still, sizeof still);
	seen_count = still_count;
	changed = task_count != before_count;
	for (i = 0; i < task_count && !changed; i++)
		changed = before[i] != tasks[i].frame || before_hidden[i] != tasks[i].hidden || strcmp(before_title[i], tasks[i].title);
	for (i = 0; i < task_count; i++) {
		before[i] = tasks[i].frame;
		before_hidden[i] = tasks[i].hidden;
		strcpy(before_title[i], tasks[i].title);
	}
	before_count = task_count;
	return changed;
}

/* The frame of the window with the keyboard, which an activation may have changed. */
static void
find_active(void)
{
	GR_WINDOW_ID focus = GrGetFocus();
	GR_WINDOW_INFO info;
	int i;

	for (i = 0; i < task_count; i++)
		if (tasks[i].client == focus || tasks[i].frame == focus)
			break;
	if (i == task_count && focus) {	/* (a window inside a program's: its frame is its top) */
		GrGetWindowInfo(focus, &info);
		for (i = 0; i < task_count && tasks[i].process != info.processid; i++)
			;
	}
	active_frame = i < task_count ? tasks[i].frame : 0;
}

/* Opens the Start menu, its first row picked out when a key opened it, or closes whatever
 * menu is open. Open, it has the keyboard; closed, the window system hands that back. */
static void
show_menu(int open, int keyboard)
{
	task_asked = pin_asked = -1;
	if (open)
		ui_menu_open(top, top_count, 0, screen.rows - BAR_HEIGHT, 1, keyboard);
	else
		ui_menu_close();
	draw_bar();
}

/* The program a window is of, as a command to pin: its file's name. */
static int
task_command(int task, char *out, int room)
{
	char path[40], program[96];
	const char *slash;
	int n;

	snprintf(path, sizeof path, "/proc/%d/exe", tasks[task].process);
	n = (int)readlink(path, program, sizeof program - 1);
	if (n <= 0)
		return 0;
	program[n] = 0;
	slash = strrchr(program, '/');
	snprintf(out, room, "%s", slash ? slash + 1 : program);
	return 1;
}

static int
pinned_index(const char *command)
{
	int i;

	for (i = 0; i < pinned_count && strcmp(pinned[i].command, command); i++)
		;
	return i < pinned_count ? i : -1;
}

/* A window's own menu, above its button. */
static void
show_task_menu(int task, int x)
{
	static struct ui_menu_item menu[4];
	char command[64];
	int can_pin = task_command(task, command, sizeof command);

	memset(menu, 0, sizeof menu);
	menu[0].label = tasks[task].hidden ? "Restore" : "Bring to front";
	menu[0].id = TASK_RESTORE;
	menu[1].label = "Minimise";
	menu[1].id = TASK_MINIMISE;
	menu[1].flags = tasks[task].hidden ? UI_MENU_GREY : 0;
	menu[2].label = can_pin && pinned_index(command) >= 0 ? "Unpin from the bar" : "Pin to the bar";
	menu[2].id = TASK_PIN;
	menu[2].flags = UI_MENU_LINE | (can_pin && (pinned_index(command) >= 0 || pinned_count < MAX_PINNED) ? 0 : UI_MENU_GREY);
	menu[3].label = "Close";
	menu[3].id = TASK_CLOSE;
	menu[3].flags = UI_MENU_LINE;
	menu[0].icon = menu[1].icon = menu[2].icon = menu[3].icon = -1;
	ui_menu_open(menu, 4, x, screen.rows - BAR_HEIGHT, 1, 0);
	task_asked = task;
}

/* A pinned program's menu: start it, or take it off the bar. */
static void
show_pin_menu(int pin)
{
	static struct ui_menu_item menu[2];
	static char start[48];

	memset(menu, 0, sizeof menu);
	label_of(pinned[pin].command, start, sizeof start);
	menu[0].label = start;
	menu[0].id = PIN_START;
	menu[0].icon = pinned[pin].icon.cell;
	menu[0].icon_file = pinned[pin].icon.file;
	menu[1].label = "Unpin from the bar";
	menu[1].id = PIN_REMOVE;
	menu[1].icon = -1;
	menu[1].flags = UI_MENU_LINE;
	ui_menu_open(menu, 2, quick_x(pin), screen.rows - BAR_HEIGHT, 1, 0);
	pin_asked = pin;
}

static void
task_front(int task)
{
	if (tasks[task].hidden) {
		GrMapWindow(tasks[task].frame);
		tasks[task].hidden = 0;
		note_window(tasks[task].frame);	/* (the server tells only a window's owner that it is shown) */
	}
	GrRaiseWindow(tasks[task].frame);
	GrSetFocus(tasks[task].client);
	active_frame = tasks[task].frame;
	desk_hidden_count = 0;	/* (a window brought back: the strip starts over) */
}

/* The strip at the right end: every window put away, or those it put away brought back. */
static void
show_desktop(void)
{
	int i;

	if (desk_hidden_count) {
		for (i = 0; i < desk_hidden_count; i++) {
			GrMapWindow(desk_hidden[i]);
			GrRaiseWindow(desk_hidden[i]);
			note_window(desk_hidden[i]);
		}
		desk_hidden_count = 0;
		return;
	}
	for (i = 0; i < task_count; i++)
		if (!tasks[i].hidden) {
			GrUnmapWindow(tasks[i].frame);
			desk_hidden[desk_hidden_count++] = tasks[i].frame;
		}
}

/* Starts a command: words separated by spaces, no shell (which would cost half a second). The
 * child's number, or 0. */
static pid_t
launch(const char *command)
{
	char text[64], *words[8];
	int n = 0;
	pid_t child;

	/* the words first: the child of a vfork has the bar's memory until its exec (no page of it
	 * copied), and may do nothing there but become the program */
	snprintf(text, sizeof text, "%s", command);
	for (words[n] = strtok(text, " "); words[n] && n < 6; words[n] = strtok(NULL, " "))
		n++;
	words[n] = NULL;
	if (!n || (child = real_vfork()) != 0)
		return n ? child : 0;
	signal(SIGCHLD, SIG_DFL);	/* (the bar's handler is not the started program's) */
	execvp(words[0], words);
	ui_exec_failed();
}

/* Starts a command for a click: the note says so until a window comes (ui_launch.h), and a
 * failure is reported. */
static void
start(const char *label, const char *command)
{
	struct ui_icon_ref icon;

	ui_icon_of_command(command, &icon);
	ui_launch_begin(label, &icon, GR_EVENT_MASK_CHLD_UPDATE);
	ui_launch_started(launch(command));
}

/* "Open this" from the host (docs/system/open.md): a count among the machine's control words, and an
 * address in two runs of them. The last count acted on is kept in a file, for a bar started
 * again, and each new one starts `open` with the address. */
#define OPEN_COUNT 144		/* word indices: 0x81000240 */
#define OPEN_FIRST 148		/* 176 bytes of the address */
#define OPEN_MORE 196		/* and 80 more */
#define OPEN_SEEN "/tmp/nxopen.count"

static void
watch_open(void)
{
	static volatile uint32_t *regs;
	static uint32_t seen;
	static int tried, opened;
	static char opened_address[257];
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
	/* the same address again while its open (or what it opened) runs: that one to the front */
	if (opened > 0 && !strcmp(address, opened_address) && kill(opened, 0) == 0) {
		int i;

		for (i = 0; i < task_count; i++)
			if (tasks[i].process == opened) {
				task_front(i);
				break;
			}
		return;
	}
	if ((opened = real_vfork()) == 0) {
		signal(SIGCHLD, SIG_DFL);
		execlp("open", "open", address, (char *)NULL);
		_exit(127);
	}
	strcpy(opened_address, address);
}

/* Tells the host which program's window has the keyboard (../apps/gameid.h). */
static void
watch_focus(void)
{
	static volatile uint32_t *regs;
	static int tried;
	static GR_WINDOW_ID focused = ~0u;
	GR_WINDOW_INFO info;
	GR_WINDOW_ID focus;
	char path[40], comm[32] = "";
	FILE *file;
	uint32_t game;

	if (!tried) {
		int fd = open("/dev/gpu", O_RDWR);

		tried = 1;
		if (fd >= 0)
			regs = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01000000);
		if (regs == MAP_FAILED)
			regs = NULL;
	}
	if (!regs)
		return;
	/* whose it is is asked only when another window has it */
	if ((focus = GrGetFocus()) == focused)
		return;
	focused = focus;
	GrGetWindowInfo(focus, &info);
	snprintf(path, sizeof path, "/proc/%d/comm", info.processid);
	if (info.processid > 0 && (file = fopen(path, "r")) != NULL) {
		if (!fgets(comm, sizeof comm, file))
			comm[0] = 0;
		fclose(file);
		comm[strcspn(comm, "\n")] = 0;
	}
	game = game_of(comm);
	if (regs[GAME_WORD] != game)
		regs[GAME_WORD] = game;
}

/* ---- the popups: the month, the volume */

#define CAL_W 196
#define CAL_H 180
#define VOL_POP_W 64
#define VOL_POP_H 168
static int cal_month, cal_year;		/* the month the calendar shows */
static int volume_held;			/* the volume's slider held */

static GR_WINDOW_ID
popup(int x, int width, int height)
{
	GR_WINDOW_ID w;

	x = x + width > screen.cols ? screen.cols - width : x;
	w = GrNewWindowEx(GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOMOVE | GR_WM_PROPS_NOAUTOMOVE | GR_WM_PROPS_POPUP,
		"popup", GR_ROOT_WINDOW_ID, x, screen.rows - BAR_HEIGHT - height, width, height, WHITE);
	GrSelectEvents(w, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(w);
	GrRaiseWindow(w);
	GrSetFocus(w);
	return w;
}

static void
popup_edge(GR_WINDOW_ID w, int width, int height)
{
	GrSetGCForeground(gc, ui_seven ? MWRGB(118, 138, 166) : BLACK);
	GrRect(w, gc, 0, 0, width, height);
}

static void
text_centred(GR_WINDOW_ID w, int x, int y, int width, const char *text)
{
	GrText(w, gc, x + (width - ui_width(&ui_menu_widths, ui_menu_font, text, strlen(text))) / 2, y, (void *)text, strlen(text),
		GR_TFASCII | GR_TFTOP);
}

static void
draw_calendar(void)
{
	static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
		"September", "October", "November", "December" };
	static const char *const days[] = { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };
	static const char *const long_days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
	time_t now = time(NULL);
	struct tm today = *localtime(&now), first;
	char text[48];
	int day, length, lead, i;

	GrSetGCForeground(gc, WHITE);
	GrFillRect(calendar, gc, 0, 0, CAL_W, CAL_H);
	popup_edge(calendar, CAL_W, CAL_H);
	GrSetGCForeground(gc, ui_seven ? MWRGB(240, 244, 250) : FACE);
	GrFillRect(calendar, gc, 1, 1, CAL_W - 2, 24);
	GrSetGCForeground(gc, BLACK);
	snprintf(text, sizeof text, "%s %d", months[cal_month], cal_year);
	text_centred(calendar, 0, 6, CAL_W, text);
	text_centred(calendar, 4, 6, 20, "<");
	text_centred(calendar, CAL_W - 24, 6, 20, ">");
	memset(&first, 0, sizeof first);
	first.tm_year = cal_year - 1900, first.tm_mon = cal_month, first.tm_mday = 1, first.tm_hour = 12;
	mktime(&first);
	lead = (first.tm_wday + 6) % 7;		/* (the week begins on Monday) */
	first.tm_mon++, first.tm_mday = 0;
	mktime(&first);
	length = first.tm_mday;
	GrSetGCForeground(gc, MWRGB(96, 96, 96));
	for (i = 0; i < 7; i++)
		text_centred(calendar, 8 + i * 26, 30, 26, days[i]);
	for (day = 1; day <= length; day++) {
		int cell = lead + day - 1, x = 8 + cell % 7 * 26, y = 48 + cell / 7 * 18;

		if (day == today.tm_mday && cal_month == today.tm_mon && cal_year == today.tm_year + 1900) {
			GrSetGCForeground(gc, ui_seven ? MWRGB(204, 232, 255) : MWRGB(10, 36, 106));
			GrFillRect(calendar, gc, x + 1, y - 1, 24, 17);
			GrSetGCForeground(gc, ui_seven ? MWRGB(51, 153, 255) : MWRGB(10, 36, 106));
			GrRect(calendar, gc, x + 1, y - 1, 24, 17);
			GrSetGCForeground(gc, ui_seven ? BLACK : WHITE);
		} else
			GrSetGCForeground(gc, BLACK);
		snprintf(text, sizeof text, "%d", day);
		text_centred(calendar, x, y + 1, 26, text);
	}
	GrSetGCForeground(gc, MWRGB(208, 208, 208));
	GrLine(calendar, gc, 6, CAL_H - 24, CAL_W - 7, CAL_H - 24);
	GrSetGCForeground(gc, BLACK);
	snprintf(text, sizeof text, "%s, %d %s %d", long_days[today.tm_wday], today.tm_mday, months[today.tm_mon], today.tm_year + 1900);
	text_centred(calendar, 0, CAL_H - 19, CAL_W, text);
}

static void
calendar_month(int step)
{
	cal_month += step;
	if (cal_month < 0)
		cal_month = 11, cal_year--;
	if (cal_month > 11)
		cal_month = 0, cal_year++;
	draw_calendar();
}

static void
close_popups(void)
{
	if (calendar)
		GrDestroyWindow(calendar);
	if (volume)
		GrDestroyWindow(volume);
	if (volume && snd_fd >= 0) {	/* (the card is let go: an open device keeps it on) */
		munmap(snd_page, 4096);
		close(snd_fd);
		snd_fd = -1;
		snd_page = NULL;
	}
	calendar = volume = 0;
	volume_held = 0;
}

static void
show_calendar(void)
{
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	close_popups();
	cal_month = tm->tm_mon, cal_year = tm->tm_year + 1900;
	calendar = popup(CLOCK_X + CLOCK_WIDTH - CAL_W, CAL_W, CAL_H);
	draw_calendar();
}

/* The volume as a share, 0 to 100. */
static int
volume_now(void)
{
	return snd_page ? (int)(SND_MASTER * 100 / 256) : 0;
}

static void
draw_volume(void)
{
	char text[8];
	int level = volume_now(), track = VOL_POP_H - 64, at = 28 + track - track * level / 100;

	GrSetGCForeground(gc, ui_seven ? MWRGB(240, 244, 250) : FACE);
	GrFillRect(volume, gc, 0, 0, VOL_POP_W, VOL_POP_H);
	popup_edge(volume, VOL_POP_W, VOL_POP_H);
	GrSetGCForeground(gc, BLACK);
	snprintf(text, sizeof text, "%d%%", level);
	text_centred(volume, 0, 6, VOL_POP_W, text);
	/* the track, and its knob */
	GrSetGCForeground(gc, MWRGB(176, 176, 176));
	GrRect(volume, gc, VOL_POP_W / 2 - 2, 28, 5, track);
	GrSetGCForeground(gc, ui_seven ? MWRGB(51, 153, 255) : MWRGB(10, 36, 106));
	GrFillRect(volume, gc, VOL_POP_W / 2 - 1, at, 3, 28 + track - at);
	GrSetGCForeground(gc, ui_seven ? MWRGB(230, 236, 244) : FACE);
	GrFillRect(volume, gc, VOL_POP_W / 2 - 10, at - 4, 21, 9);
	GrSetGCForeground(gc, ui_seven ? MWRGB(44, 98, 139) : BLACK);
	GrRect(volume, gc, VOL_POP_W / 2 - 10, at - 4, 21, 9);
	/* the mute button: the icon, under the track */
	ui_icon(volume, gc, (VOL_POP_W - 16) / 2, VOL_POP_H - 26, ui_icon_of(level ? "volume" : "volume-muted"), 16);
}

static void
volume_set(int level)
{
	static int before_mute = 70;

	if (!snd_page)
		return;
	if (level < 0) {	/* the mute button: off, or back to what it was */
		level = volume_now() ? 0 : before_mute;
		if (volume_now())
			before_mute = volume_now();
	}
	level = level < 0 ? 0 : level > 100 ? 100 : level;
	SND_MASTER = (uint32_t)level * 256 / 100;
	reg_set_int("sound.volume", level);
	draw_volume();
	draw_bar();
}

static void
show_volume(void)
{
	close_popups();
	if (snd_open() != 0) {
		ui_launch_report("Volume", "This machine has no sound card.");
		return;
	}
	volume = popup(VOLUME_X + VOLUME_W / 2 - VOL_POP_W / 2, VOL_POP_W, VOL_POP_H);
	draw_volume();
}

/* A click or a drag at y in the volume's popup. */
static void
volume_at(int y)
{
	int track = VOL_POP_H - 64;

	if (y >= VOL_POP_H - 30) {
		volume_set(-1);
		return;
	}
	volume_set((28 + track - y) * 100 / track);
}

/* What a popup's event does; true if it was one's. */
static int
popup_event(GR_EVENT *event)
{
	GR_WINDOW_ID wid = event->general.wid;

	if (!wid || (wid != calendar && wid != volume))
		return 0;
	switch (event->type) {
	case GR_EVENT_TYPE_EXPOSURE:
		if (wid == calendar)
			draw_calendar();
		else
			draw_volume();
		break;
	case GR_EVENT_TYPE_CLOSE_REQ:
		close_popups();
		break;
	case GR_EVENT_TYPE_KEY_DOWN:
		if (event->keystroke.ch == MWKEY_ESCAPE)
			close_popups();
		else if (wid == calendar && (event->keystroke.ch == MWKEY_LEFT || event->keystroke.ch == MWKEY_RIGHT))
			calendar_month(event->keystroke.ch == MWKEY_LEFT ? -1 : 1);
		else if (wid == volume && (event->keystroke.ch == MWKEY_UP || event->keystroke.ch == MWKEY_DOWN))
			volume_set(volume_now() + (event->keystroke.ch == MWKEY_UP ? 5 : -5));
		break;
	case GR_EVENT_TYPE_BUTTON_DOWN:
		if (event->button.buttons & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN)) {
			if (wid == volume)
				volume_set(volume_now() + (event->button.buttons & GR_BUTTON_SCROLLUP ? 5 : -5));
			else
				calendar_month(event->button.buttons & GR_BUTTON_SCROLLUP ? -1 : 1);
		} else if (wid == calendar && event->button.y < 26)
			calendar_month(event->button.x < 40 ? -1 : event->button.x >= CAL_W - 40 ? 1 : 0);
		else if (wid == volume) {
			volume_held = 1;
			GrSelectEvents(volume, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN |
				GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_MOUSE_MOTION | GR_EVENT_MASK_BUTTON_UP);
			volume_at(event->button.y);
		}
		break;
	case GR_EVENT_TYPE_MOUSE_MOTION:
		if (volume_held && event->mouse.y < VOL_POP_H - 30)
			volume_at(event->mouse.y);
		break;
	case GR_EVENT_TYPE_BUTTON_UP:
		/* (the moves are asked for while the knob is held: docs/system/nanox.md, ui_moves) */
		volume_held = 0;
		GrSelectEvents(volume, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
		break;
	}
	return 1;
}

/* What a menu's row does: a program is started, a window brought forward, put away, pinned or closed. */
static void
chosen(int id)
{
	int task = task_asked, pin = pin_asked;
	char command[64];

	task_asked = pin_asked = -1;
	if (id >= 1 && id <= item_count) {
		const char *label = strrchr(items[id - 1].path, '/');

		start(label ? label + 1 : items[id - 1].path, items[id - 1].command);
	} else if (task >= 0 && task < task_count && id == TASK_RESTORE)
		task_front(task);
	else if (task >= 0 && task < task_count && id == TASK_MINIMISE)
		GrUnmapWindow(tasks[task].frame);
	else if (task >= 0 && task < task_count && id == TASK_CLOSE)
		GrCloseWindow(tasks[task].client);
	else if (task >= 0 && task < task_count && id == TASK_PIN && task_command(task, command, sizeof command)) {
		int at = pinned_index(command);

		if (at >= 0) {
			memmove(&pinned[at], &pinned[at + 1], (pinned_count - at - 1) * sizeof pinned[0]);
			pinned_count--;
		} else if (pinned_count < MAX_PINNED) {
			snprintf(pinned[pinned_count].command, sizeof pinned[0].command, "%s", command);
			ui_icon_of_command(command, &pinned[pinned_count].icon);
			pinned_count++;
		}
		save_pinned();
	} else if (pin >= 0 && pin < pinned_count && id == PIN_REMOVE) {
		memmove(&pinned[pin], &pinned[pin + 1], (pinned_count - pin - 1) * sizeof pinned[0]);
		pinned_count--;
		save_pinned();
	} else if (pin >= 0 && pin < pinned_count && id == PIN_START) {
		char label[48];

		label_of(pinned[pin].command, label, sizeof label);
		start(label, pinned[pin].command);
	}
}

static unsigned
milliseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)now.tv_sec * 1000u + (unsigned)(now.tv_nsec / 1000000);
}

/* A press on the bar itself. */
static void
bar_pressed(GR_EVENT_BUTTON *b, unsigned closed_at)
{
	int i, wheel = b->buttons & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN);

	if (b->x < 3 + START_WIDTH + 3) {
		/* the press that closed the menu a moment ago was this one: it stays closed */
		if (!wheel && (ui_menu_is_open() || milliseconds() - closed_at > 300))
			show_menu(!ui_menu_is_open(), 0);
		return;
	}
	for (i = 0; i < pinned_count && !wheel; i++)
		if (b->x >= quick_x(i) && b->x < quick_x(i) + QUICK_W) {
			char label[48];

			if (b->buttons & GR_BUTTON_R)
				show_pin_menu(i);
			else {
				label_of(pinned[i].command, label, sizeof label);
				start(label, pinned[i].command);
			}
			return;
		}
	if (b->x >= tasks_x() && b->x < NAME_X && wheel && task_count) {
		/* the wheel: the window after (or before) the one in front */
		int at = active_task(), step = b->buttons & GR_BUTTON_SCROLLUP ? task_count - 1 : 1;

		task_front(at < 0 ? 0 : (at + step) % task_count);
		draw_bar();
		return;
	}
	for (i = 0; i < task_count && !wheel; i++)
		if (b->x >= task_x(i) && b->x < task_x(i) + task_width()) {
			if (b->buttons & GR_BUTTON_R)
				show_task_menu(i, task_x(i));
			else if (b->buttons & GR_BUTTON_M)
				GrCloseWindow(tasks[i].client);
			else if (i == active_task()) {	/* the window in front: put away, as Windows does */
				GrUnmapWindow(tasks[i].frame);
				active_frame = 0;
			} else
				task_front(i);
			draw_bar();
			return;
		}
	if (b->x >= VOLUME_X && b->x < VOLUME_X + VOLUME_W) {
		if (!wheel)
			volume ? close_popups() : show_volume();
		return;
	}
	if (b->x >= CLOCK_X && b->x < CLOCK_X + CLOCK_WIDTH && !wheel) {
		calendar ? close_popups() : show_calendar();
		return;
	}
	if (b->x >= DESK_X && !wheel) {
		show_desktop();
		draw_bar();
	}
}

/* The look Settings chose (ui_look.h), when it is another than the bar draws in. */
static int
restyle(void)
{
	if (!ui_look_read() && ui_menu_gc)
		return 0;
	ui_menu_restyle();
	GrSetGCFont(gc, ui_menu_font);
	return 1;
}

/* nx's word on what the desktop's start is at, in the middle of the screen until the desktop's
 * picture covers it. */
static int
splash(const char *doing)
{
	GR_SCREEN_INFO screen;
	GR_GC_ID g;
	GR_SIZE w, h, base;
	int x, y, k;
	const char *line[2] = { "Starting the desktop", doing };

	if (GrOpen() < 0)
		return 1;
	ui_look_read();
	GrGetScreenInfo(&screen);
	g = GrNewGC();
	GrSetGCUseBackground(g, GR_FALSE);
	GrSetGCFont(g, GrCreateFontEx(ui_smooth ? UI_LOOK_FONT : GR_FONT_SYSTEM_VAR, 0, 0, NULL));
	x = (screen.cols - 360) / 2, y = (screen.rows - 62) / 2;
	GrSetGCForeground(g, MWRGB(36, 38, 44));
	GrFillRect(GR_ROOT_WINDOW_ID, g, x, y, 360, 62);
	for (k = 0; k < 2; k++) {
		GrGetGCTextSize(g, (void *)line[k], -1, GR_TFASCII, &w, &h, &base);
		GrSetGCForeground(g, k ? MWRGB(200, 200, 200) : MWRGB(240, 240, 240));
		GrText(GR_ROOT_WINDOW_ID, g, x + (360 - w) / 2, y + 12 + 22 * k, (void *)line[k], -1, GR_TFASCII | GR_TFTOP);
	}
	GrClose();
	return 0;
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	int minute = -1, id;
	unsigned closed_at = 0;

	if (argc > 1 && !strcmp(argv[1], "--look")) {	/* the registry's look into the window system's word */
		ui_look_from_registry();
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "--splash"))
		return splash(argv[2]);
	if (GrOpen() < 0) {
		fprintf(stderr, "nxbar: cannot reach the Nano-X server\n");
		return 1;
	}
	ui_launch_watch();	/* started programs are reaped as they end, and a failure is reported */
	GrGetScreenInfo(&screen);
	gc = GrNewGC();
	GrSetGCUseBackground(gc, GR_FALSE);
	restyle();
	load_items();
	load_pinned();

	bar = GrNewWindowEx(GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOMOVE | GR_WM_PROPS_NOAUTOMOVE | GR_WM_PROPS_NOFOCUS,
		"nxbar", GR_ROOT_WINDOW_ID, 0, screen.rows - BAR_HEIGHT, screen.cols, BAR_HEIGHT, FACE);
	GrSelectEvents(bar, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN);
	GrMapWindow(bar);
	/* told when a window comes, goes or is put away: the buttons are looked for again then */
	GrSelectEvents(GR_ROOT_WINDOW_ID, GR_EVENT_MASK_CHLD_UPDATE);
	/* the Windows key, whatever window has the keyboard */
	GrGrabKey(bar, MWKEY_LMETA, GR_GRAB_HOTKEY);
	GrGrabKey(bar, MWKEY_RMETA, GR_GRAB_HOTKEY);
	/* NXBAR_SHOW=1 starts with the menu open, NXBAR_SHOW=Folder with that folder's beside it: for a test without a pointer */
	if (getenv("NXBAR_SHOW")) {
		show_menu(1, 0);
		ui_menu_enter_named(getenv("NXBAR_SHOW"));
	}

	for (;;) {
		static time_t looked, listed;
		static int stale = 1;	/* a window came or went since the buttons were looked for */
		static int moved = 1;	/* the keyboard may be another window's: every such change is a child update */
		static time_t asked;
		static unsigned retitle;	/* buttons whose windows' titles are asked again */
		time_t now;
		int i;

		GrGetNextEventTimeout(&event, ui_launch_wait(1000));
		ui_launch_tick();
		if (ui_launch_event(&event))
			continue;
		if (popup_event(&event))
			continue;
		switch (event.type == GR_EVENT_TYPE_KEY_DOWN && event.keystroke.hotkey ? UI_MENU_NOT : ui_menu_event(&event, &id)) {
		case UI_MENU_CHOSEN:
			chosen(id);
			/* fall through */
		case UI_MENU_CLOSED:
			closed_at = milliseconds();
			task_asked = pin_asked = -1;
			draw_bar();
			/* fall through */
		case UI_MENU_TOOK:
			event.type = GR_EVENT_TYPE_NONE;
		}
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (event.exposure.wid == bar)
				draw_bar();
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			if (event.keystroke.hotkey) {
				close_popups();
				show_menu(!ui_menu_is_open(), 1);	/* opened by a key: the first row is picked out */
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.wid == bar)
				bar_pressed(&event.button, closed_at);
			break;
		case GR_EVENT_TYPE_CHLD_UPDATE:
			if (event.update.utype == GR_UPDATE_MOVE || event.update.utype == GR_UPDATE_SIZE)
				break;
			moved = 1;
			for (i = 0; i < task_count && tasks[i].frame != event.update.subwid; i++)
				;
			/* a button's window activated may have a new title, and its button is pressed now; a
			 * window with no button that goes (a menu) changes nothing here; anything else, the
			 * buttons are looked for */
			if (i < task_count && event.update.utype == GR_UPDATE_ACTIVATE) {
				GR_WINDOW_ID was = active_frame;

				retitle |= 1u << (i & 31);
				if (tasks[i].hidden)
					note_window(tasks[i].frame);	/* shown again by its program or a key: no map is heard */
				find_active();
				if (active_frame != was)
					draw_bar();
			} else if (i < task_count || (event.update.utype != GR_UPDATE_DESTROY && event.update.utype != GR_UPDATE_UNMAP &&
			    event.update.utype != GR_UPDATE_UNMAPTEMP)) {
				stale = 1;
				note_window(event.update.subwid);
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			if (event.general.wid != bar)
				break;	/* a menu's, come after the menu went */
			GrClose();
			return 0;
		}
		/* once a second: windows that came or went, and the minute (a changed title is a child update too) */
		now = time(NULL);
		if (now == looked)
			continue;
		looked = now;
		watch_open();
		if (moved || now - asked >= 15) {
			moved = 0;
			asked = now;
			watch_focus();
		}
		i = restyle();	/* (a word of memory: Settings may have changed the look) */
		if (i) {	/* its icons: the menu's, the pinned programs', the windows' */
			row_count = 0;
			top_count = build("", &top);
			load_pinned();
			stale = 1;
		}
		if (stale || noted_count || now - listed >= 60) {
			i |= find_tasks(now - listed >= 60 || i);
			if (now - listed >= 60)
				listed = now;
			stale = 0;
			retitle = 0;
			find_active();
		}
		for (id = 0; retitle && id < task_count; id++)
			if (retitle & (1u << (id & 31))) {
				GR_WM_PROPERTIES props;
				char title[sizeof tasks[0].title];

				props.title = NULL;
				GrGetWMProperties(tasks[id].client, &props);
				snprintf(title, sizeof title, "%.13s", props.title ? (char *)props.title : "window");
				free(props.title);
				if (strcmp(title, tasks[id].title)) {
					strcpy(tasks[id].title, title);
					note_window(tasks[id].frame);
					stale = 1;	/* (the list's own record of titles is made again next second) */
					i = 1;
				}
			}
		retitle = 0;
		/* (the speed's box alone when only it reads otherwise: a tenth of a million is its step) */
		id = speed_sample(milliseconds());
		if (i || localtime(&now)->tm_min != minute) {
			minute = localtime(&now)->tm_min;
			draw_bar();
		} else if (id)
			draw_speed();
	}
}
