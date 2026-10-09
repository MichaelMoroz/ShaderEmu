/*
 * The worker cores for a Linux program (docs/multicore.md): threads of the program that the
 * kernel does not know of. A job is a function of the program and two words for it; a worker
 * runs it on a stack of its own, in the program's memory as the program has it.
 *
 * What a job may do: compute, read and write the program's memory. What it may not: make a
 * system call (nor call anything that does: printf, malloc that asks for more memory, file
 * reading) or leave the function any other way than by returning.
 *
 * What both sides have to keep to: a core sees another's stores one pass of the machine
 * later, and two cores must not store to the same 16 bytes in one pass (addresses that differ
 * only in their last four bits). Give a job memory of its own to write: rows of a buffer
 * whose width is a multiple of 16, allocations from mcw_alloc(), its own stack.
 *
 * One program at a time has the workers: mcw_open() gives 0 while another that is still alive
 * does, and where the machine has none. A program does the work itself then.
 */
#ifndef MCW_H
#define MCW_H

#include <stdint.h>
#include "mc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t (*mcw_fn)(uint32_t a0, uint32_t a1);

/* Starts up to `limit` workers; how many there are. */
int mcw_open(int limit);
int mcw_count(void);
/* Parks them and gives them up. A program calls it before it leaves (atexit will do). */
void mcw_close(void);
/* Memory that begins and ends on 16 bytes, for a core of its own to write; never freed. */
void *mcw_alloc(unsigned bytes);
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
