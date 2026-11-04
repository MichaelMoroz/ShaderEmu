/*
 * cabinst: a game's installer, as its maker ships it, unpacked in the machine after its licence is accepted
 * (docs/system/install.md). The installer lies unchanged in the ROM; its licence is read out of it and shown
 * whole, and Accept can be pressed only once the text has been scrolled to its end. Then the files wanted
 * are unpacked on the worker cores straight into memory (tmpfs files mapped in place), with a bar of how far.
 *
 *	cabinst [--title TEXT] [--licence NAME] [--key KEY] INSTALLER DEST WANTED...
 *	cabinst [--title TEXT] [--licence NAME] [--key KEY] --installed DIR DEST
 *
 *	WANTED	NAME[=PATH]: a file of the cabinet ('/' or '\' between folders, any case), put at DEST/PATH (by
 *		default its own name); '*' is every file
 *	--licence NAME	the licence's file in the cabinet, shown first
 *	--key KEY	the registry's key that keeps the acceptance (the licence's CRC-32): not asked again while
 *			/persistence is kept. The files are unpacked every session all the same
 *	--installed DIR	the installer decoded already, in the ROM (the image's data.py, 4 November 2026): the licence is
 *			read from DIR/NAME and asked as from the installer, and then every file of DIR is linked into DEST
 *			(folders made), with nothing unpacked; DEST - links nothing (a game whose files lie in place
 *			already, Half-Life's: only its licence asked)
 *
 * Exit 0: the files are there (or were this session: DEST/.unpacked); 1: the licence was declined; 2: failed,
 * and a line says why. Without a window system the licence is printed and asked on the terminal.
 *	CABINST_PRINT=1		lines for a test: the licence's window and where its buttons are, each scroll, the
 *				answer, each round of the unpacking
 *	CABINST_SUMS=1		a CRC-32 of each file unpacked
 *	CABINST_WORKERS=N	the most worker cores (255; 0 none)   CABINST_BATCH=N  blocks a round (256)
 */
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include "ui.h"
#include "ui_ask.h"
#include "registry.h"
#include "cab.h"

extern const char etext[];
extern void _start(void);

static int print_on;
static const char *title = "Installer";

static double
now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void
say(const char *format, ...)
{
	va_list a;

	if (!print_on)
		return;
	va_start(a, format);
	printf("cabinst: ");
	vprintf(format, a);
	printf("\n");
	fflush(stdout);
	va_end(a);
}

/* ---------------------------------------------------------------- the licence */

#define LIC_W 600
#define LIC_H 460
#define LIC_TEXT_X 12
#define LIC_TEXT_Y 34
#define LIC_TEXT_W (LIC_W - 24)
#define LIC_TEXT_H 352
#define LIC_LINE 15
#define LIC_BUTTON_W 96
#define LIC_BUTTON_H 26
#define LIC_BUTTON_Y (LIC_H - LIC_BUTTON_H - 12)

static struct {
	char *text;
	const char **line;
	int *length, lines, most;
	int top, shown, seen_end, focus;	/* focus: 0 the text, 1 Accept, 2 Decline */
	GR_WINDOW_ID w;
	int accept_x, decline_x;
} lic;

/* The text in lines that fit the box: its own lines, each cut at spaces where it is too wide. */
static void
lic_wrap(int width)
{
	char *s = lic.text, *end = lic.text + strlen(lic.text);

	lic.lines = 0;
	while (s < end) {
		int n = (int)strcspn(s, "\n"), take = n;

		while (take > 0 && ui_width(&ui_var_widths, ui_var, s, take) > width) {
			int k = take - 1;

			while (k > 0 && s[k] != ' ')
				k--;
			take = k > 0 ? k : take - 1;
		}
		if (lic.lines == lic.most) {
			lic.most = lic.most ? lic.most * 2 : 256;
			lic.line = realloc(lic.line, lic.most * sizeof *lic.line);
			lic.length = realloc(lic.length, lic.most * sizeof *lic.length);
		}
		lic.line[lic.lines] = s, lic.length[lic.lines++] = take;
		s += take;
		if (take == n)
			s += *s == '\n';
		else
			while (*s == ' ')
				s++;
	}
}

static void
lic_draw_text(void)
{
	int i, rows = lic.shown, bar = lic.lines > rows;

	ui_fill(lic.w, LIC_TEXT_X, LIC_TEXT_Y, LIC_TEXT_W, LIC_TEXT_H, WHITE);
	ui_bevel(lic.w, LIC_TEXT_X, LIC_TEXT_Y, LIC_TEXT_W, LIC_TEXT_H, 1);
	for (i = 0; i < rows && lic.top + i < lic.lines; i++)
		ui_text(lic.w, LIC_TEXT_X + 6, LIC_TEXT_Y + 5 + i * LIC_LINE, lic.line[lic.top + i], lic.length[lic.top + i], BLACK, 0);
	if (bar)
		ui_scrollbar(lic.w, LIC_TEXT_X + LIC_TEXT_W - UI_SCROLL_W - 2, LIC_TEXT_Y + 2, LIC_TEXT_H - 4, lic.lines, rows, lic.top);
	if (lic.focus == 0)
		ui_focus(lic.w, LIC_TEXT_X + 3, LIC_TEXT_Y + 3, LIC_TEXT_W - 6 - (bar ? UI_SCROLL_W : 0), LIC_TEXT_H - 6);
}

static void
lic_draw_buttons(void)
{
	static const char *const names[] = { "Accept", "Decline" };
	int i;

	ui_fill(lic.w, 0, LIC_BUTTON_Y - 30, LIC_W, LIC_H - LIC_BUTTON_Y + 30, UI_FACE);
	ui_text(lic.w, LIC_TEXT_X, LIC_BUTTON_Y - 22,
		lic.seen_end ? "You have read the licence to its end." : "Read the licence to its end (scroll down) to accept it.", -1,
		BLACK, 0);
	for (i = 0; i < 2; i++) {
		int x = i ? lic.decline_x : lic.accept_x, tw = ui_width(&ui_var_widths, ui_var, names[i], -1);
		int off = i == 0 && !lic.seen_end;

		ui_button(lic.w, x, LIC_BUTTON_Y, LIC_BUTTON_W, LIC_BUTTON_H, "", 0);
		if (off)	/* (greyed: a raised copy under it, as a disabled button) */
			ui_text(lic.w, x + (LIC_BUTTON_W - tw) / 2 + 1, LIC_BUTTON_Y + 8, names[i], -1, WHITE, 0);
		ui_text(lic.w, x + (LIC_BUTTON_W - tw) / 2, LIC_BUTTON_Y + 7, names[i], -1, off ? UI_SHADOW : BLACK, 0);
		if (lic.focus == i + 1)
			ui_focus(lic.w, x + 5, LIC_BUTTON_Y + 5, LIC_BUTTON_W - 10, LIC_BUTTON_H - 10);
	}
}

static void
lic_draw(void)
{
	char head[200];

	ui_fill(lic.w, 0, 0, LIC_W, LIC_TEXT_Y, UI_FACE);
	snprintf(head, sizeof head, "The licence of %s, from its installer:", title);
	ui_text(lic.w, LIC_TEXT_X, 12, head, -1, BLACK, 0);
	lic_draw_text();
	lic_draw_buttons();
	GrFlush();
}

static void
lic_scroll(int to)
{
	int most = lic.lines > lic.shown ? lic.lines - lic.shown : 0, was = lic.seen_end;

	to = to < 0 ? 0 : to > most ? most : to;
	if (to == lic.top && lic.seen_end)
		return;
	lic.top = to;
	if (lic.top >= most)
		lic.seen_end = 1;
	lic_draw_text();
	if (lic.seen_end != was)
		lic_draw_buttons();
	GrFlush();
	say("licence at line %d of %d%s", lic.top, lic.lines, lic.top >= most ? ", its end" : "");
}

/* 1 accepted, 0 declined. */
static int
lic_ask_window(void)
{
	const GR_SCREEN_INFO *screen = ui_screen();
	GR_EVENT event;
	char caption[160];
	int answer = -1, x, y, dragging = 0;

	lic_wrap(LIC_TEXT_W - 12 - UI_SCROLL_W - 4);
	lic.shown = (LIC_TEXT_H - 10) / LIC_LINE;
	lic.seen_end = lic.lines <= lic.shown;
	lic.accept_x = LIC_W - 12 - 2 * LIC_BUTTON_W - 8, lic.decline_x = LIC_W - 12 - LIC_BUTTON_W;
	lic.focus = 0;
	snprintf(caption, sizeof caption, "%s: licence agreement", title);
	lic.w = GrNewWindowEx(UI_FIXED | GR_WM_PROPS_NOAUTOMOVE, caption, GR_ROOT_WINDOW_ID, (screen->cols - LIC_W) / 2,
			      (screen->rows - LIC_H) / 3 > 0 ? (screen->rows - LIC_H) / 3 : 0, LIC_W, LIC_H, UI_FACE);
	GrSelectEvents(lic.w, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_POSITION |
			       GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(lic.w);
	GrRaiseWindow(lic.w);
	GrSetFocus(lic.w);
	ui_framed();
	/* (drawn now: ui_framed drops the events it takes, the first exposure too, which left the window bare) */
	event.type = GR_EVENT_TYPE_EXPOSURE, event.general.wid = lic.w;
	for (int first = 1; answer < 0; first = 0) {
		int ch, press = -1;

		if (!first)
			GrGetNextEvent(&event);
		if (event.general.wid != lic.w)
			continue;
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			lic_draw();
			if (first) {
				ui_window_origin(lic.w, &x, &y);
				/* (where a test clicks: the buttons' middles on the screen) */
				say("licence shown, %d lines, %d at a time; accept at %d,%d, decline at %d,%d", lic.lines, lic.shown,
				    x + lic.accept_x + LIC_BUTTON_W / 2, y + LIC_BUTTON_Y + LIC_BUTTON_H / 2,
				    x + lic.decline_x + LIC_BUTTON_W / 2, y + LIC_BUTTON_Y + LIC_BUTTON_H / 2);
			}
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			answer = 0;
			break;
		case GR_EVENT_TYPE_BUTTON_UP:
			dragging = 0;
			break;
		case GR_EVENT_TYPE_MOUSE_POSITION:
			if (dragging)
				lic_scroll(ui_scroll_to(event.mouse.y, LIC_TEXT_Y + 2, LIC_TEXT_H - 4, lic.lines, lic.shown));
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN: {
			int turn = ui_wheel_sum(&event), bx = event.button.x, by = event.button.y;

			if (turn) {
				lic_scroll(lic.top + turn * 3);
				break;
			}
			if (lic.lines > lic.shown && ui_inside(bx, by, LIC_TEXT_X + LIC_TEXT_W - UI_SCROLL_W - 2, LIC_TEXT_Y, UI_SCROLL_W + 2, LIC_TEXT_H)) {
				dragging = 1;
				lic_scroll(ui_scroll_to(by, LIC_TEXT_Y + 2, LIC_TEXT_H - 4, lic.lines, lic.shown));
			} else if (ui_inside(bx, by, lic.accept_x, LIC_BUTTON_Y, LIC_BUTTON_W, LIC_BUTTON_H))
				press = 1;
			else if (ui_inside(bx, by, lic.decline_x, LIC_BUTTON_Y, LIC_BUTTON_W, LIC_BUTTON_H))
				press = 2;
			break;
		}
		case GR_EVENT_TYPE_KEY_DOWN:
			ch = event.keystroke.ch;
			if (ch == MWKEY_ESCAPE)
				press = 2;
			else if (ch == MWKEY_TAB) {
				lic.focus = (lic.focus + ((event.keystroke.modifiers & MWKMOD_SHIFT) ? 2 : 1)) % 3;
				lic_draw_text();
				lic_draw_buttons();
				GrFlush();
			} else if (ui_press_key(ch) && lic.focus)
				press = lic.focus;
			else if (ch == MWKEY_DOWN)
				lic_scroll(lic.top + 1);
			else if (ch == MWKEY_UP)
				lic_scroll(lic.top - 1);
			else if (ch == MWKEY_PAGEDOWN || ch == ' ')
				lic_scroll(lic.top + lic.shown - 1);
			else if (ch == MWKEY_PAGEUP)
				lic_scroll(lic.top - lic.shown + 1);
			else if (ch == MWKEY_HOME)
				lic_scroll(0);
			else if (ch == MWKEY_END)
				lic_scroll(lic.lines);
			else if ((ch | 32) == 'a')
				press = 1;
			else if ((ch | 32) == 'd')
				press = 2;
			break;
		}
		if (press == 1 && !lic.seen_end)
			say("accept refused: the licence is not read to its end");
		else if (press == 1)
			answer = 1;
		else if (press == 2)
			answer = 0;
	}
	GrDestroyWindow(lic.w);
	GrFlush();
	return answer;
}

/* Without a window system: the text on the terminal, then the question. */
static int
lic_ask_terminal(void)
{
	char line[64];

	printf("\n%s\n\nThe licence of %s, above, is from its installer. Do you accept it? Type yes to accept: ", lic.text, title);
	fflush(stdout);
	if (!fgets(line, sizeof line, stdin))
		return 0;
	return !strncasecmp(line, "yes", 3) || ((line[0] | 32) == 'y' && (line[1] == '\n' || line[1] == '\r' || !line[1]));
}

/* ---------------------------------------------------------------- how far: a window, or lines */

#define BAR_W 440
#define BAR_H 104

static struct {
	GR_WINDOW_ID w;
	double began, drawn;
	uint64_t total;
	int files, windowed;
	char line[160];
} bar;

static void
bar_draw(uint64_t done, const char *line)
{
	int width = BAR_W - 24, fill = bar.total ? (int)((width - 2) * done / bar.total) : 0;
	GR_EVENT event;

	while (bar.windowed && (GrCheckNextEvent(&event), event.type != GR_EVENT_TYPE_NONE))
		continue;	/* (a close asked for is not taken: the files are half made) */
	if (!bar.windowed)
		return;
	ui_fill(bar.w, 0, 0, BAR_W, BAR_H, UI_FACE);
	ui_text(bar.w, 12, 10, "Unpacking the installer's files into memory", -1, BLACK, 0);
	ui_bevel(bar.w, 12, 32, width, 20, 1);
	ui_fill(bar.w, 13, 33, width - 2, 18, WHITE);
	if (fill > 0)
		ui_fill(bar.w, 13, 33, fill, 18, GR_RGB(0, 0, 128));
	ui_text_fit(bar.w, 12, 62, width, line, BLACK);
	GrFlush();
}

/* After each round: the bar by the folder's bytes made of those up to the last one wanted, the line by the files' own. */
static void
bar_progress(void *ctx, uint64_t done, uint64_t total)
{
	struct cab *c = ctx;
	double t = now() - bar.began;
	uint64_t have = 0;
	uint32_t i;
	int whole = 0;
	char line[160];

	for (i = 0; i < c->nfiles; i++)
		if (c->files[i].to || c->files[i].fd >= 0) {
			have += c->files[i].made;
			whole += c->files[i].made == c->files[i].size;
		}
	snprintf(line, sizeof line, "%llu.%llu of %llu.%llu MB, %d of %d file%s, %.1f s", (unsigned long long)(have >> 20),
		 (unsigned long long)((have & 0xfffff) * 10 >> 20), (unsigned long long)(bar.total >> 20),
		 (unsigned long long)((bar.total & 0xfffff) * 10 >> 20), whole, bar.files, bar.files == 1 ? "" : "s", t);
	say("round %u: %llu of %llu bytes of the files, %d of %d whole, %.2f s", c->rounds, (unsigned long long)have,
	    (unsigned long long)bar.total, whole, bar.files, t);
	bar_draw(total ? done * bar.total / total : 0, line);
}

static void
bar_open(int windowed)
{
	const GR_SCREEN_INFO *screen;
	char caption[160];

	bar.windowed = windowed;
	if (!windowed)
		return;
	screen = ui_screen();
	snprintf(caption, sizeof caption, "%s: installing", title);
	bar.w = GrNewWindowEx(UI_FIXED | GR_WM_PROPS_NOAUTOMOVE, caption, GR_ROOT_WINDOW_ID, (screen->cols - BAR_W) / 2,
			      (screen->rows - BAR_H) / 3, BAR_W, BAR_H, UI_FACE);
	GrSelectEvents(bar.w, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(bar.w);
	ui_framed();
}

/* ---------------------------------------------------------------- files */

static void
make_dirs(char *path)
{
	char *s;

	for (s = path + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(path, 0755);
			*s = '/';
		}
}

/* A file of `size` bytes at path, mapped for its bytes to be written in place (tmpfs: its pages in one run), or
 * open for this core to write. */
static int
file_ready(struct cab_file *e, char *path, int *mapped)
{
	int fd;

	make_dirs(path);
	if ((fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644)) < 0)
		return -1;
	if (!e->size) {
		close(fd);
		return 0;
	}
	if (ftruncate(fd, e->size) == 0) {
		void *m = mmap(NULL, e->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

		if (m != MAP_FAILED) {
			e->to = m;
			close(fd);
			(*mapped)++;
			return 0;
		}
	}
	e->fd = fd;	/* (pwrite where it lies: no mapping in one piece) */
	return 0;
}

/* ---------------------------------------------------------------- an installer decoded already */

/* Every file under FROM linked at the same place under TO, folders made: the count, or -1 (errno says why). */
static int
link_tree(const char *from, const char *to)
{
	DIR *d = opendir(from);
	struct dirent *e;
	int n = 0;

	if (!d)
		return -1;
	mkdir(to, 0755);
	while ((e = readdir(d))) {
		char a[1024], b[1024];
		struct stat st;
		int k;

		if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2])))
			continue;
		snprintf(a, sizeof a, "%s/%s", from, e->d_name);
		snprintf(b, sizeof b, "%s/%s", to, e->d_name);
		if (stat(a, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			if ((k = link_tree(a, b)) < 0) {
				closedir(d);
				return -1;
			}
			n += k;
		} else {
			unlink(b);
			if (symlink(a, b)) {
				closedir(d);
				return -1;
			}
			n++;
		}
	}
	closedir(d);
	return n;
}

/* ---------------------------------------------------------------- the program */

static void
usage(void)
{
	fprintf(stderr, "usage: cabinst [--title TEXT] [--licence NAME] [--key KEY] INSTALLER DEST WANTED...\n"
		"       cabinst [--title TEXT] [--licence NAME] [--key KEY] --installed DIR DEST\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	const char *licence = NULL, *key = NULL, *installed = NULL, *installer, *dest, *why, *e;
	struct cab c;
	struct stat st;
	char path[1024], mark[1024];
	uint8_t *f;
	uint32_t i;
	int fd, a, windowed = 0, workers = 255, mapped = 0, sums;
	double t0, t1;

	print_on = getenv("CABINST_PRINT") != NULL;
	sums = getenv("CABINST_SUMS") != NULL;
	if ((e = getenv("CABINST_WORKERS")))
		workers = atoi(e);
	for (a = 1; a + 1 < argc && argv[a][0] == '-' && argv[a][1] == '-'; a += 2) {
		if (!strcmp(argv[a], "--title"))
			title = argv[a + 1];
		else if (!strcmp(argv[a], "--licence"))
			licence = argv[a + 1];
		else if (!strcmp(argv[a], "--key"))
			key = argv[a + 1];
		else if (!strcmp(argv[a], "--installed"))
			installed = argv[a + 1];
		else
			usage();
	}
	if (installed ? argc - a < 1 : argc - a < 3)
		usage();
	installer = installed ? installed : argv[a], dest = argv[installed ? a : a + 1];
	if (installed)
		a--;	/* (no WANTED: argv[a + 2] is past the end) */
	snprintf(mark, sizeof mark, "%s/.unpacked", dest);
	if (stat(mark, &st) == 0) {
		say("unpacked already this session");
		return 0;
	}
	if (installed)
		memset(&c, 0, sizeof c), st.st_size = 0;
	else if ((fd = open(installer, O_RDONLY)) < 0 || fstat(fd, &st) ||
	    (f = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED) {
		fprintf(stderr, "cabinst: %s: %s\n", installer, strerror(errno));
		return 2;
	}
	if (installed)
		;
	else if (close(fd), (why = cab_open(&c, f, (uint32_t)st.st_size))) {
		fprintf(stderr, "cabinst: %s: %s\n", installer, why);
		return 2;
	}
	if ((e = getenv("CABINST_BATCH")))
		c.batch = (uint32_t)atoi(e);
	c.workers = workers;
	if (workers > 0 && !installed) {
		/* a worker stops at every page of code no core has run: the text read here once (from _start's page) */
		const char *text = (const char *)((uintptr_t)_start & ~(uintptr_t)4095);

		mcw_touch_code(text, (unsigned)(etext - text));
		c.workers = mcw_open(workers);
	}
	if (!installed)
		say("%s: a cabinet of %u files in %u folder%s, at %u; %d workers", installer, c.nfiles, c.nfolders, c.nfolders == 1 ? "" : "s",
	    c.at, c.workers);
	windowed = GrOpen() >= 0;
	if (windowed)
		ui_init();

	/* the licence, read out of the installer and accepted before anything is unpacked */
	if (licence && installed) {	/* the licence the installer holds, decoded with it */
		FILE *l;
		long size;

		snprintf(path, sizeof path, "%s/%s", installed, licence);
		for (char *q = path; *q; q++)
			if (*q == '\\')
				*q = '/';
		if (!(l = fopen(path, "rb")) || fseek(l, 0, SEEK_END) || (size = ftell(l)) < 0 || fseek(l, 0, SEEK_SET) ||
		    !(lic.text = malloc(size + 1)) || fread(lic.text, 1, size, l) != (size_t)size) {
			fprintf(stderr, "cabinst: %s: %s\n", path, strerror(errno));
			return 2;
		}
		fclose(l);
		lic.text[size] = 0;
	}
	if (licence) {
		int k = installed ? 0 : cab_find(&c, licence);
		uint32_t crc, size = installed ? (uint32_t)strlen(lic.text) : 0;
		char value[16];

		if (installed) {
			crc = media_crc32(0, lic.text, size);
			say("licence %s: %u bytes, CRC-32 %08x, decoded already", licence, size, crc);
		} else {
		if (k < 0) {
			fprintf(stderr, "cabinst: %s has no %s\n", installer, licence);
			return 2;
		}
		if (!(lic.text = malloc(c.files[k].size + 1)))
			return 2;
		c.files[k].to = (uint8_t *)lic.text;
		t0 = now();
		why = cab_unpack(&c);
		c.files[k].to = NULL;
		if (why) {
			fprintf(stderr, "cabinst: %s: %s\n", installer, why);
			return 2;
		}
		lic.text[c.files[k].size] = 0;
		crc = media_crc32(0, lic.text, c.files[k].size);
		say("licence %s: %u bytes, CRC-32 %08x, read out in %.2f s", c.files[k].name, c.files[k].size, crc, now() - t0);
		}
		/* (the text as lines: CR LF and CR to LF, a tab a space, a NUL the end) */
		{
			char *s, *o;

			for (s = o = lic.text; *s; s++)
				if (*s == '\r')
					*o++ = '\n', s += s[1] == '\n';
				else
					*o++ = *s == '\t' ? ' ' : *s;
			*o = 0;
		}
		snprintf(value, sizeof value, "%08x", crc);
		if (key && !strcmp(reg_get(key, ""), value))
			say("licence accepted before (%s)", key);
		else {
			int yes = windowed ? lic_ask_window() : lic_ask_terminal();

			say(yes ? "accepted" : "declined");
			if (!yes) {
				if (windowed)
					GrClose();
				return 1;
			}
			if (key)
				reg_set(key, value);
		}
		c.made = 0, c.rounds = 0, c.sums_seen = 0;
		memset(cab_times, 0, sizeof cab_times);
	}

	if (installed) {	/* nothing to unpack: the decoded files linked into place */
		int n = strcmp(dest, "-") ? link_tree(installed, dest) : 0;
		FILE *m;

		if (windowed)
			GrClose();
		if (!strcmp(dest, "-")) {
			printf("cabinst: %s: licence accepted, nothing to link\n", title);
			fflush(stdout);
			return 0;
		}
		if (n < 0) {
			fprintf(stderr, "cabinst: %s into %s: %s\n", installed, dest, strerror(errno));
			return 2;
		}
		if ((m = fopen(mark, "w")))
			fprintf(m, "%s\n", installed), fclose(m);
		printf("cabinst: %s: %d files linked from %s\n", title, n, installed);
		fflush(stdout);
		return 0;
	}

	/* the files wanted, each mapped where it will be */
	bar.total = 0;
	for (; a + 2 < argc; a++) {
		char name[300], *to, *s;
		int k, all = argv[a + 2][0] == '*', n = 0;	/* '*': every file, '*.ext': those whose names end so */

		snprintf(name, sizeof name, "%s", argv[a + 2]);
		to = strchr(name, '=');
		if (to)
			*to++ = 0;
		for (k = all ? 0 : cab_find(&c, name); k >= 0 && k < (int)c.nfiles; k = all ? k + 1 : (int)c.nfiles) {
			struct cab_file *x = &c.files[k];
			size_t xl = strlen(x->name), nl = strlen(name + 1);

			if (all && name[1] && (xl < nl || strcasecmp(x->name + xl - nl, name + 1)))
				continue;
			n++;
			if (x->to || x->fd >= 0)
				continue;
			snprintf(path, sizeof path, "%s/%s", dest, to && !all ? to : x->name + (x->name[0] == '\\'));
			for (s = path; *s; s++)
				if (*s == '\\')
					*s = '/';
			if (file_ready(x, path, &mapped)) {
				fprintf(stderr, "cabinst: %s: %s\n", path, strerror(errno));
				return 2;
			}
			if (x->to || x->fd >= 0)
				bar.total += x->size;
			bar.files++;
		}
		if (k < 0 || !n) {
			fprintf(stderr, "cabinst: %s has no %s\n", installer, name);
			return 2;
		}
	}
	say("%d files, %llu bytes, %d mapped in place", bar.files, (unsigned long long)bar.total, mapped);
	bar_open(windowed);
	for (i = 0; i < c.nfiles; i++)
		c.files[i].made = 0, c.files[i].crc = 0;
	c.sums = sums;
	c.progress = bar_progress;
	c.progress_ctx = &c;
	bar.began = t0 = now();
	why = cab_unpack(&c);
	t1 = now();
	for (i = 0; i < c.nfiles; i++) {
		if (c.files[i].to)
			munmap(c.files[i].to, c.files[i].size);
		if (c.files[i].fd >= 0)
			close(c.files[i].fd);
		if (sums && (c.files[i].to || c.files[i].fd >= 0))
			printf("cabinst: sum %08x %u %s\n", c.files[i].crc, c.files[i].size, c.files[i].name);
	}
	if (why) {
		fprintf(stderr, "cabinst: %s: %s\n", installer, why);
		if (windowed) {	/* (said in a window too: a start from the desktop has no terminal) */
			static const char *const ok[] = { "OK" };
			char text[300];

			GrDestroyWindow(bar.w);
			snprintf(text, sizeof text, "The installer could not be unpacked:\n%s.", why);
			ui_ask_icon = "error";
			ui_ask(title, text, ok, 1, NULL);
			GrClose();
		}
		return 2;
	}
	{
		FILE *m = fopen(mark, "w");

		if (m) {
			fprintf(m, "%s %llu\n", installer, (unsigned long long)st.st_size);
			fclose(m);
		}
	}
	printf("cabinst: %s: %d files, %llu bytes unpacked in %.2f s (%llu bytes inflated in %u rounds, %u block sums)\n", title, bar.files,
	       (unsigned long long)bar.total, t1 - t0, (unsigned long long)c.made, c.rounds, c.sums_seen);
	say("seconds in the blocks' sums %.2f, inflating %.2f, copies into the files %.2f", cab_times[0], cab_times[2], cab_times[1]);
	fflush(stdout);
	if (windowed) {
		char line[160];

		snprintf(line, sizeof line, "Done: %d file%s, %llu.%llu MB, in %.1f s", bar.files, bar.files == 1 ? "" : "s",
			 (unsigned long long)(bar.total >> 20), (unsigned long long)((bar.total & 0xfffff) * 10 >> 20), t1 - t0);
		bar_draw(bar.total, line);
		usleep(1200000);
		GrDestroyWindow(bar.w);
		GrClose();
	}
	return 0;
}
