#ifndef MEM_H
#define MEM_H


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


#define WORD_SIZE_NONE 0
#define WORD_SIZE_BYTE 1
#define WORD_SIZE_HALF 2
#define WORD_SIZE_FULL 4

uint mem_get_instruction(uint addr) {
    addr = addr & 0x7FFFFFFF;
    uint idx = (addr >> 2) & 0x3;
    addr = addr >> 4;

    PROF(PROF_fetch_tex)
    uint4 raw = RAM_TEXEL(addr);
    return idx_uint4(raw, idx);
}

// Basic bloom filter to slightly optimize memory caching (declared in types.h)

static uint dr_addr = 0xffffffff;   // the last RAM texel read for data, and its number
static uint4 dr_tex;

// addr must be aligned to word boundary (4 byte)
// Where RAM texel t is in the cache: its entry as 4 * bucket + entry (from 1), for L1_DATA(), or 0.
uint l1_find_l1(L1P uint t) {
    uint found = 0;
    uint b = L1_B0(t);
    bool more = false;
    // An array read costs as much as a dozen additions: the tags of one bucket, and those of
    // the second table's only when the first one's is full.
    [branch]
    if (L1_OCC(b)) {
        uint4 tags = L1_TAGS(b);
        uint e = l1_slot(tags, t + 1);
        found = e != 0 ? b * 4 + e : 0;
        more = e == 0 && L1_LAST(tags) != 0;
    }
    [branch]
    if (more) {
        b = L1_B1(t);
        [branch]
        if (L1_OCC(b)) {
            uint e = l1_slot(L1_TAGS(b), t + 1);
            found = e != 0 ? b * 4 + e : 0;
        }
    }
    return found;
}

uint mem_get_cached_or_tex_l1(L1P uint addr) {
    PROF(PROF_ram_read)
#ifdef RAM_BUFFER_ON
    return _RamB.Load(addr);
#endif
#ifndef RAM_DIRECT_ON
    // query L1 cache
    if ((addr & mem_cache_bloom) == addr) {
        PROF(PROF_ram_read_bloom_pass)
        uint at = l1_find(addr >> 4);
        [branch]
        if (at != 0) {
            PROF(PROF_ram_read_l1_hit)
            return idx_uint4(L1_DATA(at), (addr >> 2) & 0x3);
        }

        if (cpu.cache.ram_l1_last_addr == addr) {
            // the word the cache had no room for (the store that ended the pass)
            return cpu.cache.ram_l1_last_val;
        }
    }
#endif

    // Not in the cache: the RAM texture, whose texels hold four words. The texture does not
    // change during a pass, so the last texel read stays good, and data next to what was just
    // read (the rest of a structure, the next stack slot) costs no texture read.
    uint t = addr >> 4;
    [branch]
    if (t != dr_addr) {
        PROF(PROF_ram_read_tex)
        dr_tex = RAM_TEXEL(t);
        dr_addr = t;
    }
    return idx_uint4(dr_tex, (addr >> 2) & 0x3);
}


// little endian, zero extended, addr must be aligned to word boundary
uint mem_get_word_l1(L1P uint addr) {
    //addr &= ~(0x3);

    if ((addr & 0x80000000) == 0) {
        [branch]
        if (addr & 0x40000000) {
            // MTD/ROM
            PROF(PROF_mtd_read)
            uint mtd_addr = addr - 0x40000000;
            uint mtd_idx = (mtd_addr >> 2) & 0x3;
            mtd_addr = mtd_addr >> 4;
            uint2 mtd_lin = uint2(mtd_addr % m_dim.x, mtd_addr / m_dim.x);
            mtd_lin.y = m_dim.y - mtd_lin.y - 1;
            //[branch] if (mtd_lin.y >= m_dim.y) return 0;
            float4 full;
            [branch]
            switch (mtd_idx) {
                case 0:
                    full = _Data_MTD_R[mtd_lin];
                    break;
                case 1:
                    full = _Data_MTD_G[mtd_lin];
                    break;
                case 2:
                    full = _Data_MTD_B[mtd_lin];
                    break;
                case 3:
                    full = _Data_MTD_A[mtd_lin];
                    break;
            }
            return unpack_raw_float4(full);
        }

        [branch]
        if (addr >= 0x1020 && addr <= 0x1fff) {
            PROF(PROF_dtb_read)
            uint dtb_addr = addr - 0x1020;
            uint dtb_idx = (dtb_addr >> 2) & 0x3;
            dtb_addr = dtb_addr >> 4;
            float4 full;
            [branch]
            switch (dtb_idx) {
                case 0:
                    full = _Data_DTB_R[uint2(dtb_addr, 0)];
                    break;
                case 1:
                    full = _Data_DTB_G[uint2(dtb_addr, 0)];
                    break;
                case 2:
                    full = _Data_DTB_B[uint2(dtb_addr, 0)];
                    break;
                case 3:
                    full = _Data_DTB_A[uint2(dtb_addr, 0)];
                    break;
            }
            return unpack_raw_float4(full);
        }

        PROF(PROF_mmio_read)
        [forcecase]
        switch (addr) {
            // CLINT
            case 0x02000000: return cpu.clint.msip ? 1 : 0;

            case 0x02004000: return cpu.clint.mtimecmp_lo;
            case 0x02004004: return cpu.clint.mtimecmp_hi;
            case 0x0200bff8: return cpu.clint.mtime_lo;
            case 0x0200bffc: return cpu.clint.mtime_hi;

            // UART (first has rbr_thr_ier_iir, second has lcr_mcr_lsr_scr)
            case 0x10000000: { // braces: DXC rejects a declaration that later cases jump over
                uint ret = 0;
                if ((UART_GET2(LCR) >> 7) == 0) {
                    uint rbr = UART_GET1(RBR);
                    UART_SET1(RBR, 0);
                    UART_SET2(LSR, (UART_GET2(LSR) & ~LSR_DATA_AVAILABLE));
                    uart_update_iir();
                    ret = rbr;
                }
                return ret | ((UART_GET2(LCR) >> 7 == 0 ? UART_GET1(IER) : 0) << 8) | (UART_GET1(IIR) << 16) | (UART_GET2(LCR) << 24);
            }
            /* case 0x10000001: return UART_GET2(LCR) >> 7 == 0 ? UART_GET1(IER) : 0; */
            /* case 0x10000002: return UART_GET1(IIR); */
            /* case 0x10000003: return UART_GET2(LCR); */
            case 0x10000004: return UART_GET2(MCR) | (UART_GET2(LSR) << 8) | (UART_GET2(SCR) << 24);
            /* case 0x10000005: return UART_GET2(LSR); */
            /* case 0x10000007: return UART_GET2(SCR); */

            case 0x030007f8: return cpu.rtc0;
            case 0x030007fc: return cpu.rtc1;
        }

        return 0;
        //return addr;
    }

    addr = addr & 0x7FFFFFFF;

    if (addr >= RAM_MAX) {
        return 0;
    }

    return mem_get_cached_or_tex(addr);
}

// A store that lies within one RAM word: `bits` replace the word's under `mask`. The cache
// keeps the whole RAM texel around the word, read from the texture when its entry is made, so
// a store that changes nothing costs no entry and a partial one needs no second read.
void mem_set_ram_l1(L1P uint word_addr, uint bits, uint mask) {
    if (word_addr >= RAM_MAX) {
        return;
    }
    PROF(PROF_ram_write_byte)
#ifdef RAM_BUFFER_ON
    {
        uint val = bits;
        [branch]
        if (mask != 0xffffffff) {
            val = (_RamB.Load(word_addr) & ~mask) | (bits & mask);
        }
        _RamB.Store(word_addr, val);
        // the instruction window's two texels are copies: keep them right
        uint t = word_addr >> 4;
        [branch]
        if (t == fw_addr0 || t == fw_addr1) {
            if (t == fw_addr0) { fw_tex0 = RAM_TEXEL(t); }
            if (t == fw_addr1) { fw_tex1 = RAM_TEXEL(t); }
        }
    }
#elif defined(RAM_DIRECT_ON)
    // RAM is written where it is. dr_tex, the last texel read, is the texel being written while
    // stores stay in it; the instruction window's two texels are kept right as well.
    {
        uint t = word_addr >> 4, wi = (word_addr >> 2) & 0x3;
        [branch]
        if (t != dr_addr) {
            PROF(PROF_ram_read_tex)
            dr_tex = RAM_TEXEL(t);
            dr_addr = t;
        }
        uint cur_val = idx_uint4(dr_tex, wi);
        uint val = (cur_val & ~mask) | (bits & mask);
        [branch]
        if (val != cur_val) {
            PROF(PROF_ram_write_store)
            mem_dirty |= 1u << ((word_addr >> 22) & 31);
            set_idx_uint4(dr_tex, val, wi);
            _Ram[RAM_ADDR(t)] = dr_tex;
            if (t == fw_addr0) { fw_tex0 = dr_tex; }
            if (t == fw_addr1) { fw_tex1 = dr_tex; }
        }
    }
#else
    uint t = word_addr >> 4, tag = t + 1, wi = (word_addr >> 2) & 0x3;
    uint b0 = L1_B0(t), b1 = L1_B1(t);
    uint4 tags0 = 0, tags1 = 0;
    [branch]
    if (L1_OCC(b0)) {
        tags0 = L1_TAGS(b0);
    }
    uint e0 = l1_slot(tags0, tag), e1 = 0;
    uint f0 = l1_slot(tags0, 0), f1 = 0;       // the first free entry: they fill in order
    [branch]
    if (e0 == 0 && f0 == 0) {
        [branch]
        if (L1_OCC(b1)) {
            tags1 = L1_TAGS(b1);
        }
        e1 = l1_slot(tags1, tag);
        f1 = l1_slot(tags1, 0);
    }
    uint at = e0 != 0 ? b0 * 4 + e0 : e1 != 0 ? b1 * 4 + e1 : 0;
    uint4 texel;
    [branch]
    if (at != 0 && mask == 0xffffffff) {
        // A whole word into a texel the cache has (the rest of a register save, of a copy):
        // what was there does not matter, so the texel is not read, only one of its words written.
        PROF(PROF_ram_write_store)
        mem_cache_bloom |= word_addr;
        [branch]
        if (wi < 2) {
            if (wi == 0) { L1_DATA(at).x = bits; } else { L1_DATA(at).y = bits; }
        } else {
            if (wi == 2) { L1_DATA(at).z = bits; } else { L1_DATA(at).w = bits; }
        }
    } else {
    [branch]
    if (at != 0) {
        texel = L1_DATA(at);
    } else {
        [branch]
        if (t != dr_addr) {
            PROF(PROF_ram_read_tex)
            dr_tex = RAM_TEXEL(t);
            dr_addr = t;
        }
        texel = dr_tex;
    }
    uint cur_val = idx_uint4(texel, wi);
    if (at == 0 && cpu.cache.ram_l1_last_addr == word_addr && (word_addr & mem_cache_bloom) == word_addr) {
        cur_val = cpu.cache.ram_l1_last_val;
    }
    uint val = (cur_val & ~mask) | (bits & mask);
    [branch]
    if (val != cur_val) {
    PROF(PROF_ram_write_store)
    mem_cache_bloom |= word_addr;
    mem_dirty |= 1u << ((word_addr >> 22) & 31);
    set_idx_uint4(texel, val, wi);
    [branch]
    if (at != 0) {
        L1_DATA(at) = texel;
    } else if (f0 != 0) {
        set_idx_uint4(tags0, tag, f0 - 1);
        L1_TAGS(b0) = tags0;
        L1_DATA(b0 * 4 + f0) = texel;
        L1_OCC_SET(b0)
    } else if (f1 != 0) {
        set_idx_uint4(tags1, tag, f1 - 1);
        L1_TAGS(b1) = tags1;
        L1_DATA(b1 * 4 + f1) = texel;
        L1_OCC_SET(b1)
    } else {
        PROF(PROF_l1_stall)
        cpu.cache.ram_l1_last_addr = word_addr;
        cpu.cache.ram_l1_last_val = val;
        cpu.stall = STALL_MEM_CACHE_L1;
    }
    }
    }
#endif
}

// whole_word: addr is a word-aligned RAM address and val is the full 32-bit value.
void mem_set_byte_l1(L1P uint addr, uint val, bool whole_word) {
    if ((addr & 0x80000000) == 0) {
        irq_quiet = false;  // MMIO: UART, RTC
        [branch]
        switch (addr) {
            // UART (first has rbr_thr_ier_iir, second has lcr_mcr_lsr_scr)
            case 0x10000000:
                if ((UART_GET2(LCR) >> 7) == 0) {
                    UART_SET1(THR, val);
                    UART_SET2(LSR, (UART_GET2(LSR) & ~LSR_THR_EMPTY));
                    uart_update_iir();
                }
                return;
            case 0x10000001:
                if (UART_GET2(LCR) >> 7 == 0) {
                    if ((UART_GET1(IER) & IER_THREINT_BIT) == 0 &&
                        (val & IER_THREINT_BIT) != 0 &&
                        UART_GET1(THR) == 0)
                    {
                        cpu.uart.thre_ip = true;
                    }
                    UART_SET1(IER, val);
                    uart_update_iir();
                }
                return;
            case 0x10000003: UART_SET2(LCR, val); return;
            case 0x10000004: UART_SET2(MCR, val); return;
            case 0x10000007: UART_SET2(SCR, val); return;
            case 0x030007f8:
                // ignore control value, always perform RTC_READ
                cpu.rtc0 = _RTC0;
                cpu.rtc1 = _RTC1;
                return;
        }
        return;
    }

    addr = addr & 0x7FFFFFFF;

    if (addr >= RAM_MAX) {
        return;
    }

    uint word_addr = addr & (~0x3);
    uint byte_offset = (addr & 0x3) * 8;
    mem_set_ram(word_addr, whole_word ? val : (val << byte_offset), whole_word ? 0xffffffff : (0xffu << byte_offset));
}

void mem_set_l1(L1P uint addr, uint val, uint word_size) {

    if (word_size == WORD_SIZE_FULL && addr & 0x02000000) {
        if ((addr & 0x80000000) == 0) {
            irq_quiet = false;  // may be CLINT (msip, mtimecmp)
        }
        [branch]
        switch (addr & (~0x3)) {
            // CLINT/timer - only supports full word write as optimization
            case 0x02000000: cpu.clint.msip = (val & 1) != 0; return;
            case 0x02004000: cpu.clint.mtimecmp_lo = val; return;
            case 0x02004004: cpu.clint.mtimecmp_hi = val; return;
            case 0x0200bff8: cpu.clint.mtime_lo = val; return;
            case 0x0200bffc: cpu.clint.mtime_hi = val; return;
        }
    }

    bool whole_word = word_size == WORD_SIZE_FULL && (addr & 0x3) == 0 && (addr & 0x80000000) != 0;
    uint steps = whole_word ? 1 : word_size;
    [loop]
    for (uint i = 0; i < steps; i++) {
        mem_set_byte(addr + i, whole_word ? val : ((val >> (8 * i)) & 0xff), whole_word);
    }
}

#endif
