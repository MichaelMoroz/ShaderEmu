/*
 * Red Alert's side of the machine's worker cores (workers.h): the arena, the workers' code
 * put into it, their start, and jobs.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "mc.h"
#include "workers.h"
#include "worker_code.h"	/* worker.c built for the arena: worker_code[], made by build.sh */

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

unsigned workers_frames, workers_passes;

static uint8_t *arena;		/* MC_ARENA_PHYS, here as on the workers */
static int count = -1;
static uint32_t seq[MC_MAX_CORES];
static uint32_t top = MC_DATA_AT, held;
static struct workers_unvq *shared;
static struct workers_lcw *unpacking;
static int unpacker_busy;
/* memory outside the arena that the workers may write: here, and where it is for them */
static uint8_t *target, *target_physical;
static uint32_t target_bytes;

static mc_job *job_of(int k) { return (mc_job *)(arena + MC_JOB_AT(k)); }
static mc_answer *answer_of(int k) { return (mc_answer *)(arena + MC_ANSWER_AT(k)); }

int workers_count(void)
{
	const char *limit = getenv("RALERT_WORKERS");
	int want = limit ? atoi(limit) : MC_MAX_CORES - 1, fd, k;

	if (count >= 0) return count;
	count = 0;
	if (want <= 0 || (fd = open("/dev/gpu", O_RDWR)) < 0) return 0;
	if (want > MC_MAX_CORES - 1) want = MC_MAX_CORES - 1;
	arena = mmap((void *)MC_ARENA_PHYS, MC_ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd,
		     MC_ARENA_GPU_OFFSET);
	close(fd);
	if (arena != (uint8_t *)MC_ARENA_PHYS) {
		if (arena != MAP_FAILED) munmap(arena, MC_ARENA_SIZE);
		arena = NULL;
		return 0;
	}
	/* workers an earlier program left running park first: they start again below */
	for (k = 1; k <= want; k++) {
		mc_job *j = job_of(k);
		j->fn = MC_FN_PARK;
		j->seq = j->seq + 1;
	}
	mc_next_pass();
	mc_next_pass();
	mc_next_pass();
	/* the code, and mailboxes with nothing in them; a pass, so that every core sees them */
	memset(arena, 0, MC_CODE_AT);
	memcpy(arena + MC_CODE_AT, worker_code, sizeof worker_code);
	mc_next_pass();
	for (k = 1; k <= want; k++) {
		volatile uint32_t *start = (volatile uint32_t *)(arena + MC_START_AT(k));
		start[1] = MC_ARENA_PHYS + MC_CODE_AT;
		start[2] = MC_ARENA_PHYS + MC_STACK_TOP(k);
		start[3] = 0;
		start[0] = MC_START;
	}
	/* a core that is there says so within a few passes */
	for (k = 0; k < 8; k++) mc_next_pass();
	for (k = 1; k <= want && answer_of(k)->alive == (MC_ALIVE | (uint32_t)k); k++) count = k;
	for (k = 1; k <= want; k++) *(volatile uint32_t *)(arena + MC_START_AT(k)) = 0;
	if (getenv("RALERT_STATS_MS") || getenv("RALERT_FRAMES")) fprintf(stderr, "ralert: %d worker cores\n", count);
	return count;
}

void *workers_alloc(unsigned bytes)
{
	void *memory;

	bytes = (bytes + 15) & ~15u;
	if (workers_count() == 0 || top + bytes > MC_ARENA_SIZE) return NULL;
	memory = arena + top;
	top += bytes;
	held++;
	return memory;
}

int workers_owns(const void *memory)
{
	return arena && (const uint8_t *)memory >= arena && (const uint8_t *)memory < arena + MC_ARENA_SIZE;
}

void workers_release(void *memory)
{
	if (workers_owns(memory) && held > 0 && --held == 0) {
		workers_lcw_wait();
		top = MC_DATA_AT;
		shared = NULL;
		unpacking = NULL;
	}
}

/* The last worker unpacks; the others, and it when it has nothing to unpack, share frames. */
void workers_lcw_wait(void)
{
	if (!unpacker_busy) return;
	while (answer_of(count)->done != seq[count]) {
		mc_next_pass();
		workers_passes++;
	}
	unpacker_busy = 0;
}

int workers_lcw(const uint8_t *source, uint8_t *dest, unsigned length)
{
	mc_job *j;

	if (workers_count() < 2 || !workers_owns(source) || !workers_owns(dest) || ((uintptr_t)dest & 15) != 0) return 0;
	workers_lcw_wait();
	if (!unpacking) {
		if (!(unpacking = workers_alloc(sizeof *unpacking))) return 0;
		held--;	/* ours: it goes with the movie's last buffer */
	}
	unpacking->source = source;
	unpacking->dest = dest;
	unpacking->length = length;
	j = job_of(count);
	j->fn = WORKERS_FN_LCW;
	j->a0 = (uint32_t)unpacking;
	j->a1 = 0;
	j->seq = ++seq[count];
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
	if (unpacker_busy && answer_of(count)->done == seq[count]) unpacker_busy = 0;
	cores = (unsigned)count + 1 - (unpacker_busy ? 1 : 0);
	if (cores > num_rows) cores = num_rows;
	share = (num_rows + cores - 1) / cores;
	/* the first share is this core's, the rest a worker's each */
	for (k = 1, row = share; k < cores && row < num_rows; k++, row += share) {
		unsigned end = row + share < num_rows ? row + share : num_rows;
		mc_job *j = job_of((int)k);
		j->fn = WORKERS_FN_UNVQ;
		j->a0 = (uint32_t)shared;
		j->a1 = (row << 16) | end;
		j->seq = ++seq[k];
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
	for (k = 1; k < cores; k++) {
		while (answer_of((int)k)->done != seq[k]) {
			mc_next_pass();
			workers_passes++;
		}
	}
	workers_frames++;
	return 1;
}
