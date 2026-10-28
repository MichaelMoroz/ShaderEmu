/* Westwood's VQA pieces (docs/system/media.md, "Films"): a frame's 4x4 or 4x2 blocks from a codebook and
 * two bytes of pointer a block, and LCW ("format 80"). Red Alert's player and libmedia's share
 * them; this file needs nothing of the library but the workers. */
#include <string.h>
#include "m_video.h"

void
m_vq_rows(void *job, uint32_t first, uint32_t end)
{
	const struct m_vq *j = job;
	const uint32_t *codebook = (const uint32_t *)j->codebook;
	const uint8_t *low = j->pointers + first * j->blocks_per_row;
	const uint8_t *high = low + j->blocks_per_row * j->num_rows;
	uint32_t width = j->out_width, r, b;
	uint8_t *row = j->out + first * (uint32_t)j->tall * width;

	if (j->tall == 2) {
		for (r = first; r < end; r++, row += 2 * width) {
			uint8_t *out = row;

			for (b = 0; b < j->blocks_per_row; b++, out += 4) {
				uint32_t p1 = *low++, p2 = *high++;

				if (p2 == 15) {
					uint32_t colour = p1 * 0x01010101u;

					*(uint32_t *)out = colour;
					*(uint32_t *)(out + width) = colour;
				} else {
					const uint32_t *block = codebook + 2 * (p2 << 8 | p1);

					*(uint32_t *)out = block[0];
					*(uint32_t *)(out + width) = block[1];
				}
			}
		}
		return;
	}
	for (r = first; r < end; r++, row += 4 * width) {
		uint8_t *out = row;

		for (b = 0; b < j->blocks_per_row; b++, out += 4) {
			uint32_t p1 = *low++, p2 = *high++;

			if (p2 == 255) {
				uint32_t colour = p1 * 0x01010101u;

				*(uint32_t *)out = colour;
				*(uint32_t *)(out + width) = colour;
				*(uint32_t *)(out + 2 * width) = colour;
				*(uint32_t *)(out + 3 * width) = colour;
			} else {
				const uint32_t *block = codebook + 4 * (p2 << 8 | p1);

				*(uint32_t *)out = block[0];
				*(uint32_t *)(out + width) = block[1];
				*(uint32_t *)(out + 2 * width) = block[2];
				*(uint32_t *)(out + 3 * width) = block[3];
			}
		}
	}
}

/* LCW_Uncompress (vanilla-conquer's common/lcw.cpp), stopping at either end: a copy may
 * overlap what it writes, so it goes a byte at a time, as the game's does. */
uint32_t
m_lcw(const uint8_t *source, uint32_t source_bytes, uint8_t *dest, uint32_t length)
{
	const uint8_t *in_end = source + source_bytes, *from;
	uint8_t *to = dest, *end = dest + length;
	uint32_t count, op;

	if (source_bytes > 0xffffffffu - (uint32_t)(uintptr_t)source)
		in_end = (const uint8_t *)~(uintptr_t)0;
/* what is left of the stream, unsigned: with no end given (media_lcw) it is the rest of the address space, which
 * as a pointer difference is negative where the buffer lies below 2 GB */
#define LEFT ((uintptr_t)in_end - (uintptr_t)source)
	while (to < end && source < in_end) {
		op = *source++;
		if (!(op & 0x80)) {
			if (source >= in_end)
				break;
			count = (op >> 4) + 3;
			from = to - (*source++ + ((op & 0x0f) << 8));
		} else if (!(op & 0x40)) {
			if (op == 0x80)
				break;
			count = op & 0x3f;
			if (count > (uint32_t)(end - to))
				count = (uint32_t)(end - to);
			if (count > LEFT)
				count = (uint32_t)LEFT;
			while (count--)
				*to++ = *source++;
			continue;
		} else if (op == 0xfe) {
			uint8_t value;

			if (LEFT < 3)
				break;
			count = source[0] + (source[1] << 8);
			value = source[2];
			source += 3;
			if (count > (uint32_t)(end - to))
				count = (uint32_t)(end - to);
			memset(to, value, count);
			to += count;
			continue;
		} else if (op == 0xff) {
			if (LEFT < 4)
				break;
			count = source[0] + (source[1] << 8);
			from = dest + source[2] + (source[3] << 8);
			source += 4;
		} else {
			if (LEFT < 2)
				break;
			count = (op & 0x3f) + 3;
			from = dest + source[0] + (source[1] << 8);
			source += 2;
		}
		if (from < dest || from >= to)
			break;	/* (a broken stream: nothing before the output, nothing not made yet) */
		if (count > (uint32_t)(end - to))
			count = (uint32_t)(end - to);
		while (count--)
			*to++ = *from++;
	}
	return (uint32_t)(to - dest);
#undef LEFT
}

uint32_t
media_lcw(const uint8_t *source, uint8_t *dest, uint32_t length)
{
	return m_lcw(source, 0xffffffffu, dest, length);
}

static void
vq_rows(const uint8_t *codebook, const uint8_t *pointers, uint8_t *out, unsigned blocks_per_row, unsigned num_rows,
	unsigned out_width, int workers, int tall)
{
	struct m_vq job;
	mcw_for_opts o;
	int have = mcw_count(), opened = 0;

	job.codebook = codebook, job.pointers = pointers, job.out = out;
	job.blocks_per_row = blocks_per_row, job.num_rows = num_rows, job.out_width = out_width, job.tall = tall;
	if (workers > 0 && !have)
		opened = (have = mcw_open(workers)) > 0;
	/* rows of whole 16 bytes each, or two cores would store to one */
	if (workers <= 0 || have <= 0 || num_rows < 2 || (out_width & 15) || ((uintptr_t)out & 15)
	    || blocks_per_row * 4 > out_width) {
		m_vq_rows(&job, 0, num_rows);
	} else {
		memset(&o, 0, sizeof o);
		o.workers = workers < have ? workers : have;
		o.out_bytes = (uint32_t)tall * out_width;
		mcw_for_opt(num_rows, m_vq_rows, &job, &o);
	}
	if (opened) {
		mcw_for_end();
		mcw_close();
	}
}

void
media_vq4_rows(const uint8_t *codebook, const uint8_t *pointers, uint8_t *out, unsigned blocks_per_row,
	       unsigned num_rows, unsigned out_width, int workers)
{
	vq_rows(codebook, pointers, out, blocks_per_row, num_rows, out_width, workers, 4);
}

void
media_vq2_rows(const uint8_t *codebook, const uint8_t *pointers, uint8_t *out, unsigned blocks_per_row,
	       unsigned num_rows, unsigned out_width, int workers)
{
	vq_rows(codebook, pointers, out, blocks_per_row, num_rows, out_width, workers, 2);
}
