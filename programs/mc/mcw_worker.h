/*
 * What every program's worker cores run around its own jobs (mcw.h, docs/multicore.md). The
 * program's worker file defines
 *
 *     static uint32_t worker_job(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t core);
 *
 * and then includes this, which makes the entry the machine starts a worker at. It is built
 * bare (no library, no paging, no float instructions), linked with worker.ld to run in the
 * arena, and given to mcw_open() as bytes.
 */
#ifndef MCW_WORKER_H
#define MCW_WORKER_H

#include "mc.h"

__attribute__((section(".text.start"), noreturn)) void mc_worker(uint32_t arg, uint32_t core)
{
	mc_job *job = (mc_job *)(MC_ARENA_PHYS + MC_JOB_AT(core));
	mc_answer *answer = (mc_answer *)(MC_ARENA_PHYS + MC_ANSWER_AT(core));
	uint32_t last = job->seq;

	(void)arg;
	answer->done = last;
	answer->alive = MC_ALIVE | core;
	for (;;) {
		uint32_t seq = job->seq;
		if (seq != last) {
			uint32_t fn = job->fn;
			if (fn == MC_FN_PARK) {
				answer->alive = 0;
				answer->done = seq;
				__asm__ volatile("ebreak");	/* the machine parks this core; its stores are kept */
			}
			answer->result = worker_job(fn, job->a0, job->a1, core);
			answer->done = seq;
			last = seq;
		}
		__asm__ volatile("wfi");	/* asleep until the job's first word changes */
	}
}

#endif
