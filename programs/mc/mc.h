// Worker cores (docs/multicore.md): the mailboxes, which are what the machine, the library
// (mcw.h) and a worker agree on. They are one page of the GPU device's memory, which /dev/gpu
// maps, at a physical address the machine knows; everything else a worker touches is the
// program's own memory, which it has as the program has it.
//
// A core reads memory as the last pass left it, plus its own writes: what one core stores,
// another sees one pass later. Two cores must not store to the same 16 bytes (one texel of
// the machine's memory: addresses that differ only in their last four bits) in one pass.
#ifndef MC_H
#define MC_H
#include <stdint.h>

#define MC_PAGE_PHYS 0x86C00000u
#define MC_PAGE_SIZE 0x1000u
#define MC_PAGE_GPU_OFFSET (MC_PAGE_PHYS - 0x86000000u)   // where it is in /dev/gpu
#define MC_MAX_CORES 16

// { MC_START, pc, sp, root } starts core k there in user mode with k in a0, on the page table
// whose first page is number `root`. A program gets its cores and its root from the kernel:
// MC_WORKERS on /dev/gpu, with { how many it wants, a bit a core it got, its root }
#define MC_START 0x5553434du
#define MC_STOP 0x5453434du   // there instead: the core parks (the kernel's, when the program is gone)
#define MC_START_AT(k) (16u * (k))
#define MC_WORKERS 0xc00c4704u   // _IOWR('G', 4, three words)
// core k's 64 bytes: a job (core 0 writes it), its answer (the worker's), a fault (the
// worker's, written by the machine) and the word that lets it go on (core 0's)
#define MC_JOB_AT(k) (0x400u + 64u * (k))
#define MC_ANSWER_AT(k) (0x400u + 64u * (k) + 16u)
#define MC_FAULT_AT(k) (0x400u + 64u * (k) + 32u)
#define MC_RESUME_AT(k) (0x400u + 64u * (k) + 48u)
// a worker's system call, for core 0 to make: a7 and a0 to a5 (the fault's cause is 8); the
// answer goes in the resume texel's second word before its first
#define MC_CALL_AT(k) (0x800u + 32u * (k))

#define MC_ALIVE 0x600d0000u

typedef struct { volatile uint32_t seq, fn, a0, a1; } mc_job;                  // seq changes: a new job
typedef struct { volatile uint32_t done, result, alive, pad; } mc_answer;      // done == seq: finished
typedef struct { volatile uint32_t count, cause, address, pc; } mc_fault;      // count != resume: stopped

// The pause hint ends this core's pass: its stores reach the others, and theirs reach it. On a
// worker it is a sleep until the first word of its job changes.
#define mc_next_pass() __asm__ volatile(".word 0x0100000f")

#endif
