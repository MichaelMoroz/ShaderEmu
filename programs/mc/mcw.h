/*
 * The worker cores for a Linux program (docs/multicore.md): threads of the program that the
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
 * The kernel hands the cores out: mcw_open() gives as many as are wanted of those no other
 * program has, which may be none, and where the machine has none. A program does the work
 * itself then. They are free again when the program closes them or ends.
 */
#ifndef MCW_H
#define MCW_H

#include <stdint.h>
#include "mc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t (*mcw_fn)(uint32_t a0, uint32_t a1);

/*
 * The machine's geometry (docs/multicore.md): `workers` cores, worker k's write cache with
 * tables of 2^bits[k - 1] buckets (3 to 6: 0.75, 1.5, 3 or 6 KB of new stores a pass). They
 * share 64 rows of the machine's state and a worker takes 2, 3, 5 or 9 of them, so it is
 * few workers that store much or many that store little: 7 of the largest, 21 of size 4
 * (the machine has 15 at most). Only while no program has a worker: 0 when it was done, the
 * kernel's error otherwise (-16: someone has workers; -22: more than fits; -19: a machine
 * whose cores are as they are). Until someone asks, there are 3 of the largest and 12 of size 4.
 */
int mcw_shape(const unsigned char *bits, int workers);
/* Starts up to `limit` workers; how many there are. */
int mcw_open(int limit);
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
/* Whether the code that asks is running on a worker. */
int mcw_on_worker(void);

#ifdef __cplusplus
}
#endif
#endif
