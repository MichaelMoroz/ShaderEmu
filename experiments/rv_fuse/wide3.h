#ifndef WIDE3_H
#define WIDE3_H
// The third wide loop: blocks of RV32 list-scheduled by xlate3.py into bundles of three ops, the memory ops first (two at most, one store at most),
// RV's registers. A bundle: X, Y, Z ops, W control. Code area as wide.h's.
//   op: rd 0-5 (32: sink), ra 6-10, rb 11-15, fn 16-19, imm 20-31. fn: 0 add 1 sub 2 and 3 or 4 xor 5 sll 6 srl 7 sra 8 slt 9 sltu 10 mul 15 (X only) Z as a constant.
//   a memory op: fn bit 3 a store, bits 0-2 RV's funct3; the address ra + imm; a store's data rb. It reads nothing its bundle makes.
//   W: bit 0 X is memory, bit 1 Y is memory, bit 2 slow (path 4: Z the RV pc, X the word, by the old step; path 5 leave),
//      RV instructions 3-5, control 6-7 (0 on, 1 branch, 2 far jump, 3 ret), funct3 8-10, ra 11-15, rb 16-20, offset 21-31; far jump's offset 8-31.
// The table of pcs gives each bundle the RV pc to go on from: everything before it is done, and whatever later was done with it can be done again.
// W3_SHAPE, bits: 1 both successors fetched every trip; 2 the six register reads first (not in order: forbids nothing, ops chain by selects)
#ifndef W3_SHAPE
#define W3_SHAPE 0
#endif
// 2048: x8 to x15 (the registers compilers use most) kept in two uint4 temporaries as well as in the array, so a read of one is a few selects, not an array read;
// a write goes to both. (The old step is out of the loop, shape 8: nothing else writes the array while it runs.)
#if W3_SHAPE & 2048
#define XRD(i) w3r((i), h0, h1)
#define XWR(i, v) { uint wi_ = (i), wv_ = (v); xr[wi_] = wv_; uint wk_ = wi_ - 8u; \
    h0 = uint4(wk_ == 0u ? wv_ : h0.x, wk_ == 1u ? wv_ : h0.y, wk_ == 2u ? wv_ : h0.z, wk_ == 3u ? wv_ : h0.w); \
    h1 = uint4(wk_ == 4u ? wv_ : h1.x, wk_ == 5u ? wv_ : h1.y, wk_ == 6u ? wv_ : h1.z, wk_ == 7u ? wv_ : h1.w); }
uint w3r(uint i, uint4 a, uint4 b) {
    uint v;
    [branch]
    if ((i & 24u) == 8u) {
        uint4 q = (i & 4u) ? b : a;
        v = (i & 2u) ? ((i & 1u) ? q.w : q.z) : ((i & 1u) ? q.y : q.x);
    } else {
        v = xr[i];
    }
    return v;
}
#else
#define XRD(i) xr[i]
#define XWR(i, v) xr[i] = (v)
#endif
// 1024: floats as ops: fn 11 fadd, 12 fsub, 13 fmul, 14 a float register as it is (fsgnj of one, fmv.x), a destination 33 and up is a float register,
// flw a load to one, fsw a store with funct3 3 (its data from a float register)
#if W3_SHAPE & 1024
#define W3_OP(op) { uint op_ = (op); [branch] if ((op_ & 0x3f) != 32u) { uint rb_ = (op_ >> 11) & 0x1f; uint fn_ = (op_ >> 16) & 15; \
    [branch] if (fn_ >= 11u) { uint fa_ = xr[33u + ((op_ >> 6) & 0x1f)]; [branch] if (fn_ != 14u) { precise float pa_ = asfloat(fa_); precise float pb_ = asfloat(xr[33u + rb_]); precise float ps_ = pa_ + pb_; precise float pd_ = pa_ - pb_; precise float pm_ = pa_ * pb_; \
            fa_ = asuint(fn_ == 11u ? ps_ : fn_ == 12u ? pd_ : pm_); } XWR(op_ & 0x3f, fa_); } \
    else { uint c_ = 0; [branch] if (rb_ != 0u) c_ = XRD(rb_); XWR(op_ & 0x3f, w3_alu(op_, XRD((op_ >> 6) & 0x1f), c_)); } } }
#define W3_DATA(op, f) xr[(((op) >> 11) & 0x1f) + ((f) == 3u ? 33u : 0u)]
#else
#define W3_DATA(op, f) XRD(((op) >> 11) & 0x1f)
#endif
#if W3_SHAPE & 1024
#elif W3_SHAPE & 256
#define W3_OP(op) { uint op_ = (op); [branch] if ((op_ & 0x3f) != 32u) { uint rb_ = (op_ >> 11) & 0x1f; uint c_ = 0; [branch] if (rb_ != 0u) c_ = XRD(rb_); XWR(op_ & 0x3f, w3_alu(op_, XRD((op_ >> 6) & 0x1f), c_)); } }
#elif W3_SHAPE & 64
#define W3_OP(op) { uint op_ = (op); [branch] if ((op_ & 0x3f) != 32u) XWR(op_ & 0x3f, w3_alu(op_, XRD((op_ >> 6) & 0x1f), XRD((op_ >> 11) & 0x1f))); }
#else
#define W3_OP(op) { uint op_ = (op); XWR(op_ & 0x3f, w3_alu(op_, XRD((op_ >> 6) & 0x1f), XRD((op_ >> 11) & 0x1f))); }
#endif
#define WIDE_B0 (2304u * 2048u)
#define WIDE_PCT (5000u * 2048u)
#define CODE_TEX(c) asuint(_Data_ROM[uint2((c) & 2047, (c) >> 11)])

// 32: bundles in tiles of 16 x 16 texels of the code area, not along its rows (what lies near in the list lies near in the texture's cache)
#if W3_SHAPE & 512
#define W3_CR(c, X) c = W3_DATA(X, xf)
#else
#define W3_CR(c, X)
#endif
#if W3_SHAPE & 32
#define BUNDLE_TEX(c) asuint(_Data_ROM[uint2(((c) & 15) | (((c) >> 4) & 0x7f0), 2304u + ((((c) >> 4) & 15) | (((c) >> 11) & 0xf0)))])
#else
#define BUNDLE_TEX(c) CODE_TEX(c)
#endif

uint wide_lookup(uint pc) {
    uint k = pc - 0x82000000u, p = pc - 0x84000000u;
    uint at = k < 0x800000u ? k >> 4 : (256u * 2048u) + (p >> 4);
    bool inr = k < 0x800000u || p < 0x4000000u;
    return inr ? idx_uint4(CODE_TEX(at), (pc >> 2) & 3) : 0u;
}
uint w3_alu(uint op, uint a, uint c) {
    uint f = (op >> 16) & 15, b = c + AS_UNSIGNED(AS_SIGNED(op) >> 20), sh = b & 0x1f;
    uint r0 = (f & 1) ? a - b : a + b;
    uint r1 = (f & 1) ? a | b : a & b;
    uint r2 = (f & 1) ? a << sh : a ^ b;
    uint r3 = (f & 1) ? AS_UNSIGNED(AS_SIGNED(a) >> sh) : a >> sh;
    uint turn = (f & 1) ? 0 : 0x80000000;
    uint r4 = (f & 2) ? a * b : (a ^ turn) < (b ^ turn) ? 1 : 0;
    return (f & 8) ? r4 : (f & 4) ? ((f & 2) ? r3 : r2) : ((f & 2) ? r1 : r0);
}
uint w3_ext(uint m, uint f3) {
    uint up = (f3 & 3) == 0 ? 24 : (f3 & 3) == 1 ? 16 : 0;
    m <<= up;
    return (f3 & 4) ? m >> up : AS_UNSIGNED(AS_SIGNED(m) >> up);
}
#define W3_LOAD_OK(va, f3) ((va) != 0 && ((va) & 3) + (1u << ((f3) & 3)) <= 4 && ((va) ^ 0x80000000) < RAM_MAX)
#define W3_STORE_OK(va, f3) (((va) & 0x80000000) != 0 && ((va) & 3) + (1u << ((f3) & 2 ? 2u : (f3))) <= 4)

uint wide_run_l1(L1P uint room) {
    pre_valid = false;
    fast_same = false;
    if (!irq_quiet) return 0;
    uint b = wide_lookup(cpu.pc);
    if (b == 0) return 0;
    fast_budget = room;
    uint rv = 0;
#if W3_SHAPE & 2048
    uint4 h0 = uint4(xr[8], xr[9], xr[10], xr[11]), h1 = uint4(xr[12], xr[13], xr[14], xr[15]);
#endif
    uint4 cur = BUNDLE_TEX(b);
#if !(W3_SHAPE & 4)
    uint4 nx = BUNDLE_TEX(b + 1);
#endif
    [loop]
    while (true) {
        uint W = cur.w, X = cur.x, Y = cur.y, Z = cur.z;
#if W3_SHAPE & 1
        uint tg3 = b + AS_UNSIGNED(AS_SIGNED(W) >> 21);
        uint4 tc = BUNDLE_TEX(tg3);
#endif
        [branch]
        if ((W & 4u) != 0u) {
#if W3_SHAPE & 8
            break;
#else
            [branch]
            if ((W & 7u) != 4u) break;
            cpu.pc = Z;
            [branch]
            if (!fast_exec(X)) { pre_word = X; pre_valid = true; b = 0; break; }
            rv += 1;
            [branch]
            if ((W & 0xc0u) != 0u) {     // a jump the old step made
                b = wide_lookup(cpu.pc);
                [branch]
                if (b == 0 || rv >= fast_budget) { break; }
                cur = BUNDLE_TEX(b);
                nx = BUNDLE_TEX(b + 1);
                continue;
            }
            b += 1;
            [branch]
            if (rv >= fast_budget) break;
            cur = nx;
            nx = BUNDLE_TEX(b + 1);
            continue;
#endif
        }
        uint xv, xd = X & 0x3f;
        [branch]
        if ((W & 3u) != 0u) {
            uint xf = (X >> 16) & 7, xa = XRD((X >> 6) & 0x1f) + AS_UNSIGNED(AS_SIGNED(X) >> 20);
#if W3_SHAPE & 512
            uint xc = 0;
#else
            uint xc = W3_DATA(X, xf);
#endif
            bool xs = (X & 0x80000u) != 0;
#if !(W3_SHAPE & 16)
            [branch]
            if ((W & 2u) != 0u) {
                uint yf = (Y >> 16) & 7, ya = XRD((Y >> 6) & 0x1f) + AS_UNSIGNED(AS_SIGNED(Y) >> 20);
#if W3_SHAPE & 512
                uint yc = W3_DATA(Y, yf);
#else
                uint yc = W3_DATA(Y, yf);
#endif
                bool ys = (Y & 0x80000u) != 0;
                [branch]
                if (!((xs ? W3_STORE_OK(xa, xf) : W3_LOAD_OK(xa, xf)) && (ys ? W3_STORE_OK(ya, yf) : W3_LOAD_OK(ya, yf)))) break;
                xv = 0;
                [branch]
                if (xs) {
                    uint xm = ((xf & 2) ? 0xffffffff : xf == 1 ? 0xffff : 0xff) << ((xa & 3) * 8);
                    W3_CR(xc, X);
                    mem_set_ram(xa & 0x7ffffffc, xc << ((xa & 3) * 8), xm, false);
                    xd = 32;
                } else {
                    xv = w3_ext(mem_get_cached_or_tex(xa & 0x7ffffffc) >> ((xa & 3) * 8), xf);
                }
                XWR(xd, xv);
                uint yv = 0, yd = Y & 0x3f;
                [branch]
                if (ys) {
                    uint ym = ((yf & 2) ? 0xffffffff : yf == 1 ? 0xffff : 0xff) << ((ya & 3) * 8);
                    mem_set_ram(ya & 0x7ffffffc, yc << ((ya & 3) * 8), ym, false);
                    yd = 32;
                } else {
                    yv = w3_ext(mem_get_cached_or_tex(ya & 0x7ffffffc) >> ((ya & 3) * 8), yf);
                }
                XWR(yd, yv);
            } else
#endif
            {
                [branch]
                if (!(xs ? W3_STORE_OK(xa, xf) : W3_LOAD_OK(xa, xf))) break;
                xv = 0;
                [branch]
                if (xs) {
                    uint xm = ((xf & 2) ? 0xffffffff : xf == 1 ? 0xffff : 0xff) << ((xa & 3) * 8);
                    W3_CR(xc, X);
                    mem_set_ram(xa & 0x7ffffffc, xc << ((xa & 3) * 8), xm, false);
                    xd = 32;
                } else {
                    xv = w3_ext(mem_get_cached_or_tex(xa & 0x7ffffffc) >> ((xa & 3) * 8), xf);
                }
                XWR(xd, xv);
                W3_OP(Y);
            }
        } else {
#if W3_SHAPE & 1024
            [branch]
            if ((X & 0xf0000u) == 0xf0000u) XWR(xd, Z);
            else W3_OP(X);
#else
            uint xu = w3_alu(X, XRD((X >> 6) & 0x1f), XRD((X >> 11) & 0x1f));
            XWR(xd, (X & 0xf0000u) == 0xf0000u ? Z : xu);
#endif
            W3_OP(Y);
        }
        uint zop = (X & 0xf0000u) == 0xf0000u ? 32u : Z;
        W3_OP(zop);
        rv += (W >> 3) & 7;
#if W3_SHAPE & 128
        bool go = false;
        uint ca = 0;
        [branch]
        if ((W & 0xc0u) != 0u) {
            ca = XRD((W >> 11) & 0x1f);
            uint cb = XRD((W >> 16) & 0x1f);
            uint f3 = (W >> 8) & 7;
            bool lt = (f3 & 2) ? ca < cb : AS_SIGNED(ca) < AS_SIGNED(cb);
            go = (W & 0x40u) != 0 && ((f3 & 4) ? lt : ca == cb) != ((f3 & 1) != 0);
        }
#else
        uint ca = XRD((W >> 11) & 0x1f), cb = XRD((W >> 16) & 0x1f);
        uint f3 = (W >> 8) & 7;
        bool lt = (f3 & 2) ? ca < cb : AS_SIGNED(ca) < AS_SIGNED(cb);
        bool go = (W & 0x40u) != 0 && ((f3 & 4) ? lt : ca == cb) != ((f3 & 1) != 0);
#endif
        [branch]
        if ((W & 0x80u) != 0u) {         // far jump, or ret
            uint to;
            [branch]
            if ((W & 0x40u) != 0u) {
                cpu.pc = (ca + AS_UNSIGNED(AS_SIGNED(W) >> 21)) & 0xfffffffe;
                to = wide_lookup(cpu.pc);
            } else {
                to = b + AS_UNSIGNED(AS_SIGNED(W) >> 8);
            }
            b = to;
            [branch]
            if (b == 0 || rv >= fast_budget) break;
            cur = BUNDLE_TEX(b);
            nx = BUNDLE_TEX(b + 1);
            continue;
        }
#if W3_SHAPE & 1
        uint4 nxt = go ? tc : nx;
        b = go ? tg3 : b + 1;
        [branch]
        if (rv >= fast_budget) break;
        cur = nxt;
        nx = BUNDLE_TEX(b + 1);
#else
        [branch]
        if (go) {
            b += AS_UNSIGNED(AS_SIGNED(W) >> 21);
            [branch]
            if (rv >= fast_budget) break;
            cur = BUNDLE_TEX(b);
            nx = BUNDLE_TEX(b + 1);
            continue;
        }
        b += 1;
        [branch]
        if (rv >= fast_budget) break;
        cur = nx;
        nx = BUNDLE_TEX(b + 1);
#endif
    }
    [branch]
    if (b != 0) {
        uint bn = b - WIDE_B0;
        cpu.pc = idx_uint4(CODE_TEX(WIDE_PCT + (bn >> 2)), bn & 3);
    }
    cpu.clock += rv;
    return rv;
}
#endif
