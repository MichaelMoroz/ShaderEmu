/*
 * lanetest: the lanes of every worker (docs/machine/lanes.md). Each worker makes its share of an
 * array with mcw_lanes(), then makes it again with plain calls and compares; the first core says
 * how many words differ, a sum of the array, and how long the lanes took.
 *
 *   lanetest [WORKERS [INDICES [TEXELS [ROUNDS]]]]
 *
 * An index is ROUNDS turns of integer and float arithmetic a word (more for some indices, so the
 * lanes' paths part), and indices 5 and 9 of every share (on the field, one in 9,973 from 5) are
 * longer than a pass: their lanes skip them and the library makes them as plain calls.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mcw.h"

struct job {
	uint32_t *out;		/* the array: TEXELS * 4 words an index */
	uint32_t texels, rounds, per, total;
	uint32_t mode;		/* 0 lanes and the check, 1 lanes alone, 2 plain calls alone, 3 the field and the check */
	uint32_t pad[2];
};
static struct job job __attribute__((aligned(16)));
static struct { volatile uint32_t wrong, lanes, cycles, pad; } said[MC_MAX_CORES] __attribute__((aligned(16)));

static uint32_t word_of(uint32_t i, uint32_t rounds)
{
	uint32_t h = i * 2654435761u, r;
	float f = (float)(i & 1023) * 0.001f;

	for (r = 0; r < rounds; r++) {
		h = (h ^ (h >> 13)) * 0x5bd1e995u + r;
		f = f * 0.999f + (float)(h & 255) * 0.0001f;
		if (h & 0x100) f = __builtin_sqrtf(f + 1.0f);
	}
	return h ^ (uint32_t)(f * 65536.0f);
}

/* How long index i is: two of every share (one in 9,973 of the field's job) longer than a pass, which
 * a lane skips and the library makes (not in a timing) */
static uint32_t rounds_of(const struct job *j, uint32_t i)
{
	uint32_t rounds = j->rounds + (i % 7) * (j->rounds / 4);

	if ((j->mode == 0 && (i % j->per == 5 || i % j->per == 9)) || (j->mode >= 3 && i % 9973 == 5)) rounds += 20000;
	return rounds;
}

/* index i of the array: its texels' words */
static void make(uint32_t i, void *ctx)
{
	const struct job *j = ctx;
	uint32_t *o = j->out + 4 * j->texels * i, rounds = rounds_of(j, i), t;

	for (t = 0; t < 4 * j->texels; t++) o[t] = word_of(i * 64 + t, rounds);
}

static uint32_t cycles(void)
{
	uint32_t c;

	__asm__ volatile("rdcycle %0" : "=r"(c));
	return c;
}

/* worker k's share: by lanes, then compared with plain calls */
static uint32_t share(uint32_t k, uint32_t unused)
{
	uint32_t first = (k - 1) * job.per, count = job.per, i, t, wrong = 0, c0;
	(void)unused;
	if (first >= job.total) return 0;
	if (first + count > job.total) count = job.total - first;
	c0 = cycles();
	if (job.mode == 4)
		;	/* (the field made them) */
	else if (job.mode == 2)
		for (i = first; i < first + count; i++) make(i, &job);
	else
		said[k].lanes = (uint32_t)mcw_lanes(make, &job, first, count, job.out + 4 * job.texels * first, job.texels);
	said[k].cycles = cycles() - c0;
	for (i = first; i < first + count && (job.mode == 0 || job.mode == 4); i++) {
		uint32_t rounds = rounds_of(&job, i);

		for (t = 0; t < 4 * job.texels; t++) wrong += job.out[4 * job.texels * i + t] != word_of(i * 64 + t, rounds);
	}
	said[k].wrong = wrong;
	return 0;
}

static unsigned now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
}

int main(int argc, char **argv)
{
	extern char __DATA_BEGIN__[], _end[], _start[], etext[];
	const volatile char *code;
	int want = argc > 1 ? atoi(argv[1]) : 255, workers, k, lanes = 0;
	uint32_t i, sum = 0, wrong = 0, most = 0, least = ~0u;
	uint64_t all = 0;
	unsigned t0, ms;

	job.per = argc > 2 ? (uint32_t)atoi(argv[2]) : 200;
	job.texels = argc > 3 ? (uint32_t)atoi(argv[3]) : 1;
	job.rounds = argc > 4 ? (uint32_t)atoi(argv[4]) : 40;
	job.mode = argc > 5 ? (uint32_t)atoi(argv[5]) : 0;
	workers = mcw_open(want);
	if (workers <= 0) {
		printf("lanetest: no workers\n");
		return 1;
	}
	job.total = job.per * (uint32_t)workers;
	job.out = mcw_alloc(job.total * job.texels * 16);
	memset(job.out, 0, job.total * job.texels * 16);
	mcw_touch(job.out, job.total * job.texels * 16);
	mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
	for (code = (const char *)((uintptr_t)_start & ~4095u); code < etext; code += 4096) (void)*code;
	t0 = now_ms();
	if (job.mode == 3) {
		/* the whole array the field's job, from this core, then the workers check their shares */
		extern unsigned mcw_field_stuck;	/* (mcw.c: indices the field's lanes left to this core) */
		unsigned n;

		printf("lanetest: the field has %d lanes\n", mcw_field_lanes());
		mcw_field_apart(1);	/* (the skipped indices kept, then made here in two parts: as a program gives them to its workers) */
		mcw_field_begin(make, &job, 0, job.total, job.out, job.texels);
		mcw_field_wait();
		n = mcw_field_skips_make(mcw_field_skips(), 0, 2) + mcw_field_skips_make(mcw_field_skips(), 1, 2);
		ms = now_ms() - t0;
		printf("lanetest: the field: %u ms, %u indices left by the lanes (%u made by the library)\n", ms, n, mcw_field_stuck);
		job.mode = 4;	/* (the workers: the check alone) */
	}
	for (k = 1; k <= workers; k++) mcw_post(k, share, (uint32_t)k, 0);
	for (k = 1; k <= workers; k++) mcw_wait(k);
	if (job.mode != 4) ms = now_ms() - t0;
	for (k = 1; k <= workers; k++) {
		wrong += said[k].wrong;
		all += said[k].cycles;
		if (said[k].lanes < least) least = said[k].lanes;
		if (said[k].lanes > lanes) lanes = (int)said[k].lanes;
		if (said[k].cycles > most) most = said[k].cycles;
	}
	for (i = 0; i < job.total * job.texels * 4; i++) sum = sum * 31 + job.out[i];
	printf("lanetest: %d workers, %u indices of %u texels, %u rounds, %s: %u words wrong, sum %08x, %u ms, lanes %u to %d, the longest share %u instructions, "
	       "%u million instructions in all, %u million a second\n",
	       workers, job.total, job.texels, job.rounds, job.mode == 0 ? "lanes and the check" : job.mode == 4 ? "the field and the check" : job.mode == 1 ? "lanes" : "plain calls", wrong, sum, ms,
	       least, lanes, most, (unsigned)(all / 1000000), ms ? (unsigned)(all / 1000 / ms) : 0);
	printf(wrong ? "lanetest: FAIL\n" : "lanetest: PASS\n");
	mcw_close();
	return wrong != 0;
}
