#ifndef MC_H
#define MC_H

// More than one core (CORES > 1, docs/multicore.md). Core 0 is the machine as it always was.
// The others are workers: a block of state each (the CPU's texels, the write cache and the
// float registers; no TLBs kept from one pass to the next), no devices, the same RAM. A worker
// reads RAM as the last commit left it and its own writes; the commit pass merges every core's
// writes, the highest core last. So a core sees another's writes one pass later, and two cores
// must not write the same 16 bytes in one pass.
//
// A worker runs the code of a program on core 0, as a thread of it that the kernel does not
// know of: in user mode, on the page table the program names when it starts it (the kernel
// tells a program its own: core 0 may be in another process by then). Its mailbox is 64
// bytes of RAM at MC_MBOX + 0x400 + 64 * core, and a word more at MC_MBOX + 16 * core
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
//   call    a system call (ecall) is a stop of its own kind: the eight words at MC_MBOX + 0x800
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
#define MC_MBOX 0x86C00000u
#define MC_START 0x5553434du   // "MCSU"
#define MC_STOP 0x5453434du    // "MCST"
#define MC_PARKED_STOP 2
#define MC_PARKED_EBREAK 1
#define MC_START_TEXEL(core) RAM_ADDR(((MC_MBOX & 0x7fffffff) >> 4) + (core))
#define MC_JOB_TEXEL(core) RAM_ADDR(((MC_MBOX & 0x7fffffff) >> 4) + 0x40 + 4 * (core))
#define MC_RESUME_TEXEL(core) RAM_ADDR(((MC_MBOX & 0x7fffffff) >> 4) + 0x43 + 4 * (core))
#define MC_FAULT_WORD(core) ((MC_MBOX & 0x7fffffff) + 0x400 + 64 * (core) + 32)
#define MC_CALL_WORD(core) ((MC_MBOX & 0x7fffffff) + 0x800 + 32 * (core))

#ifdef PASS_TICK
// Whether this pixel's core is a worker with nothing to do in this pass.
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

void mc_enter() {
    mc_word = STATE_TEX(uint2(41, 0)).a;
    if (hart == 0) return;
    bool called = (mc_word & 9) == 9;   // it waited for a system call's answer, which is there now
    if (mc_word & 14) mc_word = 1;      // its job word changed, or core 0 says go on: awake
    uint4 box = RAM_TEX(MC_START_TEXEL(hart));
    if (box.x == MC_START) {
        cpu.pc = box.y;
        cpu.xreg[2] = box.z;
        cpu.xreg[10] = hart;
        cpu.csr.privilege = 0;
        cpu.mmu.mode = 1;
        cpu.mmu.ppn = box.w;
        cpu.stall = 0;
        mc_word = 1;
    } else if (box.x == MC_STOP || (mc_word & 1) == 0) {
        if (mc_word & 1) mc_word = MC_PARKED_STOP << 8;
        cpu.stall = STALL_WFI;   // nothing runs; the commit pass clears it
    } else if (called) {
        cpu.xreg[10] = RAM_TEX(MC_RESUME_TEXEL(hart)).y;
        cpu.pc += 4;
    }
}

#define MC_PARK(why) if (hart != 0) { mc_word = (why) << 8; cpu.stall = STALL_WFI; }
#define MC_SLEEP if (hart != 0) { mc_word = 3 | (RAM_TEX(MC_JOB_TEXEL(hart)).x << 8); }
// A worker's trap: where this is used the write cache is at hand. If the cache has no room
// for the fault's texel the instruction is simply run again in the next pass, which starts
// with an empty one.
#define MC_FAULT(ret) \
    if (hart != 0 && ret.trap.en) { \
        uint seen_ = RAM_TEX(MC_RESUME_TEXEL(hart)).x; \
        bool call_ = ret.trap.type == 8; \
        if (call_) { \
            mem_set_ram(MC_CALL_WORD(hart), xreg(17), 0xffffffff); \
            mem_set_ram(MC_CALL_WORD(hart) + 4, xreg(10), 0xffffffff); \
            mem_set_ram(MC_CALL_WORD(hart) + 8, xreg(11), 0xffffffff); \
            mem_set_ram(MC_CALL_WORD(hart) + 12, xreg(12), 0xffffffff); \
            mem_set_ram(MC_CALL_WORD(hart) + 16, xreg(13), 0xffffffff); \
            mem_set_ram(MC_CALL_WORD(hart) + 20, xreg(14), 0xffffffff); \
            mem_set_ram(MC_CALL_WORD(hart) + 24, xreg(15), 0xffffffff); \
        } \
        mem_set_ram(MC_FAULT_WORD(hart) + 4, ret.trap.type, 0xffffffff); \
        mem_set_ram(MC_FAULT_WORD(hart) + 8, ret.trap.value, 0xffffffff); \
        mem_set_ram(MC_FAULT_WORD(hart) + 12, cpu.pc, 0xffffffff); \
        /* the count last: with it the record is whole */ \
        mem_set_ram(MC_FAULT_WORD(hart), seen_ + 1, 0xffffffff); \
        if (cpu.stall == 0) { \
            mc_word = (call_ ? 9 : 5) | (seen_ << 8); \
            cpu.stall = STALL_WFI; \
        } \
        ret.trap.en = false; \
        ret.pc_val = cpu.pc; \
    }
#endif

#else
#define MC_PARK(why)
#define MC_SLEEP
#define MC_FAULT(ret)
#endif

#endif
