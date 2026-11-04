/*
 * nxperf's tests on the build computer itself, natively (docs/apps/bench.md, "The host natively"): the
 * same perf_tests.c compiled by the build computer's gcc and timed by its own clock, so the machine can
 * be set beside the processor it is emulated on. The tests of the worker cores, the lanes and the GPU
 * are not run here: the machine's workers and GPU are not the host's threads and card, and a number
 * for them would compare nothing (they are "-" in the table, as the 486's are).
 *   linux/bench/build.sh makes $WORK/src/bench-build/nxperf-host
 *   nxperf-host                 every test it can run, a line each, then the part scores
 *   nxperf-host --table         and the values as perf_score.c's column
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "perf.h"

/* the GPU's tests are the machine's (perf_gpu.c): none here */
int perf_gpu_ready;
int perf_gpu_init(unsigned window) { (void)window; return -1; }
void perf_gpu_lists(struct perf_run *r) { r->ok = -1; }
void perf_gpu_tris(struct perf_run *r) { r->ok = -1; }
void perf_gpu_quads(struct perf_run *r) { r->ok = -1; }
void perf_gpu_fill(struct perf_run *r) { r->ok = -1; }
void perf_gpu_memcpy(struct perf_run *r) { r->ok = -1; }

static char cpu[96];

/* the processor's name as /proc/cpuinfo says it */
static const char *cpu_name(void)
{
	FILE *f = fopen("/proc/cpuinfo", "r");
	char line[256];

	snprintf(cpu, sizeof cpu, "this computer");
	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, "model name", 10) && strchr(line, ':')) {
			char *v = strchr(line, ':') + 2;

			v[strcspn(v, "\n")] = 0;
			snprintf(cpu, sizeof cpu, "%s", v);
			break;
		}
	if (f) fclose(f);
	return cpu;
}

int main(int argc, char **argv)
{
	float value[128] = {0}, parts[PERF_PARTS];
	int counted[PERF_PARTS], i, p, ok = 0, failed = 0, table = argc > 1 && !strcmp(argv[1], "--table");

	perf_workers = 0;
	perf_most_runs = 4096;
	printf("nxperf-host: %d tests on %s, natively, its own clock\n", perf_count, cpu_name());
	for (i = 0; i < perf_count && i < 128; i++) {
		const struct perf_test *t = &perf_tests[i];
		struct perf_run r;

		memset(&r, 0, sizeof r);
		if (t->flags & (PERF_WORKERS | PERF_GPU)) {
			printf("nxperf-host: %-12s %10s %-8s not run (the machine's %s)\n", t->name, "-", t->unit,
			       t->flags & PERF_GPU ? "GPU" : "workers");
			continue;
		}
		perf_run_test(i, &r);
		printf("nxperf-host: %-12s %10.4g %-8s %s (%s) %d runs\n", t->name, (double)r.value, t->unit,
		       r.ok > 0 ? "OK" : r.ok == 0 ? "FAIL" : "not run", r.note, r.runs);
		fflush(stdout);
		if (r.ok > 0) value[i] = r.value, ok++;
		else failed += r.ok == 0;
	}
	printf("nxperf-host: score %.0f (%d OK, %d FAIL)", (double)perf_score(value, parts, counted), ok, failed);
	for (p = 0; p < PERF_PARTS; p++)
		if (counted[p]) printf(", %s %.0f", perf_parts[p], (double)parts[p]);
	printf("\n");
	if (table)
		for (i = 0; i < perf_count && i < 128; i++) printf("table: %-14s %.4g\n", perf_tests[i].name, (double)value[i]);
	return failed ? 1 : 0;
}
