// What mctest's worker cores run (docs/multicore.md): the jobs, around which mcw_worker.h puts
// the entry and the loop every program's workers have. Bare metal, linked at the arena's MC_CODE_AT.
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

static uint32_t worker_job(uint32_t fn, uint32_t a0, uint32_t a1, uint32_t core) {
    if (fn == MC_FN_PRIMES) return mc_primes(a0, a1);
    if (fn == MC_FN_FILL) return mc_fill(a0, a1, core);
    if (fn == MC_FN_SUM) return mc_sum(a0, a1);
    if (fn == MC_FN_STRIPE) return mc_stripe(a0, a1, core - 1);
    return 0;
}

#include "mcw_worker.h"   // the entry and the loop that takes jobs
