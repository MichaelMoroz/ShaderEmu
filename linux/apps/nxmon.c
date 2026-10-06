/*
 * nxmon: a system monitor. Once a second: how many instructions the emulated processor ran
 * (its cycle counter counts them), how busy Linux was, and how much memory is in use, with
 * a minute of history for the first.
 */
#include <sys/time.h>
#include "ui.h"

#define WIDTH 260
#define HEIGHT 170
#define HISTORY 60
#define GRAPH_H 70

static GR_WINDOW_ID window;
static unsigned rate[HISTORY];		/* instructions a second, oldest first */
static int busy_percent, memory_total_kb, memory_free_kb;

/* The processor's count of instructions run (its low 32 bits; it wraps every few minutes). */
static unsigned
instructions(void)
{
	unsigned n;

	__asm__ volatile("rdcycle %0" : "=r"(n));
	return n;
}

/* A number that follows `name` in a /proc file, or 0. */
static long
field(const char *file_name, const char *name)
{
	FILE *file = fopen(file_name, "r");
	char line[160];
	long value = 0;

	while (file && fgets(line, sizeof line, file))
		if (!strncmp(line, name, strlen(name))) {
			value = atol(line + strlen(name));
			break;
		}
	if (file)
		fclose(file);
	return value;
}

/* The share of the last second Linux was not idle, from the first line of /proc/stat. */
static void
sample_busy(void)
{
	static long before_idle, before_all;
	FILE *file = fopen("/proc/stat", "r");
	long v[8] = {0}, all = 0, idle;
	int i;

	if (!file)
		return;
	if (fscanf(file, "cpu %ld %ld %ld %ld %ld %ld %ld %ld", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) < 4)
		v[3] = 0;
	fclose(file);
	for (i = 0; i < 8; i++)
		all += v[i];
	idle = v[3] + v[4];
	if (all > before_all)
		busy_percent = (int)(100 - 100 * (idle - before_idle) / (all - before_all));
	before_idle = idle;
	before_all = all;
}

static void
draw(void)
{
	char text[80];
	unsigned most = 1000000, i;

	ui_fill(window, 0, 0, WIDTH, HEIGHT, UI_FACE);
	for (i = 0; i < HISTORY; i++)
		if (rate[i] > most)
			most = rate[i];
	ui_fill(window, 8, 8, HISTORY * 4 + 2, GRAPH_H + 2, BLACK);
	for (i = 0; i < HISTORY; i++) {
		int bar = (int)((unsigned long long)rate[i] * GRAPH_H / most);

		ui_fill(window, 9 + i * 4, 9 + GRAPH_H - bar, 3, bar, MWRGB(80, 220, 100));
	}
	snprintf(text, sizeof text, "%u.%02u million instructions a second", rate[HISTORY - 1] / 1000000,
		rate[HISTORY - 1] % 1000000 / 10000);
	ui_text(window, 8, GRAPH_H + 18, text, -1, BLACK, 0);
	snprintf(text, sizeof text, "(graph: the last minute, top = %u.%u million)", most / 1000000, most % 1000000 / 100000);
	ui_text(window, 8, GRAPH_H + 34, text, -1, UI_SHADOW, 0);
	snprintf(text, sizeof text, "Linux busy %d%%", busy_percent);
	ui_text(window, 8, GRAPH_H + 54, text, -1, BLACK, 0);
	snprintf(text, sizeof text, "Memory %d of %d MB in use", (memory_total_kb - memory_free_kb) / 1024, memory_total_kb / 1024);
	ui_text(window, 8, GRAPH_H + 70, text, -1, BLACK, 0);
}

int
main(void)
{
	GR_EVENT event;
	struct timeval then, now;
	unsigned count_then = instructions();

	if (GrOpen() < 0)
		return 1;
	ui_init();
	gettimeofday(&then, NULL);
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Monitor", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(window);
	for (;;) {
		long ms;

		GrGetNextEventTimeout(&event, 1000);
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ) {
			GrClose();
			return 0;
		}
		gettimeofday(&now, NULL);
		ms = (now.tv_sec - then.tv_sec) * 1000L + (now.tv_usec - then.tv_usec) / 1000;
		if (ms >= 1000) {
			unsigned count_now = instructions();

			memmove(rate, rate + 1, (HISTORY - 1) * sizeof rate[0]);
			rate[HISTORY - 1] = (unsigned)((unsigned long long)(count_now - count_then) * 1000 / ms);
			count_then = count_now;
			then = now;
			sample_busy();
			memory_total_kb = field("/proc/meminfo", "MemTotal:");
			memory_free_kb = field("/proc/meminfo", "MemAvailable:");
			if (!memory_free_kb)
				memory_free_kb = field("/proc/meminfo", "MemFree:");
			draw();
		} else if (event.type == GR_EVENT_TYPE_EXPOSURE) {
			draw();
		}
	}
}
