/*
 * Over mcw.h (docs/machine/multicore.md, "The second layer"): a parallel loop, calls collected later,
 * what a job may do that only the first core may (print, defer, allocate, fail), and counters
 * every core adds to. With no workers the first core does all of it.
 */
#ifndef MCWX_H
#define MCWX_H

#include <stdint.h>
#include "mcw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- a parallel loop ---- */

/* A piece of a loop: items from..to-1. */
typedef void (*mcw_body)(void *ctx, uint32_t from, uint32_t to);

typedef struct {
	uint32_t grain;		/* items a piece; 0: four pieces a core */
	uint32_t out_bytes;	/* what an item writes, in a row: a piece is then whole 16 bytes of it */
	void *touch;		/* memory made present on the first core first (mcw_touch) */
	uint32_t touch_bytes;
	int workers;		/* the most to use; 0: all there are */
	int first_idle;		/* the first core takes no piece (it rests until they are done) */
} mcw_for_opts;

/*
 * fn over 0..n-1 in pieces, every core taking every so many; returns with all done and their
 * prints, deferred calls and failures handled. Workers wait mcw_for_linger() passes for the
 * next round on a word, so rounds in a row cost no posting. Outputs from 16-aligned memory.
 */
void mcw_for(uint32_t n, mcw_body fn, void *ctx);
void mcw_for_opt(uint32_t n, mcw_body fn, void *ctx, const mcw_for_opts *o);
void mcw_for_linger(int passes);	/* 16 at first; 0: a worker leaves the team at once */
void mcw_for_end(void);			/* the team leaves, and is waited for (before calls or mcw_post) */
/*
 * The same round begun and not waited for: the first core's own pieces (unless first_idle) are
 * made before it returns, the workers' while it goes on. mcw_for_ready() asks (and helps a
 * worker on); mcw_for_wait() waits and handles prints, deferred calls and failures. One out at a time.
 */
void mcw_for_begin(uint32_t n, mcw_body fn, void *ctx, const mcw_for_opts *o);
int mcw_for_ready(void);
void mcw_for_wait(void);

/* ---- a picture made as a fragment shader makes it (docs/machine/lanes.md, "Shaders") ---- */

/*
 * A pixel's colour, 0x00RRGGBB, at column x and row y of the picture (0, 0 its top left). Called
 * once for every pixel, on any core and in no set order, mostly on a worker's lanes: it computes
 * and reads, and stores nothing but to its own stack (no globals, no system calls, no stdio).
 */
typedef uint32_t (*mcw_pixel_fn)(int x, int y, void *ctx);
/* fn for every pixel of a width x height picture of words at `pixels` (16-byte aligned, width
 * a multiple of 4, e.g. seglMemory()), on the lanes' field where the machine has one, else on the
 * workers' lanes (a program that opened them); returns with it whole. */
void mcw_shade(uint32_t *pixels, int width, int height, mcw_pixel_fn fn, void *ctx);
/* The same begun and not waited for (the round of mcw_for_begin()), and the wait. One out at a time. */
void mcw_shade_begin(uint32_t *pixels, int width, int height, mcw_pixel_fn fn, void *ctx);
void mcw_shade_wait(void);

/* ---- calls collected later ---- */

/*
 * fn(a0, a1) on a worker that has nothing to do, or here and now when none has. A handle is
 * joined once: mcw_join() waits, makes what the call deferred and returns its answer.
 */
typedef int mcw_handle;
mcw_handle mcw_call(mcw_fn fn, uint32_t a0, uint32_t a1);
/* The same only where a worker has nothing to do: 0, and nothing made, where none has. */
mcw_handle mcw_try_call(mcw_fn fn, uint32_t a0, uint32_t a1);
int mcw_ready(mcw_handle h);
uint32_t mcw_join(mcw_handle h);
/* mcw_rest_for() the workers of these calls (handles of 0 or made here are passed over). */
void mcw_rest_on(const mcw_handle *h, int n);

/* ---- what a job may do (on the first core it is done at once) ---- */

/* A line for the first core to write when the job is joined (%d %u %x %s %c %p, widths, 0). */
void mcw_print(const char *format, ...);
/* A long job's lines so far written now (on a worker by a system call the first core makes). */
void mcw_print_now(void);
/* fn(a0, a1) made by the first core when the job is joined, in the order asked. */
void mcw_defer(mcw_fn fn, uint32_t a0, uint32_t a1);
/* Memory of this core's own, 16-byte aligned, until mcw_scratch_reset() on this core. */
void *mcw_scratch(uint32_t bytes);
void mcw_scratch_reset(void);
/* Ends the job here; the first core raises it when it joins (mcw_on_failure()). */
void mcw_fail(const char *what, uint32_t value) __attribute__((noreturn));
/* What the first core does with a failure: by default it says so and the program ends. */
typedef void (*mcw_failure_fn)(int worker, const char *what, uint32_t value);
void mcw_on_failure(mcw_failure_fn fn);
/* Where printed lines go: write() to 1 by default. */
void mcw_on_print(void (*sink)(const char *text, uint32_t bytes));

/* ---- counters ---- */

/*
 * MC_COUNTERS words every core may add to in one pass, without a lock. mcw_get(): the total as
 * the machine last merged it (a few passes after the adds) plus this core's adds since; two cores
 * may get the same, so a counter is a sum, not a ticket. mcw_counters() first, on the first core
 * (it zeroes them); mcw_set() while no other core adds to it. Without workers they are words.
 */
int mcw_counters(void);
void mcw_add(int counter, uint32_t value);
uint32_t mcw_get(int counter);
void mcw_set(int counter, uint32_t value);
/* Whether the machine merges them (else mcw_get() adds every core's region itself). */
int mcw_counters_merged(void);
/* On the first core, once no core adds any more: waits until the totals have every add. */
void mcw_counters_settle(void);

#ifdef __cplusplus
}
#endif
#endif
