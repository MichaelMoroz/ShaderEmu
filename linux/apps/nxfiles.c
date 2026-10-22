/*
 * nxfiles: the file explorer, and with --desktop the desktop itself (the icons of
 * /root/Desktop on the screen's own picture, each where it was last put).
 *
 *	nxfiles [FOLDER]	a window on a folder, as tiles or as a list (View)
 *	nxfiles --desktop	the desktop: started once, by nx
 *	nxfiles --run FILE.app	starts what a shortcut names (open does this: docs/system/open.md)
 *	nxfiles --properties X	a window that says what X is (the menu's Properties)
 *
 * Two clicks open an entry: a folder is entered, a shortcut's program started, anything else
 * goes to `open`. Ctrl and Shift with a click, or Shift with the arrows, select several. The
 * right button (or the Menu key, or Shift+F10) brings a menu of what can be done. Entries are
 * dragged onto a folder, into another Files window or onto the desktop (Ctrl held: copied),
 * and on the desktop to another place. docs/system/nanox.md has the keys.
 */
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#define UI_PICTURES	/* the desktop's picture of any kind decoded (media.h) */
#include "ui.h"
#include "ui_icons.h"
#include "ui_menu.h"
#include "ui_ask.h"
#include "ui_glyph.h"
#include "ui_launch.h"

#define CARRIED "/tmp/clipboard.files"	/* "copy" or "cut", then a path a line */
#define DROPPED "/tmp/nxdrop"		/* "x y": where on the desktop a drag from a window ended */
#define DESKTOP "/root/Desktop"
#define PLACES ".places"		/* the desktop's icons' places: "x y name" a line */
#define PERSIST "/persistence"		/* the folder the host keeps between sessions (docs/system/persist.md) */
#define PERSIST_MOST 100000L		/* its bytes at most: the host keeps 100 KB a player */

#define BAR 26
#define STATUS 18
#define TILE_W 78
#define TILE_H 70
#define ROW_H 18
#define CELL_W 76			/* a place on the desktop */
#define CELL_H 70
#define WIDTH 636
#define SIDE 104			/* the places, left of the folder's entries */
#define PLACE_H 22
#define HEIGHT 350
#define MAX_ENTRIES 512
#define SCREEN_BAR 26			/* the bar along the screen's bottom (nxbar.c) */
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_KEY_DOWN | \
	GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_CLOSE_REQ)

enum { K_UNKNOWN, K_OTHER, K_FOLDER, K_PROGRAM, K_PICTURE, K_PAGE, K_SOUND, K_VIDEO, K_ARCHIVE, K_SHORTCUT };
static const char *kind_names[] = { "", "File", "Folder", "Program", "Picture", "Page", "Sound", "Video", "Archive", "Shortcut" };

static struct entry {
	char name[128], label[40], run[64];	/* run: a shortcut's command */
	long size;
	int kind, icon, chosen;			/* icon: a cell of the sheets, unless icon_file */
	char icon_file[96];			/* a file whose own icon comes first (this program, a shortcut's): `icon` if it has none */
	int x, y, placed;			/* on the desktop */
	char line[2][20];			/* and its name there, in one line or two */
	short line_w[2], lines;
} entries[MAX_ENTRIES];
static unsigned char shown[MAX_ENTRIES];	/* how each entry was last drawn: selected, at the cursor */
static char wall[200];				/* the desktop's picture, or its colour */
static int wall_w, wall_h;			/* the picture's size: 0 not read yet, -1 not a picture */
static int count, cursor = -1, anchor = -1, top, rows, columns, width = WIDTH, height = HEIGHT;
static int desktop, details;	/* the desktop, not a window; a list, not tiles */
static int thumb_held;		/* the scroll bar's thumb is held */
static char where[512];
static struct timespec folder_time;
static GR_WINDOW_ID window;
static GR_SCREEN_INFO screen;
enum { B_BACK, B_FORWARD, B_UP, B_HOME, B_VIEW, B_NEW, B_TERMINAL, BUTTONS };
#define BUTTON_W 28
static int side;		/* the places' width: none on the desktop */
/* The places: where a click on one goes (Network: the program that shows the other machines). */
static const struct { const char *label, *icon, *path; } places[] = {
	{ "Home", "home", "/root" }, { "Desktop", "desktop", "/root/Desktop" }, { "Pictures", "pictures", "/root/Pictures" },
	{ "Music", "music", "/root/Music" }, { "Videos", "videos", "/root/Videos" }, { "Computer", "computer", "/" },
	{ "Network", "nxnet", NULL },
};
#define PLACES_N ((int)(sizeof places / sizeof places[0]))
static int renaming;		/* 1: the entry at the cursor is being given a name, typed over its own; 2: a path is typed, in its field */
static char typed[128];
static int edit_at, edit_from, edit_left, edit_w;	/* the name typed: its caret, the selection's other end, the first letter shown, the box's width */
static void edit_draw(void);
static char notice[160];
static char history[16][512];
static int history_at, history_count;
static int press_tile = -1, press_x, press_y;	/* where the left button went down, on the screen */
static int press_narrow;	/* it went down on one of several selected: let go there, that one alone is */
static unsigned press_time;
static int click_tile = -1;
static unsigned click_time;
/* A box pulled with the left button from an empty place: what it touches is selected. */
static int band, band_x0, band_y0, band_x1, band_y1;	/* band: 1 held, 2 pulled far enough to be drawn */
static unsigned char band_before[MAX_ENTRIES];		/* what was selected when it began: Ctrl keeps it */
#define DESK_EVENTS (GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_KEY_DOWN)
static char ahead[24];		/* letters typed one after another: the entry that starts so */
static unsigned ahead_time;

enum { M_OPEN = 1, M_EDIT, M_CUT, M_COPY, M_PASTE, M_PASTE_IN, M_RENAME, M_DELETE, M_SHORTCUT, M_PROPERTIES,
	M_TILES, M_DETAILS, M_REFRESH, M_FOLDER, M_FILE, M_ARRANGE, M_SETTINGS, M_FILES, M_ALL, M_TERMINAL, M_TERMINAL_IN, M_PERSIST };

static unsigned
milliseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)now.tv_sec * 1000u + (unsigned)(now.tv_nsec / 1000000);
}

/* Something that went wrong, said in a window of nxask's beside this one (the desktop waits for
 * nothing), and in the status line. */
static void __attribute__((format(printf, 2, 3)))
report(const char *title, const char *format, ...)
{
	char text[400];
	va_list list;

	va_start(list, format);
	vsnprintf(text, sizeof text, format, list);
	va_end(list);
	snprintf(notice, sizeof notice, "%s", text);
	for (char *c = notice; *c; c++)
		if (*c == '\n')
			*c = ' ';
	ui_launch_report(title, text);
}

/* A text into a place of `size` bytes, cut to fit (what snprintf "%s" does, without it). */
static void
copy_text(char *to, const char *from, size_t size)
{
	size_t n = strlen(from);

	n = n < size ? n : size - 1;
	memcpy(to, from, n);
	to[n] = 0;
}

static int
ends_with(const char *name, const char *end)
{
	size_t n = strlen(name), m = strlen(end);

	return n >= m && !strcasecmp(name + n - m, end);
}

/* True if the name ends in one of the endings, each with its dot, a space between them. */
static int
ends_in(const char *name, const char *endings)
{
	int length = (int)strlen(name), n;

	for (; *endings; endings += n + (endings[n] == ' ')) {
		for (n = 0; endings[n] && endings[n] != ' '; n++)
			continue;
		if (n < 12 && length >= n && !strncasecmp(name + length - n, endings, (size_t)n))
			return 1;
	}
	return 0;
}

static int sort_by;		/* the list's order: 0 by name, 1 by size (largest first), 2 by kind */

static int
order(const void *a, const void *b)
{
	const struct entry *x = &entries[*(const short *)a], *y = &entries[*(const short *)b];
	int fx = x->kind == K_FOLDER, fy = y->kind == K_FOLDER;

	if (fx != fy)
		return fy - fx;
	if (sort_by == 1 && x->size != y->size)
		return x->size < y->size ? 1 : -1;
	if (sort_by == 2 && x->kind != y->kind)
		return x->kind - y->kind;
	return strcasecmp(x->name, y->name);
}

static int kind_of(int i);
static long size_of(int i);

/* Folders first, then by name (or by what a column's heading asked for, then by name). The entries' numbers are sorted and each entry then moved
 * once: an entry is hundreds of bytes, and sorting them was mostly copying. */
/* An entry to another place. One only just read (read_folder) has its name, kind and size
 * and nothing else to carry: the whole of it is 400 bytes, and a folder has hundreds. */
static void
move_entry(struct entry *to, const struct entry *from, int only_read)
{
	if (!only_read) {
		*to = *from;
		return;
	}
	strcpy(to->name, from->name);
	strcpy(to->label, from->label);
	to->kind = from->kind;
	to->size = from->size;
}

/* `only_read`: the entries are as read_folder made them (see move_entry). A merge sort of
 * the numbers: the C library's qsort moved each two bytes through memcpy, and compared twice
 * as often. Entries that compare the same keep the folder's order. */
static void
sort_entries(int only_read)
{
	static short to[MAX_ENTRIES], other[MAX_ENTRIES];
	static struct entry held;
	short *in = to, *out = other, *swap;
	int i, k, from, span, low, mid, high, a, b;

	for (i = 0; i < count; i++) {
		to[i] = (short)i;
		if (sort_by)	/* every entry's size and kind are asked for: slow in a folder of hundreds */
			kind_of(i), size_of(i);
	}
	if (sort_by)
		only_read = 0;
	for (span = 1; span < count; span *= 2) {
		for (low = 0; low < count; low += 2 * span) {
			mid = low + span < count ? low + span : count;
			high = low + 2 * span < count ? low + 2 * span : count;
			for (a = low, b = mid, k = low; k < high; k++)
				out[k] = b >= high || (a < mid && order(&in[a], &in[b]) <= 0) ? in[a++] : in[b++];
		}
		swap = in, in = out, out = swap;
	}
	if (in != to)
		memcpy(to, in, count * sizeof to[0]);
	for (i = 0; i < count; i++) {
		if (to[i] < 0 || to[i] == i)
			continue;
		move_entry(&held, &entries[i], only_read);
		for (k = i; (from = to[k]) != i; k = from) {
			move_entry(&entries[k], &entries[from], only_read);
			to[k] = -1;
		}
		move_entry(&entries[k], &held, only_read);
		to[k] = -1;
	}
}

/* A shortcut's lines: name=, icon= (a name of the icons' list) and run= (a command). */
static void
read_shortcut(struct entry *e)
{
	char line[128], icon[24] = "";
	FILE *file = fopen(e->name, "r");

	while (file && fgets(line, sizeof line, file)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "name=", 5))
			snprintf(e->label, sizeof e->label, "%s", line + 5);
		else if (!strncmp(line, "icon=", 5))
			snprintf(icon, sizeof icon, "%s", line + 5);
		else if (!strncmp(line, "run=", 4))
			snprintf(e->run, sizeof e->run, "%s", line + 4);
	}
	if (file)
		fclose(file);
	/* the icon it names (of the sheets, or a file that carries one), else its program's */
	e->icon_file[0] = 0;
	if (icon[0] == '/' && strlen(icon) < sizeof e->icon_file && ui_icon_in(icon)) {
		strcpy(e->icon_file, icon);
		e->icon = -1;
	} else if (icon[0] && ui_icon_of(icon) >= 0) {
		e->icon = ui_icon_of(icon);
	} else {
		struct ui_icon_ref found;

		ui_icon_of_command(e->run, &found);
		e->icon = found.cell;
		strcpy(e->icon_file, found.file);
	}
}

/* An entry's icon, 16 or 32 pixels. */
static void
draw_icon(GR_DRAW_ID to, int x, int y, const struct entry *e, int size)
{
	if (e->icon_file[0])
		GrDrawImageFromFile(to, ui_gc, x, y, size, size, (char *)e->icon_file,
			e->icon >= 0 ? GR_IMAGE_ICON_OR(size, e->icon) : GR_IMAGE_ICON(size));
	else
		ui_icon(to, ui_gc, x, y, e->icon, size);
}

/* A file's icon by its name's ending (icons.txt has the pictures), where the kind's own says
 * less: a game's data has the game's, a source file its language's. */
static const struct { const char *endings, *icon; } types[] = {
	{ ".nes", "nes" }, { ".wad .dsg", "doom" }, { ".pak .bsp .mdl", "quake" }, { ".pk3", "quake3" }, { ".cw", "classicube" }, { ".cty", "nxmicropolis" },
	{ ".c", "c-source" }, { ".h", "c-header" }, { ".cpp .cc .cxx .hpp", "cpp-source" }, { ".py .js .json .mk", "code" },
	{ ".css .xml .svg", "page" }, { ".ttf .otf .bdf .pcf .fnt", "font" }, { ".png", "png" }, { ".jpg .jpeg", "jpeg" },
	{ ".gif", "gif" }, { ".bmp .ppm .pgm .pbm .pnm", "bitmap" }, { ".flac .mp3 .wav .ogg .oga", "tune" }, { ".so", "library" },
	{ ".o .a .dat .rom .mix .aud .idx", "data" }, { ".iso .img", "disk" },
};

static const char *
type_icon(const char *lower)
{
	unsigned k;

	if (!strcmp(lower, "makefile"))
		return "code";
	for (k = 0; k < sizeof types / sizeof types[0]; k++)
		if (ends_in(lower, types[k].endings) && ui_icon_of(types[k].icon) >= 0)
			return types[k].icon;
	return NULL;
}

/* Whether a file with its execute bit is a program: the image's files all have it, so it
 * must also begin as a program does. */
static int
is_program(const char *name)
{
	char head[4] = "";
	int fd;

	if ((fd = open(name, O_RDONLY)) >= 0) {
		if (read(fd, head, sizeof head) < 4)
			head[0] = 0;
		close(fd);
	}
	return !memcmp(head, "\177ELF", 4) || !memcmp(head, "bFLT", 4) || !memcmp(head, "#!", 2);
}

static const char *kind_icon;	/* the icon's name kind_of() last chose (for --kinds) */

/* What an entry is, asked of the file system the first time it matters: in a folder of
 * hundreds of names, asking for all of them takes seconds. */
static int
kind_of(int i)
{
	struct entry *e = &entries[i];
	struct stat info;
	static char lower[128];	/* (static: kind_icon may be it, and --kinds prints it after the return) */
	const char *icon = "file", *typed;
	int k;

	if (e->kind != K_UNKNOWN && e->icon != -2)
		return e->kind;
	if (e->kind == K_SHORTCUT) {
		/* (read_folder's word for a file ending in .app: its size is asked for when it is shown) */
	} else if (e->kind == K_UNKNOWN) {
		e->kind = K_OTHER;
		if (stat(e->name, &info) == 0) {
			e->size = info.st_size;
			if (S_ISDIR(info.st_mode))
				e->kind = K_FOLDER;
			else if (ends_with(e->name, ".app"))
				e->kind = K_SHORTCUT;
			else if (ends_in(e->name, ".ppm .pgm .pbm .pnm .bmp .gif .xpm .png .jpg .jpeg .tga"))
				e->kind = K_PICTURE;
			else if (ends_in(e->name, ".html .htm"))
				e->kind = K_PAGE;
			else if (ends_in(e->name, ".wav .mp3 .flac .mod .ogg .oga"))
				e->kind = K_SOUND;
			else if (ends_in(e->name, ".mpg .mpeg .avi .vqa"))
				e->kind = K_VIDEO;
			else if (ends_in(e->name, ".zip .tar .gz .tgz"))
				e->kind = K_ARCHIVE;
			else if ((info.st_mode & 0111) && is_program(e->name))
				e->kind = K_PROGRAM;
		}
	}
	for (k = 0; e->name[k]; k++)
		lower[k] = tolower((unsigned char)e->name[k]);
	lower[k] = 0;
	copy_text(e->label, e->name, sizeof e->label);
	switch (e->kind) {
	case K_FOLDER: icon = ui_icon_of(lower) >= 0 && strcmp(lower, "default") ? lower : "folder"; break;
	case K_PROGRAM: icon = ui_icon_of(lower) >= 0 ? lower : "program"; break;
	case K_PICTURE: icon = "picture"; break;
	case K_PAGE: icon = "page"; break;
	case K_SOUND: icon = "sound"; break;
	case K_VIDEO: icon = "video"; break;
	case K_ARCHIVE: icon = "archive"; break;
	}
	/* (the image's files all say they can be run: one whose ending says what it is, is that,
	 * and is opened with its program; a program under its own name keeps its own icon) */
	if (e->kind != K_FOLDER && e->kind != K_SHORTCUT && (typed = type_icon(lower)) != NULL &&
	    !(e->kind == K_PROGRAM && ui_icon_of(lower) >= 0)) {
		icon = typed;
		if (e->kind == K_PROGRAM)
			e->kind = K_OTHER;
	}
	e->icon = ui_icon_of(icon);
	kind_icon = icon;
	e->icon_file[0] = 0;
	if (e->kind == K_PROGRAM) {
		/* its own icon, when its file ends in one: the window system looks, by its whole path,
		 * and draws the cell above for a file without (a look from here was four calls a file) */
		char path[600];

		snprintf(path, sizeof path, "%s/%s", strcmp(where, "/") ? where : "", e->name);
		if (strlen(path) < sizeof e->icon_file)
			strcpy(e->icon_file, path);
	}
	if (e->kind == K_SHORTCUT) {
		e->label[strlen(e->label) - 4] = 0;	/* without its ".app", unless it says its name */
		read_shortcut(e);
	}
	return e->kind;
}

/* An entry's size: a shortcut's is asked for here, the first time it is shown. */
static long
size_of(int i)
{
	struct stat info;

	if (entries[i].size < 0)
		entries[i].size = stat(entries[i].name, &info) == 0 ? (long)info.st_size : 0;
	return entries[i].size;
}

static int
cells_down(void)
{
	int n = (screen.rows - SCREEN_BAR - 4) / CELL_H;

	return n < 1 ? 1 : n;
}

/* True if no placed icon but `but` is at that place of the desktop. */
static int
place_free(int x, int y, int but)
{
	int i;

	for (i = 0; i < count; i++)
		if (i != but && entries[i].placed && entries[i].x == x && entries[i].y == y)
			return 0;
	return 1;
}

/* The free place nearest x, y, in whole cells: columns from the left, each from the top. */
static void
place_near(int i, int x, int y)
{
	int across = screen.cols / CELL_W, down = cells_down(), cx, cy, best = -1, bx = 0, by = 0, d;

	for (cx = 0; cx < across; cx++)
		for (cy = 0; cy < down; cy++) {
			if (!place_free(cx * CELL_W, 4 + cy * CELL_H, i))
				continue;
			d = abs(cx * CELL_W - x) + abs(4 + cy * CELL_H - y);
			if (best < 0 || d < best)
				best = d, bx = cx * CELL_W, by = 4 + cy * CELL_H;
		}
	entries[i].x = bx;
	entries[i].y = by;
	entries[i].placed = 1;
}

static void
save_places(void)
{
	FILE *file = fopen(PLACES, "w");
	int i;

	for (i = 0; file && i < count; i++)
		fprintf(file, "%d %d %s\n", entries[i].x, entries[i].y, entries[i].name);
	if (file)
		fclose(file);
}

/* Gives every icon of the desktop a place: the one it had, the one something was dropped
 * at, or the first free one, columns from the left. */
static void
place_icons(void)
{
	char line[200];
	FILE *file = fopen(PLACES, "r");
	int i, x, y, n, dropped = 0, changed = 0;

	while (file && fgets(line, sizeof line, file)) {
		line[strcspn(line, "\r\n")] = 0;
		if (sscanf(line, "%d %d %n", &x, &y, &n) < 2)
			continue;
		for (i = 0; i < count; i++)
			if (!entries[i].placed && !strcmp(entries[i].name, line + n)) {
				entries[i].x = x, entries[i].y = y, entries[i].placed = 1;
				break;
			}
	}
	if (file)
		fclose(file);
	if ((file = fopen(DROPPED, "r")) != NULL) {
		dropped = fscanf(file, "%d %d", &x, &y) == 2;
		fclose(file);
		unlink(DROPPED);
	}
	for (i = 0; i < count; i++)
		if (!entries[i].placed) {
			place_near(i, dropped ? x - CELL_W / 2 : 0, dropped ? y - CELL_H / 2 : 0);
			changed = 1;
		}
	if (changed)
		save_places();
}

static void
read_folder(void)
{
	DIR *folder;
	struct dirent *item;
	struct stat info;
	char title[540];
	int i, left_out = 0;

	if (!getcwd(where, sizeof where))
		strcpy(where, "/");
	count = 0;
	folder = opendir(".");
	while (folder && (item = readdir(folder)) != NULL) {
		struct entry *e = &entries[count];

		if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..") || (desktop && item->d_name[0] == '.'))
			continue;
		/* what is not shown is said: a name cut short would be an entry that cannot be opened */
		if (count == MAX_ENTRIES || strlen(item->d_name) >= sizeof e->name) {
			left_out++;
			continue;
		}
		/* (what an entry starts with, field by field: cleared whole it was 400 bytes of stores) */
		copy_text(e->name, item->d_name, sizeof e->name);
		copy_text(e->label, item->d_name, sizeof e->label);
		e->run[0] = e->icon_file[0] = e->line[0][0] = e->line[1][0] = 0;
		e->size = 0;
		e->chosen = e->x = e->y = e->placed = 0;
		e->line_w[0] = e->line_w[1] = e->lines = 0;
		e->kind = item->d_type == DT_DIR ? K_FOLDER : K_UNKNOWN;
		if (item->d_type == DT_REG && ends_with(e->name, ".app"))
			e->kind = K_SHORTCUT, e->size = -1;	/* what a stat would say, but for the size */
		e->icon = -2;	/* not looked for yet */
		count++;
	}
	if (folder)
		closedir(folder);
	if (left_out)
		snprintf(notice, sizeof notice, "%d more are not shown (%d entries at most, names under %d letters)", left_out, MAX_ENTRIES,
			 (int)sizeof entries[0].name);
	sort_entries(1);
	if (stat(".", &info) == 0)
		folder_time = info.st_mtim;
	cursor = anchor = -1;
	top = 0;
	if (desktop) {
		for (i = 0; i < count; i++)
			kind_of(i);
		place_icons();
	} else if (window) {
		snprintf(title, sizeof title, "Files - %s", where);
		GrSetWindowTitle(window, title);
	}
}

static int
chosen_count(void)
{
	int i, n = 0;

	for (i = 0; i < count; i++)
		n += entries[i].chosen;
	return n;
}

static void
choose_only(int i)
{
	int k;

	for (k = 0; k < count; k++)
		entries[k].chosen = k == i;
	cursor = anchor = i;
}

static int
total_rows(void)
{
	return details ? count : (count + columns - 1) / columns;
}

/* Where entry i is drawn; false if it is not in view. */
static int
tile_at(int i, int *x, int *y, int *w, int *h)
{
	int row;

	if (i < 0 || i >= count)
		return 0;
	if (desktop) {
		*x = entries[i].x, *y = entries[i].y, *w = CELL_W, *h = CELL_H;
		return 1;
	}
	row = (details ? i : i / columns) - top;
	if (row < 0 || row >= rows)
		return 0;
	if (details)
		*x = side + 2, *y = BAR + 2 + ROW_H + row * ROW_H, *w = width - side - UI_SCROLL_W - 4, *h = ROW_H;
	else
		*x = side + 3 + (i % columns) * TILE_W, *y = BAR + 3 + row * TILE_H, *w = TILE_W - 2, *h = TILE_H - 2;
	return 1;
}

/* The entry drawn at a point of the window (of the screen, for the desktop), or -1. */
static int
hit(int px, int py)
{
	int i, x, y, w, h;

	for (i = desktop || details ? 0 : top * columns; i < count; i++)
		if (tile_at(i, &x, &y, &w, &h) && ui_inside(px, py, x, y, w, h))
			return i;
	return -1;
}

/* As much of a text as fits a width, with two dots where it was cut. Returns its width. */
static int
fitted(const char *text, int most, int room, char *out, int size)
{
	int full = strlen(text), n, w = 0, dots = 2 * ui_width(&ui_var_widths, ui_var, ".", 1);

	GrSetGCFont(ui_gc, ui_var);
	/* the longest beginning that fits, with its two dots when it is not the whole */
	n = full > most ? most : full;
	n = n > size - 3 ? size - 3 : n;
	for (w = ui_width(&ui_var_widths, ui_var, text, n); n > 1 && w + (n < full ? dots : 0) > room; n--)
		w -= ui_width(&ui_var_widths, ui_var, text + n - 1, 1);
	memcpy(out, text, (size_t)n);
	strcpy(out + n, n < full ? ".." : "");
	return w + (n < full ? dots : 0);
}

/* How many of a text's first letters fit a width. */
static int
fits(const char *text, int room)
{
	int n = 0, w = 0;

	while (text[n] && (w += ui_width(&ui_var_widths, ui_var, text + n, 1)) <= room)
		n++;
	return n;
}

/* An icon of the desktop: its picture, and its name under it in one line or two. The name's
 * lines are worked out once: a text's width is a question to the window system each time. */
static void
draw_desktop_icon(int i)
{
	struct entry *e = &entries[i];
	char part[40];
	const char *label = e->label, *space;
	int k, lit = e->chosen;

	if (!e->lines) {
		/* two lines when the name has a space to break at and is too long for one */
		if (strlen(label) > 11 && (space = strchr(label + 3, ' ')) != NULL && space - label < 14) {
			snprintf(part, sizeof part, "%.*s", (int)(space - label), label);
			e->line_w[0] = fitted(part, 13, CELL_W - 4, e->line[0], sizeof e->line[0]);
			e->line_w[1] = fitted(space + 1, 13, CELL_W - 4, e->line[1], sizeof e->line[1]);
			e->lines = 2;
		} else {
			e->line_w[0] = fitted(label, 13, CELL_W - 4, e->line[0], sizeof e->line[0]);
			e->lines = 1;
		}
	}
	draw_icon(GR_ROOT_WINDOW_ID, e->x + (CELL_W - 32) / 2, e->y + 2, e, 32);
	if (renaming == 1 && i == cursor) {
		edit_draw();
		return;
	}
	for (k = 0; k < e->lines; k++) {
		int tx = e->x + (CELL_W - e->line_w[k]) / 2, ty = e->y + 37 + k * 13;

		if (lit)
			ui_fill(GR_ROOT_WINDOW_ID, tx - 2, ty, e->line_w[k] + 4, 13, MWRGB(10, 36, 106));
		else
			ui_text(GR_ROOT_WINDOW_ID, tx + 1, ty + 1, e->line[k], -1, BLACK, 0);	/* a shadow: any picture may be behind */
		ui_text(GR_ROOT_WINDOW_ID, tx, ty, e->line[k], -1, WHITE, 0);
	}
	if (i == cursor && !lit)
		ui_focus(GR_ROOT_WINDOW_ID, e->x + 2, e->y + 36, CELL_W - 4, e->lines * 13 + 2);
}

/* The desktop's picture, all of it or (the drawing clipped) what is behind one icon. */
static int wall_waiting;	/* the chosen picture's decoded copy is being made */
static int wall_turns;		/* quarter turns it is shown with (the viewer's Set as desktop) */

static void
draw_wall(void)
{
	if (wall[0] == '#') {
		unsigned long rgb = strtoul(wall + 1, NULL, 16);

		ui_fill(GR_ROOT_WINDOW_ID, 0, 0, screen.cols, screen.rows, MWRGB(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255));
		return;
	}
	/* (a PPM's size is read when the choice is: this is drawn behind every icon that changes;
	 * a picture turned in the viewer is decoded turned, whatever its kind) */
	if (!wall_w && (wall_turns || !ui_picture_size(wall, &wall_w, &wall_h)))
		wall_w = -1;
	ui_picture_turns = wall_turns;
	if (wall_w > 0)
		ui_picture_sized(GR_ROOT_WINDOW_ID, wall, screen.cols, screen.rows, BLACK, wall_w, wall_h);
	else if (!ui_picture_any(GR_ROOT_WINDOW_ID, wall, screen.cols, screen.rows, BLACK)) {
		/* (no room in the GPU's memory: its copy in /tmp is made, shown when there) */
		ui_fill(GR_ROOT_WINDOW_ID, 0, 0, screen.cols, screen.rows, MWRGB(0, 128, 128));
		wall_waiting = 1;
	}
	ui_picture_turns = 0;
}

/* What the desktop shows (Settings writes it): a picture's path, or a colour. */
static void
read_wall(void)
{
	static char told[sizeof wall];	/* a missing picture is said once */
	const char *kept = reg_get(UI_WALLPAPER_CHOICE, UI_WALLPAPER_DEFAULT);

	wall_w = wall_h = 0;
	wall_waiting = 0;
	ui_wallpaper_choice(wall, sizeof wall);
	wall_turns = ui_wallpaper_turns(wall);
	if (wall[0] == '#' && kept[0] == '/' && strcmp(told, kept)) {
		snprintf(told, sizeof told, "%s", kept);
		report("Desktop picture", "The desktop's picture %.120s\ncannot be read (%s): the desktop shows a colour.\nSettings chooses another.", kept,
		       strerror(access(kept, R_OK) == 0 ? EIO : errno));
	}
}

static void
draw_tile(int i)
{
	struct entry *e = &entries[i];
	char name[48], size[24];
	int x, y, w, h, tw;

	if (!tile_at(i, &x, &y, &w, &h))
		return;
	kind_of(i);
	shown[i] = (unsigned char)(e->chosen | (i == cursor) << 1);
	if (desktop) {
		draw_desktop_icon(i);
		return;
	}
	ui_fill(window, x, y, w, h, e->chosen ? MWRGB(10, 36, 106) : WHITE);
	if (details) {
		draw_icon(window, x + 2, y + 1, e, 16);
		fitted(e->label, 40, w - 190, name, sizeof name);
		ui_text(window, x + 22, y + 2, name, -1, e->chosen ? WHITE : BLACK, 0);
		if (e->kind != K_FOLDER) {
			if (size_of(i) < 10000)
				snprintf(size, sizeof size, "%ld", e->size);
			else
				snprintf(size, sizeof size, "%ld KB", (e->size + 512) / 1024);
			ui_text(window, x + w - 150, y + 2, size, -1, e->chosen ? WHITE : BLACK, 0);
		}
		ui_text(window, x + w - 80, y + 2, kind_names[e->kind], -1, e->chosen ? WHITE : BLACK, 0);
	} else {
		const char *label = e->label;
		int n = fits(label, w - 4), cut = n;

		draw_icon(window, x + (w - 32) / 2, y + 3, e, 32);
		/* a name too long for one line goes on in a second, from a space when one is near */
		while (label[n] && cut > 3 && label[cut] != ' ')
			cut--;
		if (label[cut] != ' ')
			cut = n;
		tw = ui_width(&ui_var_widths, ui_var, label, cut);
		/* (a name being typed has its box where the name was) */
		if (!(renaming == 1 && i == cursor)) {
			ui_text(window, x + (w - tw) / 2, y + 38, label, cut, e->chosen ? WHITE : BLACK, 0);
			if (label[n]) {
				tw = fitted(label + cut + (label[cut] == ' '), 24, w - 4, name, sizeof name);
				ui_text(window, x + (w - tw) / 2, y + 51, name, -1, e->chosen ? WHITE : BLACK, 0);
			}
		}
	}
	/* (inside the tile: ui_focus draws 2 pixels out, where the tile's own fill does not reach) */
	if (i == cursor && !e->chosen)
		ui_focus(window, x + 2, y + 2, w - 4, h - 4);
	if (renaming == 1 && i == cursor)
		edit_draw();
}

static void
draw_status(void)
{
	char text[700];
	int n = chosen_count();

	if (renaming == 2)
		snprintf(text, sizeof text, "A folder's path: Enter goes there, Escape leaves it");
	else if (renaming)
		snprintf(text, sizeof text, "A new name for %s: Enter gives it, Escape leaves the old one", entries[cursor].name);
	else if (notice[0])
		snprintf(text, sizeof text, "%s", notice);
	else if (n > 1)
		snprintf(text, sizeof text, "%d items selected", n);
	else if (cursor >= 0 && entries[cursor].chosen && kind_of(cursor) != K_FOLDER)
		snprintf(text, sizeof text, "%s   %ld bytes", entries[cursor].name, size_of(cursor));
	else {
		struct statvfs room;

		/* (what can still be written: it is kept in memory) */
		if (!desktop && statvfs(".", &room) == 0)
			snprintf(text, sizeof text, "%d items   %lu MB free", count, (unsigned long)((unsigned long long)room.f_bavail * room.f_frsize >> 20));
		else
			snprintf(text, sizeof text, "%d items", count);
	}
	if (desktop) {
		/* the desktop has no line of its own: a strip above the bar while there is something to say */
		if (notice[0]) {
			ui_fill(GR_ROOT_WINDOW_ID, 4, screen.rows - SCREEN_BAR - STATUS - 4, 440, STATUS, UI_FACE);
			ui_text_fit(GR_ROOT_WINDOW_ID, 10, screen.rows - SCREEN_BAR - STATUS - 1, 428, text, BLACK);
		}
		return;
	}
	ui_fill(window, 0, height - STATUS, width, STATUS, UI_FACE);
	ui_text_fit(window, 6, height - STATUS + 3, width - 12, text, BLACK);
}

/* The folder shown: its picture and its path, or the path being typed. */
static void
draw_path(void)
{
	int at = 3 + BUTTONS * (BUTTON_W + 3);
	char text[140];

	ui_fill(window, at, 3, width - at - 3, BAR - 6, WHITE);
	ui_bevel(window, at, 3, width - at - 3, BAR - 6, 1);
	ui_icon(window, ui_gc, at + 3, 5, ui_icon_of("folder-open"), 16);
	snprintf(text, sizeof text, "%s_", typed);
	ui_text_fit(window, at + 23, 7, width - at - 30, renaming == 2 ? text : where, BLACK);
}

/* The keys along the top, and the places down the left side. */
static void
draw_panels(void)
{
	int signs[BUTTONS] = { UI_G_LEFT, UI_G_RIGHT, UI_G_UP, UI_G_HOME, details ? UI_G_TILES : UI_G_LIST, UI_G_FOLDER, UI_G_TERMINAL };
	int grey[BUTTONS] = { history_at < 1, history_at + 1 >= history_count, !where[1], 0, 0, 0, 0 };
	int i, y, here;

	ui_fill(window, 0, 0, width, BAR, UI_FACE);
	for (i = 0; i < BUTTONS; i++)
		ui_tool(window, 3 + i * (BUTTON_W + 3), 3, BUTTON_W, BAR - 6, signs[i], grey[i] ? 2 : 0);
	draw_path();
	ui_fill(window, 0, BAR, side, height - BAR - STATUS, UI_FACE);
	for (i = 0; i < PLACES_N; i++) {
		y = BAR + 4 + i * PLACE_H + (places[i].path ? 0 : 8);
		here = places[i].path && !strcmp(where, places[i].path);
		if (!places[i].path)
			ui_fill(window, 6, y - 5, side - 12, 1, UI_SHADOW);
		if (here)
			ui_fill(window, 2, y, side - 4, PLACE_H - 2, UI_SELECTED);
		ui_icon(window, ui_gc, 6, y + 2, ui_icon_of(places[i].icon) >= 0 ? ui_icon_of(places[i].icon) : ui_icon_of("computer"), 16);
		ui_text(window, 28, y + 4, places[i].label, -1, here ? WHITE : BLACK, 0);
	}
}

static void
draw_all(void)
{
	int i, area = height - BAR - STATUS;

	if (desktop) {
		read_wall();
		draw_wall();
		for (i = 0; i < count; i++)
			draw_tile(i);
		edit_draw();
		draw_status();
		return;
	}
	draw_panels();
	ui_fill(window, side, BAR, width - side - UI_SCROLL_W, area, WHITE);
	ui_bevel(window, side, BAR, width - side - UI_SCROLL_W, area, 1);
	if (details) {
		ui_button(window, side + 2, BAR + 2, width - side - UI_SCROLL_W - 154, ROW_H, "Name", 0);
		ui_button(window, width - UI_SCROLL_W - 152, BAR + 2, 70, ROW_H, "Size", 0);
		ui_button(window, width - UI_SCROLL_W - 82, BAR + 2, 80, ROW_H, "Kind", 0);
	}
	for (i = details ? top : top * columns; i < (details ? top + rows : (top + rows) * columns) && i < count; i++)
		draw_tile(i);
	edit_draw();
	ui_scrollbar(window, width - UI_SCROLL_W, BAR, area, total_rows(), rows, top);
	draw_status();
}

/* Draws again the entries that are selected, or at the cursor, otherwise than they were drawn. */
static void
refresh(void)
{
	int i, x, y, w, h;

	for (i = 0; i < count; i++) {
		if (shown[i] == (unsigned char)(entries[i].chosen | (i == cursor) << 1) || !tile_at(i, &x, &y, &w, &h))
			continue;
		if (desktop) {
			/* the picture behind this icon first, and no more of it than that. (One region for
			 * all of them, emptied each time: a new one is a question to the window system.) */
			static GR_REGION_ID clip;
			GR_RECT cell;

			if (!clip)
				clip = GrNewRegion();
			else
				GrSubtractRegion(clip, clip, clip);
			cell.x = x, cell.y = y, cell.width = w, cell.height = h;
			GrUnionRectWithRegion(clip, &cell);
			GrSetGCRegion(ui_gc, clip);
			draw_wall();
			GrSetGCRegion(ui_gc, 0);
		}
		draw_tile(i);
	}
	edit_draw();
	draw_status();
}

/* The box's four edges, a pixel wide, and (returned) whether it is drawn at all. */
static int
band_edges(GR_RECT *e)
{
	int x = band_x0 < band_x1 ? band_x0 : band_x1, y = band_y0 < band_y1 ? band_y0 : band_y1;
	int w = abs(band_x1 - band_x0) + 1, h = abs(band_y1 - band_y0) + 1;

	e[0].x = e[1].x = e[2].x = x, e[3].x = x + w - 1;
	e[0].y = e[2].y = e[3].y = y, e[1].y = y + h - 1;
	e[0].width = e[1].width = w, e[2].width = e[3].width = 1;
	e[0].height = e[1].height = 1, e[2].height = e[3].height = h;
	return band == 2;
}

/* The box's outline, or (not `on`) what was under it again: drawn clipped to the four edges. */
static void
band_draw(int on)
{
	static GR_REGION_ID clip;
	GR_RECT e[4];
	int i, k, x, y, w, h;

	if (!band_edges(e))
		return;
	if (on) {
		for (k = 0; k < 4; k++)
			ui_fill(window, e[k].x, e[k].y, e[k].width, e[k].height, desktop ? WHITE : UI_SELECTED);
		GrFlush();
		return;
	}
	if (!clip)
		clip = GrNewRegion();
	else
		GrSubtractRegion(clip, clip, clip);
	for (k = 0; k < 4; k++)
		GrUnionRectWithRegion(clip, &e[k]);
	GrSetGCRegion(ui_gc, clip);
	if (desktop)
		draw_wall();
	else
		ui_fill(window, e[0].x, e[0].y, e[0].width, e[2].height, WHITE);
	for (i = 0; i < count; i++)
		for (k = 0; k < 4 && tile_at(i, &x, &y, &w, &h); k++)
			if (x < e[k].x + e[k].width && e[k].x < x + w && y < e[k].y + e[k].height && e[k].y < y + h) {
				draw_tile(i);
				break;
			}
	GrSetGCRegion(ui_gc, 0);
}

/* The box's other corner follows the pointer: every entry it touches is selected, with what
 * was selected before it began. */
static void
band_move(int px, int py)
{
	int left = desktop ? 0 : side + 2, right = desktop ? screen.cols - 1 : width - UI_SCROLL_W - 3;
	int up = desktop ? 0 : BAR + 2 + (details ? ROW_H : 0), down = desktop ? screen.rows - SCREEN_BAR - 1 : height - STATUS - 3;
	int i, x, y, w, h, x0, y0, x1, y1;

	band_draw(0);
	band_x1 = px < left ? left : px > right ? right : px;
	band_y1 = py < up ? up : py > down ? down : py;
	if (abs(band_x1 - band_x0) + abs(band_y1 - band_y0) > 4)
		band = 2;
	if (band != 2)
		return;
	x0 = band_x0 < band_x1 ? band_x0 : band_x1, x1 = band_x0 + band_x1 - x0;
	y0 = band_y0 < band_y1 ? band_y0 : band_y1, y1 = band_y0 + band_y1 - y0;
	for (i = 0; i < count; i++) {
		struct entry *e = &entries[i];
		int tw, lines = desktop && e->lines ? e->lines : 1, in = 0;

		if (!tile_at(i, &x, &y, &w, &h))
			continue;
		/* what is touched is the picture or the name, not the whole place they stand in
		 * (a row of the list is lit from side to side, and is touched so) */
		if (details) {
			in = x <= x1 && x0 < x + w && y <= y1 && y0 < y + h;
		} else {
			tw = desktop && e->lines ? (e->line_w[0] > e->line_w[1] ? e->line_w[0] : e->line_w[1]) :
			     ui_width(&ui_var_widths, ui_var, e->label, -1);
			tw = tw > w - 4 ? w - 4 : tw;
			in = (x + (w - 32) / 2 <= x1 && x0 < x + (w + 32) / 2 && y + 2 <= y1 && y0 < y + 35) ||
			     (x + (w - tw) / 2 <= x1 && x0 < x + (w + tw) / 2 && y + 37 <= y1 && y0 < y + 37 + lines * 13);
		}
		entries[i].chosen = band_before[i] || in;
		if (entries[i].chosen && !band_before[i])
			cursor = anchor = i;
	}
	refresh();
	band_draw(1);
}

/* The button is let go (or the pull is otherwise over): the box goes, the selection stays. */
static void
band_end(void)
{
	band_draw(0);
	if (band == 2 && getenv("NXFILES_PRINT")) {	/* for a test without a screen */
		printf("nxfiles: the box selected %d (%d,%d to %d,%d)\n", chosen_count(), band_x0, band_y0, band_x1, band_y1);
		fflush(stdout);
	}
	band = 0;
	if (desktop)
		GrSelectEvents(GR_ROOT_WINDOW_ID, DESK_EVENTS);
	else
		ui_moves(window, EVENTS, 0);
	draw_status();
}

static void
layout(void)
{
	if (details) {
		columns = 1;
		rows = (height - BAR - STATUS - 4 - ROW_H) / ROW_H;
	} else {
		columns = (width - side - UI_SCROLL_W - 6) / TILE_W;
		rows = (height - BAR - STATUS - 6) / TILE_H;
	}
	if (columns < 1)
		columns = 1;
	if (rows < 1)
		rows = 1;
}

static void
scroll_to(int row)
{
	int most = total_rows() - rows;

	if (row > most)
		row = most;
	if (row < 0)
		row = 0;
	if (row != top) {
		top = row;
		draw_all();
	}
}

/* Starts a command: words separated by spaces, no shell (which would cost half a second). The
 * child's number, or 0. */
static pid_t
run_command(const char *command)
{
	char text[64], *words[8];
	int n = 0;
	pid_t child;

	if (!command[0])
		return 0;
	/* the words first: the child (vfork, as ui_run's) has this program's memory until its exec */
	snprintf(text, sizeof text, "%s", command);
	for (words[n] = strtok(text, " "); words[n] && n < 7; words[n] = strtok(NULL, " "))
		n++;
	words[n] = NULL;
	if (!n || (child = real_vfork()) != 0)
		return n ? child : 0;
	signal(SIGCHLD, SIG_DFL);	/* (ignored here, and that would be the started program's too: its children gone unseen) */
	execvp(words[0], words);
	ui_exec_failed();
}

/* Shows another folder; `remember` adds it to the way back. */
static int
go(const char *path, int remember)
{
	if (chdir(path) != 0) {
		report("Cannot open the folder", "%.100s could not be opened:\n%s.", path, strerror(errno));
		return 0;
	}
	read_folder();
	if (remember) {
		if (history_at == 15) {
			memmove(history[0], history[1], sizeof history - sizeof history[0]);
			history_at--;
		}
		snprintf(history[++history_at], sizeof history[0], "%s", where);
		history_count = history_at + 1;
	}
	return 1;
}

static void
entry_path(int i, char *path, int size)
{
	snprintf(path, size, "%s/%s", strcmp(where, "/") ? where : "", entries[i].name);
}

static void
open_entry(int i, int edit)
{
	struct entry *e = &entries[i];
	struct ui_icon_ref icon;
	char path[600];
	pid_t child;

	if (i < 0 || i >= count)
		return;
	entry_path(i, path, sizeof path);
	if (kind_of(i) == K_FOLDER && !desktop) {
		go(e->name, 1);
		draw_all();
		return;
	}
	/* what was clicked says it is starting, first, until a window comes (ui_launch.h) */
	icon.cell = e->icon;
	snprintf(icon.file, sizeof icon.file, "%s", e->icon_file);
	ui_launch_begin(e->label[0] ? e->label : e->name, &icon, desktop ? DESK_EVENTS : 0);
	if (kind_of(i) == K_FOLDER) {
		child = ui_run("nxfiles", path);
	} else if (edit) {
		/* C is the IDE's, as it is when it is opened (nxopen.15-ide) */
		const char *dot = strrchr(path, '.');

		child = ui_run(dot && (!strcmp(dot, ".c") || !strcmp(dot, ".h")) ? "nxide" : "nxedit", path);
	} else if (e->kind == K_SHORTCUT) {
		if (!e->run[0]) {
			ui_launch_started(0);
			report("Could not start", "The shortcut %.60s names no program to start\n(it has no run= line).", e->name);
			return;
		}
		child = run_command(e->run);
	} else if (e->kind == K_PROGRAM) {
		child = ui_run(path, NULL);
	} else {
		/* whatever is registered for it (docs/system/open.md); the kinds here are only its icon */
		child = ui_run("open", path);
	}
	ui_launch_started(child);
}

/* Makes entry i the one the keys work on, in view; `extend` selects from the anchor to it. */
static void
move_to(int i, int extend)
{
	int k, row, was_top = top;

	if (count == 0)
		return;
	i = i < 0 ? 0 : i >= count ? count - 1 : i;
	if (extend && anchor >= 0) {
		for (k = 0; k < count; k++)
			entries[k].chosen = (k >= anchor && k <= i) || (k >= i && k <= anchor);
		cursor = i;
	} else
		choose_only(i);
	row = details ? i : i / columns;
	if (!desktop && (row < top || row >= top + rows))
		top = row < top ? row : row - rows + 1;
	if (top != was_top)
		draw_all();
	else
		refresh();
}

/* The entry of that name is the selected one, and in view. */
static void
select_name(const char *name)
{
	int i;

	for (i = 0; i < count; i++)
		if (!strcmp(entries[i].name, name)) {
			choose_only(i);
			if (!desktop && ((details ? i : i / columns) < top || (details ? i : i / columns) >= top + rows))
				top = details ? i : i / columns;
			return;
		}
}

static char run_said[160];	/* the first line run_wait's program wrote to its errors, or why it did not run */

/* Runs a program to its end; true if it said all went well. */
static int
run_wait(const char *program, const char *option, const char *from, const char *to)
{
	char said[40];
	pid_t child;
	int status = 0, fd;
	FILE *file;

	snprintf(said, sizeof said, "/tmp/nxfiles-%d.said", (int)getpid());
	fd = open(said, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	run_said[0] = 0;
	ui_children_hold(1);	/* (the handler would take the child this waits for) */
	child = real_vfork();
	if (child == 0) {
		ui_children_hold(0);
		if (fd >= 0)
			dup2(fd, 2);
		execlp(program, program, option, from, to, (char *)NULL);
		_exit(127);
	}
	if (child < 0)
		snprintf(run_said, sizeof run_said, "%s could not be run: %s", program, strerror(errno));
	if (child < 0 || waitpid(child, &status, 0) < 0)
		status = -1;
	ui_children_hold(0);
	if (fd >= 0)
		close(fd);
	if (status && !run_said[0] && (file = fopen(said, "r")) != NULL) {
		if (fgets(run_said, sizeof run_said, file))
			run_said[strcspn(run_said, "\n")] = 0;
		fclose(file);
	}
	if (status && !run_said[0])
		snprintf(run_said, sizeof run_said, WIFSIGNALED(status) ? "%s was stopped by signal %d" : "%s said it failed (status %d)",
			 program, WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
	unlink(said);
	return status == 0;
}

/* An entry moved, or renamed. A file of the image (the ROM, under the overlay) is copied and
 * its old name deleted: the overlay cannot rename it, its upper file system (memory) having no
 * renames that leave a whiteout (EINVAL). True if done, or run_said says why not. */
static int
move_path(const char *from, const char *to)
{
	struct stat info;

	if (rename(from, to) == 0)
		return 1;
	if ((errno != EINVAL && errno != EXDEV) || lstat(from, &info) != 0) {
		snprintf(run_said, sizeof run_said, "%s", strerror(errno));
		return 0;
	}
	/* (nor can such a folder be deleted, once its files are: it is not tried) */
	if (S_ISDIR(info.st_mode)) {
		snprintf(run_said, sizeof run_said, "a folder of the image (the ROM) can be copied, not moved or renamed");
		return 0;
	}
	if (!run_wait("cp", "-a", from, to))
		return 0;
	if (unlink(from) == 0)
		return 1;
	snprintf(run_said, sizeof run_said, "the old one could not be deleted: %s", strerror(errno));
	unlink(to);
	return 0;
}

/* Remembers the selected entries for a Paste, in this window or another. */
static void
carry(const char *how)
{
	FILE *file;
	char path[600];
	int i;

	if (!chosen_count()) {
		strcpy(notice, "nothing is selected");
		return;
	}
	if ((file = fopen(CARRIED, "w")) == NULL) {
		report(how[1] == 'o' ? "Cannot copy" : "Cannot cut", "What was selected could not be remembered for a Paste:\n" CARRIED ": %s.", strerror(errno));
		return;
	}
	fprintf(file, "%s\n", how);
	for (i = 0; i < count; i++)
		if (entries[i].chosen) {
			entry_path(i, path, sizeof path);
			fprintf(file, "%s\n", path);
		}
	fclose(file);
	strcpy(notice, how[1] == 'o' ? "copied: Paste puts it in the folder shown" : "cut: Paste moves it to the folder shown");
}

/* Puts what was copied or cut into this folder (or `into`, a folder of it), each under a name
 * that is free there. */
static void
paste(const char *into)
{
	char how[8] = "", from[600], name[160] = "", first[160] = "", why[160] = "";
	const char *base, *dot;
	FILE *file = fopen(CARRIED, "r");
	int n, stem, done = 0, failed = 0;

	if (into && chdir(into) != 0) {
		report("Cannot paste", "Nothing could be put into %.80s:\n%s.", into, strerror(errno));
		if (file)
			fclose(file);
		return;
	}
	if (file && fgets(how, sizeof how, file))
		while (fgets(from, sizeof from, file)) {
			from[strcspn(from, "\r\n")] = 0;
			if (!from[0] || access(from, F_OK) != 0)
				continue;
			base = strrchr(from, '/') ? strrchr(from, '/') + 1 : from;
			dot = strrchr(base, '.');
			stem = dot && dot != base ? (int)(dot - base) : (int)strlen(base);
			for (n = 0; n < 100; n++) {
				if (n == 0)
					snprintf(name, sizeof name, "%.127s", base);
				else
					snprintf(name, sizeof name, "%.*s-%d%.12s", stem > 100 ? 100 : stem, base, n + 1, base + stem);
				if (access(name, F_OK) != 0)
					break;
			}
			if (how[1] == 'u' ? move_path(from, name) : run_wait("cp", "-a", from, name)) {
				done++;
			} else if (!failed++) {
				snprintf(first, sizeof first, "%.100s", base);
				snprintf(why, sizeof why, "%s", run_said);
			}
		}
	if (file)
		fclose(file);
	if (into && chdir(where) != 0)
		return;
	if (!done && !failed) {
		strcpy(notice, "nothing was copied or cut");
		return;
	}
	if (how[1] == 'u')
		unlink(CARRIED);
	read_folder();
	select_name(into ? into : name);
	notice[0] = 0;
	if (failed == 1)
		report(how[1] == 'u' ? "Cannot move" : "Cannot copy", "%s could not be %s to %.80s:\n%.120s.", first,
		       how[1] == 'u' ? "moved" : "copied", into ? into : where, why);
	else if (failed)
		report(how[1] == 'u' ? "Cannot move" : "Cannot copy", "%d entries could not be %s to %.80s.\nThe first, %s:\n%.120s.",
		       failed, how[1] == 'u' ? "moved" : "copied", into ? into : where, first, why);
}

/* Where a new name is typed: over the entry's own, under its icon or in the list's row. */
static void
edit_box(int *x, int *y, int *w, int *h)
{
	int tx = 0, ty = 0, tw = 0, th = 0;

	tile_at(cursor, &tx, &ty, &tw, &th);
	if (details) {
		*x = tx + 20, *y = ty, *w = tw - 188, *h = ROW_H;
		return;
	}
	/* as wide as the name was, up to two places, about the tile's middle and inside the view */
	*w = edit_w < tw ? tw : edit_w > 2 * tw ? 2 * tw : edit_w;
	*x = tx + (tw - *w) / 2;
	*x = *x < (desktop ? 0 : side + 2) ? (desktop ? 0 : side + 2) : *x;
	if (*x + *w > (desktop ? screen.cols : width - UI_SCROLL_W - 2))
		*x = (desktop ? screen.cols : width - UI_SCROLL_W - 2) - *w;
	*y = ty + 35, *h = 17;
}

/* The name being typed, the caret in view: the selected part lit, the caret a line where none is. */
static void
edit_draw(void)
{
	GR_DRAW_ID to = desktop ? GR_ROOT_WINDOW_ID : window;
	int x, y, w, h, room, shown, lo = edit_at < edit_from ? edit_at : edit_from, hi = edit_at + edit_from - lo, a, b, ax, bx, ty;
	const char *t = typed;

	if (renaming != 1 || !tile_at(cursor, &x, &y, &w, &h))
		return;
	edit_box(&x, &y, &w, &h);
	room = w - 7;
	if (edit_left > edit_at)
		edit_left = edit_at;
	while (edit_left < edit_at && ui_width(&ui_var_widths, ui_var, t + edit_left, edit_at - edit_left) > room)
		edit_left++;
	shown = fits(t + edit_left, room);
	a = lo < edit_left ? edit_left : lo > edit_left + shown ? edit_left + shown : lo;
	b = hi < edit_left ? edit_left : hi > edit_left + shown ? edit_left + shown : hi;
	ax = x + 3 + ui_width(&ui_var_widths, ui_var, t + edit_left, a - edit_left);
	bx = x + 3 + ui_width(&ui_var_widths, ui_var, t + edit_left, b - edit_left);
	ty = y + (h - 13) / 2;
	ui_fill(to, x, y, w, h, WHITE);
	GrSetGCForeground(ui_gc, BLACK);
	GrRect(to, ui_gc, x, y, w, h);
	if (b > a)
		ui_fill(to, ax, y + 2, bx - ax, h - 4, UI_SELECTED);
	if (a > edit_left)
		ui_text(to, x + 3, ty, t + edit_left, a - edit_left, BLACK, 0);
	if (b > a)
		ui_text(to, ax, ty, t + a, b - a, WHITE, 0);
	if (edit_left + shown > b)
		ui_text(to, bx, ty, t + b, edit_left + shown - b, BLACK, 0);
	if (lo == hi)
		ui_fill(to, x + 3 + ui_width(&ui_var_widths, ui_var, t + edit_left, edit_at - edit_left), y + 2, 1, h - 4, BLACK);
}

/* The name's letter nearest a point of the box (a click in it puts the caret there). */
static int
edit_letter(int px)
{
	int x, y, w, h, n, at = edit_left, left, wide;

	edit_box(&x, &y, &w, &h);
	left = x + 3;
	for (n = strlen(typed); at < n; at++, left += wide) {
		wide = ui_width(&ui_var_widths, ui_var, typed + at, 1);
		if (px < left + wide / 2)
			break;
	}
	return at;
}

/* Begins the giving of a new name to the entry at the cursor, typed over its own name there,
 * with the name selected but for its ending (as Windows does). */
static void
rename_begin(void)
{
	const char *dot;

	if (cursor < 0) {
		strcpy(notice, "nothing is selected");
		return;
	}
	choose_only(cursor);
	kind_of(cursor);
	if (!desktop && ((details ? cursor : cursor / columns) < top || (details ? cursor : cursor / columns) >= top + rows))
		top = details ? cursor : cursor / columns;
	snprintf(typed, sizeof typed, "%s", entries[cursor].kind == K_SHORTCUT ? entries[cursor].label : entries[cursor].name);
	dot = strrchr(typed, '.');
	edit_from = 0;
	edit_at = entries[cursor].kind != K_FOLDER && entries[cursor].kind != K_SHORTCUT && dot && dot != typed ? (int)(dot - typed) : (int)strlen(typed);
	edit_left = 0;
	edit_w = ui_width(&ui_var_widths, ui_var, typed, -1) + 12;
	renaming = 1;
}

/* A shortcut file's name= line made `name`: written beside it and put in its place. */
static int
shortcut_named(const char *file_name, const char *name)
{
	char line[160], part[140];
	FILE *in = fopen(file_name, "r"), *out;
	int fine = 0;

	snprintf(part, sizeof part, "%s.new", file_name);
	if (!in || (out = fopen(part, "w")) == NULL) {
		snprintf(run_said, sizeof run_said, "%s", strerror(errno));
		if (in)
			fclose(in);
		return 0;
	}
	fprintf(out, "name=%s\n", name);
	while (fgets(line, sizeof line, in))
		if (strncmp(line, "name=", 5))
			fputs(line, out);
	fclose(in);
	fine = fclose(out) == 0 && rename(part, file_name) == 0;
	if (!fine) {
		snprintf(run_said, sizeof run_said, "%s", strerror(errno));
		unlink(part);
	}
	return fine;
}

/* The name typed is the entry's, or (`keep` 0) the old one stays. False while the box stays up
 * because the name could not be given. */
static int
rename_end(int keep)
{
	char old[128], label[40];
	int shortcut = entries[cursor].kind == K_SHORTCUT;

	if (renaming != 1)
		return 1;
	snprintf(old, sizeof old, "%s", entries[cursor].name);
	snprintf(label, sizeof label, "%s", entries[cursor].label);
	if (keep && shortcut && typed[0] && strcmp(typed, label)) {
		/* a shortcut is named by its name= line, and its file after it */
		if (strlen(typed) > sizeof label - 1) {
			report("Cannot rename", "A shortcut's name has %d letters at most.", (int)sizeof label - 1);
			return 0;
		}
		if (!shortcut_named(old, typed)) {
			report("Cannot rename", "The shortcut %s could not be written:\n%s.", old, run_said);
			return 0;
		}
		/* (the file keeps its own name where another has the new one, or it cannot be renamed) */
		snprintf(label, sizeof label, "%s", typed);
		snprintf(typed, sizeof typed, "%s.app", label);
		if ((access(typed, F_OK) == 0 && strcasecmp(typed, old)) || !move_path(old, typed))
			snprintf(typed, sizeof typed, "%s", old);
		else if (desktop) {
			snprintf(entries[cursor].name, sizeof entries[0].name, "%s", typed);
			save_places();
		}
		renaming = 0;
		read_folder();
		select_name(typed);
	}
	if (keep && !shortcut && typed[0] && strcmp(typed, old)) {
		if (!strcmp(typed, ".") || !strcmp(typed, "..")) {
			report("Cannot rename", "%s cannot be named \"%s\":\nthat name is the folder's own.", old, typed);
			return 0;
		}
		if (access(typed, F_OK) == 0 && strcasecmp(typed, old)) {
			report("Cannot rename", "%s cannot be named %s:\nthere is one of that name here already.", old, typed);
			return 0;
		}
		if (!move_path(old, typed)) {
			report("Cannot rename", "%s cannot be named %s:\n%s.", old, typed, run_said);
			return 0;
		}
		if (desktop) {
			/* the icon keeps its place under its new name */
			snprintf(entries[cursor].name, sizeof entries[0].name, "%s", typed);
			save_places();
		}
		renaming = 0;
		read_folder();
		select_name(typed);
	}
	renaming = 0;
	return 1;
}

static void
new_entry(int folder)
{
	char name[80];
	FILE *file;
	int n;

	/* the first of new.txt, new1.txt, ... that is not there yet; then its name is asked for */
	for (n = 0; n < 100; n++) {
		if (folder)
			snprintf(name, sizeof name, n ? "folder%d" : "folder", n);
		else
			snprintf(name, sizeof name, n ? "new%d.txt" : "new.txt", n);
		if (access(name, F_OK) != 0)
			break;
	}
	if (folder) {
		if (mkdir(name, 0755) != 0)
			report("Cannot make a folder", "A new folder could not be made in %.100s:\n%s.", where, strerror(errno));
	} else if ((file = fopen(name, "w")) != NULL) {
		fclose(file);
	} else {
		report("Cannot make a file", "A new file could not be made in %.100s:\n%s.", where, strerror(errno));
	}
	read_folder();
	select_name(name);
	if (!notice[0])
		rename_begin();
}

static void draw_all(void);

/* This window's events while a question's window is up. */
static void
others(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == window && !desktop)
		draw_all();
}

/* Deletes the selected entries, a folder with all it holds, once the question is answered. */
static void
delete_chosen(void)
{
	static const char *const answers[] = { "Delete", "Cancel" };
	char text[240], first[104] = "", why[160] = "";
	int i, n = chosen_count(), failed = 0, done = 0, folders = 0, one = -1;

	for (i = 0; i < count; i++)
		if (entries[i].chosen)
			folders += kind_of(i) == K_FOLDER, one = i;
	if (!n) {
		strcpy(notice, "nothing is selected");
		return;
	}
	if (n == 1)
		snprintf(text, sizeof text, "Delete %.80s?%s", entries[one].label, folders ? "\nIt is a folder: everything in it goes too." : "");
	else
		snprintf(text, sizeof text, "Delete these %d entries?%s", n, folders ? "\nA folder goes with everything in it." : "");
	if (ui_ask("Delete", text, answers, 2, others) != 0)
		return;
	for (i = 0; i < count; i++)
		if (entries[i].chosen) {
			if (kind_of(i) == K_FOLDER ? run_wait("rm", "-rf", "--", entries[i].name) : unlink(entries[i].name) == 0) {
				done++;
			} else if (!failed++) {
				snprintf(first, sizeof first, "%.100s", entries[i].name);
				snprintf(why, sizeof why, "%s", entries[i].kind == K_FOLDER ? run_said : strerror(errno));
			}
		}
	if (done)
		read_folder();
	if (failed == 1)
		report("Cannot delete", "%s could not be deleted:\n%.120s.", first, why);
	else if (failed)
		report("Cannot delete", "%d of the %d could not be deleted.\nThe first, %s:\n%.120s.", failed, n, first, why);
}

/* A shortcut to entry i on the desktop: what two clicks on the entry would do. */
static void
shortcut_of(int i)
{
	char path[600], file_name[160];
	struct entry *e = &entries[i];
	FILE *file;

	entry_path(i, path, sizeof path);
	snprintf(file_name, sizeof file_name, DESKTOP "/%.60s.app", e->label);
	mkdir(DESKTOP, 0755);
	if ((file = fopen(file_name, "w")) == NULL) {
		report("Cannot make a shortcut", "The shortcut %.80s could not be made:\n%s.", file_name, strerror(errno));
		return;
	}
	fprintf(file, "name=%s\n", e->label);
	if (kind_of(i) == K_FOLDER)
		fprintf(file, "icon=folder\nrun=nxfiles %s\n", path);
	else if (e->kind == K_SHORTCUT)
		fprintf(file, "run=%s\n", e->run);
	else if (e->kind == K_PROGRAM)
		fprintf(file, "run=%s\n", path);
	else
		fprintf(file, "icon=%s\nrun=open %s\n", e->kind == K_PICTURE ? "picture" : e->kind == K_SOUND ? "sound" :
			e->kind == K_VIDEO ? "video" : e->kind == K_PAGE ? "page" : "file", path);
	fclose(file);
	strcpy(notice, "a shortcut is on the desktop");
}

/* What is known of an entry (or of the folder shown: -1), in a window of its own: this program
 * again, so that it stays when this window goes elsewhere. */
static void
properties(int i)
{
	char path[600];
	pid_t child;

	if (i < 0)
		snprintf(path, sizeof path, "%s", where);
	else
		entry_path(i, path, sizeof path);
	ui_exec_errno = 0;
	if ((child = real_vfork()) == 0) {
		signal(SIGCHLD, SIG_DFL);
		execlp("nxfiles", "nxfiles", "--properties", path, (char *)NULL);
		ui_exec_failed();
	}
	if (child < 0 || ui_exec_errno)
		report("Could not start", "The properties of %.100s could not be shown:\n%s.", path, strerror(child < 0 ? errno : ui_exec_errno));
}

/* When a file of the ROM was made, from /usr/share/rom-dates ("SECONDS /PATH" a line), or 0. */
static time_t
rom_date(const char *folder, const char *leaf)
{
	FILE *list = fopen("/usr/share/rom-dates", "r");
	char line[700], path[600];
	char *name;
	long when = 0;

	if (!list)
		return 0;
	snprintf(path, sizeof path, "%s/%s", folder[1] ? folder : "", leaf);
	while (fgets(line, sizeof line, list)) {
		line[strcspn(line, "\n")] = 0;
		if ((name = strchr(line, ' ')) != NULL && !strcmp(name + 1, path)) {
			when = atol(line);
			break;
		}
	}
	fclose(list);
	return (time_t)when;
}

/* nxfiles --properties PATH: a small window that says what a file, a folder or a shortcut is. */
static int
properties_window(const char *path)
{
	static char lines[7][2][200];
	struct entry *e = &entries[0];
	struct stat info;
	struct tm *tm;
	char folder[512], title[120];
	const char *slash = strrchr(path, '/'), *leaf = slash && slash[1] ? slash + 1 : path;
	GR_EVENT event;
	DIR *inside;
	int n = 0, i, items = 0;
	const int wide = 360, high = 236;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	snprintf(folder, sizeof folder, "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
	if (chdir(folder[0] ? folder : "/") != 0 || stat(leaf, &info) != 0) {
		fprintf(stderr, "nxfiles: %s: no such entry\n", path);
		return 1;
	}
	if (!getcwd(where, sizeof where))
		strcpy(where, "/");
	memset(e, 0, sizeof *e);
	snprintf(e->name, sizeof e->name, "%s", leaf);
	e->icon = -2;
	count = 1;
	kind_of(0);
#define LINE(what, ...) (snprintf(lines[n][0], sizeof lines[0][0], "%s", what), snprintf(lines[n][1], sizeof lines[0][1], __VA_ARGS__), n++)
	LINE("Kind", "%s", e->kind == K_PROGRAM && e->icon_file[0] && ui_icon_in(e->icon_file) ? "Program, with an icon of its own" : kind_names[e->kind]);
	LINE("Where", "%s", where);
	if (e->kind == K_FOLDER) {
		if ((inside = opendir(leaf)) != NULL) {
			while (readdir(inside))
				items++;
			closedir(inside);
		}
		LINE("Holds", "%d entries", items > 2 ? items - 2 : 0);
	} else if (info.st_size < 10000)
		LINE("Size", "%ld bytes", (long)info.st_size);
	else
		LINE("Size", "%ld KB (%ld bytes)", (long)(info.st_size + 512) / 1024, (long)info.st_size);
	/* the ROM keeps no dates (a file of the image says 1970) and marks every file as a program:
	 * the image's builder wrote its files' dates down beside them */
	if (info.st_mtime < 86400)
		info.st_mtime = rom_date(where, leaf);
	if (info.st_mtime < 86400)
		LINE("Changed", "not known: older than this image's own files");
	else if ((tm = localtime(&info.st_mtime)) != NULL)
		strftime(lines[n][1], sizeof lines[0][1], "%Y-%m-%d %H:%M", tm), strcpy(lines[n++][0], "Changed");
	LINE("May", "%s%s%s", info.st_mode & 0444 ? "be read" : "not be read", info.st_mode & 0222 ? ", written" : "",
	     !(info.st_mode & 0111) ? "" : e->kind == K_FOLDER ? ", entered" : e->kind == K_PROGRAM ? ", run" : "");
	if (e->kind == K_SHORTCUT)
		LINE("Starts", "%s", e->run);
	else if (info.st_nlink > 1 && e->kind != K_FOLDER)
		LINE("Names", "%d: the same file under other names too", (int)info.st_nlink);
#undef LINE
	/* NXFILES_PRINT=1: the lines on the console too, for a test without a screen */
	for (i = 0; i < n && getenv("NXFILES_PRINT"); i++)
		printf("nxfiles: %s: %s\n", lines[i][0], lines[i][1]);
	fflush(stdout);
	snprintf(title, sizeof title, "%.90s - Properties", e->label);
	window = GrNewWindowEx(UI_FIXED, title, GR_ROOT_WINDOW_ID, -1, -1, wide, high, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(window);
	GrSetFocus(window);
	for (;;) {
		GrGetNextEvent(&event);
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			ui_fill(window, 0, 0, wide, high, UI_FACE);
			draw_icon(window, 14, 12, e, 32);
			ui_text_fit(window, 58, 14, wide - 70, e->label, BLACK);
			ui_text_fit(window, 59, 14, wide - 70, e->label, BLACK);	/* twice: bold */
			ui_text_fit(window, 58, 30, wide - 70, e->name, UI_SHADOW);
			ui_fill(window, 12, 54, wide - 24, 1, UI_SHADOW);
			ui_fill(window, 12, 55, wide - 24, 1, WHITE);
			for (i = 0; i < n; i++) {
				ui_text(window, 14, 66 + i * 20, lines[i][0], -1, UI_SHADOW, 0);
				ui_text_fit(window, 84, 66 + i * 20, wide - 96, lines[i][1], BLACK);
			}
			ui_button(window, wide - 84, high - 34, 72, 24, "   OK", 0);
			ui_focus(window, wide - 80, high - 30, 64, 16);
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (!ui_inside(event.button.x, event.button.y, wide - 84, high - 34, 72, 24))
				break;
			/* fall through */
		case GR_EVENT_TYPE_KEY_DOWN:
		case GR_EVENT_TYPE_CLOSE_REQ:
			GrClose();
			return 0;
		}
	}
}

/* The bytes of a file, or of a folder with all it holds, as a copy that follows links has them. */
static long
bytes_in(const char *path, int depth)
{
	struct stat info;
	struct dirent *item;
	char inner[700];
	long total = 0;
	DIR *folder;

	if (stat(path, &info) != 0)
		return 0;
	if (!S_ISDIR(info.st_mode))
		return S_ISREG(info.st_mode) ? (long)info.st_size : 0;
	if (depth > 16 || (folder = opendir(path)) == NULL)
		return 0;
	while ((item = readdir(folder)) != NULL)
		if (strcmp(item->d_name, ".") && strcmp(item->d_name, "..")) {
			snprintf(inner, sizeof inner, "%s/%s", path, item->d_name);
			total += bytes_in(inner, depth + 1);
		}
	closedir(folder);
	return total;
}

/* Bytes as kilobytes of a thousand, to a tenth (the host's "KB of 100"). */
static const char *
kilobytes(long bytes, char *text, int size)
{
	snprintf(text, size, "%ld.%ld KB", bytes / 1000, bytes % 1000 / 100);
	return text;
}

/* Whether the folder shown is /persistence or in it. */
static int
in_persistence(void)
{
	return !strncmp(where, PERSIST, sizeof PERSIST - 1) && (where[sizeof PERSIST - 1] == 0 || where[sizeof PERSIST - 1] == '/');
}

/* Save to persistence: the selected entries copied into /persistence, unless the folder would
 * then hold more than the host keeps; then it is asked whether to send it to the host now. */
static void
persist_chosen(void)
{
	static const char *const replace[] = { "Replace", "Cancel" }, *const send[] = { "Save now", "Later" };
	char path[600], there[700], text[480], a[24], b[24], c[24], name[64] = "";
	long have, adding = 0, replaced = 0, after;
	int i, n = 0, there_already = 0, failed = 0;

	for (i = 0; i < count; i++)
		if (entries[i].chosen) {
			entry_path(i, path, sizeof path);
			snprintf(there, sizeof there, PERSIST "/%s", entries[i].name);
			adding += bytes_in(path, 0);
			if (access(there, F_OK) == 0)
				replaced += bytes_in(there, 0), there_already++;
			if (!n++)
				snprintf(name, sizeof name, "%.60s", entries[i].name);
		}
	if (!n) {
		strcpy(notice, "nothing is selected");
		return;
	}
	if (n > 1)
		snprintf(name, sizeof name, "These %d entries", n);
	have = bytes_in(PERSIST, 0);
	after = have - replaced + adding;
	if (after > PERSIST_MOST) {
		report("Not saved to persistence", "%s: %s, and /persistence holds %s already.\nTogether %s: more than the %ld KB the host keeps.\n"
		       "Nothing was copied. Delete something in /persistence first.", name, kilobytes(adding, a, sizeof a),
		       kilobytes(have - replaced, b, sizeof b), kilobytes(after, c, sizeof c), PERSIST_MOST / 1000);
		return;
	}
	if (there_already) {
		if (n == 1)
			snprintf(text, sizeof text, "/persistence has a %s already.\nReplace it?", name);
		else
			snprintf(text, sizeof text, "/persistence has %d of these already.\nReplace them?", there_already);
		if (ui_ask("Save to persistence", text, replace, 2, others) != 0)
			return;
	}
	mkdir(PERSIST, 0755);
	for (i = 0; i < count; i++)
		if (entries[i].chosen) {
			entry_path(i, path, sizeof path);
			snprintf(there, sizeof there, PERSIST "/%s", entries[i].name);
			if ((access(there, F_OK) == 0 && !run_wait("rm", "-rf", there, NULL)) || !run_wait("cp", "-RL", path, there)) {
				if (!failed++)
					report("Not saved to persistence", "%.60s could not be copied into /persistence:\n%.120s.", entries[i].name, run_said);
			}
		}
	if (failed)
		return;
	snprintf(text, sizeof text, "%s %s in /persistence now: it holds %s of the %ld KB the host keeps.\n"
		 "The host keeps the folder when it is saved. Save it now?", name, n > 1 ? "are" : "is", kilobytes(bytes_in(PERSIST, 0), a, sizeof a),
		 PERSIST_MOST / 1000);
	if (getenv("NXFILES_PRINT")) {
		printf("nxfiles: saved to persistence: %s\n", name);
		fflush(stdout);
	}
	ui_ask_icon = "information";
	i = ui_ask("Saved to persistence", text, send, 2, others);
	ui_ask_icon = NULL;
	if (i == 0)
		ui_run("nxpersist", "save");
}

/* The desktop's icons in rows again, columns from the left, in name order. */
static void
arrange(void)
{
	int i;

	for (i = 0; i < count; i++)
		entries[i].placed = 0;
	for (i = 0; i < count; i++)
		place_near(i, 0, 0);
	save_places();
}

/* How a window shows a folder, for the next window and (once saved) the next session. */
static void
keep_view(void)
{
	reg_set_int("files.details", details);
	reg_set_int("files.order", sort_by);
}

static void
set_view(int list)
{
	details = list;
	keep_view();
	layout();
	top = 0;
	if (cursor >= 0)
		top = (details ? cursor : cursor / columns) - rows + 1 > 0 ? (details ? cursor : cursor / columns) - rows + 1 : 0;
}

static void
choose_all(void)
{
	int i;

	for (i = 0; i < count; i++)
		entries[i].chosen = 1;
}

/* A terminal whose shell starts in `folder` (NULL: the one shown, which is where this program is). */
static void
terminal_in(const char *folder)
{
	pid_t child;

	ui_exec_errno = 0;
	if ((child = real_vfork()) != 0) {
		/* (the child said why it did not become the terminal: ui_exec.h) */
		if (child < 0 || ui_exec_errno)
			report("Could not start", "A terminal could not be started in %.100s:\n%s.", folder ? folder : where,
			       strerror(child < 0 ? errno : ui_exec_errno));
		return;
	}
	signal(SIGCHLD, SIG_DFL);	/* (as ui_run: the child may do nothing here that writes our memory) */
	if (folder && chdir(folder) != 0)
		ui_exec_failed();
	execlp("nxterm", "nxterm", (char *)NULL);
	ui_exec_failed();
}

/* What a menu's row (or a key, or a button that does the same) does. */
static void
act(int what)
{
	notice[0] = 0;
	switch (what) {
	case M_OPEN: open_entry(cursor, 0); return;
	case M_EDIT: open_entry(cursor, 1); return;
	case M_CUT: carry("cut"); break;
	case M_COPY: carry("copy"); break;
	case M_PASTE: paste(NULL); break;
	case M_PASTE_IN: if (cursor >= 0) paste(entries[cursor].name); break;
	case M_RENAME: rename_begin(); break;
	case M_DELETE: delete_chosen(); break;
	case M_SHORTCUT: if (cursor >= 0) shortcut_of(cursor); break;
	case M_PROPERTIES: properties(cursor >= 0 && entries[cursor].chosen ? cursor : -1); break;
	case M_TILES: set_view(0); break;
	case M_DETAILS: set_view(1); break;
	case M_REFRESH: read_folder(); break;
	case M_FOLDER: new_entry(1); break;
	case M_FILE: new_entry(0); break;
	case M_ARRANGE: arrange(); break;
	case M_SETTINGS: ui_run("nxsettings", NULL); return;
	case M_FILES: ui_run("nxfiles", where); return;
	case M_ALL: choose_all(); break;
	case M_TERMINAL: terminal_in(NULL); return;
	case M_TERMINAL_IN: if (cursor >= 0) terminal_in(entries[cursor].name); return;
	case M_PERSIST: persist_chosen(); break;
	}
	draw_all();
}

/* The menu for a place of the window (of the screen, on the desktop): an entry's when one is
 * there, the folder's otherwise. x, y is where on the screen it opens. */
static void
context_menu(int on, int x, int y, int keyboard)
{
	static struct ui_menu_item menu[12], made[2], view[2];
	int n = 0, carried = access(CARRIED, F_OK) == 0;
#define ROW(text, what, mark) (menu[n].label = (text), menu[n].id = (what), menu[n].icon = -1, menu[n].sub = NULL, menu[n].flags = (mark), n++)

	made[0].label = "Folder", made[0].id = M_FOLDER, made[0].icon = ui_icon_of("folder");
	made[1].label = "Text file", made[1].id = M_FILE, made[1].icon = ui_icon_of("file");
	view[0].label = "Tiles", view[0].id = M_TILES, view[0].icon = -1, view[0].flags = details ? 0 : UI_MENU_GREY;
	view[1].label = "Details", view[1].id = M_DETAILS, view[1].icon = -1, view[1].flags = details ? UI_MENU_GREY : 0;
	if (on >= 0) {
		if (!entries[on].chosen)
			choose_only(on);
		cursor = on;
		refresh();
		ROW("Open", M_OPEN, 0);
		if (kind_of(on) != K_FOLDER && entries[on].kind != K_SHORTCUT)
			ROW("Edit", M_EDIT, 0);
		ROW("Cut", M_CUT, UI_MENU_LINE);
		ROW("Copy", M_COPY, 0);
		if (entries[on].kind == K_FOLDER)
			ROW("Paste into it", M_PASTE_IN, carried ? 0 : UI_MENU_GREY);
		if (entries[on].kind == K_FOLDER)
			ROW("Terminal in it", M_TERMINAL_IN, 0), menu[n - 1].icon = ui_icon_of("nxterm");
		ROW("Rename", M_RENAME, UI_MENU_LINE);
		ROW("Delete", M_DELETE, 0);
		if (!in_persistence())
			ROW("Save to persistence", M_PERSIST, 0), menu[n - 1].icon = ui_icon_of("persistence");
		if (!desktop)
			ROW("Shortcut on the desktop", M_SHORTCUT, UI_MENU_LINE);
		ROW("Properties", M_PROPERTIES, UI_MENU_LINE);
	} else {
		if (!desktop) {
			ROW("View", 0, 0);
			menu[n - 1].sub = view, menu[n - 1].sub_count = 2;
		} else
			ROW("Arrange icons", M_ARRANGE, 0);
		ROW("Refresh", M_REFRESH, 0);
		ROW("Select all", M_ALL, 0);
		ROW("Paste", M_PASTE, UI_MENU_LINE | (carried ? 0 : UI_MENU_GREY));
		ROW("New", 0, UI_MENU_LINE);
		menu[n - 1].sub = made, menu[n - 1].sub_count = 2;
		ROW("Terminal here", M_TERMINAL, 0);
		menu[n - 1].icon = ui_icon_of("nxterm");
		if (desktop) {
			ROW("Open in Files", M_FILES, UI_MENU_LINE);
			ROW("Settings", M_SETTINGS, 0);
			menu[n - 1].icon = ui_icon_of("nxsettings");
		} else
			ROW("Properties", M_PROPERTIES, UI_MENU_LINE);
	}
#undef ROW
	ui_menu_open(menu, n, x, y, 0, keyboard);
}

/* The folder a drop at a place of the screen goes to: a Files window's, or the desktop's
 * when no window is there. False for a window that takes no files. */
static int
drop_target(int x, int y, char *path, int size)
{
	GR_WINDOW_ID parent, *children = NULL, *inside;
	GR_COUNT n = 0, m, i;
	GR_WINDOW_INFO info;
	GR_WM_PROPERTIES props;
	int found = 0, any = 0;

	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &children, &n);
	for (i = 0; i < n && !any; i++) {	/* the one in front first */
		GrGetWindowInfo(children[i], &info);
		if (!info.mapped || !ui_inside(x, y, info.x, info.y, info.width, info.height))
			continue;
		any = 1;
		inside = NULL, m = 0;
		GrQueryTree(children[i], &parent, &inside, &m);
		props.title = NULL;
		if (m > 0)
			GrGetWMProperties(inside[0], &props);
		free(inside);
		if (props.title && !strncmp((char *)props.title, "Files - ", 8)) {
			snprintf(path, size, "%s", (char *)props.title + 8);
			found = 1;
		}
		free(props.title);
	}
	free(children);
	if (!any) {
		snprintf(path, size, "%s", DESKTOP);
		found = 1;
	}
	return found;
}

/* The selected entries were dragged and let go at a place of the screen. */
static void
drop(int x, int y, int wx, int wy, int copy)
{
	char target[512], from[600], first[104] = "", why[160] = "";
	int i, on, moved = 0, failed = 0, dx = x - press_x, dy = y - press_y;
	FILE *file;

	if (!drop_target(x, y, target, sizeof target))
		return;
	if (!strcmp(target, where)) {
		/* within this folder: onto one of its folders, or (the desktop) to another place */
		on = hit(desktop ? x : wx, desktop ? y : wy);
		if (on >= 0 && kind_of(on) == K_FOLDER && !entries[on].chosen)
			snprintf(target, sizeof target, "%s/%s", strcmp(where, "/") ? where : "", entries[on].name);
		else if (desktop) {
			for (i = 0; i < count; i++)
				if (entries[i].chosen)
					entries[i].placed = 0;
			for (i = 0; i < count; i++)
				if (entries[i].chosen)
					place_near(i, entries[i].x + dx, entries[i].y + dy);
			save_places();
			draw_all();
			return;
		} else
			return;
	} else if (!strcmp(target, DESKTOP) && (file = fopen(DROPPED, "w")) != NULL) {
		fprintf(file, "%d %d\n", x, y);
		fclose(file);
	}
	for (i = 0; i < count; i++)
		if (entries[i].chosen) {
			char there[700];
			int fine;

			entry_path(i, from, sizeof from);
			/* one of that name there already stays as it is: nothing is replaced unasked */
			snprintf(there, sizeof there, "%s/%s", target, entries[i].name);
			if (access(there, F_OK) == 0)
				fine = 0, snprintf(run_said, sizeof run_said, "one of that name is there already, and stays");
			else
				fine = copy ? run_wait("cp", "-a", from, target) : move_path(from, there);
			if (fine)
				moved++;
			else if (!failed++)
				snprintf(first, sizeof first, "%.100s", entries[i].name), snprintf(why, sizeof why, "%s", run_said);
		}
	snprintf(notice, sizeof notice, copy ? "%d copied to %.100s" : "%d moved to %.100s", moved, target);
	read_folder();
	if (failed == 1)
		report(copy ? "Cannot copy" : "Cannot move", "%s could not be %s to %.80s:\n%.120s.", first, copy ? "copied" : "moved", target, why);
	else if (failed)
		report(copy ? "Cannot copy" : "Cannot move", "%d entries could not be %s to %.80s.\nThe first, %s:\n%.120s.", failed,
		       copy ? "copied" : "moved", target, first, why);
	draw_all();
}

static void
press(int button)
{
	char path[512];

	notice[0] = 0;
	switch (button) {
	case B_BACK:
		if (history_at > 0)
			go(history[--history_at], 0);
		break;
	case B_FORWARD:
		if (history_at + 1 < history_count)
			go(history[++history_at], 0);
		break;
	case B_UP:
		snprintf(path, sizeof path, "%s", strrchr(where, '/') ? strrchr(where, '/') + 1 : "");
		if (go("..", 1))
			select_name(path);	/* the folder left is the one picked out */
		break;
	case B_HOME:
		go("/root", 1);
		break;
	case B_VIEW:
		set_view(!details);
		break;
	case B_NEW:
		act(M_FOLDER);
		return;
	case B_TERMINAL:
		act(M_TERMINAL);
		return;
	}
	draw_all();
}

/* Begins the typing of a path in its field: the folder's own to start with. */
static void
path_begin(void)
{
	snprintf(typed, sizeof typed, "%s%s", where, where[1] ? "/" : "");
	renaming = 2;
	draw_path();
	draw_status();
}

/* A key of the name being typed over an entry's: the caret and the selection move as in any
 * field of text (Shift extends it, Ctrl+A takes it all), a letter replaces what is selected. */
static void
edit_key(int ch, int modifiers)
{
	int n = strlen(typed), lo = edit_at < edit_from ? edit_at : edit_from, hi = edit_at + edit_from - lo;
	int shift = (modifiers & MWKMOD_SHIFT) != 0, to = -1;

	if (ch == MWKEY_ESCAPE || ch == MWKEY_ENTER || ch == '\n') {
		if (rename_end(ch != MWKEY_ESCAPE))
			draw_all();
		return;
	}
	if (((modifiers & MWKMOD_CTRL) && (ch == 'a' || ch == 'A')) || ch == 1) {
		edit_from = 0, edit_at = n;
	} else if (ch == MWKEY_LEFT || ch == MWKEY_RIGHT || ch == MWKEY_HOME || ch == MWKEY_END) {
		if (ch == MWKEY_LEFT)
			to = !shift && lo != hi ? lo : edit_at > 0 ? edit_at - 1 : 0;
		else if (ch == MWKEY_RIGHT)
			to = !shift && lo != hi ? hi : edit_at < n ? edit_at + 1 : n;
		else
			to = ch == MWKEY_HOME ? 0 : n;
		edit_at = to;
		if (!shift)
			edit_from = to;
	} else if (ch == MWKEY_BACKSPACE || ch == MWKEY_DELETE || ch == 127 || (ch >= ' ' && ch < 127 && ch != '/' && !(modifiers & MWKMOD_CTRL))) {
		if (lo == hi && (ch == MWKEY_BACKSPACE || ch == 127) && lo > 0)
			lo--;
		else if (lo == hi && ch == MWKEY_DELETE && hi < n)
			hi++;
		memmove(typed + lo, typed + hi, (size_t)(n - hi + 1));
		n -= hi - lo;
		if (ch >= ' ' && ch < 127 && n < (int)sizeof typed - 1) {
			memmove(typed + lo + 1, typed + lo, (size_t)(n - lo + 1));
			typed[lo++] = (char)ch;
		}
		edit_at = edit_from = lo;
	} else {
		return;
	}
	edit_draw();
}

/* A key of the path typed in its field. */
static void
rename_key(int ch, int modifiers)
{
	int n = strlen(typed);

	if (renaming == 1) {
		edit_key(ch, modifiers);
		return;
	}
	if (ch == MWKEY_ESCAPE) {
		renaming = 0;
		draw_all();
		return;
	} else if (ch == MWKEY_ENTER || ch == '\n') {
		renaming = 0;
		if (n)
			go(typed, 1);	/* (a path that is no folder is said to be none) */
		draw_all();
		return;
	} else if (ch == MWKEY_BACKSPACE || ch == 127) {
		if (n)
			typed[n - 1] = 0;
	} else if (ch > 32 && ch < 127 && n < (int)sizeof typed - 1) {
		typed[n] = ch, typed[n + 1] = 0;
	} else if (ch == ' ' && n && n < (int)sizeof typed - 1) {
		typed[n] = ' ', typed[n + 1] = 0;
	}
	draw_path();
}

/* The icon of the desktop nearest the cursor's in a direction (dx, dy one of -1, 0, 1). */
static int
neighbour(int dx, int dy)
{
	int i, best = -1, score = 0, s, ex, ey;

	if (cursor < 0)
		return count ? 0 : -1;
	for (i = 0; i < count; i++) {
		ex = entries[i].x - entries[cursor].x;
		ey = entries[i].y - entries[cursor].y;
		if (i == cursor || ex * dx + ey * dy <= 0)
			continue;
		/* straight on counts once, sideways three times */
		s = abs(ex) * (dx ? 1 : 3) + abs(ey) * (dy ? 1 : 3);
		if (best < 0 || s < score)
			best = i, score = s;
	}
	return best < 0 ? cursor : best;
}

/* Letters typed one after another choose the entry whose name starts with them. */
static void
type_ahead(int ch)
{
	unsigned now = milliseconds();
	int n = strlen(ahead), i, from;

	if (now - ahead_time > 900)
		n = 0;
	ahead_time = now;
	if (n < (int)sizeof ahead - 1)
		ahead[n++] = ch, ahead[n] = 0;
	/* one letter again and again goes on to the next that starts with it */
	from = n == 1 ? cursor + 1 : cursor < 0 ? 0 : cursor;
	for (i = 0; i < count; i++) {
		kind_of((from + i + count) % count);
		if (!strncasecmp(entries[(from + i + count) % count].label, ahead, n)) {
			move_to((from + i + count) % count, 0);
			return;
		}
	}
	if (n > 1) {	/* nothing starts so: the letter begins anew */
		ahead[0] = 0;
		ahead_time = 0;
		type_ahead(ch);
	}
}

static void
key(const GR_EVENT *event)
{
	int ch = event->keystroke.ch, command = 0, x, y, w, h;
	int shift = (event->keystroke.modifiers & MWKMOD_SHIFT) != 0, alt = (event->keystroke.modifiers & MWKMOD_ALT) != 0;
	int row = details ? 1 : columns;

	if (renaming) {
		rename_key(ch, event->keystroke.modifiers);
		return;
	}
	/* a letter with Ctrl comes as the letter with the modifier, or as its control code */
	if ((event->keystroke.modifiers & MWKMOD_CTRL) && ch < 128 && isalpha(ch))
		command = tolower(ch);
	else if (ch == 1 || ch == 3 || ch == 12 || ch == 22 || ch == 24)
		command = 'a' + ch - 1;
	if (command == 'l' && !desktop) {
		path_begin();
		return;
	}
	if (command) {
		if (command == 'c' || command == 'x' || command == 'v' || command == 'a')
			act(command == 'c' ? M_COPY : command == 'x' ? M_CUT : command == 'v' ? M_PASTE : M_ALL);
		return;
	}
	if (alt && !desktop && (ch == MWKEY_UP || ch == MWKEY_LEFT || ch == MWKEY_RIGHT)) {
		press(ch == MWKEY_UP ? B_UP : ch == MWKEY_LEFT ? B_BACK : B_FORWARD);
		return;
	}
#ifdef MWKEY_MENU
	if (ch == MWKEY_MENU)
		ch = MWKEY_F10, shift = 1;
#endif
	if (ch == MWKEY_F10 && shift) {
		/* the menu, at the entry the keys are on */
		struct { int x, y; } info = { 0, 0 };

		if (!desktop)
			ui_window_origin(window, &info.x, &info.y);
		if (cursor >= 0 && entries[cursor].chosen && tile_at(cursor, &x, &y, &w, &h))
			context_menu(cursor, info.x + x + w / 2, info.y + y + h / 2, 1);
		else
			context_menu(-1, info.x + 40, info.y + (desktop ? 40 : BAR + 20), 1);
		return;
	}
	if (ch < 256 && ch > ' ' && ch != 127) {
		type_ahead(ch);
		return;
	}
	if (notice[0]) {
		notice[0] = 0;
		if (desktop)
			draw_all();
	}
	switch (ch) {
	case MWKEY_LEFT: move_to(desktop ? neighbour(-1, 0) : details ? cursor : cursor - 1, shift); break;
	case MWKEY_RIGHT: move_to(desktop ? neighbour(1, 0) : details ? cursor : cursor + 1, shift); break;
	case MWKEY_UP: move_to(desktop ? neighbour(0, -1) : cursor < 0 ? 0 : cursor >= row ? cursor - row : cursor, shift); break;
	case MWKEY_DOWN: move_to(desktop ? neighbour(0, 1) : cursor < 0 ? 0 : cursor + row < count ? cursor + row : cursor, shift); break;
	case MWKEY_PAGEUP: move_to(cursor - rows * row, shift); break;
	case MWKEY_PAGEDOWN: move_to(cursor + rows * row, shift); break;
	case MWKEY_HOME: move_to(0, shift); break;
	case MWKEY_END: move_to(count - 1, shift); break;
	case MWKEY_ENTER: open_entry(cursor, 0); break;
	case ' ':
		if (cursor >= 0) {
			entries[cursor].chosen = !entries[cursor].chosen;
			refresh();
		}
		break;
	case MWKEY_BACKSPACE: if (!desktop) press(B_UP); break;
	case MWKEY_DELETE: act(M_DELETE); break;
	case MWKEY_F2: act(M_RENAME); break;
	case MWKEY_F5: act(M_REFRESH); break;
	case MWKEY_ESCAPE:
		choose_only(-1);
		refresh();
		break;
	}
}

/* The left button went down on an entry (or on none: -1). */
static void
click(int i, const GR_EVENT *event)
{
	unsigned now = event->button.time;	/* when it was pressed, not when this got to it */
	int k, modifiers = event->button.modifiers;

	press_tile = i;
	press_narrow = 0;
	press_x = event->button.rootx;
	press_y = event->button.rooty;
	press_time = now;
	if (i < 0) {
		/* an empty place: nothing is selected any more (Ctrl: what is, stays), and a box begins */
		for (k = 0; k < count; k++)
			band_before[k] = (modifiers & MWKMOD_CTRL) ? entries[k].chosen : 0;
		if (!(modifiers & MWKMOD_CTRL) && (chosen_count() || cursor >= 0)) {
			choose_only(-1);
			refresh();
		}
		click_tile = -1;
		band = 1;
		band_x0 = band_x1 = event->button.x;
		band_y0 = band_y1 = event->button.y;
		if (desktop)
			GrSelectEvents(GR_ROOT_WINDOW_ID, DESK_EVENTS | GR_EVENT_MASK_MOUSE_MOTION);
		else
			ui_moves(window, EVENTS, 1);
		return;
	}
	if (i == click_tile && now - click_time < 700 && !(modifiers & (MWKMOD_CTRL | MWKMOD_SHIFT))) {
		click_tile = -1;
		press_tile = -1;
		choose_only(i);
		refresh();
		open_entry(i, 0);
		return;
	}
	click_tile = i;
	click_time = now;
	if (modifiers & MWKMOD_CTRL) {
		entries[i].chosen = !entries[i].chosen;
		cursor = anchor = i;
	} else if ((modifiers & MWKMOD_SHIFT) && anchor >= 0) {
		for (k = 0; k < count; k++)
			entries[k].chosen = (k >= anchor && k <= i) || (k >= i && k <= anchor);
		cursor = i;
	} else if (!entries[i].chosen)
		choose_only(i);
	else {
		cursor = i;	/* (the others stay selected until the button is let go: they may be dragged) */
		press_narrow = chosen_count() > 1;
	}
	refresh();
}

/* Once a second: the folder shown again if something changed it from outside. */
static void
watch(void)
{
	struct stat info;

	if (renaming || band || ui_menu_is_open())
		return;
	if (stat(".", &info) == 0 && (info.st_mtim.tv_sec != folder_time.tv_sec || info.st_mtim.tv_nsec != folder_time.tv_nsec)) {
		read_folder();
		draw_all();
	} else if (desktop) {
		/* Settings chose another picture and painted it over the icons */
		static char chosen[sizeof wall];

		char file[sizeof wall], now[sizeof chosen];
		int changed = reg_changed();

		if (changed)	/* (the viewer's turn too: a picture turned again is the same file) */
			snprintf(now, sizeof now, "%s|%.20s", reg_get(UI_WALLPAPER_CHOICE, UI_WALLPAPER_DEFAULT), reg_get(UI_WALLPAPER_TURN, ""));
		if (changed && strcmp(chosen, now)) {
			snprintf(chosen, sizeof chosen, "%s", now);
			draw_all();
		} else if (wall_waiting && ui_wallpaper_file(wall, file, sizeof file)) {
			read_wall();	/* the decoded copy is there now */
			draw_all();
		}
	}
}

/* Starts what a shortcut file names. */
static int
run_shortcut(const char *path)
{
	static struct entry e;
	char *slash;

	snprintf(e.name, sizeof e.name, "%s", path);
	if ((slash = strrchr(path, '/')) != NULL) {
		char folder[512];

		snprintf(folder, sizeof folder, "%.*s", (int)(slash - path), path);
		if (chdir(folder[0] ? folder : "/") != 0)
			return 1;
		snprintf(e.name, sizeof e.name, "%s", slash + 1);
	}
	read_shortcut(&e);
	if (!e.run[0])
		return 1;
	signal(SIGCHLD, SIG_IGN);
	chdir("/root");
	run_command(e.run);
	return 0;
}

/* --kinds FOLDER: each entry's kind and icon, with no window (a test's) */
static int
print_kinds(const char *folder)
{
	int i;

	if (chdir(folder) != 0)
		return 1;
	read_folder();
	for (i = 0; i < count; i++) {
		kind_icon = NULL;
		kind_of(i);
		printf("nxfiles: %s: %s, icon %s\n", entries[i].name, kind_names[entries[i].kind],
		       entries[i].kind == K_SHORTCUT ? "its own" : kind_icon ? kind_icon : "?");
	}
	return 0;
}

int
main(int argc, char **argv)
{
	GR_EVENT event;
	time_t looked = 0;
	int id;

	if (argc > 2 && !strcmp(argv[1], "--run"))
		return run_shortcut(argv[2]);
	if (argc > 2 && !strcmp(argv[1], "--properties"))
		return properties_window(argv[2]);
	if (argc > 2 && !strcmp(argv[1], "--kinds"))
		return print_kinds(argv[2]);
	if (GrOpen() < 0)
		return 1;
	ui_launch_watch();	/* (children reaped as they end: a program that failed is reported) */
	ui_init();
	GrGetScreenInfo(&screen);
	desktop = argc > 1 && !strcmp(argv[1], "--desktop");
	if (desktop) {
		mkdir(DESKTOP, 0755);
		if (chdir(DESKTOP) != 0)
			return 1;
		window = GR_ROOT_WINDOW_ID;
		read_folder();
		GrSelectEvents(GR_ROOT_WINDOW_ID, DESK_EVENTS);
		draw_all();
	} else {
		if (chdir(argc > 1 ? argv[1] : "/root") != 0) {
			report("Cannot open the folder", "%.100s could not be opened:\n%s. Files shows / instead.", argc > 1 ? argv[1] : "/root",
			       strerror(errno));
			chdir("/");
		}
		details = reg_int("files.details", 0) != 0;
		sort_by = reg_int("files.order", 0);
		sort_by = sort_by < 0 || sort_by > 2 ? 0 : sort_by;
		side = SIDE;
		layout();
		window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Files", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
		read_folder();
		snprintf(history[0], sizeof history[0], "%s", where);
		history_count = 1;
		GrSelectEvents(window, EVENTS);
		GrMapWindow(window);
		GrSetFocus(window);
	}
	for (;;) {
		int i, area;

		GrGetNextEventTimeout(&event, ui_launch_wait(1000));
		ui_origin_note(&event);
		if (time(NULL) != looked) {
			looked = time(NULL);
			watch();
			if (ui_restyle()) {	/* (Settings changed the look: a word of memory read) */
				ui_menu_restyle();
				read_folder();	/* (icons of the other look's sheets, names measured in its font) */
				/* the picture decoded again: a copy from the kept one does not cover the old names */
				if (ui_decoded.pixmap)
					GrDestroyWindow(ui_decoded.pixmap);
				ui_decoded.pixmap = 0;
				draw_all();
			}
		}
		ui_launch_tick();
		if (ui_launch_event(&event))
			continue;
		switch (ui_menu_event(&event, &id)) {
		case UI_MENU_CHOSEN:
			act(id);
			/* fall through */
		case UI_MENU_CLOSED:
			if (!desktop)
				GrSetFocus(window);
			/* fall through */
		case UI_MENU_TOOK:
			continue;
		}
		area = height - BAR - STATUS;
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			if (event.exposure.wid == window && !desktop)
				draw_all();
			break;
		case GR_EVENT_TYPE_UPDATE:
			if (event.update.utype == GR_UPDATE_SIZE && event.update.wid == window) {
				width = event.update.width;
				height = event.update.height;
				layout();
				scroll_to(top);
				draw_all();
			}
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			if (event.button.wid != window)
				break;
			/* (the root's client is told of a press in any window it has no other of: the bar's, a program's) */
			if (desktop && event.button.subwid != GR_ROOT_WINDOW_ID)
				break;
			if (renaming == 1) {
				int bx, by, bw, bh;

				edit_box(&bx, &by, &bw, &bh);
				if (ui_inside(event.button.x, event.button.y, bx, by, bw, bh) && (event.button.changebuttons & GR_BUTTON_L)) {
					edit_at = edit_letter(event.button.x);
					if (!(event.button.modifiers & MWKMOD_SHIFT))
						edit_from = edit_at;
					edit_draw();
					break;
				}
				/* a click elsewhere gives the name typed, as Windows does */
				if (!rename_end(1))
					break;
				draw_all();
			} else if (renaming) {
				renaming = 0;	/* a click elsewhere leaves the path as it was */
				draw_all();
			}
			if (notice[0]) {
				notice[0] = 0;
				draw_all();
			}
			if (desktop) {
				GrSetFocus(GR_ROOT_WINDOW_ID);
				i = hit(event.button.x, event.button.y);
				if (event.button.changebuttons & GR_BUTTON_R)
					context_menu(i, event.button.rootx, event.button.rooty, 0);
				else if (event.button.changebuttons & GR_BUTTON_L)
					click(i, &event);
				break;
			}
			if (ui_wheel(&event)) {
				scroll_to(top + ui_wheel_sum(&event));
			} else if (event.button.y < BAR) {
				for (i = 0; i < BUTTONS; i++)
					if (ui_inside(event.button.x, event.button.y, 3 + i * (BUTTON_W + 3), 3, BUTTON_W, BAR - 6))
						press(i);
				if (event.button.x >= 3 + BUTTONS * (BUTTON_W + 3))
					path_begin();	/* the path's field: another is typed there */
			} else if (event.button.y >= height - STATUS) {
				break;
			} else if (event.button.x < side) {
				/* a place: its folder (or, the last one, the program for the other machines) */
				i = (event.button.y - BAR - 4) / PLACE_H;
				if (i >= 0 && i < PLACES_N - 1)
					go(places[i].path, 1);
				else if (event.button.y >= BAR + 12 + (PLACES_N - 1) * PLACE_H && event.button.y < BAR + 12 + PLACES_N * PLACE_H)
					ui_run("nxnet", NULL);
				draw_all();
			} else if (event.button.x >= width - UI_SCROLL_W) {
				thumb_held = 1;
				ui_moves(window, EVENTS, 1);	/* (the pointer's moves, until the button is let go) */
				scroll_to(ui_scroll_to(event.button.y, BAR, area, total_rows(), rows));
			} else {
				i = hit(event.button.x, event.button.y);
				if (details && event.button.y < BAR + 2 + ROW_H) {
					/* a column's heading: the list in its order */
					sort_by = event.button.x < width - UI_SCROLL_W - 152 ? 0 : event.button.x < width - UI_SCROLL_W - 82 ? 1 : 2;
					keep_view();
					read_folder();
					draw_all();
				} else if (event.button.changebuttons & GR_BUTTON_R)
					context_menu(i, event.button.rootx, event.button.rooty, 0);
				else if (event.button.changebuttons & GR_BUTTON_L)
					click(i, &event);
			}
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			thumb_held = 0;
			if (band)
				band_end();
			if (!desktop)
				ui_moves(window, EVENTS, 0);
			/* let go some way from where it went down, with entries selected: they were dragged */
			if (press_tile >= 0 && (event.button.changebuttons & GR_BUTTON_L) && chosen_count() &&
			    abs(event.button.rootx - press_x) + abs(event.button.rooty - press_y) > 6) {
				press_tile = -1;
				click_tile = -1;
				drop(event.button.rootx, event.button.rooty, event.button.x, event.button.y,
					(event.button.modifiers & MWKMOD_CTRL) != 0);
			} else if (press_tile >= 0 && press_narrow && (event.button.changebuttons & GR_BUTTON_L)) {
				choose_only(press_tile);
				refresh();
			}
			press_tile = -1;
			press_narrow = 0;
			break;
		case GR_EVENT_TYPE_MOUSE_MOTION:
			if (band && (event.mouse.buttons & GR_BUTTON_L)) {
				/* (the last of the moves that wait: each one is every touched entry drawn) */
				GR_EVENT next;

				while (GrPeekEvent(&next) && next.type == GR_EVENT_TYPE_MOUSE_MOTION)
					GrGetNextEvent(&event);
				band_move(event.mouse.x, event.mouse.y);
			} else if (band)
				band_end();
			else if (thumb_held && (event.mouse.buttons & GR_BUTTON_L))
				scroll_to(ui_scroll_to(event.mouse.y, BAR, area, total_rows(), rows));
			else {
				thumb_held = 0;
				ui_moves(window, EVENTS, 0);
			}
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key(&event);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			if (event.general.wid != window)
				break;	/* a menu's, come after the menu went */
			GrClose();
			return 0;
		}
	}
}
