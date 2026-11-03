/*
 * nxperf: the machine measured (docs/apps/bench.md). Tests of the processor, memory, the worker
 * cores, the lanes, numerical kernels, libmedia and the GPU (perf_tests.c, perf_gpu.c), each with a
 * check of what it made. A window shows a group's results as bars and runs one test, a group or
 * all; the last results are kept in the registry (nxperf.NAME = "VALUE CHECK").
 *
 *   nxperf                          the window
 *   nxperf --cli [NAME|GROUP ...]   the tests named (all when none), a line each, then a count
 *   nxperf --list                   the tests and their groups
 *
 * NXPERF_WORKERS=N asks for N workers (255 by default; 0: the tests that want them are not run).
 * --cli draws its GPU tests into a window of its own when there is a window system, and does not
 * touch the registry unless --save is given.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mcw.h"
#include "mcwx.h"
#include "perf.h"
#include "ui.h"
#include "ui_single.h"

#define WIDTH 580
#define HEIGHT 372
#define TOP 34
#define ROW 19
#define LEFT 144
#define GROUP_H 22
#define PIC_X 8
#define PIC_Y (TOP + 4 + 7 * GROUP_H + 10)
#define PIC_W 120
#define PIC_H 90

struct result { float value, cpo; int ok, have; char note[72]; };
static struct result *results;
static int workers_wanted = 255;

/* ---------------------------------------------------------------- running a test */

static int wants_workers(const int *list, int n)
{
	int i;

	for (i = 0; i < n; i++)
		if (perf_tests[list[i]].flags & PERF_WORKERS) return 1;
	return 0;
}

static void workers_open(void)
{
	extern char _start[], etext[], __DATA_BEGIN__[], _end[];
	/* (from _start's page: a flat program's __executable_start is 0) */
	const char *text = (const char *)((uintptr_t)_start & ~(uintptr_t)4095);

	if (perf_workers || workers_wanted <= 0) return;
	perf_workers = mcw_open(workers_wanted);
	if (perf_workers > 0) {
		/* a worker stops at every page of code or data no core has touched yet */
		mcw_touch_code(text, (unsigned)(etext - text));
		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
	}
}

static void workers_close(void)
{
	if (perf_workers) mcw_close();
	perf_workers = 0;
}

static void run_one(int i)
{
	const struct perf_test *t = &perf_tests[i];
	struct perf_run r;
	struct result *s = &results[i];

	memset(&r, 0, sizeof r);
	r.ok = 1;
	if ((t->flags & PERF_WORKERS) && !perf_workers) {
		r.ok = -1;
		snprintf(r.note, sizeof r.note, "no workers here");
	} else if ((t->flags & PERF_GPU) && !perf_gpu_ready) {
		r.ok = -1;
		snprintf(r.note, sizeof r.note, "no window to draw in");
	} else
		t->run(&r);
	s->value = r.ok < 0 ? 0 : r.value, s->cpo = r.cpo, s->ok = r.ok, s->have = 1;
	memcpy(s->note, r.note, sizeof s->note);
}

/* a value with three or four digits */
static void value_text(char *to, int room, float v)
{
	snprintf(to, (size_t)room, v >= 1000.0f ? "%.0f" : v >= 100.0f ? "%.1f" : v >= 1.0f ? "%.2f" : "%.3f", (double)v);
}

static int group_of(const char *name)
{
	int g;

	for (g = 0; g < perf_group_count; g++)
		if (!strcmp(perf_groups[g], name)) return g;
	return -1;
}

static int test_of(const char *name)
{
	int i;

	for (i = 0; i < perf_count; i++)
		if (!strcmp(perf_tests[i].name, name)) return i;
	return -1;
}

static void save_result(int i)
{
	char key[48], value[40];

	snprintf(key, sizeof key, "nxperf.%s", perf_tests[i].name);
	value_text(value, sizeof value - 4, results[i].value);
	strcat(value, results[i].ok > 0 ? " 1" : results[i].ok == 0 ? " 0" : " -");
	reg_set(key, value);
}

/* ---------------------------------------------------------------- the command line */

static int cli(int argc, char **argv, int save)
{
	int *list = calloc((size_t)perf_count, sizeof *list), n = 0, i, k, ok = 0, failed = 0, skipped = 0;
	GR_WINDOW_ID window = 0;

	for (k = 0; k < argc; k++) {
		int g = group_of(argv[k]), t = test_of(argv[k]);

		if (g < 0 && t < 0) {
			fprintf(stderr, "nxperf: no test or group %s (nxperf --list)\n", argv[k]);
			return 2;
		}
		for (i = 0; i < perf_count; i++)
			if (g >= 0 ? !strcmp(perf_tests[i].group, perf_groups[g]) : i == t) {
				int j;

				for (j = 0; j < n && list[j] != i; j++) continue;
				if (j == n) list[n++] = i;
			}
	}
	if (!argc)
		for (i = 0; i < perf_count; i++) list[n++] = i;
	for (i = 0; i < n; i++)
		if (perf_tests[list[i]].flags & PERF_GPU) break;
	if (i < n && GrOpen() >= 0) {
		window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "nxperf", GR_ROOT_WINDOW_ID, 40, 40, 256, 192, BLACK);
		GrMapWindow(window);
		GrFlush();
		perf_gpu_init(window);
	}
	if (wants_workers(list, n)) workers_open();
	printf("nxperf: %d tests, %d workers, GPU %s\n", n, perf_workers, perf_gpu_ready ? "in a window" : "none");
	fflush(stdout);
	for (k = 0; k < n; k++) {
		struct result *s;
		char v[24], c[24] = "";

		i = list[k], s = &results[i];
		run_one(i);
		value_text(v, sizeof v, s->value);
		if (s->cpo > 0) snprintf(c, sizeof c, "%.2f i/op", (double)s->cpo);
		printf("nxperf: %-12s %10s %-8s %-11s %s (%s)\n", perf_tests[i].name, s->ok < 0 ? "-" : v, perf_tests[i].unit, c,
		       s->ok > 0 ? "OK" : s->ok == 0 ? "FAIL" : "not run", s->note);
		fflush(stdout);
		ok += s->ok > 0, failed += s->ok == 0, skipped += s->ok < 0;
		if (save) save_result(i);
	}
	workers_close();
	printf("nxperf: %d results: %d OK, %d FAIL, %d not run\n", n, ok, failed, skipped);
	if (window) GrClose();
	return failed ? 1 : 0;
}

/* ---------------------------------------------------------------- the window */

static GR_WINDOW_ID window, picture;
static int group, running, stop_asked, quit_asked, current = -1, selected = -1;
static char status[120] = "Run all, or a group, or click a test.";

static int row_count(int g)
{
	int i, n = 0;

	for (i = 0; i < perf_count; i++) n += !strcmp(perf_tests[i].group, perf_groups[g]);
	return n;
}

/* the test in row `row` of group g */
static int row_test(int g, int row)
{
	int i;

	for (i = 0; i < perf_count; i++)
		if (!strcmp(perf_tests[i].group, perf_groups[g]) && row-- == 0) return i;
	return -1;
}

/* the largest value of a unit in the group: what a full bar is */
static float unit_most(int g, const char *unit)
{
	float most = 0;
	int i;

	for (i = 0; i < perf_count; i++)
		if (!strcmp(perf_tests[i].group, perf_groups[g]) && !strcmp(perf_tests[i].unit, unit) && results[i].have && results[i].ok >= 0 &&
		    results[i].value > most)
			most = results[i].value;
	return most;
}

static void draw_status(void)
{
	ui_fill(window, 8, HEIGHT - 22, WIDTH - 16, 18, UI_FACE);
	ui_text(window, 10, HEIGHT - 20, status, -1, BLACK, 0);
}

static void draw_buttons(void)
{
	ui_fill(window, 0, 0, WIDTH, TOP, UI_FACE);
	ui_button(window, 8, 6, 72, 22, "Run all", running == 2);
	ui_button(window, 86, 6, 84, 22, "Run group", running == 1);
	ui_button(window, 176, 6, 52, 22, "Stop", 0);
	if (running) ui_text(window, 240, 11, "Running: Stop or Esc ends it after the test.", -1, BLACK, 0);
}

static void draw_groups(void)
{
	int g;

	ui_fill(window, 0, TOP, LEFT - 4, PIC_Y - TOP - 4, UI_FACE);
	for (g = 0; g < perf_group_count; g++) {
		int y = TOP + 4 + g * GROUP_H;

		if (g == group) ui_fill(window, 8, y, LEFT - 20, GROUP_H - 2, UI_SELECTED);
		ui_text(window, 14, y + 4, perf_groups[g], -1, g == group ? WHITE : BLACK, 0);
	}
}

static void draw_row(int row)
{
	int i = row_test(group, row), x = LEFT, y = TOP + 4 + row * ROW, w;
	struct result *s;
	char text[48];
	float most;

	if (i < 0) return;
	s = &results[i];
	ui_fill(window, x, y, WIDTH - x - 8, ROW, i == current ? MWRGB(255, 255, 200) : i == selected ? MWRGB(220, 230, 250) : WHITE);
	ui_text(window, x + 4, y + 3, perf_tests[i].name, -1, BLACK, 1);
	ui_fill(window, x + 104, y + 4, 180, ROW - 8, MWRGB(230, 230, 230));
	if (s->have && s->ok >= 0) {
		most = unit_most(group, perf_tests[i].unit);
		w = most > 0 ? (int)(180.0f * s->value / most + 0.5f) : 0;
		ui_fill(window, x + 104, y + 4, w < 1 ? 1 : w > 180 ? 180 : w, ROW - 8, s->ok ? MWRGB(60, 140, 220) : MWRGB(210, 60, 50));
		value_text(text, 24, s->value);
		strcat(text, " ");
		strcat(text, perf_tests[i].unit);
		ui_text(window, x + 292, y + 3, text, -1, BLACK, 0);
	}
	if (s->have)
		ui_text(window, x + 394, y + 3, s->ok > 0 ? "OK" : s->ok == 0 ? "FAIL" : "-", -1,
			s->ok > 0 ? MWRGB(0, 120, 0) : s->ok == 0 ? MWRGB(200, 0, 0) : UI_SHADOW, 0);
}

static void draw_rows(void)
{
	int row, n = row_count(group);

	ui_fill(window, LEFT - 4, TOP, WIDTH - LEFT + 4, HEIGHT - 24 - TOP, UI_FACE);
	ui_bevel(window, LEFT - 1, TOP + 3, WIDTH - LEFT - 6, n * ROW + 2, 1);
	for (row = 0; row < n; row++) draw_row(row);
}

static void draw_all(void)
{
	draw_buttons();
	draw_groups();
	draw_rows();
	draw_status();
}

/* the row of a test of the group shown, -1 for another */
static int row_in_group(int i)
{
	int row, n = row_count(group);

	for (row = 0; row < n; row++)
		if (row_test(group, row) == i) return row;
	return -1;
}

static void handle(GR_EVENT *e);

/* events between two tests: Stop, a close, a group chosen, a window to draw again */
static void poll_events(void)
{
	GR_EVENT e;

	for (;;) {
		GrCheckNextEvent(&e);
		if (e.type == GR_EVENT_TYPE_NONE) break;
		handle(&e);
	}
}

static void run_list(const int *list, int n, int all)
{
	int k, had = 0;

	running = all ? 2 : 1, stop_asked = 0;
	draw_buttons();
	if (wants_workers(list, n)) {
		snprintf(status, sizeof status, "Opening the workers...");
		draw_status();
		GrFlush();
		workers_open();
		had = perf_workers;
	}
	for (k = 0; k < n && !stop_asked && !quit_asked; k++) {
		int i = list[k], row;

		/* a test of another group shows its group */
		if (strcmp(perf_tests[i].group, perf_groups[group])) {
			group = group_of(perf_tests[i].group);
			draw_groups();
			draw_rows();
		}
		current = i;
		row = row_in_group(i);
		snprintf(status, sizeof status, "%s: %s", perf_tests[i].name, perf_tests[i].what);
		draw_row(row);
		draw_status();
		GrFlush();
		run_one(i);
		save_result(i);
		current = -1;
		/* the bars of the unit are drawn again: the largest may have changed */
		draw_rows();
		snprintf(status, sizeof status, "%s: %s", perf_tests[i].name, results[i].note);
		draw_status();
		GrFlush();
		poll_events();
	}
	workers_close();
	running = 0;
	if (stop_asked) snprintf(status, sizeof status, "Stopped.");
	else if (n > 1) {
		int ok = 0, failed = 0;

		for (k = 0; k < n; k++) ok += results[list[k]].ok > 0, failed += results[list[k]].ok == 0;
		snprintf(status, sizeof status, "%d tests: %d OK, %d FAIL, %d not run (%d workers)", n, ok, failed, n - ok - failed, had);
	}
	draw_all();
}

static void run_group(int g, int all)
{
	int list[128], n = 0, i;

	for (i = 0; i < perf_count && n < 128; i++)
		if (all || !strcmp(perf_tests[i].group, perf_groups[g])) list[n++] = i;
	run_list(list, n, all);
}

static void click(int x, int y)
{
	if (y >= 6 && y < 28) {
		if (x >= 176 && x < 228) {
			stop_asked = 1;
			return;
		}
		if (running) return;
		if (x >= 8 && x < 80) run_group(0, 1);
		else if (x >= 86 && x < 170) run_group(group, 0);
		return;
	}
	if (x >= 8 && x < LEFT - 12 && y >= TOP + 4 && y < TOP + 4 + perf_group_count * GROUP_H && !running) {
		group = (y - TOP - 4) / GROUP_H;
		selected = -1;
		draw_groups();
		draw_rows();
		return;
	}
	if (x >= LEFT && y >= TOP + 4 && !running) {
		int row = (y - TOP - 4) / ROW, i = row < row_count(group) ? row_test(group, row) : -1;

		if (i >= 0) {
			selected = i;
			run_list(&i, 1, 0);
		}
	}
}

static void handle(GR_EVENT *e)
{
	switch (e->type) {
	case GR_EVENT_TYPE_EXPOSURE:
		if (e->exposure.wid == window) draw_all();
		break;
	case GR_EVENT_TYPE_BUTTON_DOWN:
		if (e->button.wid == window) click(e->button.x, e->button.y);
		break;
	case GR_EVENT_TYPE_KEY_DOWN:
		if (e->keystroke.ch == MWKEY_ESCAPE) stop_asked = 1;
		else if (running) break;
		else if (e->keystroke.ch == 'a' || e->keystroke.ch == 'A') run_group(0, 1);
		else if (e->keystroke.ch == '\r' || e->keystroke.ch == '\n' || e->keystroke.ch == ' ') run_group(group, 0);
		else if (e->keystroke.ch == MWKEY_UP || e->keystroke.ch == MWKEY_DOWN) {
			group = (group + (e->keystroke.ch == MWKEY_UP ? perf_group_count - 1 : 1)) % perf_group_count;
			draw_groups();
			draw_rows();
		}
		break;
	case GR_EVENT_TYPE_CLOSE_REQ:
		quit_asked = 1, stop_asked = 1;
		break;
	}
}

/* the last results, from the registry */
static void load_results(void)
{
	char key[48], ok[8];
	int i;

	for (i = 0; i < perf_count; i++) {
		const char *v;
		float value;

		snprintf(key, sizeof key, "nxperf.%s", perf_tests[i].name);
		v = reg_get(key, NULL);
		if (v && sscanf(v, "%f %7s", &value, ok) == 2) {
			results[i].value = value, results[i].have = 1;
			results[i].ok = ok[0] == '1' ? 1 : ok[0] == '0' ? 0 : -1;
			snprintf(results[i].note, sizeof results[i].note, "the last run's");
		}
	}
}

static int window_main(void)
{
	GR_EVENT e;

	if (GrOpen() < 0) {
		fprintf(stderr, "nxperf: no window system (nxperf --cli runs without one)\n");
		return 1;
	}
	ui_init();
	if (ui_single_instance()) return 0;
	load_results();
	window = GrNewWindowEx(UI_FIXED, "Benchmark", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	picture = GrNewWindow(window, PIC_X, PIC_Y, PIC_W, PIC_H, 0, BLACK, BLACK);
	GrMapWindow(window);
	GrMapWindow(picture);
	ui_framed();
	if (perf_gpu_init(picture) < 0) snprintf(status, sizeof status, "The GPU is another program's: its tests are not run.");
	draw_all();
	while (!quit_asked) {
		GrGetNextEventTimeout(&e, 60000);
		if (e.type != GR_EVENT_TYPE_TIMEOUT) handle(&e);
		if (ui_restyle()) draw_all();
	}
	GrClose();
	return 0;
}

int main(int argc, char **argv)
{
	const char *w = getenv("NXPERF_WORKERS");
	int i;

	results = calloc((size_t)perf_count, sizeof *results);
	if (w) workers_wanted = atoi(w);
	signal(SIGPIPE, SIG_IGN);
	if (argc > 1 && !strcmp(argv[1], "--list")) {
		for (i = 0; i < perf_count; i++)
			printf("%-12s %-8s %-9s %s\n", perf_tests[i].name, perf_tests[i].group, perf_tests[i].unit, perf_tests[i].what);
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "--cli")) {
		int save = argc > 2 && !strcmp(argv[2], "--save");

		return cli(argc - 2 - save, argv + 2 + save, save);
	}
	return window_main();
}
