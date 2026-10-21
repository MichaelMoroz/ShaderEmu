/*
 * steptest: jobs in steps (docs/machine/lanes.md, "Steps"). Index i is work(i) units of hashing, a
 * few up to a few hundred thousand instructions; a step does units while its pass has room, then
 * stores where it is and returns not through. One index in 997 has a first unit longer than a pass,
 * which no lane can make: the library makes it on a core and says so. The field's job, then every
 * worker's share on its own lanes, each compared with plain calls; then the self-test on a worker.
 *
 *   steptest [WORKERS [INDICES]]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mcw.h"

#define MARK 0x57E9u	/* an index's first texel's last word while it is not through */
#define DONE 0xD0E5u
#define UNIT 24		/* a unit's turns: some 200 instructions */

struct job {
	uint32_t *out;	/* two texels an index */
	uint32_t total, per, pad;
};
static struct job job __attribute__((aligned(16)));
static struct { volatile uint32_t wrong, rounds, too_long, pad; } said[MC_MAX_CORES] __attribute__((aligned(16)));
static struct mcw_steps_probe probed __attribute__((aligned(16)));

static uint32_t work(uint32_t i) { return i % 101 == 7 ? 600 + i % 300 : 1 + (i % 37) * 3; }

static uint32_t unit(uint32_t h, uint32_t turns)
{
	uint32_t r;

	for (r = 0; r < turns; r++) h = (h ^ (h >> 13)) * 0x5bd1e995u + r;
	return h;
}

/* one in 997: its first unit 60 times as long, longer than a pass */
static uint32_t turns_of(uint32_t i, uint32_t u) { return u == 0 && i % 997 == 3 ? 60 * UNIT : UNIT; }

static uint32_t expected(uint32_t i)
{
	uint32_t h = i * 2654435761u, u;

	for (u = 0; u < work(i); u++) h = unit(h, turns_of(i, u));
	return h;
}

static uint32_t step(uint32_t i, void *ctx, uint32_t round)
{
	const struct job *j = ctx;
	uint32_t *o = j->out + 8 * i, h, u, w = work(i);

	if (round == 0)
		h = i * 2654435761u, u = 0;
	else if (o[3] != MARK)
		return 0;	/* (through in an earlier round: nothing written) */
	else
		h = o[4] ^ 0xa5a5a5a5u, u = o[1];	/* (both texels: a step not through writes two, one through one) */
	for (; u < w; u++) {
		if (mcw_steps_left() < 600) {
			o[0] = h, o[1] = u, o[2] = w, o[3] = MARK;
			o[4] = h ^ 0xa5a5a5a5u, o[5] = u, o[6] = i, o[7] = round;
			return 1;
		}
		h = unit(h, turns_of(i, u));
	}
	o[0] = h, o[1] = w, o[2] = i, o[3] = DONE;
	return 0;
}

static uint32_t check(uint32_t first, uint32_t count)
{
	uint32_t i, wrong = 0;

	for (i = first; i < first + count; i++)
		wrong += job.out[8 * i] != expected(i) || job.out[8 * i + 3] != DONE;
	return wrong;
}

/* worker k's share: checked (mode 0), or made on its lanes in steps and checked (mode 1) */
static uint32_t share(uint32_t k, uint32_t mode)
{
	uint32_t first = (k - 1) * job.per, count = job.per;
	struct mcw_steps_report r = {0, 0};

	if (first >= job.total) return 0;
	if (first + count > job.total) count = job.total - first;
	if (mode == 1) mcw_lanes_steps(step, &job, first, count, job.out + 8 * first, 2, &r);
	said[k].rounds = r.rounds, said[k].too_long = r.too_long;
	said[k].wrong = check(first, count);
	return 0;
}

static uint32_t probe(uint32_t first, uint32_t count)
{
	probed = mcw_steps_probe(step, &job, first, count, 0);
	return 0;
}

static unsigned now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
}

static uint32_t run_shares(int workers, uint32_t mode, uint32_t *rounds, uint32_t *too_long)
{
	uint32_t wrong = 0;
	int k;

	for (k = 1; k <= workers; k++) mcw_post(k, share, (uint32_t)k, mode);
	for (k = 1; k <= workers; k++) mcw_wait(k);
	*rounds = 0, *too_long = 0;
	for (k = 1; k <= workers; k++) {
		wrong += said[k].wrong, *too_long += said[k].too_long;
		if (said[k].rounds > *rounds) *rounds = said[k].rounds;
	}
	return wrong;
}

int main(int argc, char **argv)
{
	extern char __DATA_BEGIN__[], _end[], _start[], etext[];
	const volatile char *code;
	int want = argc > 1 ? atoi(argv[1]) : 255, workers;
	uint32_t i, sum = 0, wrong, all_wrong = 0, rounds, too_long, bytes;
	unsigned t0;
	struct mcw_steps_report r;

	workers = mcw_open(want);
	if (workers <= 0) {
		printf("steptest: no workers\n");
		return 1;
	}
	job.total = argc > 2 ? (uint32_t)atoi(argv[2]) : 20000;
	job.per = (job.total + (uint32_t)workers - 1) / (uint32_t)workers;
	bytes = job.total * 32;
	job.out = mcw_alloc(bytes);
	mcw_touch(job.out, bytes);
	mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
	for (code = (const char *)((uintptr_t)_start & ~4095u); code < etext; code += 4096) (void)*code;

	/* the field's job, from this core */
	memset(job.out, 0, bytes);
	t0 = now_ms();
	mcw_field_steps(step, &job, 0, job.total, job.out, 2);
	mcw_field_wait();
	r = mcw_field_steps_report();
	printf("steptest: the field (%d lanes): %u indices in %u rounds, %u steps too long for a lane, %u ms\n",
	       mcw_field_lanes(), job.total, r.rounds, r.too_long, now_ms() - t0);
	wrong = run_shares(workers, 0, &rounds, &too_long);
	for (i = 0; i < job.total; i++) sum = sum * 31 + job.out[8 * i];
	printf("steptest: the field: %u indices wrong, sum %08x\n", wrong, sum);
	all_wrong += wrong;

	/* every worker's share on its own lanes */
	memset(job.out, 0, bytes);
	t0 = now_ms();
	wrong = run_shares(workers, 1, &rounds, &too_long);
	for (sum = 0, i = 0; i < job.total; i++) sum = sum * 31 + job.out[8 * i];
	printf("steptest: %d workers' lanes: the most rounds %u, %u steps too long for a lane, %u ms, %u indices wrong, sum %08x\n",
	       workers, rounds, too_long, now_ms() - t0, wrong, sum);
	all_wrong += wrong;

	/* the self-test, on a worker: the long indices among them */
	memset(job.out, 0, bytes);
	mcw_post(1, probe, 0, job.total < 2000 ? job.total : 2000);
	mcw_wait(1);
	printf("steptest: the self-test: %u steps, %u rounds at most, %u instructions a step on average, the longest %u, %u over a pass\n",
	       probed.steps, probed.rounds, probed.mean, probed.longest, probed.over);
	printf(all_wrong ? "steptest: FAIL\n" : "steptest: PASS\n");
	mcw_close();
	return all_wrong != 0;
}
