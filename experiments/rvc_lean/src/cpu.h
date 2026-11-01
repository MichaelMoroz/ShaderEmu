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
#ifdef SBI_HLE
    // straight into the kernel, as firmware would enter it: supervisor mode, hart 0 in a0 and
    // the device tree (which the image has in RAM at this address) in a1
    ret.xreg[11] = TREE_AT;
    ret.pc = KERNEL_AT;
    ret.csr.privilege = 1;
#endif

#ifndef CSR_FIELDS
    ret.start_time_ref = _Time.y;
#endif

    return ret;
}

// The short path: instructions that cannot trap or change what interrupts see, run back to
// back in a tight loop of their own. That matters more than the work saved per instruction:
// every value the general path can modify must be merged wherever its control flow rejoins,
// and with both paths in one loop body that merge of the whole CPU state happened on every
// tick. This loop only carries what a fast instruction can change.
//
// The loop is left from where a thing is found out (a fetch that needs the general path, an
// instruction fast_exec() does not take, the budget, a stall) and nowhere else: a step that
// reported success through a return value was tested twice more on its way out, and a branch
// costs as much here as six additions.
//
// Runs fast steps back to back, at most `room` of them, and returns how many ran. Everything
// that is the same for a whole run is decided once up front: single-stepping, the interrupt
// gate (a fast step cannot change it) and the distance to the next UART poll tick, which has
// to take the general path. The clock is advanced once at the end. When the loop ends on an
// instruction that was fetched and not run, pre_valid says so and the general path reuses
// the word.
uint fast_run_l1(L1P uint room) {
    pre_valid = false;
    fast_same = false;
#ifdef CSR_FIELDS
    // (no UART to poll and no single-stepping: the console is memory, docs/machine/console.md)
    if (!irq_quiet) {
        return 0;
    }
    fast_budget = room;
#else
    if (_DoTick != 0 || !irq_quiet) {
        return 0;
    }
    // the tick that makes (clock & 0xff) == 0xff polls the UART for input, on the general path;
    // there is nothing to poll for unless the host has a character the guest has not taken
    uint to_poll = (0xff - (cpu.clock & 0xff)) & 0xff;
    if (to_poll == 0) {
        to_poll = 256;
    }
    fast_budget = UART_INPUT_WAITING ? min(room, to_poll - 1) : room;
#endif
    uint n = 0;
    [branch]
    if (fast_budget > 0) {
        [loop]
        while (true) {
            // After an instruction that stayed within its texel (three in four of straight-line code)
            // the window already holds this one: no translation, alignment or window check.
            [branch]
            if (!fast_same) {
            PROF(PROF_x_refetch)
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
                break;                              // the general path takes this tick
            }
            // Instruction window: a RAM texel holds four instructions, and fetches read the texture
            // directly (never the write cache), so a fetched texel stays valid for the whole pass.
            // Entering a new texel also reads the one after it, so in straight-line code the texture
            // read is finished long before its instructions are needed. Both are keyed by physical
            // texel number, so a wrong guess is simply not used.
            uint f_t = (f_pa & 0x7FFFFFFF) >> 4;
            // (no test of "this texel is the window's": a jump within the texel reads it again,
            // 3 in 1,000, and every other entry passes a block less: docs/machine/tick-costs.md)
            {
                [branch]
                if (f_t == fw_addr1) {
                    fw_tex0 = fw_tex1;
                } else {
                    PROF(PROF_fetch_tex)
                    fw_tex0 = RAM_TEX(RAM_ADDR(f_t));
                }
                fw_addr0 = f_t;
                fw_addr1 = f_t + 1;
                fw_tex1 = RAM_TEX(RAM_ADDR(f_t + 1));
            }
            // (the texel's words turned so that this instruction's is the first: the ones after
            // it then need no choosing among four)
            uint f_slot = (cpu.pc >> 2) & 3;
            fw_cur = (f_slot & 2) ? ((f_slot & 1) ? fw_tex0.wxyz : fw_tex0.zwxy) : ((f_slot & 1) ? fw_tex0.yzwx : fw_tex0);
            }
            uint word = fw_cur.x;
            if (!fast_exec(word)) {
                pre_word = word;
                pre_valid = true;
                break;
            }
            PROF(PROF_tick)
            PROF(PROF_fast_step)
            n++;
            // (a store that stalled has made the budget none: types.h)
            if (n >= fast_budget) {
                break;
            }
        }
    }
    // only the last instruction of a run can have stalled: the loop ends there
    if (n > 0 && cpu.stall) {
        PROF(PROF_stalled_tick)
    }
    cpu.clock += n;
#ifdef X_PROBE
    cpu.debug_last_stall ^= probe;   // (so that the probe's work is not thrown away)
#endif
    return n;
}

void cpu_tick_l1(L1P0) {
#ifndef CSR_FIELDS
    // DEBUG: single stepping
    if (_DoTick && _DoTick == cpu.debug_do_tick) {
        return;
    }
    cpu.debug_do_tick = _DoTick;
#endif

    cpu.clock++;
    PROF(PROF_tick)
    // (with OPT_FAST_STEP, fast_tick() has already declined this tick)
    emulate_l1(L1A0);

    /* if ((_BreakpointClock && _BreakpointClock == cpu.clock) || (_Breakpoint && _Breakpoint == cpu.pc)) { */
    /*     cpu.debug_do_tick = 0xffffffff; */
    /*     _DoTick = cpu.debug_do_tick; */
    /* } */

    if (cpu.stall) {
        PROF(PROF_stalled_tick)
    }
}

#endif
