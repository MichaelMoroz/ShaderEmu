/*
 * The machine's worker cores, for Red Alert (docs/ralert.md, docs/multicore.md). The game is
 * core 0's; a worker runs functions of workers.c in the game's own memory. What a worker is
 * to write must be nobody else's to the 16 bytes, which workers_alloc() sees to, and a core
 * sees another's stores one pass of the machine later.
 */
#ifndef RALERT_WORKERS_H
#define RALERT_WORKERS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A movie frame for UnVQ_4x4; a job is rows of it. */
struct workers_unvq {
	uint8_t *codebook, *pointers, *buffer;
	uint32_t blocks_per_row, num_rows, buff_width;
};

/* A block for LCW_Uncompress. */
struct workers_lcw {
	const uint8_t *source;
	uint8_t *dest;
	uint32_t length;
};

/* How many workers the machine has (0: none, and nothing below does anything). The first
 * call starts them, which takes some passes; RALERT_WORKERS=N uses no more than N. */
int workers_count(void);
/* Memory that begins and ends on 16 bytes, its pages already there, or NULL (no workers);
 * whether an address is inside such memory; giving it back. */
void *workers_alloc(unsigned bytes);
int workers_owns(const void *memory);
void workers_release(void *memory);
/* The memory the workers are to write frames into, and how much (its physical address is
 * not used any more). NULL: none. */
void workers_target(void *memory, unsigned physical, unsigned bytes);
/* UnVQ_4x4 with the block rows shared between the cores, into memory of workers_target(). 0 if it was not done (no workers,
 * or the frame is not in that memory): the
 * caller decodes. */
int workers_unvq(uint8_t *codebook, uint8_t *pointers, uint8_t *buffer, unsigned blocks_per_row, unsigned num_rows,
		 unsigned buff_width);
/*
 * LCW_Uncompress on a worker of its own, while this core goes on: 1 if it was begun (the
 * machine has two workers or more, and both ends are workers_alloc()'s), and then nothing may
 * touch the block before workers_lcw_wait() has returned. One block at a time: beginning
 * another waits for the one before. The first 16 bytes of `dest` must be nobody else's.
 */
int workers_lcw(const uint8_t *source, uint8_t *dest, unsigned length);
void workers_lcw_wait(void);
/* For the game's figures: frames shared so far, and the machine's passes they waited. */
extern unsigned workers_frames, workers_passes;

#ifdef __cplusplus
}
#endif
#endif
