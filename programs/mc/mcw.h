/*
 * The worker cores for a Linux program (docs/multicore.md): one library for every program
 * that uses them. The program gives it the code its workers run (a file built bare for the
 * arena around mcw_worker.h) and then posts jobs; what the jobs are is the program's.
 *
 * The arena is 4 MiB that the program has at the address the workers know it by, so a
 * pointer into it means the same on both sides. A core sees another's stores one pass of the
 * machine later, and two cores must not store to the same 16 bytes in one pass.
 *
 * One program at a time: the arena carries its owner's process number, and mcw_open() gives
 * 0 while another program that is still alive has it.
 */
#ifndef MCW_H
#define MCW_H

#include <stdint.h>
#include "mc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts up to `limit` workers on `code`; how many there are (0: none, or not ours to use). */
int mcw_open(const unsigned char *code, unsigned bytes, int limit);
int mcw_count(void);
/* Parks them and gives the arena up. A program calls it before it leaves (atexit will do). */
void mcw_close(void);
/* Memory of the arena, 16 bytes aligned, or NULL; mcw_free_all() takes all of it back. */
void *mcw_alloc(unsigned bytes);
void mcw_free_all(void);
int mcw_owns(const void *memory);
/* A job for worker k (1 to mcw_count()): the worker's code is called with fn, a0, a1. */
void mcw_post(int k, uint32_t fn, uint32_t a0, uint32_t a1);
/* Whether worker k has finished what it was posted last; its answer. */
int mcw_done(int k);
uint32_t mcw_result(int k);
/* Ends this core's passes until it has. How many passes that took. */
int mcw_wait(int k);

#ifdef __cplusplus
}
#endif
#endif
