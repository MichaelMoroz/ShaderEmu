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
#define MC_PAGE_SIZE 0x3000u   // (one page for the first 16 cores; the rest is cores 16 to 63's)
#define MC_PAGE_GPU_OFFSET (MC_PAGE_PHYS - 0x86000000u)   // where it is in /dev/gpu
#define MC_MAX_CORES 64

// { MC_START, pc, sp, root } starts core k there in user mode with k in a0, on the page table
// whose first page is number `root`. A program gets its cores and its root from the kernel:
// MC_WORKERS on /dev/gpu, with { how many it wants, a bit a core it got, its root }
#define MC_START 0x5553434du
#define MC_STOP 0x5453434du   // there instead: the core parks (the kernel's, when the program is gone)
#define MC_START_AT(k) (16u * (k))
#define MC_WORKERS 0xc00c4704u   // _IOWR('G', 4, three words)
#define MC_SHAPE 0x40084706u     // _IOW('G', 6, two words): the geometry, four bits a worker
// a machine of more than 16 cores: the geometry in eight words, and { how many, root, a bit a
// core in two words } for the workers
#define MC_SHAPE_ALL 0x40204707u     // _IOW('G', 7, eight words)
#define MC_WORKERS_ALL 0xc0104708u   // _IOWR('G', 8, four words)
// A program whose workers are only lent to it says so (mcw_lend) and looks at the count at
// MC_ASKED_AT: it goes up when another program is waiting for them.
#define MC_LENT 0x4709u              // _IO('G', 9)
#define MC_ASKED_AT 0xf20u
// core k's 64 bytes: a job (core 0 writes it), its answer (the worker's), a fault (the
// worker's, written by the machine) and the word that lets it go on (core 0's)
// (cores 16 and up have theirs after the first 16 cores' system calls)
#define MC_JOB_AT(k) ((k) < 16 ? 0x400u + 64u * (k) : 0x1000u + 64u * (k))
#define MC_ANSWER_AT(k) (MC_JOB_AT(k) + 16u)
#define MC_FAULT_AT(k) (MC_JOB_AT(k) + 32u)
#define MC_RESUME_AT(k) (MC_JOB_AT(k) + 48u)
// a worker's system call, for core 0 to make: a7 and a0 to a5 (the fault's cause is 8); the
// answer goes in the resume texel's second word before its first
#define MC_CALL_AT(k) ((k) < 16 ? 0x800u + 32u * (k) : 0x2000u + 32u * (k))

#define MC_ALIVE 0x600d0000u

typedef struct { volatile uint32_t seq, fn, a0, a1; } mc_job;                  // seq changes: a new job
typedef struct { volatile uint32_t done, result, alive, pad; } mc_answer;      // done == seq: finished
typedef struct { volatile uint32_t count, cause, address, pc; } mc_fault;      // count != resume: stopped

// The pause hint ends this core's pass: its stores reach the others, and theirs reach it. On a
// worker it is a sleep until the first word of its job changes.
#define mc_next_pass() __asm__ volatile(".word 0x0100000f")

#endif
