#ifndef CPU_H
#define CPU_H

cpu_t cpu_init() {
    cpu_t ret = (cpu_t)0;

    ret.xreg[11] = 0x1020; // device tree base
    ret.pc = 0x80000000;
    ret.reservation_en = false;

    ret.uart.rbr_thr_ier_iir = 0x00000000;
    ret.uart.lcr_mcr_lsr_scr = 0x00200000; // LSR_THR_EMPTY is set

    ret.csr.privilege = 3; // PRIV_MACHINE

    ret.start_time_ref = _Time.y;

    return ret;
}

// One instruction on the short path, or false with nothing changed (the general path then
// runs the tick, reusing the instruction word if this got as far as fetching it).
//
// fast_run() calls this in a tight inner loop of its own. That matters more than the work
// saved per instruction: every value the general path can modify must be merged wherever its
// control flow rejoins, and with both paths in one loop body that merge of the whole CPU state
// happened on every tick. The inner loop only carries what a fast step can change.
bool fast_step() {
    pre_valid = false;
    // Same page as the last fetch is the common case and costs one compare; anything else goes
    // through the TLBs and becomes the new "last page".
#ifdef NO_PAGING
    bool f_ok = true;
    uint f_pa = cpu.pc;
#else
    bool f_ok = (cpu.pc >> 12) == fetch_vpn;
    uint f_pa = fetch_page | (cpu.pc & 0xfff);
    [branch]
    if (!f_ok) {
        FAST_XL(xl_ident_f, MMU_ACCESS_FETCH, tlb_f_vpn, tlb_f_page, cpu.pc, g_ok, g_pa)
        if (g_ok) {
            f_ok = true;
            f_pa = g_pa;
            fetch_vpn = cpu.pc >> 12;
            fetch_page = g_pa & ~0xfff;
        }
    }
#endif
    [branch]
    if (!(f_ok && (cpu.pc & 0x3) == 0)) {
        return false;
    }
    // Instruction window: a RAM texel holds four instructions, and fetches read the texture
    // directly (never the write cache), so a fetched texel stays valid for the whole pass.
    // Entering a new texel also reads the one after it, so in straight-line code the texture
    // read is finished long before its instructions are needed. Both are keyed by physical
    // texel number, so a wrong guess is simply not used.
    uint f_t = (f_pa & 0x7FFFFFFF) >> 4;
    [branch]
    if (f_t != fw_addr0) {
        [branch]
        if (f_t == fw_addr1) {
            fw_tex0 = fw_tex1;
        } else {
            PROF(PROF_fetch_tex)
            fw_tex0 = STATE_TEX(RAM_ADDR(f_t));
        }
        fw_addr0 = f_t;
        fw_addr1 = f_t + 1;
        fw_tex1 = STATE_TEX(RAM_ADDR(f_t + 1));
    }
    pre_word = idx_uint4(fw_tex0, (f_pa >> 2) & 0x3);
    pre_valid = true;
    if (!fast_exec(pre_word)) {
        return false;                       // the general path reuses pre_word
    }
    pre_valid = false;
    PROF(PROF_tick)
    PROF(PROF_fast_step)
    if (cpu.stall) {
        PROF(PROF_stalled_tick)
        cpu.stall_count++;
    }
    return true;
}

// Runs fast steps back to back, at most `room` of them, and returns how many ran. Everything
// that is the same for a whole run is decided once up front: single-stepping, the interrupt
// gate (a fast step cannot change it) and the distance to the next UART poll tick, which has
// to take the general path. The clock is advanced once at the end.
uint fast_run(uint room) {
    pre_valid = false;
    if (_DoTick != 0 || !irq_quiet) {
        return 0;
    }
    // the tick that makes (clock & 0xff) == 0xff polls the UART
    uint to_poll = (0xff - (cpu.clock & 0xff)) & 0xff;
    if (to_poll == 0) {
        to_poll = 256;
    }
    uint budget = min(room, to_poll - 1);
    uint n = 0;
    bool again = budget > 0;
    [loop]
    while (again) {
        again = false;
        if (fast_step()) {
            n++;
            again = n < budget && !cpu.stall;
        }
    }
    cpu.clock += n;
    return n;
}

void cpu_tick() {
    // DEBUG: single stepping
    if (_DoTick && _DoTick == cpu.debug_do_tick) {
        return;
    }
    cpu.debug_do_tick = _DoTick;

    cpu.clock++;
    PROF(PROF_tick)
    // (with OPT_FAST_STEP, fast_tick() has already declined this tick)
    emulate();

    /* if ((_BreakpointClock && _BreakpointClock == cpu.clock) || (_Breakpoint && _Breakpoint == cpu.pc)) { */
    /*     cpu.debug_do_tick = 0xffffffff; */
    /*     _DoTick = cpu.debug_do_tick; */
    /* } */

    if (cpu.stall) {
        PROF(PROF_stalled_tick)
        cpu.stall_count++;
    }
}

#endif
