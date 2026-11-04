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
 * A run of every test ends with its score (perf_score.c): the window's Score page, and --cli's last lines; the
 * last run's and the best are kept in the registry (nxperf.score, nxperf.best: "SCORE PARTS... TIME WORKERS LANES").
 *
 * NXPERF_WORKERS=N asks for N workers (255 by default; 0: the tests that want them are not run).
 * --cli draws its GPU tests into a window of its own when there is a window system, and does not
 * touch the registry unless --save is given.
 */
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "mcw.h"
#include "mcwx.h"
#include "perf.h"
#include "ui.h"
#include "ui_single.h"

#define WIDTH 700	/* (the score page's table: seven machines' names and eight parts of up to seven digits) */
#define HEIGHT 540
#define TOP 46
#define ROW 19
#define LEFT 144
#define GROUP_H 22
#define PIC_X 8
#define PIC_Y (TOP + 4 + 7 * GROUP_H + 10)
#define PIC_W 120
#define PIC_H 90

struct result { float value, cpo; int ok, have, runs; char note[72]; };
static struct result *results;
static int workers_wanted = 255;

/* ---------------------------------------------------------------- the score */

/* a score as kept: the whole, the parts, when, with how many workers and lanes in the field (-1: not known, -2:
 * timed by the host's clock: rvc_cpu, docs/apps/bench.md "The host's clock") */
struct kept_score { float all, parts[PERF_PARTS]; long when; int workers, lanes, have; };
static struct kept_score this_run, last_run, best_run;

static void score_of_results(struct kept_score *k, int workers)
{
	float v[128];
	int i;

	for (i = 0; i < perf_count && i < 128; i++) v[i] = results[i].have && results[i].ok > 0 ? results[i].value : 0;
	k->all = perf_score(v, k->parts, NULL);
	k->when = (long)time(NULL), k->workers = workers, k->lanes = perf_host_clock > 0 ? -2 : perf_field_lanes, k->have = k->all > 0;
}

static void score_read(const char *key, struct kept_score *k)
{
	const char *v = reg_get(key, NULL);
	float *p = k->parts;

	memset(k, 0, sizeof *k);
	if (v && sscanf(v, "%f %f %f %f %f %f %f %f %f %ld %d %d", &k->all, &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6],
			&p[7], &k->when, &k->workers, &k->lanes) == 12)
		k->have = 1;
}

static void score_write(const char *key, const struct kept_score *k)
{
	char text[160];
	const float *p = k->parts;

	snprintf(text, sizeof text, "%.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %.0f %ld %d %d", (double)k->all, (double)p[0], (double)p[1],
		 (double)p[2], (double)p[3], (double)p[4], (double)p[5], (double)p[6], (double)p[7], k->when, k->workers, k->lanes);
	reg_set(key, text);
}

/* a whole run's score kept: the last, and the best if it is */
static void score_keep(void)
{
	score_read("nxperf.score", &last_run);
	score_read("nxperf.best", &best_run);
	score_write("nxperf.score", &this_run);
	if (!best_run.have || this_run.all > best_run.all) score_write("nxperf.best", &this_run);
}

/* The host's processor short: CPUID's brand string without " Processor" and a "16-Core" (AMD Ryzen 9 9950X3D). */
static const char *host_short(void)
{
	static char name[52];
	char *at;

	snprintf(name, sizeof name, "%s", perf_host_name[0] ? perf_host_name : "the host");
	if ((at = strstr(name, " Processor"))) *at = 0;
	if ((at = strstr(name, "-Core")))
		for (*at = 0; at > name && at[-1] != ' '; at--) at[-1] = 0;
	for (at = name + strlen(name); at > name && at[-1] == ' ';) *--at = 0;
	return name;
}

/* what the machine was, in parts the score page puts on lines of their own: the field has lanes on the card's lean tick only */
static void machine_text(char *to, int room, const struct kept_score *k)
{
	if (k->lanes == -2)
		snprintf(to, (size_t)room, "rvc_cpu, by the host's clock, on %s, %d workers", host_short(), k->workers);
	else if (k->lanes > 0) snprintf(to, (size_t)room, "the card, %d lanes in the field, %d workers", k->lanes, k->workers);
	else if (k->lanes == 0) snprintf(to, (size_t)room, "no lanes, rvc_cpu or a tick without them, %d workers", k->workers);
	else snprintf(to, (size_t)room, "%d workers, the field not run", k->workers);
}

static void date_text(char *to, int room, long when)
{
	time_t t = (time_t)when;
	struct tm *tm = localtime(&t);

	if (!when || !tm || !strftime(to, (size_t)room, "%d %b %Y %H:%M", tm)) snprintf(to, (size_t)room, "-");
}

static void score_print(const struct kept_score *k, int n)
{
	char text[96];
	float parts[PERF_PARTS];
	int counted[PERF_PARTS], p, m;
	float v[128];
	int i;

	for (i = 0; i < perf_count && i < 128; i++) v[i] = results[i].have && results[i].ok > 0 ? results[i].value : 0;
	perf_score(v, parts, counted);
	machine_text(text, sizeof text, k);
	printf("nxperf: score %.0f of %d results (1,000: the card model), %s\n", (double)k->all, n, text);
	for (p = 0; p < PERF_PARTS; p++)
		if (counted[p]) printf("nxperf: part %-12s %6.0f (%d results)\n", perf_parts[p], (double)parts[p], counted[p]);
		else printf("nxperf: part %-12s      - (none)\n", perf_parts[p]);
	for (m = 0; m < PERF_MACHINES; m++) {
		float all = perf_machine_score(m, parts, counted);

		if (all <= 0) continue;	/* (no values yet: not said) */
		printf("nxperf: beside it: %-26s %7.0f (%s;", perf_machines[m].name, (double)all, perf_machines[m].note);
		for (p = 0; p < PERF_PARTS; p++)
			if (counted[p]) printf(" %s %.0f", perf_parts[p], (double)parts[p]);
		printf(")\n");
	}
}

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
		perf_run_test(i, &r);
	s->runs = r.runs;
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
	perf_clock_init();
	printf("nxperf: %d tests, %d workers, GPU %s, %s\n", n, perf_workers, perf_gpu_ready ? "in a window" : "none",
	       perf_host_clock ? "the host's clock" : "the guest's clock");
	if (perf_host_clock) printf("nxperf: host %s\n", perf_host_name);
	fflush(stdout);
	for (k = 0; k < n; k++) {
		struct result *s;
		char v[24], c[24] = "";

		i = list[k], s = &results[i];
		run_one(i);
		value_text(v, sizeof v, s->value);
		if (s->cpo > 0) snprintf(c, sizeof c, "%.2f i/op", (double)s->cpo);
		printf("nxperf: %-12s %10s %-8s %-11s %s (%s)", perf_tests[i].name, s->ok < 0 ? "-" : v, perf_tests[i].unit, c,
		       s->ok > 0 ? "OK" : s->ok == 0 ? "FAIL" : "not run", s->note);
		if (s->runs > 1) printf(" %d runs", s->runs);
		printf("\n");
		fflush(stdout);
		ok += s->ok > 0, failed += s->ok == 0, skipped += s->ok < 0;
		if (save) save_result(i);
	}
	score_of_results(&this_run, perf_workers);
	workers_close();
	printf("nxperf: %d results: %d OK, %d FAIL, %d not run\n", n, ok, failed, skipped);
	score_print(&this_run, ok);
	if (save && n == perf_count) score_keep();
	if (window) GrClose();
	return failed ? 1 : 0;
}

/* ---------------------------------------------------------------- the window */

static GR_WINDOW_ID window, picture;
static GR_FONT_ID big_font;
static int group, running, stop_asked, quit_asked, current = -1, selected = -1;
/* what is shown under the buttons: a group's bars, the score, or every result */
enum { VIEW_GROUP, VIEW_SCORE, VIEW_RESULTS };
static int view = VIEW_GROUP;
static char status[120] = "Run all, or a group, or click a test.";

/* the buttons along the top, large enough for a hand in a world: x, width, label */
static const struct { int x, w; const char *label; } buttons[] = {
	{8, 96, "Run all"}, {110, 110, "Run group"}, {226, 70, "Stop"}, {310, 92, "Score"}, {408, 104, "Results"},
};
#define BUTTON_Y 6
#define BUTTON_H 34

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
	int b;

	ui_fill(window, 0, 0, WIDTH, TOP, UI_FACE);
	for (b = 0; b < (int)(sizeof buttons / sizeof buttons[0]); b++)
		ui_button(window, buttons[b].x, BUTTON_Y, buttons[b].w, BUTTON_H, buttons[b].label,
			  (b == 0 && running == 2) || (b == 1 && running == 1) || (b == 3 && view == VIEW_SCORE) || (b == 4 && view == VIEW_RESULTS));
}

static void draw_groups(void)
{
	int g;

	ui_fill(window, 0, TOP, LEFT - 4, PIC_Y - TOP - 4, UI_FACE);
	for (g = 0; g < perf_group_count; g++) {
		int y = TOP + 4 + g * GROUP_H;

		if (g == group && view == VIEW_GROUP) ui_fill(window, 8, y, LEFT - 20, GROUP_H - 2, UI_SELECTED);
		ui_text(window, 14, y + 4, perf_groups[g], -1, g == group && view == VIEW_GROUP ? WHITE : BLACK, 0);
	}
}

static void draw_row(int row)
{
	int i = row_test(group, row), x = LEFT, y = TOP + 4 + row * ROW, w;
	struct result *s;
	char text[48];
	float most;

	if (i < 0 || view != VIEW_GROUP) return;
	s = &results[i];
	ui_fill(window, x, y, WIDTH - x - 8, ROW, i == current ? MWRGB(255, 255, 200) : i == selected ? MWRGB(220, 230, 250) : WHITE);
	ui_text(window, x + 4, y + 3, perf_tests[i].name, -1, BLACK, 1);
	ui_fill(window, x + 104, y + 4, 200, ROW - 8, MWRGB(230, 230, 230));
	if (s->have && s->ok >= 0) {
		most = unit_most(group, perf_tests[i].unit);
		w = most > 0 ? (int)(200.0f * s->value / most + 0.5f) : 0;
		ui_fill(window, x + 104, y + 4, w < 1 ? 1 : w > 200 ? 200 : w, ROW - 8, s->ok ? MWRGB(60, 140, 220) : MWRGB(210, 60, 50));
		value_text(text, 24, s->value);
		strcat(text, " ");
		strcat(text, perf_tests[i].unit);
		ui_text(window, x + 312, y + 3, text, -1, BLACK, 0);
	}
	if (s->have)
		ui_text(window, x + 424, y + 3, s->ok > 0 ? "OK" : s->ok == 0 ? "FAIL" : "-", -1,
			s->ok > 0 ? MWRGB(0, 120, 0) : s->ok == 0 ? MWRGB(200, 0, 0) : UI_SHADOW, 0);
}

static void draw_rows(void)
{
	int row, n = row_count(group);

	ui_fill(window, LEFT - 4, TOP, WIDTH - LEFT + 4, HEIGHT - 24 - TOP, UI_FACE);
	ui_bevel(window, LEFT - 1, TOP + 3, WIDTH - LEFT - 6, n * ROW + 2, 1);
	for (row = 0; row < n; row++) draw_row(row);
}

/* ---- the score page: the whole large, a bar for each part (a click shows its group), the other machines beside it */

#define PART_X 224
#define PART_Y (TOP + 6)
#define PART_H 28
#define PART_BAR 300
#define TABLE_Y (PART_Y + PERF_PARTS * PART_H + 10)
/* the bars are logarithmic, 10 to 10 million: the card model's 1,000 a third of the way, rvc_cpu's tens of
 * thousands and a part of millions in view, where a bar by the largest part made every other one 0 pixels */
#define BAR_DECADES 6.0f

static int bar_width(float v)
{
	float w = v > 10.0f ? (float)PART_BAR * (log10f(v) - 1.0f) / BAR_DECADES : 0;

	return w < 0 ? 0 : w > PART_BAR ? PART_BAR : (int)(w + 0.5f);
}

/* the group a part's tests are in */
static int part_group(int p)
{
	int i;

	for (i = 0; i < perf_count; i++)
		if (perf_part_of(i) == p) return group_of(perf_tests[i].group);
	return 0;
}

static void number_text(char *to, int room, float v)
{
	if (v > 0) snprintf(to, (size_t)room, "%.0f", (double)v);
	else snprintf(to, (size_t)room, "-");
}

static void draw_score(void)
{
	const struct kept_score *k = this_run.have ? &this_run : &last_run;
	char text[96], when[40];
	float parts[PERF_PARTS];
	int p, m, y, failed = 0, i;

	ui_fill(window, 0, TOP, WIDTH, HEIGHT - 24 - TOP, UI_FACE);
	/* the whole */
	ui_bevel(window, 8, TOP + 4, PART_X - 20, TABLE_Y - TOP - 10, 1);
	ui_fill(window, 9, TOP + 5, PART_X - 22, TABLE_Y - TOP - 12, WHITE);
	ui_text(window, 16, TOP + 10, k == &this_run ? "Score of this run" : "Score of the last run", -1, BLACK, 0);
	number_text(text, sizeof text, k->have ? k->all : 0);
	GrSetGCFont(ui_gc, big_font);
	GrSetGCForeground(ui_gc, MWRGB(20, 70, 150));
	GrText(window, ui_gc, 16, TOP + 30, text, (int)strlen(text), GR_TFASCII | GR_TFTOP);
	ui_text(window, 16, TOP + 74, "1,000: the card model", -1, UI_SHADOW, 0);
	for (i = 0; i < perf_count; i++) failed += results[i].have && results[i].ok == 0;
	date_text(when, sizeof when, k->when);
	ui_text(window, 16, TOP + 96, when, -1, BLACK, 0);
	machine_text(text, sizeof text, k);
	y = TOP + 114;
	{	/* (the machine's words a line each: one line was wider than the panel) */
		char *part = text, *comma;

		for (; part; part = comma ? comma + 2 : NULL, y += 16) {
			if ((comma = strstr(part, ", "))) *comma = 0;
			ui_text(window, 16, y, part, -1, BLACK, 0);
		}
	}
	snprintf(text, sizeof text, failed ? "%d checks FAILED" : "every check held", failed);
	ui_text(window, 16, y + 4, text, -1, failed ? MWRGB(200, 0, 0) : MWRGB(0, 120, 0), 0);
	y += 30;
	if (k == &this_run && last_run.have) {
		date_text(when, sizeof when, last_run.when);
		snprintf(text, sizeof text, "last run: %.0f", (double)last_run.all);
		ui_text(window, 16, y, text, -1, BLACK, 0);
		ui_text(window, 16, y + 16, when, -1, UI_SHADOW, 0);
		y += 38;
	}
	if (best_run.have) {
		date_text(when, sizeof when, best_run.when);
		snprintf(text, sizeof text, "best: %.0f", (double)(this_run.have && this_run.all > best_run.all ? this_run.all : best_run.all));
		ui_text(window, 16, y, text, -1, BLACK, 0);
		ui_text(window, 16, y + 16, this_run.have && this_run.all > best_run.all ? "this run" : when, -1, UI_SHADOW, 0);
	}
	/* the parts */
	for (p = 0; p < PERF_PARTS; p++) {
		int w, yy = PART_Y + p * PART_H;

		ui_fill(window, PART_X, yy, WIDTH - PART_X - 8, PART_H - 4, WHITE);
		ui_text(window, PART_X + 6, yy + 7, perf_parts[p], -1, BLACK, 0);
		ui_fill(window, PART_X + 100, yy + 6, PART_BAR, PART_H - 16, MWRGB(230, 230, 230));
		w = bar_width(k->parts[p]);
		if (w > 0) ui_fill(window, PART_X + 100, yy + 6, w, PART_H - 16, MWRGB(60, 140, 220));
		/* the reference's 1,000 */
		ui_fill(window, PART_X + 100 + bar_width(1000), yy + 3, 1, PART_H - 10, MWRGB(120, 120, 120));
		number_text(text, sizeof text, k->parts[p]);
		ui_text(window, PART_X + 110 + PART_BAR, yy + 7, text, -1, BLACK, 0);
	}
	/* the machines side by side: this run, then the others of perf_machines, each column as wide as its widest text */
	{
		static const char *const heads[PERF_PARTS] = {"single", "FPU", "memory", "multi", "lanes", "numbers", "media", "GPU"};
		float shown[PERF_MACHINES + 1][PERF_PARTS + 1];
		const char *name[PERF_MACHINES + 1] = {"this run"};
		int x[PERF_PARTS + 2], width[PERF_PARTS + 2], rows = 1, c, yy;

		shown[0][0] = k->have ? k->all : 0;
		memcpy(&shown[0][1], k->parts, sizeof k->parts);
		for (m = 0; m < PERF_MACHINES; m++) {	/* (a machine with no values yet is not shown) */
			if ((shown[rows][0] = perf_machine_score(m, parts, NULL)) <= 0) continue;
			memcpy(&shown[rows][1], parts, sizeof parts);
			name[rows++] = perf_machines[m].name;
		}
		/* column 0 the names, 1 the score, then the parts */
		width[0] = ui_width(&ui_var_widths, ui_var, "machine", 7);
		for (m = 1; m < rows; m++) {
			int w = ui_width(&ui_var_widths, ui_var, name[m], (int)strlen(name[m]));

			if (w > width[0]) width[0] = w;
		}
		for (c = 1; c < PERF_PARTS + 2; c++) {
			const char *head = c == 1 ? "score" : heads[c - 2];

			width[c] = ui_width(&ui_var_widths, ui_var, head, (int)strlen(head));
			for (m = 0; m < rows; m++) {
				number_text(text, sizeof text, shown[m][c - 1]);
				if ((int)strlen(text) * ui_fw > width[c]) width[c] = (int)strlen(text) * ui_fw;
			}
		}
		x[0] = 14;
		for (c = 1; c < PERF_PARTS + 2; c++) x[c] = x[c - 1] + width[c - 1] + (c == 1 ? 14 : 10);
		ui_fill(window, 8, TABLE_Y, WIDTH - 16, HEIGHT - 28 - TABLE_Y, WHITE);
		ui_text(window, x[0], TABLE_Y + 3, "machine", -1, UI_SHADOW, 0);
		for (c = 1; c < PERF_PARTS + 2; c++)
			ui_text(window, x[c], TABLE_Y + 3, c == 1 ? "score" : heads[c - 2], -1, UI_SHADOW, 0);
		for (m = 0; m < rows; m++) {
			GR_COLOR ink = m == 0 ? MWRGB(20, 70, 150) : BLACK;

			yy = TABLE_Y + 22 + m * 17;
			ui_text(window, x[0], yy, name[m], -1, ink, 0);
			for (c = 1; c < PERF_PARTS + 2; c++) {
				number_text(text, sizeof text, shown[m][c - 1]);
				ui_text(window, x[c], yy, text, -1, ink, 1);
			}
		}
		yy = TABLE_Y + 22 + rows * 17 + 6;
		ui_text(window, x[0], yy, "486DX-33: estimates from published numbers (docs/apps/bench.md); - is not measured there.", -1, UI_SHADOW, 0);
		ui_text(window, x[0], yy + 16, "native: the tests compiled for the host, with no workers, lanes or GPU; 1,000 is the card model.", -1,
			UI_SHADOW, 0);
	}
}

/* ---- every result: three columns of name, value, unit and check (a click shows the test's group) */

#define RES_COLS 3
#define RES_ROWS 21
#define RES_ROW 16
#define RES_W ((WIDTH - 16) / RES_COLS)

static void draw_results(void)
{
	int i;

	ui_fill(window, 0, TOP, WIDTH, HEIGHT - 24 - TOP, UI_FACE);
	ui_fill(window, 8, TOP + 4, WIDTH - 16, RES_ROWS * RES_ROW + 4, WHITE);
	for (i = 0; i < perf_count && i < RES_COLS * RES_ROWS; i++) {
		const struct result *s = &results[i];
		int x = 12 + (i / RES_ROWS) * RES_W, y = TOP + 6 + (i % RES_ROWS) * RES_ROW;
		char text[40];

		ui_text(window, x, y, perf_tests[i].name, -1, BLACK, 1);
		if (s->have && s->ok >= 0) {
			value_text(text, 24, s->value);
			strcat(text, " ");
			strcat(text, perf_tests[i].unit);
			ui_text(window, x + 88, y, text, -1, BLACK, 0);
		}
		if (s->have)
			ui_text(window, x + RES_W - 34, y, s->ok > 0 ? "OK" : s->ok == 0 ? "FAIL" : "-", -1,
				s->ok > 0 ? MWRGB(0, 120, 0) : s->ok == 0 ? MWRGB(200, 0, 0) : UI_SHADOW, 0);
	}
}

static void draw_all(void)
{
	draw_buttons();
	if (view == VIEW_SCORE) draw_score();
	else if (view == VIEW_RESULTS) draw_results();
	else {
		draw_groups();
		ui_fill(window, 0, PIC_Y - 4, LEFT - 4, HEIGHT - 24 - PIC_Y + 4, UI_FACE);
		ui_text(window, PIC_X, PIC_Y + PIC_H + 4, "the GPU's tests", -1, UI_SHADOW, 0);
		draw_rows();
	}
	draw_status();
}

/* the GPU's square is the group view's: hidden under the other pages */
static void show_view(int v)
{
	if (v == view) return;
	view = v;
	if (view == VIEW_GROUP) GrMapWindow(picture);
	else GrUnmapWindow(picture);
	draw_all();
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
	show_view(VIEW_GROUP);
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
		snprintf(status, sizeof status, "Running %s (Stop or Esc ends after it): %s", perf_tests[i].name, perf_tests[i].what);
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
	running = 0;
	if (stop_asked) snprintf(status, sizeof status, "Stopped.");
	else if (n > 1) {
		int ok = 0, failed = 0;

		for (k = 0; k < n; k++) ok += results[list[k]].ok > 0, failed += results[list[k]].ok == 0;
		snprintf(status, sizeof status, "%d tests: %d OK, %d FAIL, %d not run", n, ok, failed, n - ok - failed);
		if (had) snprintf(status + strlen(status), sizeof status - strlen(status), " (%d workers)", had);
	}
	/* every test run to its end: the score, kept and shown */
	if (all && !stop_asked && n == perf_count) {
		score_of_results(&this_run, had);
		score_keep();
		workers_close();
		show_view(VIEW_SCORE);
		draw_all();
		return;
	}
	workers_close();
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
	int b;

	if (y >= BUTTON_Y && y < BUTTON_Y + BUTTON_H) {
		for (b = 0; b < (int)(sizeof buttons / sizeof buttons[0]); b++)
			if (x >= buttons[b].x && x < buttons[b].x + buttons[b].w) break;
		if (b == 2) {
			stop_asked = 1;
			return;
		}
		if (running) return;
		if (b == 0) run_group(0, 1);
		else if (b == 1) run_group(group, 0);
		else if (b == 3) show_view(view == VIEW_SCORE ? VIEW_GROUP : VIEW_SCORE);
		else if (b == 4) show_view(view == VIEW_RESULTS ? VIEW_GROUP : VIEW_RESULTS);
		return;
	}
	if (running) return;
	if (view == VIEW_SCORE) {
		/* a part's bar: its group */
		if (x >= PART_X && y >= PART_Y && y < PART_Y + PERF_PARTS * PART_H) {
			group = part_group((y - PART_Y) / PART_H);
			show_view(VIEW_GROUP);
		}
		return;
	}
	if (view == VIEW_RESULTS) {
		int col = (x - 8) / RES_W, row = (y - TOP - 6) / RES_ROW, i = col * RES_ROWS + row;

		if (x >= 8 && col < RES_COLS && y >= TOP + 6 && row < RES_ROWS && i < perf_count) {
			group = group_of(perf_tests[i].group), selected = i;
			show_view(VIEW_GROUP);
		}
		return;
	}
	if (x >= 8 && x < LEFT - 12 && y >= TOP + 4 && y < TOP + 4 + perf_group_count * GROUP_H) {
		group = (y - TOP - 4) / GROUP_H;
		selected = -1;
		draw_groups();
		draw_rows();
		return;
	}
	if (x >= LEFT && y >= TOP + 4) {
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
		else if (e->keystroke.ch == 's' || e->keystroke.ch == 'S') show_view(view == VIEW_SCORE ? VIEW_GROUP : VIEW_SCORE);
		else if (e->keystroke.ch == 'r' || e->keystroke.ch == 'R') show_view(view == VIEW_RESULTS ? VIEW_GROUP : VIEW_RESULTS);
		else if (e->keystroke.ch == '\r' || e->keystroke.ch == '\n' || e->keystroke.ch == ' ') run_group(group, 0);
		else if (e->keystroke.ch == MWKEY_UP || e->keystroke.ch == MWKEY_DOWN) {
			group = (group + (e->keystroke.ch == MWKEY_UP ? perf_group_count - 1 : 1)) % perf_group_count;
			show_view(VIEW_GROUP);
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
	score_read("nxperf.score", &last_run);
	score_read("nxperf.best", &best_run);
}

static int window_main(void)
{
	GR_EVENT e;
	const char *open_on = getenv("NXPERF_VIEW");	/* score or results: the page it opens on (captures) */

	if (GrOpen() < 0) {
		fprintf(stderr, "nxperf: no window system (nxperf --cli runs without one)\n");
		return 1;
	}
	ui_init();
	if (ui_single_instance()) return 0;
	load_results();
	perf_clock_init();
	big_font = GrCreateFontEx(access("/usr/share/font-sans-b-32.sef", R_OK) == 0 ? "/usr/share/font-sans-b-32.sef" : GR_FONT_SYSTEM_VAR,
				  32, 0, NULL);
	window = GrNewWindowEx(UI_FIXED, "Benchmark", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	picture = GrNewWindow(window, PIC_X, PIC_Y, PIC_W, PIC_H, 0, BLACK, BLACK);
	GrMapWindow(window);
	GrMapWindow(picture);
	ui_framed();
	if (perf_gpu_init(picture) < 0) snprintf(status, sizeof status, "The GPU is another program's: its tests are not run.");
	if (open_on && !strcmp(open_on, "score")) show_view(VIEW_SCORE);
	else if (open_on && !strcmp(open_on, "results")) show_view(VIEW_RESULTS);
	draw_all();
	/* NXPERF_RUN=all: Run all at once (a capture of the score page after a run) */
	if (getenv("NXPERF_RUN") && !strcmp(getenv("NXPERF_RUN"), "all")) run_group(0, 1);
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
