#ifndef MC_H
#define MC_H
#include "layout.h"

// More than one core (CORES > 1, docs/machine/multicore.md). Core 0 is the machine as it always was.
// The others are workers: a block of state each (the CPU's texels, the write cache and the
// float registers; no TLBs kept from one pass to the next), no devices, the same RAM. A worker
// reads RAM as the last commit left it and its own writes; the commit pass merges every core's
// writes, the highest core last. So a core sees another's writes one pass later, and two cores
// must not write the same 16 bytes in one pass.
//
// A worker runs the code of a program on core 0, as a thread of it that the kernel does not
// know of: in user mode, on the page table the program names when it starts it (the kernel
// tells a program its own: core 0 may be in another process by then). Its mailbox is 64
// bytes of RAM at MC_MBOX + 0x4000 + 64 * core, and a word more at MC_MBOX + 16 * core
// (programs/mc/mc.h is the same layout for the programs):
//
//   start   the texel at MC_MBOX + 16 * core: { MC_START, pc, sp, root } starts the worker there,
//           whatever it was doing, with its core number in a0 and the page table whose
//           first page is number `root`. Core 0 takes the word away
//           again when the worker has said it runs.
//           MC_STOP in that word parks it (the kernel writes it when the program is gone).
//   job     the mailbox's first texel, core 0's. The worker's `pause` is a sleep until the
//           first word of it changes.
//   fault   its third texel, the worker's. A worker cannot be given a trap, so one stops it
//           where it is: { count, cause, address, pc }, the count one more than the resume
//           word. A page the program has not touched yet is the usual cause.
//   resume  its fourth texel, core 0's: when its first word is the fault's count the worker
//           runs the same instruction again.
//   call    a system call (ecall) is a stop of its own kind: the eight words at MC_MBOX + 0x2000
//           + 32 * core take a7 and a0 to a5, the fault's cause is 8, and when the resume
//           word is the count the worker goes on after the ecall with the resume texel's
//           second word in a0. Core 0 makes the call for it, in the program they share.
//
// `ebreak` parks a worker until its next start. The state word (41,0).a says what it does: 1
// running, 3 asleep, 5 stopped by a fault and 9 waiting for a system call's answer (with the
// word waited on, as it was, in bits 8 and up), an even number parked.
//
// A worker that is parked, asleep or stopped costs next to nothing: each of its pixels reads
// its state word and its mailbox and returns what it holds (mc_idle(), before anything of the
// CPU is decoded), and the commit pass passes over a core that stored nothing.
#if CORES > 1
#define MC_MBOX (DEV_AT + 0xC00000u)
#define MC_START 0x5553434du   // "MCSU"
#define MC_STOP 0x5453434du    // "MCST"
#define MC_START_TEXEL(core) RAM_ADDR(((MC_MBOX & 0x7fffffff) >> 4) + (core))
#define MC_BOX_OF(core) (0x4000 + 64 * (core))
#define MC_JOB_TEXEL(core) RAM_ADDR(((MC_MBOX & 0x7fffffff) >> 4) + (MC_BOX_OF(core) >> 4))
#define MC_RESUME_TEXEL(core) RAM_ADDR(((MC_MBOX & 0x7fffffff) >> 4) + (MC_BOX_OF(core) >> 4) + 3)

#ifdef PASS_TICK
// Whether this pixel's core is a worker with nothing to do in this pass (Unity's Unpack pass in
// Machine.shader, which takes a worker's texels from the tick's targets only when it ran).
bool mc_idle() {
    if (hart == 0) return false;
    uint start = RAM_TEX(MC_START_TEXEL(hart)).x;
    if (start == MC_START) return false;                                                        // it is to start
    uint word = STATE_TEX(uint2(41, 0)).a;
    if (start == MC_STOP && (word & 1) != 0) return false;                                      // it is to park
    if ((word & 1) == 0) return true;                                                           // parked
    if (word & 12) return (RAM_TEX(MC_RESUME_TEXEL(hart)).x & 0xffffff) == (word >> 8);         // stopped by a fault, or a call
    return (word & 2) != 0 && (RAM_TEX(MC_JOB_TEXEL(hart)).x & 0xffffff) == (word >> 8);       // asleep
}

#endif
#endif

#endif
