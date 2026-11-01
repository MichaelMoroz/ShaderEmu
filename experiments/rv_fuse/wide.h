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
#ifndef WIDE_STRIDE
#define WIDE_STRIDE 1u
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

#if WIDE_TEST == 6
#define WIDE_LEAVE w_stop = true
#else
#define WIDE_LEAVE break
#endif

#if WIDE_PROBE == 6
static uint w_scratch[32];
#endif
#ifdef WIDE_FAKE
static uint fk[32];
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
#ifdef WIDE_FAKE
    // (a test: what kinds of operation cost in a loop of 64 trips, the machine not touched; each re-entry: 64 trips, then 24 of the old loop)
    {
        uint fk_s = room + cpu.pc, fk_t = 0;
        uint4 fk_c = uint4(fk_s, fk_s, fk_s, fk_s), fk_n = fk_c;
        [loop]
        while (fk_t < 64u) {
#if WIDE_FAKE & 1
            fk_s += fk[fk_s & 31];
            fk_s = fk_s * 3 + fk_t;
#endif
#if WIDE_FAKE & 2
            fk[(fk_s >> 5) & 31] = fk_s;
            fk_s = fk_s * 3 + fk_t;
#endif
#if WIDE_FAKE & 4
            fk_s += xr[fk_s & 31];
            fk_s = fk_s * 3 + fk_t;
#endif
#if WIDE_FAKE & 8
            fk_c = CODE_TEX((fk_s & 0xfffff) + 4096u);
            fk_s += fk_c.x ^ fk_c.w;
#endif
#ifdef WIDE_FAKE_MASK
            fk_c = CODE_TEX((fk_s & WIDE_FAKE_MASK) + WIDE_B0);
            fk_s += fk_c.x ^ fk_c.w;
#endif
#if WIDE_FAKE & 128
            fk_c = CODE_TEX(fk_t + 4096u);
            fk_s += fk_c.x ^ fk_c.w;
#endif
#if WIDE_FAKE & 256
            fk_s += fk_c.x ^ fk_c.w;
            fk_c = fk_n;
            fk_n = CODE_TEX(fk_t + 4096u);
#endif
#if WIDE_FAKE & 512
            fk_c = CODE_TEX((fk_t & 3) + 4096u);
            fk_s += fk_c.x ^ fk_c.w;
#endif
#if WIDE_FAKE & 16
            fk_s += fk[(fk_s >> 10) & 31];
            fk_s = fk_s * 3 + fk_t;
#endif
#if WIDE_FAKE & 32
            fk[(fk_s >> 15) & 31] = fk_s;
            fk_s = fk_s * 3 + fk_t;
#endif
#if WIDE_FAKE & 64
            [branch]
            if (fk_s == 0x7654321u) fk_s += 3;
            fk_s = fk_s * 3 + fk_t;
            [branch]
            if (fk_s == 0x7654323u) fk_s += 5;
            fk_s = fk_s * 3 + fk_t;
#endif
            fk_s = fk_s * 5 + fk_t;
            fk_t++;
        }
        if (fk_s == 0x1234567u) cpu.clock += 1;
    }
    return 0;
#endif
#if WIDE_SHAPE & 131072
    // (a test: a loop that never runs, of nothing of the machine's)
    uint k_t = 0, k_s = room;
    [branch]
#if WIDE_TEST == 8
    if (wide_lookup(cpu.pc) != 0u) {
#else
    if (cpu.pc == 3u) {
#endif
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
#if WIDE_SHAPE & 1073741824
    if (cpu.pc != 3u) return 0;   // (a test: the guard of the loop that costs nothing)
#endif
#if WIDE_TEST == 1
    if (cpu.pc == 3u) return 0;   // (a test: as much text, and the lookup still made)
#elif WIDE_TEST == 2
    if (wide_lookup(cpu.pc) == 7u) cpu.clock += 1;   // (a test: the lookup alone, no loop)
    return 0;
#elif WIDE_TEST == 3
    if (cpu.pc < 0x82000000u) return 0;   // (a test: the map's lookup only where a program can be... never here, all pcs pass)
#endif
#if WIDE_TEST == 7
    uint b = cpu.pc == 3u ? room + 5u : 0u;     // (a test: no lookup, no texture read before the loop)
#else
    uint b = wide_lookup(cpu.pc);
#endif
#ifdef WIDE_SYNTH
    b = WIDE_B0;
    room = WIDE_SYNTH;
#endif
#if WIDE_TEST == 4
    uint b_has = b != 0 ? 1u : 0u;
    if (b_has != 1u) return 0;   // (a test: the same, the return the likely side by the look of the test)
#elif WIDE_TEST == 5
    if (b < 1u) return 0;
#else
    if (b == 0) return 0;
#endif
#if WIDE_SHAPE & 64
    return 0;     // (and the lookup)
#endif
#if !(WIDE_SHAPE & 16384)
    fast_budget = room;
#endif
    uint trips = 0, rv = 0, w_c = 0;
    uint4 cur = CODE_TEX(b);
#if !(WIDE_SHAPE & 4)
    uint4 nx = CODE_TEX(b + WIDE_STRIDE);
#endif
#if WIDE_SHAPE & 8
    uint4 nx2 = CODE_TEX(b + 2);
#endif
#if WIDE_SHAPE & 268435456
    for (uint wr_k = 0; wr_k < 32; wr_k++) wr[wr_k] = xr[wr_k];
#endif
#ifdef WIDE_PROBE
    uint w_probe = 0;
#endif
    [loop]
    while (true) {
#ifdef WIDE_F
        // (gpu_vm's fast step's form: both successors asked for every trip, no block around a fetch, the six registers read at the
        // top, control by selects; a near jump is a beq x0, x0. A bundle with anything else goes through the body below, one at a time.)
        [loop]
        while ((cur.w & 0x84) == 0 && trips < WIDE_ROOM) {
            uint f_ctl = cur.w, f_X = cur.x, f_Y = cur.y, f_Z = cur.z;
            uint f_tn = b + WIDE_STRIDE, f_tg = b + AS_UNSIGNED(AS_SIGNED(f_ctl) >> 21);
            uint4 f_nx = CODE_TEX(f_tn), f_tc = CODE_TEX(f_tg);
            uint f_a = WR[(f_X >> 6) & 0x1f], f_c = WR[(f_X >> 11) & 0x1f];
            uint f_ya = WR[(f_Y >> 6) & 0x1f], f_yb = WR[(f_Y >> 11) & 0x1f];
            uint f_ca = WR[(f_ctl >> 11) & 0x1f], f_cb = WR[(f_ctl >> 16) & 0x1f];
            uint f_v, f_dst = f_X & 0x3f;
            [branch]
            if ((f_ctl & 2u) == 0u) {
                uint f_b = f_c + f_Z, f_f = (f_X >> 16) & 15, f_sh = f_b & 0x1f;
                uint f_r0 = (f_f & 1) ? f_a - f_b : f_a + f_b;
                uint f_r1 = (f_f & 1) ? f_a | f_b : f_a & f_b;
                uint f_r2 = (f_f & 1) ? f_a << f_sh : f_a ^ f_b;
                uint f_r3 = (f_f & 1) ? AS_UNSIGNED(AS_SIGNED(f_a) >> f_sh) : f_a >> f_sh;
                uint f_turn = (f_f & 1) ? 0 : 0x80000000;
                uint f_r4 = (f_a ^ f_turn) < (f_b ^ f_turn) ? 1 : 0;
                uint f_u = (f_f & 8) ? f_r4 : (f_f & 4) ? ((f_f & 2) ? f_r3 : f_r2) : ((f_f & 2) ? f_r1 : f_r0);
                f_v = (f_ctl & 1u) ? f_u : f_a + f_b;
            } else {
                uint f_va = f_a + f_Z, f_3 = (f_X >> 16) & 7, f_off = f_va & 3;
                [branch]
                if ((f_ctl & 1u) == 0u) {
                    [branch]
                    if (!(f_va != 0 && f_off + (1u << (f_3 & 3)) <= 4 && (f_va ^ 0x80000000) < RAM_MAX)) break;
                    uint f_m = mem_get_cached_or_tex(f_va & 0x7ffffffc) >> (f_off * 8);
                    uint f_up = (f_3 & 3) == 0 ? 24 : (f_3 & 3) == 1 ? 16 : 0;
                    f_m <<= f_up;
                    f_v = (f_3 & 4) ? f_m >> f_up : AS_UNSIGNED(AS_SIGNED(f_m) >> f_up);
                } else {
                    [branch]
                    if (!((f_va & 0x80000000) != 0 && f_off + (1u << f_3) <= 4)) break;
                    uint f_mask = (f_3 == 2 ? 0xffffffff : f_3 == 1 ? 0xffff : 0xff) << (f_off * 8);
                    mem_set_ram(f_va & 0x7ffffffc, f_c << (f_off * 8), f_mask, false);
                    f_v = 0;
                    f_dst = 32;
                }
            }
            WR[f_dst] = f_v;
            uint f_yra = (f_Y >> 6) & 0x1f, f_yrb = (f_Y >> 11) & 0x1f;
            WR[f_Y & 0x3f] = (f_yra == f_dst ? f_v : f_ya) + (f_yrb == f_dst ? f_v : f_yb) + AS_UNSIGNED(AS_SIGNED(f_Y) >> 20);
            uint f_f3 = (f_ctl >> 8) & 7;
            bool f_lt = (f_f3 & 2) ? f_ca < f_cb : AS_SIGNED(f_ca) < AS_SIGNED(f_cb);
            bool f_go = (f_ctl & 0x40u) != 0 && ((f_f3 & 4) ? f_lt : f_ca == f_cb) != ((f_f3 & 1) != 0);
            rv += (f_ctl >> 3) & 7;
            trips++;
            b = f_go ? f_tg : f_tn;
            cur = f_go ? f_tc : f_nx;
        }
        [branch]
        if (trips >= WIDE_ROOM) break;
        nx = CODE_TEX(b + WIDE_STRIDE);
#endif
        uint ctl = cur.w, X = cur.x, path = ctl & 7;
#ifdef WIDE_V2
        // (the plain trip: arithmetic, no control, one test; no budget test on this way: every straight run ends in control or a leaving bundle)
        [branch]
        if ((ctl & 0xc6u) == 0u) {
            uint va0 = WR[(X >> 6) & 0x1f], vc0 = WR[(X >> 11) & 0x1f] + cur.z, vv0;
            [branch]
            if ((ctl & 1u) == 0u) {
                vv0 = va0 + vc0;
            } else {
                uint f0 = (X >> 16) & 15, sh0 = vc0 & 0x1f;
                uint r00 = (f0 & 1) ? va0 - vc0 : va0 + vc0;
                uint r10 = (f0 & 1) ? va0 | vc0 : va0 & vc0;
                uint r20 = (f0 & 1) ? va0 << sh0 : va0 ^ vc0;
                uint r30 = (f0 & 1) ? AS_UNSIGNED(AS_SIGNED(va0) >> sh0) : va0 >> sh0;
                uint turn0 = (f0 & 1) ? 0 : 0x80000000;
                uint r40 = (va0 ^ turn0) < (vc0 ^ turn0) ? 1 : 0;
                vv0 = (f0 & 8) ? r40 : (f0 & 4) ? ((f0 & 2) ? r30 : r20) : ((f0 & 2) ? r10 : r00);
            }
            WR[X & 0x3f] = vv0;
            uint Y0 = cur.y;
            [branch]
            if (Y0 != 32u) {
                WR[Y0 & 0x3f] = WR[(Y0 >> 6) & 0x1f] + WR[(Y0 >> 11) & 0x1f] + AS_UNSIGNED(AS_SIGNED(Y0) >> 20);
            }
            rv += (ctl >> 3) & 7;
            trips++;
            b += WIDE_STRIDE;
            cur = nx;
            nx = CODE_TEX(b + WIDE_STRIDE);
            continue;
        }
#endif
#ifdef WIDE_PROBE
        // (what one thing costs a trip, by the tick with it against the tick without)
#if WIDE_PROBE == 1
        w_probe ^= xr[(cur.z >> 3) & 31];
#elif WIDE_PROBE == 2
        w_probe ^= xr[(cur.z >> 3) & 31] ^ xr[(cur.z >> 8) & 31] ^ xr[(cur.z >> 13) & 31] ^ xr[(cur.z >> 18) & 31];
#elif WIDE_PROBE == 3
        w_probe ^= xr[5] ^ xr[6] ^ xr[7] ^ xr[8];
#elif WIDE_PROBE == 4
        w_probe = ((w_probe * 3) ^ (cur.z >> 3)) + (cur.z << 3);
        w_probe = ((w_probe >> 3) ^ (cur.z * 5)) + (cur.z >> 7);
#elif WIDE_PROBE == 5
        [branch]
        if (cur.z == 0x12345u) w_probe++;
#elif WIDE_PROBE == 6
        w_scratch[(cur.z >> 3) & 31] = X;
#elif WIDE_PROBE == 8
        w_probe ^= mem_get_cached_or_tex(xr[2] & 0x7ffffffc);
#elif WIDE_PROBE == 9
        w_probe ^= mem_get_cached_or_tex((xr[2] + 256u) & 0x7ffffffc);
#elif WIDE_PROBE == 10
        { uint w_a = xr[2] & 0x7ffffffc; uint w_v = mem_get_cached_or_tex(w_a); w_probe ^= w_v; mem_set_ram(w_a, w_v, 0xffffffffu, false); }
#elif WIDE_PROBE == 7
        w_probe ^= CODE_TEX(b + 7).x;
#endif
#endif
#if WIDE_TEST == 6
        bool w_stop = false;
#endif
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
                WIDE_LEAVE;
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
            WIDE_LEAVE;    // (the loop without memory)
#endif
            [branch]
            if (path == 2) {
                [branch]
                if (!(va != 0 && off + (1u << (f3 & 3)) <= 4 && (va ^ 0x80000000) < RAM_MAX)) { WIDE_LEAVE; }
                uint m = mem_get_cached_or_tex(va & 0x7ffffffc) >> (off * 8);
                uint up = (f3 & 3) == 0 ? 24 : (f3 & 3) == 1 ? 16 : 0;
                m <<= up;
                v = (f3 & 4) ? m >> up : AS_UNSIGNED(AS_SIGNED(m) >> up);
            } else {
                [branch]
                if (!((va & 0x80000000) != 0 && off + (1u << f3) <= 4)) { WIDE_LEAVE; }
                uint s_mask = (f3 == 2 ? 0xffffffff : f3 == 1 ? 0xffff : 0xff) << (off * 8);
                mem_set_ram(va & 0x7ffffffc, c << (off * 8), s_mask, false);
            }
        } else {
#if WIDE_SHAPE & 1
            WIDE_LEAVE;
#else
            [branch]
            if (path != 4) { WIDE_LEAVE; }
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
                if (b == 0 || trips >= fast_budget) { WIDE_LEAVE; }
                cur = CODE_TEX(b);
                nx = CODE_TEX(b + WIDE_STRIDE);
                continue;
            }
#endif
        }
#endif
#if WIDE_TEST == 6
        [branch]
        if (w_stop) break;
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
#if WIDE_COUNT == 4
            if (kind == 0xc0) w_c++;
#endif
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
#if WIDE_COUNT == 5
                w_c++;
#endif
                b = next;
                [branch]
                if (trips >= fast_budget) break;
#if WIDE_SHAPE & 4
                cur = CODE_TEX(b);
#else
                cur = CODE_TEX(b);
                nx = CODE_TEX(b + WIDE_STRIDE);
#if WIDE_SHAPE & 8
                nx2 = CODE_TEX(b + 2);
#endif
#endif
                continue;
            }
        }
#endif
        b += WIDE_STRIDE;
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
        nx = CODE_TEX(b + WIDE_STRIDE);
#endif
    }
#ifndef WIDE_SYNTH
#if WIDE_SHAPE & 268435456
    for (uint wr_j = 1; wr_j < 32; wr_j++) xr[wr_j] = wr[wr_j];
#endif
#endif
#ifdef WIDE_SYNTH
    if (b == 0x1234567u) cpu.clock += 1;
    return 0;
#endif
#if !(WIDE_SHAPE & 16384)
    [branch]
    if (b != 0) {
        cpu.pc = idx_uint4(CODE_TEX(WIDE_PCT + (((b - WIDE_B0) / WIDE_STRIDE) >> 2)), ((b - WIDE_B0) / WIDE_STRIDE) & 3);
    }
#endif
#ifdef WIDE_PROBE
    if (w_probe == 0x1234567u) cpu.clock += 1;
#endif
#if WIDE_COUNT >= 4
    rv = w_c;
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
