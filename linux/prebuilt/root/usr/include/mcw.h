/*
 * The worker cores for a Linux program (docs/machine/multicore.md): threads of the program that the
 * kernel does not know of. A job is a function of the program and two words for it; a worker
 * runs it on a stack of its own, in the program's memory as the program has it.
 *
 * What a job may do: compute, read and write the program's memory, and make system calls,
 * which this core makes for it when it next looks (mcw_wait(), mcw_done()): two passes of the
 * machine each, so not many. What it may not: use what the C library keeps one of for the
 * program while this core uses it too (malloc, stdio: one side at a time), or leave the
 * function any other way than by returning.
 *
 * What both sides have to keep to: a core sees another's stores one pass of the machine
 * later, and two cores must not store to the same 16 bytes in one pass (addresses that differ
 * only in their last four bits). Give a job memory of its own to write: rows of a buffer
 * whose width is a multiple of 16, allocations from mcw_alloc(), its own stack.
 *
 * The kernel shares the cores out: mcw_open() gives the program's share of them, which is
 * as many as it wants while there are enough for everybody, and may be none, and is none
 * where the machine has none. A program does the work itself then. The share changes as
 * other programs come and go: a worker of this program may lose its core between two jobs
 * (never during one), and have one again later. Nothing changes for the program: the
 * workers are numbered as before, and a job posted to one that has no core is run by the
 * core that posts it, there and then. mcw_bound() says which it would be.
 */
#ifndef MCW_H
#define MCW_H

#include <stdint.h>
#include "mc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t (*mcw_fn)(uint32_t a0, uint32_t a1);

/* For a job, on its worker: whether the kernel wants worker k's core back when the job ends (a
 * job that could go on for long ends early then, and is posted again for the rest). */
int mcw_recalled(int k);

/* How many workers the machine has (63 of 64 cores): all alike, each with a write cache of
 * 1.5 KB of new stores a pass (docs/machine/multicore.md). */
int mcw_most(void);
/* Starts up to `limit` workers; how many there are. */
int mcw_open(int limit);
/* Whether worker k has a core now (a job posted to it will not be run by this one). */
int mcw_bound(int k);
/*
 * For a program that can do without (a monitor that draws with them): called before
 * mcw_open(), its workers are only lent: it has what the programs that keep theirs leave.
 * mcw_asked() says that a core of this program has been taken back or asked for.
 */
void mcw_lend(void);
int mcw_asked(void);
int mcw_count(void);
/* Parks them and gives them up. A program calls it before it leaves (atexit will do). */
void mcw_close(void);
/* Memory that begins and ends on 16 bytes, for a core of its own to write; never freed. */
void *mcw_alloc(unsigned bytes);
/*
 * Makes memory's pages be there and writable now, on this core. A worker stops at every page
 * the program has not touched (or not written) yet until this core next looks, which is two
 * passes when it waits in mcw_wait() and may be a whole frame of a program that only asks
 * mcw_done() now and then: touch what a job will use first. A program's variables that start
 * as zeros are such pages (from __DATA_BEGIN__ to _end is all of its data).
 */
void mcw_touch(void *memory, unsigned bytes);
/* The same for code a job will run that this core may not have run yet: a read is enough. */
void mcw_touch_code(const void *code, unsigned bytes);
/* A job for worker k (1 to mcw_count()): it calls fn(a0, a1). */
void mcw_post(int k, mcw_fn fn, uint32_t a0, uint32_t a1);
/* Whether worker k has finished what it was posted last; what the function returned. */
int mcw_done(int k);
uint32_t mcw_result(int k);
/*
 * Ends this core's passes until it has; how many passes that took. A worker that stops at a
 * page the program has not touched yet is helped on from here (and from mcw_done()): the
 * page is touched, which the kernel answers, and the worker goes on. Any other stop ends the
 * program with a line saying where.
 */
int mcw_wait(int k);
/*
 * For the first core when it has nothing to do until the next pass (its workers have the work,
 * or it waits for the clock): the kernel is told, which runs another program or ends the pass
 * as its idle loop does. With no workers open it is the pause hint. Not for a job.
 */
void mcw_rest(void);
/*
 * The same, saying which workers (1 to mcw_count(), `n` of them) have the jobs the first core
 * waits for: on gpu_vm the machine gives them their whole pass and holds the others to the
 * first core's (gpu_vm/platform.md, "Resting"). Elsewhere it is mcw_rest().
 */
void mcw_rest_for(const int *workers, int n);
/* Whether the code that asks is running on a worker. */
int mcw_on_worker(void);
/* Which worker runs the code that asks (1 to mcw_count()); 0 on the first core. */
int mcw_self(void);
/* This program's place among the programs with workers (0 to 15), -1 without workers. */
int mcw_slot(void);
/* GPU memory at that offset of /dev/gpu, mapped through the workers' device; 0 without workers. */
void *mcw_map(uint32_t offset, uint32_t bytes);

/*
 * Lanes (docs/machine/lanes.md): on a worker, fn(index, ctx) for every index from first to
 * first + count - 1, each on a pixel of the worker's own: 6 to 55 calls at once where the
 * machine has lanes. What an index may do: compute, read any memory as the job found it, and
 * store to its own stack and to its output, which is `texels` whole 16-byte texels (1 to 6) at
 * out + 16 * texels * (index - first), and nothing else: no other memory, no system call, no
 * device, no atomics; a store elsewhere is lost. Indices run in no set order and see none of
 * each other's stores. out is 16-byte aligned. An index that cannot run in a lane (one that is
 * longer than a pass, or stops) is called here as a plain call afterwards, as every index is on
 * the first core, on a worker of a machine without lanes and with texels out of range: the
 * output is the same. Returns how many lanes the job had (0: plain calls).
 */
typedef void (*mcw_lane_fn)(uint32_t index, void *ctx);
int mcw_lanes(mcw_lane_fn fn, void *ctx, uint32_t first, uint32_t count, void *out, uint32_t texels);
/* How many lanes this core has: 0 on the first core and on a machine without them. */
int mcw_lanes_here(void);

/*
 * The field (docs/machine/lanes.md, "The field"): some 3,900 lanes of no core's, drawn by the
 * machine only while a job is out, for the first core to give a job to and go on: the same job
 * and rules as mcw_lanes(). One job is out at a time, of one program at a time. Where the machine
 * has no field, mcw_field_begin() makes the calls itself before it returns, as plain calls.
 */
int mcw_field_lanes(void);	/* how many lanes the field has; 0: none here */
void mcw_field_begin(mcw_lane_fn fn, void *ctx, uint32_t first, uint32_t count, void *out, uint32_t texels);
int mcw_field_ready(void);	/* whether the job is through (the indices a lane could not make are made here first) */
void mcw_field_wait(void);	/* until it is, the passes the kernel's (mcw_rest) */
/* The indices the field's lanes skipped (each longer than a pass), for the program's workers to make
 * in place of this core: with mcw_field_apart(1), mcw_field_ready() keeps them; mcw_field_skips() then
 * says which the last job left (0: none), and `parts` workers each call mcw_field_skips_make(skips,
 * part, parts), which returns how many it made. A job's skips are kept until the job after the next
 * is begun. */
struct mcw_field_skips;
void mcw_field_apart(int on);
const struct mcw_field_skips *mcw_field_skips(void);
unsigned mcw_field_skips_make(const struct mcw_field_skips *skips, int part, int parts);

/*
 * Steps (docs/machine/lanes.md, "Steps"): an index that may need more than a pass. fn returns 0 when
 * its index is through; else it has stored in its output what it needs to go on, and is called again
 * with round + 1 in the job's next round (round 0: its first call, its output as it was). An index
 * takes as many of a lane's six texels as it wrote: one through that writes nothing costs a few instructions.
 */
typedef uint32_t (*mcw_step_fn)(uint32_t index, void *ctx, uint32_t round);
/* The instructions this lane's pass has left; 0xffffffff off the lanes (no end). */
uint32_t mcw_steps_left(void);
/* The field's job in steps: mcw_field_ready() posts round after round until no index is left. */
void mcw_field_steps(mcw_step_fn fn, void *ctx, uint32_t first, uint32_t count, void *out, uint32_t texels);
/* The same on this worker's lanes, returned when through; plain calls where it has none. */
struct mcw_steps_report {
	uint32_t rounds;	/* rounds the job took */
	uint32_t too_long;	/* steps no lane could make in a pass, made by a core: not 0, the steps are too long for the lanes */
	uint32_t left;		/* the field's rounds ran out (mcw_field_steps_rounds): some indices are not through */
};
int mcw_lanes_steps(mcw_step_fn fn, void *ctx, uint32_t first, uint32_t count, void *out, uint32_t texels, struct mcw_steps_report *report);
struct mcw_steps_report mcw_field_steps_report(void);	/* the field's last job in steps, once through */
/* The field's jobs in steps from now on end after `most` rounds (0: when every index is through): a
 * round waits for its slowest lane, so the last few paths of a job cost a round each. With `left`
 * set, the program calls fn itself (any round but 0) on what is not through, as a plain call does. */
void mcw_field_steps_rounds(uint32_t most);
/* A self-test on this core (a worker's: the first core's counts take interrupts in): every index made in
 * steps, each step given `budget` instructions (0: 8,192), as a lane does. Give it the worst cases. */
struct mcw_steps_probe {
	uint32_t steps, rounds;	/* steps made, and the most an index took */
	uint32_t mean, longest;	/* a step's instructions */
	uint32_t over;		/* steps longer than the budget: a lane would leave them to a core */
};
struct mcw_steps_probe mcw_steps_probe(mcw_step_fn fn, void *ctx, uint32_t first, uint32_t count, uint32_t budget);

#ifdef __cplusplus
}
#endif
#endif
