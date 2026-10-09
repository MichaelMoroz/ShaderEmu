/*
 * Red Alert's side of the machine's worker cores (workers.h): its jobs, over the library every
 * program that uses the workers has (programs/mc/mcw.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcw.h"		/* programs/mc: the library every program that uses the workers has */
#include "workers.h"
#include "worker_code.h"	/* worker.c built for the arena: worker_code[], made by build.sh */

unsigned workers_frames, workers_passes;

static int count = -1;
static uint32_t held;
static struct workers_unvq *shared;
static struct workers_lcw *unpacking;
static int unpacker_busy;
/* memory outside the arena that the workers may write: here, and where it is for them */
static uint8_t *target, *target_physical;
static uint32_t target_bytes;

int workers_count(void)
{
	const char *limit = getenv("RALERT_WORKERS");

	if (count >= 0) return count;
	count = mcw_open(worker_code, sizeof worker_code, limit ? atoi(limit) : MC_MAX_CORES - 1);
	if (count > 0) atexit(mcw_close);
	if (getenv("RALERT_STATS_MS") || getenv("RALERT_FRAMES")) fprintf(stderr, "ralert: %d worker cores\n", count);
	return count;
}

void *workers_alloc(unsigned bytes)
{
	void *memory = workers_count() ? mcw_alloc(bytes) : NULL;

	if (memory) held++;
	return memory;
}

int workers_owns(const void *memory) { return mcw_owns(memory); }

void workers_release(void *memory)
{
	if (workers_owns(memory) && held > 0 && --held == 0) {
		workers_lcw_wait();
		mcw_free_all();
		shared = NULL;
		unpacking = NULL;
	}
}

/* The last worker unpacks; the others, and it when it has nothing to unpack, share frames. */
void workers_lcw_wait(void)
{
	if (!unpacker_busy) return;
	workers_passes += mcw_wait(count);
	unpacker_busy = 0;
}

int workers_lcw(const uint8_t *source, uint8_t *dest, unsigned length)
{
	if (workers_count() < 2 || !workers_owns(source) || !workers_owns(dest) || ((uintptr_t)dest & 15) != 0) return 0;
	workers_lcw_wait();
	if (!unpacking) {
		if (!(unpacking = workers_alloc(sizeof *unpacking))) return 0;
		held--;	/* ours: it goes with the movie's last buffer */
	}
	unpacking->source = source;
	unpacking->dest = dest;
	unpacking->length = length;
	mcw_post(count, WORKERS_FN_LCW, (uint32_t)unpacking, 0);
	unpacker_busy = 1;
	return 1;
}

void workers_target(void *memory, unsigned physical, unsigned bytes)
{
	target = memory;
	target_physical = (uint8_t *)physical;
	target_bytes = memory ? bytes : 0;
}

int workers_unvq(uint8_t *codebook, uint8_t *pointers, uint8_t *buffer, unsigned blocks_per_row, unsigned num_rows,
		 unsigned buff_width)
{
	unsigned bytes = num_rows * 4 * buff_width, cores, share, row, k;

	/* rows of whole texels, or two cores would store to one */
	if (workers_count() == 0 || !workers_owns(codebook) || !workers_owns(pointers) || (buff_width & 15) != 0
	    || blocks_per_row * 4 > buff_width || num_rows < 2 || !target || buffer < target
	    || buffer + bytes > target + target_bytes || ((uintptr_t)(target_physical + (buffer - target)) & 15) != 0)
		return 0;
	if (!shared) {
		if (!(shared = workers_alloc(sizeof *shared))) return 0;
		held--;	/* ours: it goes with the movie's last buffer */
	}
	shared->codebook = codebook;
	shared->pointers = pointers;
	shared->buffer = target_physical + (buffer - target);
	shared->blocks_per_row = blocks_per_row;
	shared->num_rows = num_rows;
	shared->buff_width = buff_width;

	/* (the worker that unpacks the next frame is left to it) */
	if (unpacker_busy && mcw_done(count)) unpacker_busy = 0;
	cores = (unsigned)count + 1 - (unpacker_busy ? 1 : 0);
	if (cores > num_rows) cores = num_rows;
	share = (num_rows + cores - 1) / cores;
	/* the first share is this core's, the rest a worker's each */
	for (k = 1, row = share; k < cores && row < num_rows; k++, row += share) {
		unsigned end = row + share < num_rows ? row + share : num_rows;
		mcw_post((int)k, WORKERS_FN_UNVQ, (uint32_t)shared, (row << 16) | end);
	}
	cores = k;
	mc_next_pass();	/* the workers see the frame and their jobs from here on */
	workers_passes++;
	{
		/* this core's rows: UnVQ_4x4's own loop */
		const uint32_t *cb = (const uint32_t *)codebook;
		const uint8_t *low = pointers, *high = pointers + blocks_per_row * num_rows;
		uint8_t *line = buffer;
		unsigned r, b;
		for (r = 0; r < share; r++, line += 4 * buff_width) {
			uint8_t *out = line;
			for (b = 0; b < blocks_per_row; b++, out += 4) {
				uint32_t p1 = *low++, p2 = *high++;
				if (p2 == 255) {
					uint32_t colour = p1 * 0x01010101u;
					*(uint32_t *)out = colour;
					*(uint32_t *)(out + buff_width) = colour;
					*(uint32_t *)(out + 2 * buff_width) = colour;
					*(uint32_t *)(out + 3 * buff_width) = colour;
				} else {
					const uint32_t *block = cb + 4 * ((p2 << 8) | p1);
					*(uint32_t *)out = block[0];
					*(uint32_t *)(out + buff_width) = block[1];
					*(uint32_t *)(out + 2 * buff_width) = block[2];
					*(uint32_t *)(out + 3 * buff_width) = block[3];
				}
			}
		}
	}
	for (k = 1; k < cores; k++) workers_passes += mcw_wait((int)k);
	workers_frames++;
	return 1;
}
