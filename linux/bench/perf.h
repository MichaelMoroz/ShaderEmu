/*
 * nxperf's tests (docs/apps/bench.md): each measures one thing of the machine and checks what it
 * made. A test fills a perf_run: its value in the test's unit, core 0's instructions an operation
 * where the test runs on core 0, and whether its check held.
 */
#ifndef NXPERF_PERF_H
#define NXPERF_PERF_H

#include <stdint.h>

struct perf_run {
	float value;		/* in the test's unit */
	float cpo;		/* core 0's instructions an operation (rdcycle), 0: not said */
	int ok;			/* 1 the check held, 0 it did not, -1 not run here (no workers, no GPU) */
	char note[72];		/* what was checked, or why it failed */
	/* the timed part's */
	uint64_t t0, ns;
	uint32_t c0, cycles;
};

#define PERF_WORKERS 1		/* wants the worker cores */
#define PERF_GPU 2		/* wants the GPU (a window) */

struct perf_test {
	const char *name, *group, *unit;
	void (*run)(struct perf_run *r);
	unsigned flags;
	const char *what;	/* a line for the window's status */
};

extern const struct perf_test perf_tests[];
extern const int perf_count;
extern const char *const perf_groups[];
extern const int perf_group_count;

/* the workers nxperf opened (0: none, the tests that want them are not run) */
extern int perf_workers;
/* the GPU: perf_gpu_init() with the window to draw in (0 or -1, as seglInit); perf_gpu_ready says so */
extern int perf_gpu_ready;
int perf_gpu_init(unsigned window);

uint64_t perf_now_ns(void);
static inline uint32_t perf_cycles(void)
{
	uint32_t c;

	__asm__ volatile("rdcycle %0" : "=r"(c));
	return c;
}
void perf_begin(struct perf_run *r);
void perf_end(struct perf_run *r);
/* value = amount a second over the timed part, in units of `scale` (1e6: millions) */
void perf_rate(struct perf_run *r, float amount, float scale);
/* value = the timed part's nanoseconds over `amount` */
void perf_each_ns(struct perf_run *r, float amount);
/* core 0's instructions an operation over the timed part */
void perf_cpo(struct perf_run *r, float ops);
/* the check: `ok`, and a note (printf's) */
void perf_check(struct perf_run *r, int ok, const char *format, ...) __attribute__((format(printf, 3, 4)));

/* the GPU's tests (perf_gpu.c) */
void perf_gpu_lists(struct perf_run *r);
void perf_gpu_tris(struct perf_run *r);
void perf_gpu_quads(struct perf_run *r);
void perf_gpu_fill(struct perf_run *r);
void perf_gpu_memcpy(struct perf_run *r);

#endif
