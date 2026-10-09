// What a worker core runs (docs/multicore.md): bare metal, linked at the arena's MC_CODE_AT,
// started by the machine at mc_worker with its stack set and its core number in a1. It takes
// jobs from its mailbox and ends its pass with wfi when there is none.
#include "mc.h"

#define ARENA(at) ((void*)(MC_ARENA_PHYS + (at)))

// How many primes there are in [from, to): trial division, nothing stored.
uint32_t mc_primes(uint32_t from, uint32_t to) {
    uint32_t count = 0;
    for (uint32_t n = from; n < to; n++) {
        if (n < 2) continue;
        int prime = 1;
        for (uint32_t d = 2; d * d <= n; d++)
            if (n % d == 0) { prime = 0; break; }
        count += prime;
    }
    return count;
}

uint32_t mc_pattern(uint32_t seed, uint32_t i) { return (seed + i) * 2654435761u ^ (i >> 3); }

// Fills `words` words of the arena from `at` with a pattern and returns their sum.
uint32_t mc_fill(uint32_t at, uint32_t words, uint32_t seed) {
    uint32_t* p = ARENA(at);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < words; i++) {
        p[i] = mc_pattern(seed, i);
        sum += p[i];
    }
    return sum;
}

uint32_t mc_sum(uint32_t at, uint32_t words) {
    const uint32_t* p = ARENA(at);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < words; i++) sum += p[i];
    return sum;
}

// Every third texel of a buffer that three workers fill between them: 16 bytes each, a
// neighbour's on either side.
uint32_t mc_stripe(uint32_t at, uint32_t texels, uint32_t which) {
    uint32_t* p = ARENA(at);
    uint32_t sum = 0;
    for (uint32_t t = which; t < texels; t += 3)
        for (uint32_t w = 0; w < 4; w++) {
            p[4 * t + w] = mc_pattern(0x57a1, 4 * t + w);
            sum += p[4 * t + w];
        }
    return sum;
}

__attribute__((section(".text.start"), noreturn)) void mc_worker(uint32_t arg, uint32_t core) {
    mc_job* job = ARENA(MC_JOB_AT(core));
    mc_answer* answer = ARENA(MC_ANSWER_AT(core));
    uint32_t last = job->seq;
    (void)arg;
    answer->done = last;
    answer->alive = MC_ALIVE | core;
    for (;;) {
        uint32_t seq = job->seq;
        if (seq != last) {
            uint32_t fn = job->fn, a0 = job->a0, a1 = job->a1, result = 0;
            if (fn == MC_FN_PARK) {
                answer->alive = 0;
                answer->done = seq;
                __asm__ volatile("ebreak");   // the machine parks this core; its stores are kept
            }
            else if (fn == MC_FN_PRIMES) result = mc_primes(a0, a1);
            else if (fn == MC_FN_FILL) result = mc_fill(a0, a1, core);
            else if (fn == MC_FN_SUM) result = mc_sum(a0, a1);
            else if (fn == MC_FN_STRIPE) result = mc_stripe(a0, a1, core - 1);
            answer->result = result;
            answer->done = seq;
            last = seq;
        }
        __asm__ volatile("wfi");   // nothing changes until the next pass
    }
}
