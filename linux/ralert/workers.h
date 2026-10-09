/*
 * The machine's worker cores, for Red Alert (docs/ralert.md, docs/multicore.md). The game is
 * core 0's; a worker runs worker.c, with no paging, on memory of the arena, which the game
 * maps at the address the workers know it by. So what a worker is to read or write has to be
 * allocated with workers_alloc(), and a core sees another's stores one pass of the machine
 * later.
 */
#ifndef RALERT_WORKERS_H
#define RALERT_WORKERS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { WORKERS_FN_UNVQ = 16, WORKERS_FN_LCW };

/* A movie frame for UnVQ_4x4, in the arena; a job is rows of it. */
struct workers_unvq {
	uint8_t *codebook, *pointers, *buffer;
	uint32_t blocks_per_row, num_rows, buff_width;
};

/* A block for LCW_Uncompress, in the arena. */
struct workers_lcw {
	const uint8_t *source;
	uint8_t *dest;
	uint32_t length;
};

/* How many workers the machine has (0: none, and nothing below does anything). The first
 * call starts them, which takes some passes; RALERT_WORKERS=N uses no more than N. */
int workers_count(void);
/* Memory of the arena, 16 bytes aligned, or NULL. It is all given back by workers_release()
 * of the last piece still held. */
void *workers_alloc(unsigned bytes);
int workers_owns(const void *memory);
void workers_release(void *memory);
/* Memory outside the arena that the workers are to write: where it is for the game, where
 * for them (its physical address) and how much. NULL: none. */
void workers_target(void *memory, unsigned physical, unsigned bytes);
/* UnVQ_4x4 with the block rows shared between the cores, into memory of workers_target(). 0 if it was not done (no workers,
 * the codebook and pointers are not the arena's, or the frame is not in that memory): the
 * caller decodes. */
int workers_unvq(uint8_t *codebook, uint8_t *pointers, uint8_t *buffer, unsigned blocks_per_row, unsigned num_rows,
		 unsigned buff_width);
/*
 * LCW_Uncompress on a worker of its own, while this core goes on: 1 if it was begun (the
 * machine has two workers or more, and both ends are the arena's), and then nothing may
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
