/*
 * What Red Alert's worker cores run (docs/ralert.md, docs/multicore.md): its jobs, around which
 * programs/mc/mcw_worker.h puts the entry and the loop. Bare metal, linked at the arena's
 * MC_CODE_AT. A worker has no paging: everything it touches is in the arena, which the game
 * has at the address it has here, or is given by its physical address. The jobs: a share of
 * a movie frame's blocks, and a frame's block pointers unpacked.
 */
#include "mc.h"
#include "workers.h"

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

static uint32_t worker_job(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t core)
{
	(void)core;
	if (fn == WORKERS_FN_UNVQ)
		unvq_rows((const struct workers_unvq *)a0, a1 >> 16, a1 & 0xffff);
	else if (fn == WORKERS_FN_LCW)
		lcw((const struct workers_lcw *)a0);
	return 0;
}

#include "mcw_worker.h"	/* the entry and the loop that takes jobs: every program's workers have it */
