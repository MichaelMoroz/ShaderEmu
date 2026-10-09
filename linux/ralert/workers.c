/*
 * Red Alert's side of the machine's worker cores (workers.h): its jobs, which are functions
 * here that a worker calls in the game's own memory, over the library every program that
 * uses the workers has (programs/mc/mcw.h).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mcw.h"
#include "workers.h"

unsigned workers_frames, workers_passes;

static int count = -1, at_exit;
static struct workers_unvq shared __attribute__((aligned(16)));
static struct workers_lcw unpacking __attribute__((aligned(16)));
static int unpacker_busy;
/* the memory the workers are to write a frame into */
static uint8_t *target;
static uint32_t target_bytes;
/* what workers_alloc() gave: whole 16 bytes each, so no core stores beside another's */
#define HELD_MOST 64
static uint8_t *held[HELD_MOST];
static uint32_t held_bytes[HELD_MOST];

/* ---- the jobs ---- */

/* UnVQ_4x4 (common/unvqbuff.cpp) for the block rows from `first` up to `end`. */
static void unvq_rows(const struct workers_unvq *job, uint32_t first, uint32_t end)
{
	const uint32_t *codebook = (const uint32_t *)job->codebook;
	const uint8_t *low = job->pointers + first * job->blocks_per_row;
	const uint8_t *high = low + job->blocks_per_row * job->num_rows;
	uint32_t width = job->buff_width;
	uint8_t *row = job->buffer + first * 4 * width;

	for (uint32_t r = first; r < end; r++, row += 4 * width) {
		uint8_t *out = row;
		for (uint32_t b = 0; b < job->blocks_per_row; b++, out += 4) {
			uint32_t p1 = *low++, p2 = *high++;
			if (p2 == 255) {
				uint32_t colour = p1 * 0x01010101u;
				*(uint32_t *)out = colour;
				*(uint32_t *)(out + width) = colour;
				*(uint32_t *)(out + 2 * width) = colour;
				*(uint32_t *)(out + 3 * width) = colour;
			} else {
				const uint32_t *block = codebook + 4 * ((p2 << 8) | p1);
				*(uint32_t *)out = block[0];
				*(uint32_t *)(out + width) = block[1];
				*(uint32_t *)(out + 2 * width) = block[2];
				*(uint32_t *)(out + 3 * width) = block[3];
			}
		}
	}
}

/* LCW_Uncompress (common/lcw.cpp), which may unpack into the front of the buffer it reads the back of. */
static void lcw(const struct workers_lcw *job)
{
	const uint8_t *source = job->source;
	uint8_t *dest = job->dest, *to = dest, *end = dest + job->length;
	const uint8_t *from;
	uint32_t count, op;

	while (to < end) {
		op = *source++;
		if (!(op & 0x80)) {
			count = (op >> 4) + 3;
			from = to - (*source++ + ((op & 0x0f) << 8));
		} else if (!(op & 0x40)) {
			if (op == 0x80)
				return;
			count = op & 0x3f;
			if (count > (uint32_t)(end - to))
				count = end - to;
			while (count--)
				*to++ = *source++;
			continue;
		} else if (op == 0xfe) {
			uint8_t value;
			count = source[0] + (source[1] << 8);
			value = source[2];
			source += 3;
			if (count > (uint32_t)(end - to))
				count = end - to;
			while (count--)
				*to++ = value;
			continue;
		} else if (op == 0xff) {
			count = source[0] + (source[1] << 8);
			from = dest + source[2] + (source[3] << 8);
			source += 4;
		} else {
			count = (op & 0x3f) + 3;
			from = dest + source[0] + (source[1] << 8);
			source += 2;
		}
		if (count > (uint32_t)(end - to))
			count = end - to;
		while (count--)
			*to++ = *from++;
	}
}

static uint32_t unvq_job(uint32_t job, uint32_t rows)
{
	unvq_rows((const struct workers_unvq *)job, rows >> 16, rows & 0xffff);
	return 0;
}

static uint32_t lcw_job(uint32_t job, uint32_t unused)
{
	(void)unused;
	lcw((const struct workers_lcw *)job);
	return 0;
}

/* ---- the game's side ---- */

int workers_count(void)
{
	const char *limit = getenv("RALERT_WORKERS");

	if (count >= 0) return count;
	count = mcw_open(limit ? atoi(limit) : MC_MAX_CORES - 1);
	if (count > 0 && !at_exit) {
		at_exit = 1;
		atexit(mcw_close);
	}
	if (getenv("RALERT_STATS_MS") || getenv("RALERT_FRAMES")) fprintf(stderr, "ralert: %d worker cores\n", count);
	return count;
}

void *workers_alloc(unsigned bytes)
{
	void *memory;
	unsigned i, at;

	if (workers_count() == 0) return NULL;
	for (i = 0; i < HELD_MOST && held[i]; i++) {}
	bytes = (bytes + 15) & ~15u;
	if (i == HELD_MOST || posix_memalign(&memory, 16, bytes) != 0) return NULL;
	/* its pages here now, or a worker stops at each until this core looks */
	for (at = 0; at < bytes; at += 4096) ((volatile uint8_t *)memory)[at] = 0;
	((volatile uint8_t *)memory)[bytes - 1] = 0;
	held[i] = memory;
	held_bytes[i] = bytes;
	return memory;
}

int workers_owns(const void *memory)
{
	unsigned i;

	for (i = 0; i < HELD_MOST; i++)
		if (held[i] && (const uint8_t *)memory >= held[i] && (const uint8_t *)memory < held[i] + held_bytes[i]) return 1;
	return 0;
}

void workers_release(void *memory)
{
	unsigned i;

	for (i = 0; i < HELD_MOST; i++)
		if (memory && held[i] == memory) {
			workers_lcw_wait();
			held[i] = NULL;
			free(memory);
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
	unpacking.source = source;
	unpacking.dest = dest;
	unpacking.length = length;
	mcw_post(count, lcw_job, (uint32_t)&unpacking, 0);
	unpacker_busy = 1;
	return 1;
}

void workers_target(void *memory, unsigned physical, unsigned bytes)
{
	(void)physical;
	target = memory;
	target_bytes = memory ? bytes : 0;
	/* the movie is over: the cores are another program's to have until the next one */
	if (!memory && count > 0) {
		workers_lcw_wait();
		mcw_close();
		count = -1;
	}
}

int workers_unvq(uint8_t *codebook, uint8_t *pointers, uint8_t *buffer, unsigned blocks_per_row, unsigned num_rows,
		 unsigned buff_width)
{
	unsigned bytes = num_rows * 4 * buff_width, cores, share, row, k;

	/* rows of whole texels, or two cores would store to one */
	if (workers_count() == 0 || (buff_width & 15) != 0 || blocks_per_row * 4 > buff_width || num_rows < 2 || !target
	    || buffer < target || buffer + bytes > target + target_bytes || ((uintptr_t)buffer & 15) != 0)
		return 0;
	shared.codebook = codebook;
	shared.pointers = pointers;
	shared.buffer = buffer;
	shared.blocks_per_row = blocks_per_row;
	shared.num_rows = num_rows;
	shared.buff_width = buff_width;

	/* (the worker that unpacks the next frame is left to it) */
	if (unpacker_busy && mcw_done(count)) unpacker_busy = 0;
	cores = (unsigned)count + 1 - (unpacker_busy ? 1 : 0);
	if (cores > num_rows) cores = num_rows;
	share = (num_rows + cores - 1) / cores;
	/* the first share is this core's, the rest a worker's each */
	for (k = 1, row = share; k < cores && row < num_rows; k++, row += share) {
		unsigned end = row + share < num_rows ? row + share : num_rows;
		mcw_post((int)k, unvq_job, (uint32_t)&shared, (row << 16) | end);
	}
	cores = k;
	mc_next_pass();	/* the workers see the frame and their jobs from here on */
	workers_passes++;
	unvq_rows(&shared, 0, share);	/* this core's rows */
	for (k = 1; k < cores; k++) workers_passes += mcw_wait((int)k);
	workers_frames++;
	return 1;
}
