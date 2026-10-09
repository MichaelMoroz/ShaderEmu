/* nxmon: a system monitor. Three plots of the last two minutes, a sample a second: how fast
 * the emulated processor runs (its cycle counter counts instructions), how busy Linux is, and
 * the memory in use; under them the load, the processes and the busiest programs. */
#include <dirent.h>
#include <fcntl.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include "ui.h"

#define HISTORY 120
#define STEP 3			/* pixels a sample */
#define PLOT_W (HISTORY * STEP)
#define PLOT_H 48
#define BLOCK (PLOT_H + 24)	/* a plot with its line of text */
#define WIDTH (PLOT_W + 18)
#define STATS_Y (3 * BLOCK + 6)
#define HEIGHT (STATS_Y + 5 * 15 + 6)
#define MAX_PROGRAMS 48
#define BUSIEST 3

#define SPEED_COLOUR MWRGB(80, 220, 100)
#define KERNEL_COLOUR MWRGB(240, 150, 40)
#define PROGRAM_COLOUR MWRGB(90, 150, 255)
#define MEMORY_COLOUR MWRGB(200, 110, 230)
#define FILES_COLOUR MWRGB(240, 220, 90)

/* A plot: two values a sample, the second stacked on the first, `top` at the plot's top. */
struct plot { unsigned low[HISTORY], high[HISTORY], top; GR_COLOR low_colour, high_colour; };
enum { SPEED, BUSY, MEMORY, PLOTS };
static struct plot plots[PLOTS] = {
	{ {0}, {0}, 1000000, SPEED_COLOUR, 0 },
	{ {0}, {0}, 100, KERNEL_COLOUR, PROGRAM_COLOUR },
	{ {0}, {0}, 1, MEMORY_COLOUR, FILES_COLOUR },
};

static GR_WINDOW_ID window;
static long memory_total, memory_used, memory_files, memory_cached;	/* kB */
static long switches, interrupts;	/* a second */
static struct { int pid, seen, share; unsigned long ticks; char name[16]; } programs[MAX_PROGRAMS];
static int program_count;

/* The processor's count of instructions run (its low 32 bits; it wraps every few minutes). */
static unsigned
instructions(void)
{
	unsigned n;

	__asm__ volatile("rdcycle %0" : "=r"(n));
	return n;
}

/* A whole small file of /proc; the number of bytes read. */
static int
read_file(const char *name, char *text, int size)
{
	int file = open(name, O_RDONLY), n = 0, got;

	if (file < 0)
		return text[0] = 0;
	while (n < size - 1 && (got = read(file, text + n, size - 1 - n)) > 0)
		n += got;
	close(file);
	text[n] = 0;
	return n;
}

/* The number that follows `name` in a text, or 0. */
static long
field(const char *text, const char *name)
{
	const char *at = strstr(text, name);

	return at ? atol(at + strlen(name)) : 0;
}

/* Linux's time since the last sample by what it went to, and its counts of events: /proc/stat. */
static void
sample_linux(long ms)
{
	static long before[8], before_switches, before_interrupts;
	static char text[4096];
	long v[8] = {0}, d[8], all = 0, now_switches, now_interrupts;
	int i;

	read_file("/proc/stat", text, sizeof text);
	sscanf(text, "cpu %ld %ld %ld %ld %ld %ld %ld %ld", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
	for (i = 0; i < 8; i++) {
		d[i] = v[i] - before[i];
		all += d[i];
		before[i] = v[i];
	}
	/* user and nice are programs; system and the two interrupt times the kernel */
	plots[BUSY].low[HISTORY - 1] = all > 0 ? (unsigned)(100 * (d[2] + d[5] + d[6]) / all) : 0;
	plots[BUSY].high[HISTORY - 1] = all > 0 ? (unsigned)(100 * (d[0] + d[1]) / all) : 0;
	now_switches = field(text, "\nctxt ");
	now_interrupts = field(text, "\nintr ");
	switches = (now_switches - before_switches) * 1000 / ms;
	interrupts = (now_interrupts - before_interrupts) * 1000 / ms;
	before_switches = now_switches;
	before_interrupts = now_interrupts;
}

static void
sample_memory(void)
{
	static char text[2048];

	read_file("/proc/meminfo", text, sizeof text);
	memory_total = field(text, "MemTotal:");
	memory_used = memory_total - field(text, "MemAvailable:");
	memory_files = field(text, "Shmem:");	/* files written since the start live in memory */
	memory_cached = field(text, "\nCached:");
	plots[MEMORY].top = memory_total > 0 ? memory_total : 1;
	plots[MEMORY].low[HISTORY - 1] = memory_used > memory_files ? memory_used - memory_files : 0;
	plots[MEMORY].high[HISTORY - 1] = memory_files;
}

/* Every process's share of the processor since the last look, from its /proc/PID/stat. */
static void
sample_programs(long ms)
{
	DIR *folder = opendir("/proc");
	struct dirent *item;
	char name[40], text[400], *open_at, *close_at;
	unsigned long user, kernel;
	int i, n;

	for (i = 0; i < program_count; i++)
		programs[i].seen = 0;
	while (folder && (item = readdir(folder)) != NULL) {
		int pid = atoi(item->d_name);

		if (pid <= 0)
			continue;
		snprintf(name, sizeof name, "/proc/%d/stat", pid);
		read_file(name, text, sizeof text);
		open_at = strchr(text, '(');
		close_at = strrchr(text, ')');
		/* after the name: the state and ten numbers, then the time as a program and in the kernel */
		if (!open_at || !close_at || sscanf(close_at + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &user, &kernel) != 2)
			continue;
		for (i = 0; i < program_count && programs[i].pid != pid; i++)
			;
		if (i == MAX_PROGRAMS)
			continue;
		if (i == program_count) {
			program_count++;
			programs[i].pid = pid;
			programs[i].ticks = user + kernel;	/* new: nothing to compare with yet */
			n = (int)(close_at - open_at - 1);
			snprintf(programs[i].name, sizeof programs[i].name, "%.*s", n, open_at + 1);
		}
		/* a tick is a hundredth of a second */
		programs[i].share = (int)((user + kernel - programs[i].ticks) * 1000 / ms);
		programs[i].ticks = user + kernel;
		programs[i].seen = 1;
	}
	if (folder)
		closedir(folder);
	for (i = n = 0; i < program_count; i++)
		if (programs[i].seen)
			programs[n++] = programs[i];
	program_count = n;
}

static int
plot_y(int which)
{
	return which * BLOCK + 22;
}

/* One sample's column: the first value from the bottom, the second on top of it. */
static void
draw_column(int which, int i)
{
	struct plot *p = &plots[which];
	int x = 9 + i * STEP, bottom = plot_y(which) + 1 + PLOT_H;
	int low = (int)((unsigned long long)p->low[i] * PLOT_H / p->top), high = (int)((unsigned long long)p->high[i] * PLOT_H / p->top);

	low = low > PLOT_H ? PLOT_H : low;
	high = low + high > PLOT_H ? PLOT_H - low : high;
	if (low)
		ui_fill(window, x, bottom - low, STEP - 1, low, p->low_colour);
	if (high)
		ui_fill(window, x, bottom - low - high, STEP - 1, high, p->high_colour);
}

static void
draw_plot(int which)
{
	int i;

	ui_fill(window, 8, plot_y(which), PLOT_W + 2, PLOT_H + 2, BLACK);
	for (i = 0; i < HISTORY; i++)
		draw_column(which, i);
}

/* A plot moved a sample to the left with the newest drawn in the room made. */
static void
scroll_plot(int which)
{
	int y = plot_y(which) + 1;

	GrCopyArea(window, ui_gc, 9, y, PLOT_W - STEP, PLOT_H, window, 9 + STEP, y, MWROP_COPY);
	ui_fill(window, 9 + PLOT_W - STEP, y, STEP, PLOT_H, BLACK);
	draw_column(which, HISTORY - 1);
}

/* A small square of a plot's colour and what it stands for. */
static void
legend(int x, int y, GR_COLOR colour, const char *text)
{
	ui_fill(window, x, y + 2, 8, 8, colour);
	ui_text(window, x + 12, y, text, -1, BLACK, 0);
}

/* The line over a plot: what it shows now. */
static void
draw_line(int which)
{
	char text[80];
	int y = which * BLOCK + 6;
	unsigned now = plots[which].low[HISTORY - 1], more = plots[which].high[HISTORY - 1];

	ui_fill(window, 0, y, WIDTH, 15, UI_FACE);
	switch (which) {
	case SPEED:
		snprintf(text, sizeof text, "Processor: %u.%02u million instructions a second", now / 1000000, now % 1000000 / 10000);
		ui_text(window, 8, y, text, -1, BLACK, 0);
		snprintf(text, sizeof text, "top: %u million", plots[SPEED].top / 1000000);
		ui_text(window, WIDTH - 90, y, text, -1, UI_SHADOW, 0);
		break;
	case BUSY:
		snprintf(text, sizeof text, "Linux: %u%% busy", now + more);
		ui_text(window, 8, y, text, -1, BLACK, 0);
		snprintf(text, sizeof text, "kernel %u%%", now);
		legend(150, y, KERNEL_COLOUR, text);
		snprintf(text, sizeof text, "programs %u%%", more);
		legend(250, y, PROGRAM_COLOUR, text);
		break;
	case MEMORY:
		snprintf(text, sizeof text, "Memory: %ld of %ld MB", memory_used / 1024, memory_total / 1024);
		ui_text(window, 8, y, text, -1, BLACK, 0);
		legend(150, y, MEMORY_COLOUR, "in use");
		snprintf(text, sizeof text, "files %ld.%ld MB", memory_files / 1024, memory_files % 1024 * 10 / 1024);
		legend(250, y, FILES_COLOUR, text);
		break;
	}
}

static void
draw_stats(void)
{
	struct sysinfo info;
	char text[120];
	int busiest[BUSIEST], i, k, n = 0;

	ui_fill(window, 0, STATS_Y, WIDTH, HEIGHT - STATS_Y, UI_FACE);
	sysinfo(&info);
	snprintf(text, sizeof text, "Running for %ld h %02ld min", info.uptime / 3600, info.uptime / 60 % 60);
	ui_text(window, 8, STATS_Y, text, -1, BLACK, 0);
	/* a load is in 65536ths */
	snprintf(text, sizeof text, "Load %lu.%02lu over a minute, %lu.%02lu over five", info.loads[0] >> 16,
		(info.loads[0] & 0xffff) * 100 >> 16, info.loads[1] >> 16, (info.loads[1] & 0xffff) * 100 >> 16);
	ui_text(window, 180, STATS_Y, text, -1, BLACK, 0);
	snprintf(text, sizeof text, "Processes: %d", program_count);
	ui_text(window, 8, STATS_Y + 15, text, -1, BLACK, 0);
	snprintf(text, sizeof text, "Files read and kept: %ld MB", (memory_cached - memory_files) / 1024);
	ui_text(window, 180, STATS_Y + 15, text, -1, BLACK, 0);
	snprintf(text, sizeof text, "Task switches: %ld a second", switches);
	ui_text(window, 8, STATS_Y + 30, text, -1, BLACK, 0);
	snprintf(text, sizeof text, "Interrupts: %ld a second", interrupts);
	ui_text(window, 180, STATS_Y + 30, text, -1, BLACK, 0);
	/* the busiest programs, most first */
	for (k = 0; k < BUSIEST; k++) {
		busiest[k] = -1;
		for (i = 0; i < program_count; i++)
			if (programs[i].share > 0 && (busiest[k] < 0 || programs[i].share > programs[busiest[k]].share) &&
			    (k < 1 || i != busiest[0]) && (k < 2 || i != busiest[1]))
				busiest[k] = i;
		if (busiest[k] >= 0)
			n += snprintf(text + n, sizeof text - n, "%s%s %d%%", n ? ",  " : "", programs[busiest[k]].name, programs[busiest[k]].share);
	}
	ui_text(window, 8, STATS_Y + 50, "Busiest:", -1, BLACK, 0);
	ui_text(window, 60, STATS_Y + 50, n ? text : "nothing", -1, BLACK, 0);
}

static void
draw(void)
{
	int i;

	ui_fill(window, 0, 0, WIDTH, HEIGHT, UI_FACE);
	for (i = 0; i < PLOTS; i++) {
		draw_line(i);
		draw_plot(i);
	}
	draw_stats();
}

/* A second's samples taken and shown: the plots move left, the text is written again. */
static void
sample(unsigned speed, long ms)
{
	unsigned top = 1000000;
	int i;

	for (i = 0; i < PLOTS; i++) {
		memmove(plots[i].low, plots[i].low + 1, (HISTORY - 1) * sizeof plots[i].low[0]);
		memmove(plots[i].high, plots[i].high + 1, (HISTORY - 1) * sizeof plots[i].high[0]);
	}
	plots[SPEED].low[HISTORY - 1] = speed;
	sample_linux(ms);
	sample_memory();
	/* the speed's plot is as tall as the fastest second in it needs, in doublings */
	for (i = 0; i < HISTORY; i++)
		while (plots[SPEED].low[i] > top)
			top *= 2;
	for (i = 0; i < PLOTS; i++) {
		if (i == SPEED && top != plots[SPEED].top) {
			plots[SPEED].top = top;
			draw_plot(i);
		} else {
			scroll_plot(i);
		}
		draw_line(i);
	}
}

int
main(void)
{
	GR_EVENT event;
	struct timeval then, now, looked;
	unsigned count_then = instructions();

	if (GrOpen() < 0)
		return 1;
	ui_init();
	gettimeofday(&then, NULL);
	looked = then;
	sample_linux(1000);
	sample_memory();
	sample_programs(1000);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Monitor", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(window);
	for (;;) {
		long ms;

		GrGetNextEventTimeout(&event, 1000);
		/* Escape and q close it */
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ || (event.type == GR_EVENT_TYPE_KEY_DOWN &&
		    (event.keystroke.ch == MWKEY_ESCAPE || event.keystroke.ch == 'q'))) {
			GrClose();
			return 0;
		}
		if (event.type == GR_EVENT_TYPE_EXPOSURE)
			draw();
		gettimeofday(&now, NULL);
		ms = (now.tv_sec - then.tv_sec) * 1000L + (now.tv_usec - then.tv_usec) / 1000;
		if (ms < 1000)
			continue;
		{
			unsigned count_now = instructions();

			sample((unsigned)((unsigned long long)(count_now - count_then) * 1000 / ms), ms);
			count_then = count_now;
			then = now;
		}
		/* the processes are looked at every third second: each is a file to read */
		ms = (now.tv_sec - looked.tv_sec) * 1000L + (now.tv_usec - looked.tv_usec) / 1000;
		if (ms >= 3000) {
			sample_programs(ms);
			looked = now;
		}
		draw_stats();
	}
}
