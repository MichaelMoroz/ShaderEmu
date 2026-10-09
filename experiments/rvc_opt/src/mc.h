#ifndef MC_H
#define MC_H

// More than one core (CORES > 1, docs/multicore.md). Core 0 is the machine as it always was.
// The others are workers: a block of state each (the CPU's texels and the write cache: no TLBs,
// no float registers, so no paging and no F), no devices, the same RAM. A worker reads RAM
// as the last commit left it and its own writes; the commit pass merges every core's writes,
// the highest core last. So a core sees another's writes one pass later, and two cores must
// not write the same 16 bytes in one pass.
//
// A worker is parked until core 0 writes its mailbox, the texel at MC_MBOX + 16 * core:
// { MC_START, pc, sp, a0 }. It then starts there in machine mode with paging off, its core
// number in a1. `ebreak` parks it again, and so does any trap (a worker has no handler):
// the state word (41,0).a is 1 while it runs, else what parked it in bits 8 and up.
#if CORES > 1
#define MC_MBOX 0x86C00000u
#define MC_START 0x5453434du   // "MCST"
#define MC_PARKED_EBREAK 1
#define MC_PARKED_TRAP 2       // + the trap's cause in bits 16 and up

#ifdef PASS_TICK
void mc_enter() {
    mc_word = STATE_TEX(uint2(41, 0)).a;
    if (hart == 0) return;
    if ((mc_word & 1) == 0) {
        uint4 box = RAM_TEX(RAM_ADDR(((MC_MBOX & 0x7fffffff) >> 4) + hart));
        if (box.x == MC_START && cpu.stall == 0) {
            cpu.pc = box.y;
            cpu.xreg[2] = box.z;
            cpu.xreg[10] = box.w;
            cpu.xreg[11] = hart;
            cpu.csr.privilege = 3;
            mc_word = 1;
        } else {
            cpu.stall = STALL_WFI;   // nothing runs; the commit pass clears it
        }
    }
}

#define MC_PARK(why) if (hart != 0) { mc_word = (why) << 8; cpu.stall = STALL_WFI; }
#endif

#else
#define MC_PARK(why)
#endif

#endif
