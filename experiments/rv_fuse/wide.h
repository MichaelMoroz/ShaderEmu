#ifndef WIDE_H
#define WIDE_H
// The wide loop: RV32 translated to bundles of 16 bytes kept below the ROM's rows (rows from the texture's y = 0), with RV32's
// registers, loads and stores. Layout of the code area, in texels c at (c & 2047, c >> 11):
//   rows 0-255      the kernel's map: an RV texel of 0x82000000.. -> its four words' bundle numbers, 0 where none starts
//   rows 256-2303   the programs' map: 0x84000000..0x88000000
//   rows 2304-4999  bundles, numbered by c
//   rows 5000-      the RV pc of each bundle, four a texel
// A bundle: X op 0, Y op 1 (a sum), Z op 0's immediate (or its RV pc for path 4), W control.
//   X, Y: rd 0-5 (32: the sink), ra 6-10, rb 11-15; X: size of a load or store, or the unit's function, at 16-19. Y: imm 20-31.
//   W: path 0-2 (0 sum, 1 unit, 2 load, 3 store, 4 the RISC-V word in X by the old step, 5 leave), RV instructions 3-5,
//      control 6-7 (0 on, 1 branch, 2 jump, 3 ret), branch's funct3 8-10, ra 11-15, rb 16-20, offset 21-31; jump's offset 8-31;
//      ret: ra 11-15, offset 21-31 as bytes.
#ifndef WIDE_COUNT
#define WIDE_COUNT 0
#endif
// WIDE_SHAPE, bits: 1 the old step is not inlined here (its words leave to the old loop); 2 op 1 is behind a test;
// 4 no reading ahead: the bundle is read at the top of its trip; 8 two bundles ahead (a read is done before its bundle is wanted)
#ifndef WIDE_SHAPE
#define WIDE_SHAPE 0
#endif
#if WIDE_SHAPE & 16384
#define WIDE_ROOM room
#else
#define WIDE_ROOM fast_budget
#endif
#define WIDE_B0 (2304u * 2048u)
#define WIDE_PCT (5000u * 2048u)
#define CODE_TEX(c) asuint(_Data_ROM[uint2((c) & 2047, (c) >> 11)])

// 2^28: the wide loop has an array of registers of its own
#if WIDE_SHAPE & 268435456
static uint wr[33];
#define WR wr
#else
#define WR xr
#endif

uint wide_lookup(uint pc) {
    uint k = pc - 0x82000000u, p = pc - 0x84000000u;
    uint at = k < 0x800000u ? k >> 4 : (256u * 2048u) + (p >> 4);
    bool inr = k < 0x800000u || p < 0x4000000u;
    return inr ? idx_uint4(CODE_TEX(at), (pc >> 2) & 3) : 0u;
}

uint wide_run_l1(L1P uint room) {
    pre_valid = false;
    fast_same = false;
    if (!irq_quiet) return 0;
#if WIDE_SHAPE & 131072
    // (a test: a loop that never runs, of nothing of the machine's)
    uint k_t = 0, k_s = room;
    [branch]
    if (cpu.pc == 3u) {
#if WIDE_SHAPE & 4194304
        uint4 k_c = CODE_TEX(k_s), k_n = CODE_TEX(k_s + 1);
#endif
#if WIDE_SHAPE & 2097152
        fast_budget = room;
#endif
        [loop]
        while (k_t < room) {
            k_s = k_s * 3 + k_t;
#if WIDE_SHAPE & 4194304
#if WIDE_SHAPE & 134217728
            k_s += xr[(k_c.x >> 6) & 0x1f] + xr[(k_c.x >> 11) & 0x1f];
#endif
            k_s += k_c.x ^ k_c.y ^ k_c.z ^ k_c.w;
            k_c = k_n;
            k_n = CODE_TEX(k_s + 1);
#endif
#if WIDE_SHAPE & 262144
            k_s += xr[k_s & 31];
#endif
#if WIDE_SHAPE & 524288
            k_s += CODE_TEX(k_s).x;
#endif
            k_t++;
            [branch]
            if (k_s == 7u) break;
        }
        cpu.clock += k_s;
#if WIDE_SHAPE & 1048576
        cpu.pc = k_s;
#endif
    }
    return 0;
#endif
#if WIDE_SHAPE & 512
    return 0;     // (bisecting what costs: only the old loop by stretches of 24)
#endif
    uint b = wide_lookup(cpu.pc);
    if (b == 0) return 0;
#if WIDE_SHAPE & 64
    return 0;     // (and the lookup)
#endif
#if !(WIDE_SHAPE & 16384)
    fast_budget = room;
#endif
    uint trips = 0, rv = 0;
    uint4 cur = CODE_TEX(b);
#if !(WIDE_SHAPE & 4)
    uint4 nx = CODE_TEX(b + 1);
#endif
#if WIDE_SHAPE & 8
    uint4 nx2 = CODE_TEX(b + 2);
#endif
#if WIDE_SHAPE & 268435456
    for (uint wr_k = 0; wr_k < 32; wr_k++) wr[wr_k] = xr[wr_k];
#endif
    [loop]
    while (true) {
        uint ctl = cur.w, X = cur.x, path = ctl & 7;
#if WIDE_SHAPE & 16
        // (a test: one read more a trip, of the texel just read, its result used)
        ctl ^= CODE_TEX(b).w ^ cur.w;
#endif
#if WIDE_SHAPE & 33554432
        uint a = 0, c = 0;
#elif WIDE_SHAPE & 16777216
        uint a = X, c = ctl;
#else
        uint a = WR[(X >> 6) & 0x1f], c = WR[(X >> 11) & 0x1f];
#endif
        uint v = 0, dst = X & 0x3f;
#if WIDE_SHAPE & 8388608
        v = a + c + cur.z;
#else
#if WIDE_SHAPE & 33554432
        [branch]
        if (path < 4) {
            a = WR[(X >> 6) & 0x1f];
            c = WR[(X >> 11) & 0x1f];
        }
#endif
        [branch]
        if (path < 2) {
            uint bv = c + cur.z;
            [branch]
            if (path == 0) {
                v = a + bv;
            } else {
#if WIDE_SHAPE & 1024
                break;
#endif
                uint f = (X >> 16) & 15;
                uint sh = bv & 0x1f;
                uint r0 = (f & 1) ? a - bv : a + bv;
                uint r1 = (f & 1) ? a | bv : a & bv;
                uint r2 = (f & 1) ? a << sh : a ^ bv;
                uint r3 = (f & 1) ? AS_UNSIGNED(AS_SIGNED(a) >> sh) : a >> sh;
                uint turn = (f & 1) ? 0 : 0x80000000;
                uint r4 = (a ^ turn) < (bv ^ turn) ? 1 : 0;
                v = (f & 8) ? r4 : (f & 4) ? ((f & 2) ? r3 : r2) : ((f & 2) ? r1 : r0);
            }
        } else if (path < 4) {
            uint va = a + cur.z;
            uint f3 = (X >> 16) & 7, off = va & 3;
#if WIDE_SHAPE & 256
            break;    // (the loop without memory)
#endif
            [branch]
            if (path == 2) {
                [branch]
                if (!(va != 0 && off + (1u << (f3 & 3)) <= 4 && (va ^ 0x80000000) < RAM_MAX)) break;
                uint m = mem_get_cached_or_tex(va & 0x7ffffffc) >> (off * 8);
                uint up = (f3 & 3) == 0 ? 24 : (f3 & 3) == 1 ? 16 : 0;
                m <<= up;
                v = (f3 & 4) ? m >> up : AS_UNSIGNED(AS_SIGNED(m) >> up);
            } else {
                [branch]
                if (!((va & 0x80000000) != 0 && off + (1u << f3) <= 4)) break;
                uint s_mask = (f3 == 2 ? 0xffffffff : f3 == 1 ? 0xffff : 0xff) << (off * 8);
                mem_set_ram(va & 0x7ffffffc, c << (off * 8), s_mask, false);
            }
        } else {
#if WIDE_SHAPE & 1
            break;
#else
            [branch]
            if (path != 4) break;
            cpu.pc = cur.z;
            [branch]
            if (!fast_exec(X)) { pre_word = X; pre_valid = true; break; }
            dst = 32;
            [branch]
            if ((ctl & 0xc0) != 0) {   // a jump the old step made: to where it went
                b = wide_lookup(cpu.pc);
                rv += (ctl >> 3) & 7;
                trips++;
                [branch]
                if (b == 0 || trips >= fast_budget) break;
                cur = CODE_TEX(b);
                nx = CODE_TEX(b + 1);
                continue;
            }
#endif
        }
#endif
#if WIDE_SHAPE & 67108864
        rv += (X * 3) ^ (ctl * 5) ^ (X >> 7) ^ (ctl >> 9) ^ (a * 7) ^ (c * 11);
#endif
#if WIDE_SHAPE & 8192
        rv += v + dst;
#else
        WR[dst] = v;
#endif
        uint Y = cur.y;
#if WIDE_SHAPE & 2
        [branch]
        if (Y != 32)
#endif
#if !(WIDE_SHAPE & 4096)
        WR[Y & 0x3f] = WR[(Y >> 6) & 0x1f] + WR[(Y >> 11) & 0x1f] + AS_UNSIGNED(AS_SIGNED(Y) >> 20);
#endif
        rv += (ctl >> 3) & 7;
        trips++;
#if !(WIDE_SHAPE & 2048)
        [branch]
        if ((ctl & 0xc0) != 0) {
            uint ca = WR[(ctl >> 11) & 0x1f];
            uint kind = ctl & 0xc0;
            bool go = true;
            uint next = b;
            [branch]
            if (kind == 0x40) {
                uint cb = WR[(ctl >> 16) & 0x1f];
                uint f = (ctl >> 8) & 7;
                bool lt = (f & 2) ? ca < cb : AS_SIGNED(ca) < AS_SIGNED(cb);
                go = ((f & 4) ? lt : ca == cb) != ((f & 1) != 0);
                next = b + AS_UNSIGNED(AS_SIGNED(ctl) >> 21);
            } else if (kind == 0x80) {
                next = b + AS_UNSIGNED(AS_SIGNED(ctl) >> 8);
            } else {
                cpu.pc = (ca + AS_UNSIGNED(AS_SIGNED(ctl) >> 21)) & 0xfffffffe;
                next = wide_lookup(cpu.pc);
                [branch]
                if (next == 0) { b = 0; break; }
            }
            [branch]
            if (go) {
                b = next;
                [branch]
                if (trips >= fast_budget) break;
#if WIDE_SHAPE & 4
                cur = CODE_TEX(b);
#else
                cur = CODE_TEX(b);
                nx = CODE_TEX(b + 1);
#if WIDE_SHAPE & 8
                nx2 = CODE_TEX(b + 2);
#endif
#endif
                continue;
            }
        }
#endif
        b++;
        [branch]
        if (trips >= WIDE_ROOM) break;
#if WIDE_SHAPE & 4
        cur = CODE_TEX(b);
#elif WIDE_SHAPE & 8
        cur = nx;
        nx = nx2;
        nx2 = CODE_TEX(b + 2);
#else
        cur = nx;
        nx = CODE_TEX(b + 1);
#endif
    }
#if WIDE_SHAPE & 268435456
    for (uint wr_j = 1; wr_j < 32; wr_j++) xr[wr_j] = wr[wr_j];
#endif
#if !(WIDE_SHAPE & 16384)
    [branch]
    if (b != 0) {
        cpu.pc = idx_uint4(CODE_TEX(WIDE_PCT + ((b - WIDE_B0) >> 2)), (b - WIDE_B0) & 3);
    }
#endif
#if WIDE_COUNT == 1
    rv = 0;                  // the clock says what the other loops ran
#elif WIDE_COUNT == 2
    rv = 1;                  // the clock says that and how many times this loop was entered and did not stop at once
#elif WIDE_COUNT == 3
    rv = trips;              // that and the bundles
#endif
#if !(WIDE_SHAPE & 16384)
    cpu.clock += rv;
#endif
    return trips;
}
#endif
