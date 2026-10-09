// Worker cores (docs/multicore.md): what the program on core 0 and the code on the workers
// agree on. All of it is in the arena, 4 MiB of the GPU device's memory that /dev/gpu maps, so
// a Linux program and a worker (which runs with paging off) see the same bytes.
//
// A core reads memory as the last pass left it, plus its own writes: what one core stores,
// another sees one pass later. Two cores must not store to the same 16 bytes (one texel) in
// one pass, so everything here that changes hands is 16 bytes with one writer.
#ifndef MC_H
#define MC_H
#include <stdint.h>

#define MC_ARENA_PHYS 0x86C00000u
#define MC_ARENA_SIZE 0x00400000u
#define MC_ARENA_GPU_OFFSET (MC_ARENA_PHYS - 0x86000000u)   // where it is in /dev/gpu
#define MC_MAX_CORES 16

// the machine's own mailbox: { MC_START, pc, sp, a0 } starts parked core k (it gets k in a1)
#define MC_START 0x5453434du
#define MC_START_AT(k) (16u * (k))
// then, to the code that runs there: a job (core 0 writes it) and its answer (the worker's)
#define MC_JOB_AT(k) (0x400u + 64u * (k))
#define MC_ANSWER_AT(k) (0x400u + 64u * (k) + 16u)
#define MC_CODE_AT 0x1000u                        // the workers' code is linked to run here
#define MC_STACK_TOP(k) (0x10000u + 0x10000u * (k))   // 64 KiB a worker, below this
#define MC_DATA_AT 0x100000u                      // the rest is the program's

#define MC_ALIVE 0x600d0000u
#define MC_FN_PARK 0xffffffffu

typedef struct { volatile uint32_t seq, fn, a0, a1; } mc_job;          // seq changes: a new job
typedef struct { volatile uint32_t done, result, alive, pad; } mc_answer;   // done == seq: finished

// the jobs worker.c knows
enum { MC_FN_PRIMES, MC_FN_FILL, MC_FN_SUM, MC_FN_STRIPE };

// The pause hint ends this core's pass: its stores reach the others, and theirs reach it.
#define mc_next_pass() __asm__ volatile(".word 0x0100000f")

#endif
