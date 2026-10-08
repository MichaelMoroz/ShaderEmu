#ifndef CSR_H
#define CSR_H


// RAM_ADDR / RAM_LIN: see types.h
#define RAM_MAX (2048 * (4096 - 64) * 4 * 4)


/* shift by two to ignore byte offset */
// The write cache holds RAM texels (four words), not words: stores come in runs (a register
// save, a structure, a copy), and a run then takes one entry. A bucket is the tags of its
// L1_WAYS entries in one state texel (texel number + 1; 0 is free) and then their texels.
// There are two tables of 2^L1_TABLE_BITS buckets with different hashes; a texel goes to the
// second only when its bucket in the first is full, so most lookups read one bucket. Entries
// are filled in order and never freed within a pass.
//   L1_TABLE_BITS 6, L1_WAYS 4 (the default): 512 texels, arrays of 512 and 128 in the tick
//   L1_TABLE_BITS 7, L1_WAYS 3:               768 texels, an array of 1,024
//   L1_TABLE_BITS 6, L1_WAYS 3:               384 texels, an array of 512
#ifndef L1_BUCKETS
#ifndef L1_TABLE_BITS
#define L1_TABLE_BITS 6
#endif
#ifndef L1_WAYS
#define L1_WAYS 4
#endif
#define L1_TABLE (1 << L1_TABLE_BITS)
#define L1_BUCKETS (2 * L1_TABLE)
#define L1_STRIDE (L1_WAYS + 1)
#define L1_ENTRIES (L1_BUCKETS * L1_STRIDE)
#define L1_B0(t) ((t) & (L1_TABLE - 1))
#define L1_B1(t) (L1_TABLE + ((((t) >> 3) ^ ((t) << (L1_TABLE_BITS - 3)) ^ ((t) >> L1_TABLE_BITS)) & (L1_TABLE - 1)))
#endif


#define CSR_USTATUS 0x000
#define CSR_UIE 0x004
#define CSR_UTVEC 0x005
#define _CSR_USCRATCH 0x040
#define CSR_UEPC 0x041
#define CSR_UCAUSE 0x042
#define CSR_UTVAL 0x043
#define _CSR_UIP 0x044
#define CSR_SSTATUS 0x100
#define CSR_SEDELEG 0x102
#define CSR_SIDELEG 0x103
#define CSR_SIE 0x104
#define CSR_STVEC 0x105
#define _CSR_SSCRATCH 0x140
#define CSR_SEPC 0x141
#define CSR_SCAUSE 0x142
#define CSR_STVAL 0x143
#define CSR_SIP 0x144
#define CSR_SATP 0x180
#define CSR_MSTATUS 0x300
#define CSR_MISA 0x301
#define CSR_MEDELEG 0x302
#define CSR_MIDELEG 0x303
#define CSR_MIE 0x304
#define CSR_MTVEC 0x305
#define _CSR_MSCRATCH 0x340
#define CSR_MEPC 0x341
#define CSR_MCAUSE 0x342
#define CSR_MTVAL 0x343
#define CSR_MIP 0x344
#define _CSR_PMPCFG0 0x3a0
#define _CSR_PMPADDR0 0x3b0
#define CSR_MCYCLE 0xb00
#define CSR_CYCLE 0xc00
#define CSR_TIME 0xc01
#define _CSR_INSERT 0xc02
#define CSR_MHARTID 0xf14

#define CSR_MEMOP_OP 0x0b0
#define CSR_MEMOP_SRC 0x0b1
#define CSR_MEMOP_DST 0x0b2
#define CSR_MEMOP_N 0x0b3

#define CSR_PLAYER_ID 0xbe
#define CSR_RNG 0x0bf

#define CSR_NET_TX_BUF_ADDR 0x0c0
#define CSR_NET_TX_BUF_SIZE_AND_SEND 0x0c1
#define CSR_NET_RX_BUF_ADDR 0x0c2
#define CSR_NET_RX_BUF_READY 0x0c3

bool has_csr_access_privilege(uint addr) {
#ifdef M_MODE_ONLY
    return true;
#else
    uint privilege = (addr >> 8) & 0x3;
    return privilege <= cpu.csr.privilege;
#endif
}

// SSTATUS, SIE, and SIP are subsets of MSTATUS, MIE, and MIP
uint read_csr_raw(uint address) {
    PROF(PROF_csr_read)
    address &= 0x1fff;

    uint read_mask = 0xffffffff;

    [forcecase]
    switch (address) {
        case CSR_MISA: return 0x40141101; // 0b01000000000101000001000100000001 = RV32AIMSU
        case CSR_SSTATUS: address = CSR_MSTATUS; read_mask = 0x000de162; break;
        case CSR_SIE: address = CSR_MIE; read_mask = 0x222; break;
        case CSR_SIP: address = CSR_MIP; read_mask = 0x222; break;
        case CSR_TIME: return cpu.clint.mtime_lo;
        case CSR_MCYCLE: return cpu.clock;
        case CSR_CYCLE: return cpu.clock;
        case CSR_MHARTID: return 0;
        case CSR_SATP: return (cpu.mmu.mode << 31) | cpu.mmu.ppn;
        case CSR_RNG: return xorshift(asuint(_Time.w));
        case CSR_PLAYER_ID: return _PlayerID;
    }

    uint ret;

    if (false) {}
    else if (cpu.cache.csr_cache_0_addr == address) { ret = cpu.cache.csr_cache_0_val; }
else if (cpu.cache.csr_cache_1_addr == address) { ret = cpu.cache.csr_cache_1_val; }
else if (cpu.cache.csr_cache_2_addr == address) { ret = cpu.cache.csr_cache_2_val; }
else if (cpu.cache.csr_cache_3_addr == address) { ret = cpu.cache.csr_cache_3_val; }
else if (cpu.cache.csr_cache_4_addr == address) { ret = cpu.cache.csr_cache_4_val; }
else if (cpu.cache.csr_cache_5_addr == address) { ret = cpu.cache.csr_cache_5_val; }
else if (cpu.cache.csr_cache_6_addr == address) { ret = cpu.cache.csr_cache_6_val; }
else if (cpu.cache.csr_cache_7_addr == address) { ret = cpu.cache.csr_cache_7_val; }
else if (cpu.cache.csr_cache_8_addr == address) { ret = cpu.cache.csr_cache_8_val; }
else if (cpu.cache.csr_cache_9_addr == address) { ret = cpu.cache.csr_cache_9_val; }
else if (cpu.cache.csr_cache_10_addr == address) { ret = cpu.cache.csr_cache_10_val; }
else if (cpu.cache.csr_cache_11_addr == address) { ret = cpu.cache.csr_cache_11_val; }
else if (cpu.cache.csr_cache_12_addr == address) { ret = cpu.cache.csr_cache_12_val; }
else if (cpu.cache.csr_cache_13_addr == address) { ret = cpu.cache.csr_cache_13_val; }
else if (cpu.cache.csr_cache_14_addr == address) { ret = cpu.cache.csr_cache_14_val; }
else if (cpu.cache.csr_cache_15_addr == address) { ret = cpu.cache.csr_cache_15_val; }

    else {
        // fallback, read value from CSR texture area
        PROF(PROF_csr_read_tex)
        ret = tex_get_csr(address);
    }

    return ret & read_mask;
}

uint read_mstatus() {
    if (!hot_mstatus_ok) { hot_mstatus = read_csr_raw(CSR_MSTATUS); hot_mstatus_ok = true; }
    return hot_mstatus;
}
uint read_mip() {
    if (!hot_mip_ok) { hot_mip = read_csr_raw(CSR_MIP); hot_mip_ok = true; }
    return hot_mip;
}
uint read_mie() {
    if (!hot_mie_ok) { hot_mie = read_csr_raw(CSR_MIE); hot_mie_ok = true; }
    return hot_mie;
}

void write_csr_raw(uint address, uint value) {
    PROF(PROF_csr_write)
    irq_quiet = false;
    uint where = address & 0x1fff;
    uint what = value;
    uint modify_mask = 0;
    ins_ret nop = ins_ret_noop();
    [forcecase]
    switch (address) {
        case CSR_SSTATUS:
            where = CSR_MSTATUS;
            modify_mask = 0x000de162;
            break;
        case CSR_SIE:
            where = CSR_MIE;
            modify_mask = 0x222;
            break;
        case CSR_SIP:
            where = CSR_MIP;
            modify_mask = 0x222;
            break;
        case CSR_MIDELEG:
            what &= 0x666; // from qemu
            break;
        case CSR_MEMOP_SRC:
            cpu.memop_src_v = what;
            return;
        case CSR_MEMOP_DST:
            cpu.memop_dst_v = what;
            return;
        case CSR_MEMOP_N:
            cpu.memop_n = what;
            return;
        case CSR_MEMOP_OP:   // 2: fill the range with the word in MEMOP_SRC; otherwise copy
            cpu.stall = what == 2 ? STALL_MEMOP_FILL : STALL_MEMOP_COPY;
            cpu.debug_arb_0++;
            return;
    };

    if (modify_mask) {
        what = (read_csr_raw(where) & ~modify_mask) | (value & modify_mask);
    }

    // Keep the pass-local shadows in step; only mstatus changes what a translation means.
    if (where == CSR_MIP) { hot_mip = what; hot_mip_ok = true; }
    else if (where == CSR_MIE) { hot_mie = what; hot_mie_ok = true; }
    else if (where == CSR_MSTATUS) { hot_flush(); }

    if (false) {}
else if (cpu.cache.csr_cache_0_addr == 0xffffffff || cpu.cache.csr_cache_0_addr == where) { cpu.cache.csr_cache_0_addr = where; cpu.cache.csr_cache_0_val = what; return; }
else if (cpu.cache.csr_cache_1_addr == 0xffffffff || cpu.cache.csr_cache_1_addr == where) { cpu.cache.csr_cache_1_addr = where; cpu.cache.csr_cache_1_val = what; return; }
else if (cpu.cache.csr_cache_2_addr == 0xffffffff || cpu.cache.csr_cache_2_addr == where) { cpu.cache.csr_cache_2_addr = where; cpu.cache.csr_cache_2_val = what; return; }
else if (cpu.cache.csr_cache_3_addr == 0xffffffff || cpu.cache.csr_cache_3_addr == where) { cpu.cache.csr_cache_3_addr = where; cpu.cache.csr_cache_3_val = what; return; }
else if (cpu.cache.csr_cache_4_addr == 0xffffffff || cpu.cache.csr_cache_4_addr == where) { cpu.cache.csr_cache_4_addr = where; cpu.cache.csr_cache_4_val = what; return; }
else if (cpu.cache.csr_cache_5_addr == 0xffffffff || cpu.cache.csr_cache_5_addr == where) { cpu.cache.csr_cache_5_addr = where; cpu.cache.csr_cache_5_val = what; return; }
else if (cpu.cache.csr_cache_6_addr == 0xffffffff || cpu.cache.csr_cache_6_addr == where) { cpu.cache.csr_cache_6_addr = where; cpu.cache.csr_cache_6_val = what; return; }
else if (cpu.cache.csr_cache_7_addr == 0xffffffff || cpu.cache.csr_cache_7_addr == where) { cpu.cache.csr_cache_7_addr = where; cpu.cache.csr_cache_7_val = what; return; }
else if (cpu.cache.csr_cache_8_addr == 0xffffffff || cpu.cache.csr_cache_8_addr == where) { cpu.cache.csr_cache_8_addr = where; cpu.cache.csr_cache_8_val = what; return; }
else if (cpu.cache.csr_cache_9_addr == 0xffffffff || cpu.cache.csr_cache_9_addr == where) { cpu.cache.csr_cache_9_addr = where; cpu.cache.csr_cache_9_val = what; return; }
else if (cpu.cache.csr_cache_10_addr == 0xffffffff || cpu.cache.csr_cache_10_addr == where) { cpu.cache.csr_cache_10_addr = where; cpu.cache.csr_cache_10_val = what; return; }
else if (cpu.cache.csr_cache_11_addr == 0xffffffff || cpu.cache.csr_cache_11_addr == where) { cpu.cache.csr_cache_11_addr = where; cpu.cache.csr_cache_11_val = what; return; }
else if (cpu.cache.csr_cache_12_addr == 0xffffffff || cpu.cache.csr_cache_12_addr == where) { cpu.cache.csr_cache_12_addr = where; cpu.cache.csr_cache_12_val = what; return; }
else if (cpu.cache.csr_cache_13_addr == 0xffffffff || cpu.cache.csr_cache_13_addr == where) { cpu.cache.csr_cache_13_addr = where; cpu.cache.csr_cache_13_val = what; return; }
else if (cpu.cache.csr_cache_14_addr == 0xffffffff || cpu.cache.csr_cache_14_addr == where) { cpu.cache.csr_cache_14_addr = where; cpu.cache.csr_cache_14_val = what; return; }


    // cache overflow, stall to avoid fillup
    cpu.cache.csr_cache_15_addr = where;
    cpu.cache.csr_cache_15_val = what;
    cpu.stall = STALL_CSR_CACHE;
}


uint get_csr(uint address, inout ins_ret ret) {
    if (has_csr_access_privilege(address)) {
        uint r = read_csr_raw(address);
        return r;
    } else {
        ret.trap.en = true;
        ret.trap.type = trap_IllegalInstruction;
        ret.trap.value = cpu.pc;
        return 0;
    }
}

void set_csr(uint address, uint value, inout ins_ret ret) {
    bool read_only = ((address >> 10) & 0x3) == 0x3;
    if (has_csr_access_privilege(address) && !read_only) {
        if (address == CSR_SATP) {
            mmu_update(value);
            return;
        }
        write_csr_raw(address, value);
    } else {
        ret.trap.en = true;
        ret.trap.type = trap_IllegalInstruction;
        ret.trap.value = cpu.pc;
    }
}

#endif
