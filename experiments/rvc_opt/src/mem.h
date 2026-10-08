#ifndef MEM_H
#define MEM_H


// RAM_ADDR / RAM_LIN: see types.h
#define RAM_MAX (2048 * (4096 - 64) * 4 * 4)


/* shift by two to ignore byte offset */
// Write-cache geometry: 2^L1_SET_BITS sets per slice, L1_SLICES slices, two entries per set.
// Upstream is 9 bits x 2 slices (1024 texels). The set is the word's low bits xor the bits
// above them: upstream's (bits 2-8 and 11-12) put words 512 bytes apart in one set.
#ifndef L1_SET_BITS
#define L1_SET_BITS 9
#endif
#ifndef L1_SLICES
#define L1_SLICES 2
#endif
#define L1_SETS (1 << L1_SET_BITS)
#define L1_ENTRIES (L1_SETS * L1_SLICES)
#define RAM_L1_ARRAY_IDX(a) ((((a) >> 2) ^ ((a) >> (2 + L1_SET_BITS))) & (L1_SETS - 1))


#define WORD_SIZE_NONE 0
#define WORD_SIZE_BYTE 1
#define WORD_SIZE_HALF 2
#define WORD_SIZE_FULL 4

uint mem_get_instruction(uint addr) {
    addr = addr & 0x7FFFFFFF;
    uint idx = (addr >> 2) & 0x3;
    addr = addr >> 4;

    PROF(PROF_fetch_tex)
    uint4 raw = STATE_TEX(RAM_ADDR(addr));
    return idx_uint4(raw, idx);
}

// Basic bloom filter to slightly optimize memory caching (declared in types.h)

// One bit per write-cache entry written this pass. An entry that was never written is all
// zeros, so its array read (slow: the array is far larger than the small bitmap) can be skipped.
// The bits of a set's slices are neighbours, so one read of the bitmap answers for the set.
#define L1_OCC_SET(idx) l1_occ[L1_OCC_POS(idx) >> 5] |= 1u << (L1_OCC_POS(idx) & 31);
#define L1_OCC_SLICES(set) ((l1_occ[((set) * L1_SLICES) >> 5] >> (((set) * L1_SLICES) & 31)) & ((1u << L1_SLICES) - 1))


static uint dr_addr = 0xffffffff;   // the last RAM texel read for data, and its number
static uint4 dr_tex;

// addr must be aligned to word boundary (4 byte)
uint mem_get_cached_or_tex_l1(L1P uint addr) {
    PROF(PROF_ram_read)
    // query L1 cache
    if ((addr & mem_cache_bloom) == addr) {
        PROF(PROF_ram_read_bloom_pass)
        // array-style L1: most words that pass the filter are in no slice of their set
        uint set = RAM_L1_ARRAY_IDX(addr);
        uint occ = L1_OCC_SLICES(set);
        // An array read costs as much as a dozen additions: read the two addresses of an
        // entry, then only the value that matched.
        [branch]
        if (occ & 1) {
            [branch] if (l1_cache[set].x == addr) { PROF(PROF_ram_read_l1_hit) return l1_cache[set].y; }
            [branch] if (l1_cache[set].z == addr) { PROF(PROF_ram_read_l1_hit) return l1_cache[set].w; }
        }
#if L1_SLICES > 1
        [branch]
        if (occ & 2) {
            [branch] if (l1_cache[set + L1_SETS].x == addr) { PROF(PROF_ram_read_l1_hit) return l1_cache[set + L1_SETS].y; }
            [branch] if (l1_cache[set + L1_SETS].z == addr) { PROF(PROF_ram_read_l1_hit) return l1_cache[set + L1_SETS].w; }
        }
#endif

        if (cpu.cache.ram_l1_last_addr == addr) {
            // this may seem unnecessary, but is required for multi-byte write instructions
            // which fill the 'last' buffer initially and then need it to be read back correctly
            // to intertwine the new value into the cached word
            return cpu.cache.ram_l1_last_val;
        }
    }

    // Not in the cache: the RAM texture, whose texels hold four words. The texture does not
    // change during a pass, so the last texel read stays good, and data next to what was just
    // read (the rest of a structure, the next stack slot) costs no texture read.
    uint t = addr >> 4;
    [branch]
    if (t != dr_addr) {
        PROF(PROF_ram_read_tex)
        dr_tex = STATE_TEX(RAM_ADDR(t));
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

    PROF(PROF_ram_write_byte)
    // caching can cause stalls, so check for same value before storing
    uint word_addr = addr & (~0x3);
    uint byte_offset = (addr & 0x3)*8;
    uint cur_val = mem_get_cached_or_tex(word_addr);
    uint mask = whole_word ? 0xffffffff : (0xff << byte_offset);
    val = (cur_val & ~mask) | (whole_word ? val : (val << byte_offset));
    if (val != cur_val) {
        // put written value into L1 cache
        PROF(PROF_ram_write_store)
        mem_cache_bloom |= word_addr;
        mem_dirty |= 1u << ((word_addr >> 22) & 31);

        if (word_addr == 0) {
            // very special case
            cpu.cache.ram_l1_last_addr = word_addr;
            cpu.cache.ram_l1_last_val = val;
            cpu.stall = STALL_MEM_CACHE_L1;
            return;
        }

        uint arr_idx = RAM_L1_ARRAY_IDX(word_addr);
        uint4 cur = 0;
        [branch]
        if (L1_OCC(arr_idx)) {
            cur = l1_cache[arr_idx];
        }
        if (cur.x == 0 || cur.x == word_addr) {
            l1_cache[arr_idx] = uint4(word_addr, val, cur.zw);
            L1_OCC_SET(arr_idx)
        } else if (cur.z == 0 || cur.z == word_addr) {
            l1_cache[arr_idx].z = word_addr;
            l1_cache[arr_idx].w = val;
            L1_OCC_SET(arr_idx)
        } else {
#if L1_SLICES > 1
            arr_idx += L1_SETS;
            uint4 cur = 0;
            [branch]
            if (L1_OCC(arr_idx)) {
                cur = l1_cache[arr_idx];
            }
            if (cur.x == 0 || cur.x == word_addr) {
                l1_cache[arr_idx] = uint4(word_addr, val, cur.zw);
                L1_OCC_SET(arr_idx)
            } else if (cur.z == 0 || cur.z == word_addr) {
                l1_cache[arr_idx].z = word_addr;
                l1_cache[arr_idx].w = val;
                L1_OCC_SET(arr_idx)
            } else
#endif
            {
                PROF(PROF_l1_stall)
                cpu.cache.ram_l1_last_addr = word_addr;
                cpu.cache.ram_l1_last_val = val;
                cpu.stall = STALL_MEM_CACHE_L1;
            }
        }
    }
}

// A store that lies within one RAM word: `bits` replace the word's under `mask`. Each cache set
// is read once, for the lookup and the insertion both. A whole-word store never needs the old
// value from the texture; it is only compared (to skip a store that changes nothing) when the
// word is already in the cache.
void mem_set_ram_l1(L1P uint word_addr, uint bits, uint mask) {
    if (word_addr >= RAM_MAX) {
        return;
    }
    PROF(PROF_ram_write_byte)
    uint i0 = RAM_L1_ARRAY_IDX(word_addr);
    uint i1 = i0 + L1_SETS;
    uint occ = L1_OCC_SLICES(i0);
    // Only the addresses are read (an array read is costly); a value only when part of a word
    // is stored, and the second entry only when the first does not hold the word.
    bool real = word_addr != 0;   // address 0 is never cached (0 marks an empty entry)
    uint a0x = 0, a0z = 0, a1x = 0, a1z = 0;
    [branch]
    if (occ & 1) {
        a0x = l1_cache[i0].x;
        a0z = l1_cache[i0].z;
    }
    bool h0x = real && a0x == word_addr, h0z = real && a0z == word_addr;
#if L1_SLICES > 1
    [branch]
    if ((occ & 2) && !(h0x || h0z)) {
        a1x = l1_cache[i1].x;
        a1z = l1_cache[i1].z;
    }
#endif
    bool h1x = real && a1x == word_addr, h1z = real && a1z == word_addr;
    bool last = cpu.cache.ram_l1_last_addr == word_addr && (word_addr & mem_cache_bloom) == word_addr;
    bool known = h0x || h0z || h1x || h1z || last;
    uint val = bits;
    [branch]
    if (mask != 0xffffffff) {
        uint cur_val;
        [branch]
        if (known) {
            if (h0x) { cur_val = l1_cache[i0].y; }
            else if (h0z) { cur_val = l1_cache[i0].w; }
            else if (h1x) { cur_val = l1_cache[i1].y; }
            else if (h1z) { cur_val = l1_cache[i1].w; }
            else { cur_val = cpu.cache.ram_l1_last_val; }
        } else {
            PROF(PROF_ram_read_tex)
            cur_val = idx_uint4(STATE_TEX(RAM_ADDR(word_addr >> 4)), (word_addr >> 2) & 0x3);
        }
        val = (cur_val & ~mask) | (bits & mask);
        // a store that changes nothing in a word not yet cached is not worth an entry
        if (!known && val == cur_val) {
            return;
        }
    }
    PROF(PROF_ram_write_store)
    mem_cache_bloom |= word_addr;
    mem_dirty |= 1u << ((word_addr >> 22) & 31);
    // the entry that already is this word's, else the first free one in the order lookups search
    if (h0x) {
        l1_cache[i0].y = val;
    } else if (h0z) {
        l1_cache[i0].w = val;
    } else if (h1x) {
        l1_cache[i1].y = val;
    } else if (h1z) {
        l1_cache[i1].w = val;
    } else if (real && (occ & 1) == 0) {
        l1_cache[i0] = uint4(word_addr, val, 0, 0);
        L1_OCC_SET(i0)
    } else if (real && a0z == 0) {
        l1_cache[i0].z = word_addr;
        l1_cache[i0].w = val;
#if L1_SLICES > 1
    } else if (real && (occ & 2) == 0) {
        l1_cache[i1] = uint4(word_addr, val, 0, 0);
        L1_OCC_SET(i1)
    } else if (real && a1z == 0) {
        l1_cache[i1].z = word_addr;
        l1_cache[i1].w = val;
#endif
    } else {
        PROF(PROF_l1_stall)
        cpu.cache.ram_l1_last_addr = word_addr;
        cpu.cache.ram_l1_last_val = val;
        cpu.stall = STALL_MEM_CACHE_L1;
    }
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
