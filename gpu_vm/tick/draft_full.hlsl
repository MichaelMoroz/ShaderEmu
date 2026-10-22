// gpu_vm's tick (gpu_vm/GPU_VM.md, gpu_vm/tick/README.md): every core of the machine, a row of pixels a core, each pixel
// running the whole core and writing eight of its state texels. Then the commit (four draws, a word's
// component each) and the operations' three draws (stage, write, progress). The reference is
// gpu_vm/ref/gvm_ref.h; gvmbench (gpu_vm/tick/gvmbench.cpp) checks the one against the other.
//
// Switches: WCN (write cache words, a power of two, 128), ARR (the register array's declared size),
// OPS_UNROLL (the three op slots written out rather than a loop), RAM_ROW0 (the state texture's first
// RAM row: 64 in the machine, 0 in the bench).
#include "../isa.h"

#ifndef WCN
#define WCN 128
#endif
#ifndef ARR
#define ARR 72
#endif
#ifndef RAM_ROW0
#define RAM_ROW0 0
#endif
#ifndef OPS_UNROLL
#define OPS_UNROLL 1
#endif
#define SINK 64                   // where a write to r0 goes
#define HALF (WCN / 2)
#define NONE 0xffffffffu
// the state's texels, a core's
#define T_REGS 0                  // 16 texels: r0-r63
#define T_SYS0 16                 // pc, STATUS, EPC, CAUSE
#define T_SYS1 17                 // TVAL, TVEC, SCRATCH, IEN
#define T_SYS2 18                 // TCMP_LO, TCMP_HI, flags, RANDOM's state
#define T_SYS3 19                 // CYCLE, OPS, bundles in the last pass, why it ended
#define T_OP0 20                  // the operation's descriptor, as stored at GVM_OPS
#define T_OP1 21
#define T_WC 24                   // the write cache: two (word + 1, value) pairs a texel
#define STATE_TEXELS (T_WC + WCN / 2)
#define STATE_PIX ((STATE_TEXELS + 7) / 8)
// flags (T_SYS2.z)
#define F_WAITING 1u
#define F_SOFT 2u
#define F_HALTED 4u
#define F_OP_NEW 8u               // a descriptor was stored this pass: the operation starts at the commit
#define F_OP_RUN 16u              // an operation in flight: the core waits for it
#define F_EXT 32u
// why a pass ended (T_SYS3.w)
#define E_BUDGET 0u
#define E_SYNC 1u
#define E_WAIT 2u
#define E_FULL 3u
#define E_PARKED 4u
#define E_HALT 5u
// operations
#define OP_ROW_MAX 0x40000u       // 256 KB: a copy's rows, a blit's longest row
#define OP_STAGE 18432u           // stage texels a core (9 rows of 2048)
#define OP_PER_PASS 64u           // rows a pass at most

Texture2D<uint4> _Ram : register(t0);
Texture2D<uint4> _Rom : register(t1);
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
Texture2D<uint4> _OpState : register(t10);   // a texel a core: rows done, refused, the descriptor's first word
Texture2D<uint4> _Stage : register(t11);
cbuffer PassParams : register(b0) {
    uint _Ticks;      // bundles a pass
    uint _RamEnd;     // bytes from address 0: the end of RAM
    uint _TimeLo, _TimeHi;
    uint _Player, _Flags, _RamRows, _Ext;   // _Flags bit 0: break halts the core (the bench's programs)
    uint _NCores; uint3 _ppad;
};

#define TEX_AT(t) int3((t) & 2047, ((t) >> 11) + RAM_ROW0, 0)
#define RAM(t) _Ram.Load(TEX_AT(t))
#define ROM(t) _Rom.Load(int3((t) & 2047, (t) >> 11, 0))

uint pick(uint4 q, uint i) { return (i & 2) ? ((i & 1) ? q.w : q.z) : ((i & 1) ? q.y : q.x); }
uint4 put(uint4 v, uint i, uint x) { return uint4(i == 0 ? x : v.x, i == 1 ? x : v.y, i == 2 ? x : v.z, i == 3 ? x : v.w); }

// ---- A core's state while it runs
static uint r[ARR];
static uint2 wc[WCN];
static uint4 occv;                // a bit a write cache slot: taken this pass
static uint pc, st, epc, cause, tval, tvec, scratch, ien, tcmp_lo, tcmp_hi, flags, rnd, cycle, opsrun;
static uint left, endr, ran, core;
static uint4 opd0, opd1;
static bool irq_maybe;            // an interrupt may be due: (IPEND & IEN) and IE, worked out when they change

void wr(uint rd, uint v) { r[rd == 0 ? SINK : rd] = v; }

// ---- The write cache: slot h or h + HALF for word w, filled in that order and never emptied in a pass
uint wc_h(uint w) { return (w ^ (w >> 7)) & (HALF - 1); }
bool occ(uint s) { return ((pick(occv, s >> 5) >> (s & 31)) & 1) != 0; }
void occ_set(uint s) { occv = put(occv, s >> 5, pick(occv, s >> 5) | (1u << (s & 31))); }
uint wc_get(uint w, uint def) {   // word w as this core sees it, def being RAM's
    uint h = wc_h(w), v = def;
    [branch] if (occ(h)) {
        uint2 e = wc[h];
        [branch] if (e.x == w + 1) v = e.y;
        else if (occ(h + HALF)) {
            uint2 e2 = wc[h + HALF];
            if (e2.x == w + 1) v = e2.y;
        }
    }
    return v;
}
uint wc_slot(uint w) {   // where word w is or would go, or NONE when both slots hold others
    uint h = wc_h(w);
    [branch] if (!occ(h)) return h;
    [branch] if (wc[h].x == w + 1) return h;
    [branch] if (!occ(h + HALF)) return h + HALF;
    return wc[h + HALF].x == w + 1 ? h + HALF : NONE;
}
void wc_put(uint s, uint w, uint v) { wc[s] = uint2(w + 1, v); occ_set(s); }
uint4 wc_texel(uint t, uint4 q) {   // a RAM texel with this core's stores in it
    uint w = t * 4;
    return uint4(wc_get(w, q.x), wc_get(w + 1, q.y), wc_get(w + 2, q.z), wc_get(w + 3, q.w));
}

// ---- Floats (gvm_ref.h's gvm_fop): the card's arithmetic, the one NaN, -0 below +0
uint fnan(float f) { uint u = asuint(f); return (u & 0x7fffffffu) > 0x7f800000u ? 0x7fc00000u : u; }
bool isnan_u(uint u) { return (u & 0x7fffffffu) > 0x7f800000u; }
uint fdiv(uint ra, uint rb) {
    // the card divides by multiplying with 1 / b, nothing when b is large: two ordinary numbers are divided
    // with their exponents moved towards the middle (experiments/rvc_opt/src/emu.h)
    precise float a = asfloat(ra);
    precise float b = asfloat(rb);
    uint ea = (ra >> 23) & 0xff, eb = (rb >> 23) & 0xff;
    int de = (int)ea - (int)eb;
    int ha = clamp(de, -126, 127);
    precise float da = asfloat((ra & 0x807fffffu) | ((uint)(127 + ha) << 23));
    precise float db = asfloat((rb & 0x807fffffu) | ((uint)(127 - (de - ha)) << 23));
    precise float q = (ea - 1 < 254 && eb - 1 < 254) ? da / db : a / b;
    return fnan(q);
}
uint itof(uint a) {   // by size and sign: fxc2 made (float) of an int the int's own bits
    bool neg = (a >> 31) != 0;
    return asuint((float)(neg ? 0 - a : a)) | (neg ? 0x80000000u : 0);
}
uint ftou(float x, bool isn) {
    return (isn || x <= 0.0) ? 0 : x >= 4294967296.0 ? 0xffffffffu
         : x >= 2147483648.0 ? 0x80000000u + (uint)(x - 2147483648.0) : (uint)x;
}
uint float_op(uint f, uint ra, uint rb, uint rc) {
    precise float a = asfloat(ra);
    precise float b = asfloat(rb);
    bool an = isnan_u(ra), bn = isnan_u(rb);
    uint v;
    [branch] if (f < GVM_F_EQ) {
        [branch] if (f == GVM_F_DIV) v = fdiv(ra, rb);
        else if (f >= GVM_F_MAD) {
            precise float p = a * b;
            precise float pr = asfloat(fnan(p));
            precise float c = asfloat(rc);
            precise float s = f == GVM_F_MAD ? pr + c : f == GVM_F_MSB ? pr - c : c - pr;
            v = fnan(s);
        } else if (f >= GVM_F_MIN) {
            bool less = a < b || (a == b && (ra >> 31) > (rb >> 31));
            bool more = a > b || (a == b && (rb >> 31) > (ra >> 31));
            v = an ? (bn ? 0x7fc00000u : rb) : bn ? ra : ((f == GVM_F_MIN ? less : more) ? ra : rb);
        } else {
            precise float s = f == GVM_F_ADD ? a + b : f == GVM_F_SUB ? a - b : a * b;
            v = fnan(s);
        }
    } else if (f < GVM_F_FLOOR) {
        uint cmp = (!an && !bn && (f == GVM_F_EQ ? a == b : f == GVM_F_LT ? a < b : a <= b)) ? 1u : 0u;
        uint sg = (ra & 0x7fffffffu) | ((f == GVM_F_SGNJ ? rb : f == GVM_F_SGNJN ? ~rb : ra ^ rb) & 0x80000000u);
        uint toi = an ? 0 : a >= 2147483648.0 ? 0x7fffffffu : a <= -2147483648.0 ? 0x80000000u : asuint((int)a);
        v = f <= GVM_F_LE ? cmp : f <= GVM_F_SGNJX ? sg : f == GVM_F_UNORD ? (an || bn ? 1u : 0u)
          : f == GVM_F_ITOF ? itof(ra) : f == GVM_F_UTOF ? asuint((float)ra) : f == GVM_F_FTOI ? toi : ftou(a, an);
    } else if (f < GVM_F_SQRT) {
        precise float w = f == GVM_F_FLOOR ? floor(a) : f == GVM_F_CEIL ? ceil(a) : f == GVM_F_TRUNC ? trunc(a) : round(a);
        v = fnan(w);
    } else {   // the special functions, as the card makes them
        precise float y;
        [branch] if (f == GVM_F_SQRT) y = sqrt(a);
        else if (f == GVM_F_RSQRT) y = rsqrt(a);
        else if (f == GVM_F_RCP) y = rcp(a);
        else if (f == GVM_F_SIN) y = sin(a);
        else if (f == GVM_F_COS) y = cos(a);
        else if (f == GVM_F_EXP2) y = exp2(a);
        else if (f == GVM_F_LOG2) y = log2(a);
        else y = a - floor(a);
        v = fnan(y);
    }
    return v;
}

// ---- Integers
uint mulhu(uint a, uint b) {   // the high word of a * b, by halves
    uint al = a & 0xffff, ah = a >> 16, bl = b & 0xffff, bh = b >> 16;
    uint ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint mid = (ll >> 16) + (lh & 0xffff) + (hl & 0xffff);
    return hh + (lh >> 16) + (hl >> 16) + (mid >> 16);
}
uint ext_op(uint opc, uint a, uint b) {
    uint v = 0;
    [branch] if (opc <= GVM_X_MULHSU) {
        uint hu = mulhu(a, b);
        v = opc == GVM_X_MULHU ? hu : opc == GVM_X_MULH ? hu - ((a >> 31) ? b : 0) - ((b >> 31) ? a : 0) : hu - ((a >> 31) ? b : 0);
    } else if (opc <= GVM_X_REMU) {
        bool sg = opc == GVM_X_DIV || opc == GVM_X_REM;
        bool na = sg && (a >> 31) != 0, nb = sg && (b >> 31) != 0;
        uint ua = na ? 0 - a : a, ub = nb ? 0 - b : b;
        uint q = ub == 0 ? 0xffffffffu : ua / ub, m = ub == 0 ? ua : ua - q * ub;
        uint qs = (na != nb) ? 0 - q : q, ms = na ? 0 - m : m;
        bool rem = opc == GVM_X_REM || opc == GVM_X_REMU;
        v = b == 0 ? (rem ? a : 0xffffffffu) : rem ? ms : qs;
    } else {
        v = opc == GVM_X_CLZ ? (a == 0 ? 32u : 31u - firstbithigh(a)) : opc == GVM_X_CTZ ? (a == 0 ? 32u : firstbitlow(a))
          : opc == GVM_X_POPC ? countbits(a) : opc == GVM_X_BSWAP ? (a >> 24 | ((a >> 8) & 0xff00u) | ((a << 8) & 0xff0000u) | a << 24)
          : reversebits(a);
    }
    return v;
}
uint int_op(uint f, bool I, uint a, uint B) {
    bool rs = I && f == GVM_I_SUB;   // rsub: imm - ra
    uint x = rs ? B : a, y = rs ? a : B;
    uint sum = x + (f == GVM_I_SUB ? 0 - y : y);
    uint yb = f >= GVM_I_ANDN ? ~y : y;
    uint lk = (f - 2) % 3;          // and, or, xor; then with ~y
    uint lg = lk == 0 ? (x & yb) : lk == 1 ? (x | yb) : (x ^ yb);
    uint sh = y & 31, back = (32 - sh) & 31;
    uint shv = f == GVM_I_SLL ? x << sh : f == GVM_I_SRL ? x >> sh : f == GVM_I_SRA ? asuint(asint(x) >> sh)
             : f == GVM_I_ROL ? (x << sh) | (sh ? x >> back : 0) : (x >> sh) | (sh ? x << back : 0);
    bool lts = asint(x) < asint(y), ltu = x < y, eq = x == y;
    bool cb = f == GVM_I_SLT ? lts : f == GVM_I_SLTU ? ltu : f == GVM_I_SEQ ? eq : f == GVM_I_SNE ? !eq
            : f == GVM_I_SGT ? (!lts && !eq) : (!ltu && !eq);
    uint mm = f == GVM_I_MIN ? (lts ? x : y) : f == GVM_I_MAX ? (lts ? y : x) : f == GVM_I_MINU ? (ltu ? x : y) : (ltu ? y : x);
    return f < GVM_I_SLL ? (f < GVM_I_AND ? sum : lg) : f < GVM_I_SLT ? shv : f < GVM_I_MIN ? (cb ? 1u : 0u)
         : f < GVM_I_MUL ? mm : x * y;
}
uint bfe(uint a, uint imm) {
    uint pos = imm & 31, len = ((imm >> 5) & 31) + 1, v = a >> pos;
    uint m = len >= 32 ? 0xffffffffu : (1u << len) - 1;
    v &= m;
    return ((imm & 0x400) && len < 32 && ((v >> (len - 1)) & 1)) ? v | ~m : v;
}
uint bfi(uint d, uint a, uint imm) {
    uint pos = imm & 31, len = ((imm >> 5) & 31) + 1;
    uint m = len >= 32 ? 0xffffffffu : (1u << len) - 1;
    return (d & ~(m << pos)) | ((a & m) << pos);
}

bool iform_of(uint opc) {
    uint cls = opc >> 6;
    return (cls & 1) ? ((opc >> 5) & 1) != 0 : cls == 2 ? ((opc >> 3) & 7) <= 1 : (opc == GVM_X_BFI || opc == GVM_X_LPC);
}
bool long_of(uint w) { return (w >> 20) == 0x800u && iform_of(w & 255); }

// ---- An op the fast loop makes: G_DONE, or G_GENERAL (nothing changed: the general step makes it), G_FULL
#define G_DONE 0u
#define G_GENERAL 1u
#define G_FULL 2u
uint width_bytes(uint wd) { return (wd & 3) == 0 ? 1u : (wd & 3) == 1 ? 2u : wd == 2 ? 4u : wd == 3 ? 8u : 16u; }

uint op_fast(uint w, uint imm, bool L, uint base) {
    uint opc = w & 255, rd = (w >> 8) & 63, ra = (w >> 14) & 63, rb = (w >> 20) & 63, rc = w >> 26;
    uint cls = opc >> 6;
    uint a = r[ra], b = r[rb];
    uint res = G_DONE;
    [branch] if (cls == 1) {   // integer
        uint f = opc & 31;
        bool I = (opc & 32) != 0;
        [branch] if (f < GVM_I_SHADD) wr(rd, int_op(f, I, a, I ? imm : b));
        else if (I) {
            [branch] if (f == GVM_I_BFE) wr(rd, bfe(a, imm));
            else res = G_GENERAL;
        } else if (f <= GVM_I_BORROW) {
            uint c = r[rc], cin = c & 1;
            uint s1 = a + b, s2 = s1 + cin, d1 = a - b, d2 = d1 - cin;
            uint v = f == GVM_I_SHADD ? a + (b << (rc & 31)) : f == GVM_I_SEL ? (c != 0 ? a : b)
                   : f == GVM_I_ADDCY ? s2 : f == GVM_I_CARRY ? ((s1 < a || s2 < s1) ? 1u : 0u)
                   : f == GVM_I_SUBBY ? d2 : ((a < b || d1 < cin) ? 1u : 0u);
            wr(rd, v);
        } else res = G_GENERAL;
    } else if (cls == 2) {   // memory: RAM, naturally aligned, within one texel
        uint mode = (opc >> 3) & 7, wd = opc & 7;
        uint by = width_bytes(wd);
        uint addr = mode <= 1 ? a + imm : a + (b << (rc & 31));
        bool load = mode == 0 || mode == 2;
        bool ok = mode <= 3 && wd != 7 && (addr & (by - 1)) == 0 && addr + by <= _RamEnd && addr + by > addr && (load || addr >= GVM_DEV);
        [branch] if (!ok) res = G_GENERAL;
        else {
            uint t = addr >> 4, wi = (addr >> 2) & 3;
            [branch] if (load) {
                uint4 q = RAM(t);
                [branch] if (by == 16) {
                    q = wc_texel(t, q);
                    wr(rd, q.x); wr((rd + 1) & 63, q.y); wr((rd + 2) & 63, q.z); wr((rd + 3) & 63, q.w);
                } else if (by == 8) {
                    uint w0 = wc_get(t * 4 + wi, pick(q, wi)), w1 = wc_get(t * 4 + wi + 1, pick(q, wi + 1));
                    wr(rd, w0); wr((rd + 1) & 63, w1);
                } else {
                    uint word = wc_get(addr >> 2, pick(q, wi)) >> ((addr & 3) * 8);
                    uint v = wd == GVM_W_W ? word : wd == GVM_W_B ? asuint((int)(word << 24) >> 24) : wd == GVM_W_H ? asuint((int)(word << 16) >> 16)
                           : wd == GVM_W_BU ? (word & 0xffu) : (word & 0xffffu);
                    wr(rd, v);
                }
            } else {
                uint wa = addr >> 2;
                [branch] if (by >= 8) {
                    uint n = by >> 2, s0 = wc_slot(wa), s1 = wc_slot(wa + 1), s2 = n > 2 ? wc_slot(wa + 2) : 0, s3 = n > 2 ? wc_slot(wa + 3) : 0;
                    [branch] if (s0 == NONE || s1 == NONE || s2 == NONE || s3 == NONE) res = G_FULL;
                    else {
                        wc_put(s0, wa, r[rd]); wc_put(s1, wa + 1, r[(rd + 1) & 63]);
                        [branch] if (n > 2) { wc_put(s2, wa + 2, r[(rd + 2) & 63]); wc_put(s3, wa + 3, r[(rd + 3) & 63]); }
                    }
                } else {
                    uint s = wc_slot(wa);
                    [branch] if (s == NONE) res = G_FULL;
                    else {
                        uint v = r[rd];
                        [branch] if (by < 4) {
                            uint old = occ(s) ? wc[s].y : pick(RAM(t), wi);
                            uint sh = (addr & 3) * 8, m = (by == 1 ? 0xffu : 0xffffu) << sh;
                            v = (old & ~m) | ((v << sh) & m);
                        }
                        wc_put(s, wa, v);
                    }
                }
            }
        }
    } else if (cls == 3) {   // float
        uint f = opc & 31;
        bool I = (opc & 32) != 0;
        [branch] if (I && f >= GVM_F_MAD && f <= GVM_F_NMSB) res = G_GENERAL;
        else {
            uint B = I ? (L ? imm : (w >> 20) << 20) : b;
            wr(rd, float_op(f, a, B, r[rc]));
        }
    } else {   // extended: the arithmetic, bfi and lpc here; the rest is the general step's
        [branch] if (opc >= GVM_X_MULH && opc <= GVM_X_BREV) wr(rd, ext_op(opc, a, b));
        else if (opc == GVM_X_BFI) wr(rd, bfi(r[rd], a, imm));
        else if (opc == GVM_X_LPC) wr(rd, base + imm);
        else res = G_GENERAL;
    }
    return res;
}

// ---- Traps and special registers
void recheck_irq() {
    uint pend = ((flags & F_SOFT) ? 1u : 0u) | ((flags & F_EXT) ? 4u : 0u);
    bool due = (tcmp_lo | tcmp_hi) != 0 && (_TimeHi > tcmp_hi || (_TimeHi == tcmp_hi && _TimeLo >= tcmp_lo));
    pend |= due ? 2u : 0u;
    irq_maybe = (st & GVM_ST_IE) != 0 && (pend & ien) != 0;
}
uint ipend() {
    bool due = (tcmp_lo | tcmp_hi) != 0 && (_TimeHi > tcmp_hi || (_TimeHi == tcmp_hi && _TimeLo >= tcmp_lo));
    return ((flags & F_SOFT) ? 1u : 0u) | (due ? 2u : 0u) | ((flags & F_EXT) ? 4u : 0u);
}
void trap(uint c, uint at, uint val) {
    epc = at; cause = c; tval = val;
    st = (st & ~(GVM_ST_PIE | GVM_ST_PS | GVM_ST_IE)) | ((st & GVM_ST_IE) ? GVM_ST_PIE : 0) | ((st & GVM_ST_S) ? GVM_ST_PS : 0) | GVM_ST_S;
    pc = tvec & ~15u;
    flags &= ~F_WAITING;
    recheck_irq();
}
uint sr_read(uint n) {
    uint v = 0;
    switch (n) {
    case GVM_SR_STATUS: v = st; break;
    case GVM_SR_EPC: v = epc; break;
    case GVM_SR_CAUSE: v = cause; break;
    case GVM_SR_TVAL: v = tval; break;
    case GVM_SR_TVEC: v = tvec; break;
    case GVM_SR_SCRATCH: v = scratch; break;
    case GVM_SR_IEN: v = ien; break;
    case GVM_SR_IPEND: v = ipend(); break;
    case GVM_SR_TCMP_LO: v = tcmp_lo; break;
    case GVM_SR_TCMP_HI: v = tcmp_hi; break;
    case GVM_SR_TIME_LO: v = _TimeLo; break;
    case GVM_SR_TIME_HI: v = _TimeHi; break;
    case GVM_SR_CYCLE: v = cycle; break;
    case GVM_SR_COREID: v = core; break;
    case GVM_SR_PLAYER: v = _Player; break;
    case GVM_SR_RANDOM: rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; v = rnd; break;
    case GVM_SR_LEFT: v = left; break;
    default: v = opsrun; break;   // GVM_SR_OPS
    }
    return v;
}
void sr_write(uint n, uint v) {
    switch (n) {
    case GVM_SR_STATUS: st = (v & (GVM_ST_IE | GVM_ST_PIE | GVM_ST_PS)) | (st & GVM_ST_S); break;
    case GVM_SR_EPC: epc = v; break;
    case GVM_SR_CAUSE: cause = v; break;
    case GVM_SR_TVAL: tval = v; break;
    case GVM_SR_TVEC: tvec = v; break;
    case GVM_SR_SCRATCH: scratch = v; break;
    case GVM_SR_IEN: ien = v; break;
    case GVM_SR_IPEND: flags = (v & 1) ? flags | F_SOFT : flags & ~F_SOFT; break;
    case GVM_SR_TCMP_LO: tcmp_lo = v; break;
    case GVM_SR_TCMP_HI: tcmp_hi = v; break;
    }
    recheck_irq();
}

// ---- The general step's memory: any address, any alignment, a byte at a time where it must
uint byte_at(uint a) {   // a readable address's byte (this core's stores first)
    uint v;
    [branch] if (a >= GVM_ROM) v = pick(ROM((a - GVM_ROM) >> 4), (a >> 2) & 3);
    else v = wc_get(a >> 2, pick(RAM(a >> 4), (a >> 2) & 3));
    return (v >> ((a & 3) * 8)) & 0xffu;
}
bool readable(uint a) { return a < _RamEnd || (a >= GVM_ROM && a < GVM_ROM_END); }
bool writable(uint a) { return a >= GVM_DEV && a < _RamEnd; }
bool load_any(uint a, uint by, out uint v) {   // by 1, 2 or 4
    v = 0;
    [branch] if (a >= GVM_OPS) {
        bool ok = a + by <= GVM_OPS + 32 && ((a | by) & 3) == 0;
        uint i = (a - GVM_OPS) >> 2;
        v = ok ? (i < 4 ? pick(opd0, i) : pick(opd1, i - 4)) : 0;
        return ok;
    }
    if (!readable(a) || !readable(a + by - 1)) return false;
    for (uint k = 0; k < by; k++) v |= byte_at(a + k) << (8 * k);
    return true;
}
bool store_ok(uint a, uint by) {
    if (a >= GVM_OPS) return a + by <= GVM_OPS + 32 && ((a | by) & 3) == 0;
    return writable(a) && writable(a + by - 1) && a + by > a;
}
bool room(uint a, uint by) {   // every word the store touches has a slot
    bool ok = true;
    for (uint w = a >> 2; w <= (a + by - 1) >> 2; w++) ok = ok && wc_slot(w) != NONE;
    return ok;
}
void store_any(uint a, uint by, uint v) {   // checked by store_ok and room first
    [branch] if (a >= GVM_OPS) {
        uint i = (a - GVM_OPS) >> 2;
        if (i < 4) opd0 = put(opd0, i, v); else opd1 = put(opd1, i - 4, v);
        flags |= F_OP_NEW;
        return;
    }
    for (uint k = 0; k < by; k++) {
        uint b = a + k, s = wc_slot(b >> 2), sh = (b & 3) * 8;
        uint old = occ(s) ? wc[s].y : pick(RAM(b >> 4), (b >> 2) & 3);
        wc_put(s, b >> 2, (old & ~(0xffu << sh)) | (((v >> (8 * k)) & 0xffu) << sh));
    }
}

// The general step: the bundle from pc's word on, any op and any control word. Returns why the pass ends
// (E_BUDGET: it goes on).
#define S_GO 0u
#define S_TRAP 1u
#define S_END 2u
uint op_general(uint w, uint imm, uint base, uint s, out uint endw) {
    endw = E_BUDGET;
    uint opc = w & 255, rd = (w >> 8) & 63, ra = (w >> 14) & 63, rb = (w >> 20) & 63, rc = w >> 26;
    uint cls = opc >> 6, epcw = base | (s * 4);
    bool super = (st & GVM_ST_S) != 0;
    uint a = r[ra], b = r[rb];
    [branch] if (cls == 2) {
        uint mode = (opc >> 3) & 7, wd = opc & 7;
        [branch] if (mode <= 3 && wd != 7) {
            uint by = width_bytes(wd), addr = mode <= 1 ? a + imm : a + (b << (rc & 31));
            [branch] if (mode == 0 || mode == 2) {
                uint v0 = 0, v1 = 0, v2 = 0, v3 = 0;
                bool ok;
                [branch] if (by <= 4) ok = load_any(addr, by, v0);
                else {
                    ok = load_any(addr, 4, v0) && load_any(addr + 4, 4, v1);
                    if (by == 16) ok = ok && load_any(addr + 8, 4, v2) && load_any(addr + 12, 4, v3);
                }
                [branch] if (!ok) { trap(GVM_CAUSE_LOAD, epcw, addr); return S_TRAP; }
                if (wd == GVM_W_B) v0 = asuint((int)(v0 << 24) >> 24);
                if (wd == GVM_W_H) v0 = asuint((int)(v0 << 16) >> 16);
                wr(rd, v0);
                if (by >= 8) wr((rd + 1) & 63, v1);
                if (by == 16) { wr((rd + 2) & 63, v2); wr((rd + 3) & 63, v3); }
                opsrun++;
                return S_GO;
            }
            [branch] if (!store_ok(addr, by)) { trap(GVM_CAUSE_STORE, epcw, addr); return S_TRAP; }
            [branch] if (addr < GVM_OPS && !room(addr, by)) { pc = epcw; endw = E_FULL; return S_END; }
            [branch] if (by <= 4) store_any(addr, by, r[rd]);
            else for (uint k = 0; k < by / 4; k++) store_any(addr + 4 * k, 4, r[(rd + k) & 63]);
            opsrun++;
            return S_GO;
        }
        [branch] if (mode == 4 || (mode == 5 && wd <= GVM_AMO2_MAXU)) {
            [branch] if ((a & 3) != 0 || !store_ok(a, 4) || a >= GVM_OPS) { trap(GVM_CAUSE_STORE, epcw, a); return S_TRAP; }
            uint sl = wc_slot(a >> 2);
            [branch] if (sl == NONE) { pc = epcw; endw = E_FULL; return S_END; }
            uint old = wc_get(a >> 2, pick(RAM(a >> 4), (a >> 2) & 3)), nw;
            bool lt = asint(old) < asint(b), ltu = old < b;
            if (mode == 5) nw = wd == GVM_AMO2_MINU ? (ltu ? old : b) : (ltu ? b : old);
            else nw = wd == GVM_AMO_SWAP ? b : wd == GVM_AMO_ADD ? old + b : wd == GVM_AMO_AND ? (old & b) : wd == GVM_AMO_OR ? (old | b)
                    : wd == GVM_AMO_XOR ? (old ^ b) : wd == GVM_AMO_MIN ? (lt ? old : b) : wd == GVM_AMO_MAX ? (lt ? b : old)
                    : (old == r[rd] ? b : old);
            wc_put(sl, a >> 2, nw);
            wr(rd, old);
            opsrun++;
            return S_GO;
        }
    } else if (cls == 0) {
        [branch] if (opc == GVM_X_SYSCALL) { trap(GVM_CAUSE_SYSCALL, epcw, 0); return S_TRAP; }
        [branch] if (opc == GVM_X_BREAK) {
            [branch] if (_Flags & 1) { pc = epcw; flags |= F_HALTED; endw = E_HALT; return S_END; }
            trap(GVM_CAUSE_BREAK, epcw, 0);
            return S_TRAP;
        }
        [branch] if (opc == GVM_X_WAIT) {
            opsrun++;
            pc = epcw + 4;
            [branch] if (!super) { endw = E_WAIT; return S_END; }
            [branch] if ((ipend() & ien) != 0) return S_GO;
            flags |= F_WAITING;
            endw = E_WAIT;
            return S_END;
        }
        [branch] if (opc == GVM_X_SYNC) { opsrun++; pc = epcw + 4; endw = E_SYNC; return S_END; }
        [branch] if (opc == GVM_X_FENCE) { opsrun++; return S_GO; }
        [branch] if (opc >= GVM_X_MFSR && opc <= GVM_X_SRCLR) {
            uint n = imm & 0xfff;
            [branch] if (n < GVM_SR_COUNT) {
                [branch] if (!super && (opc != GVM_X_MFSR || n < GVM_SR_USER_FIRST)) { trap(GVM_CAUSE_PRIV, epcw, w); return S_TRAP; }
                uint old = sr_read(n);
                if (opc == GVM_X_MTSR || opc == GVM_X_XSR) sr_write(n, a);
                if (opc == GVM_X_SRSET) sr_write(n, old | a);
                if (opc == GVM_X_SRCLR) sr_write(n, old & ~a);
                if (opc != GVM_X_MTSR) wr(rd, old);
                opsrun++;
                return S_GO;
            }
        }
    }
    trap(GVM_CAUSE_ILLEGAL, epcw, w);
    return S_TRAP;
}

uint bundle_general() {   // the bundle at pc from its word; E_BUDGET when the core goes on
    uint base = pc & ~15u, s = (pc >> 2) & 3, endw = E_BUDGET;
    [branch] if (base >= _RamEnd) { trap(GVM_CAUSE_FETCH, pc, pc); return E_BUDGET; }
    uint4 t = RAM(base >> 4);
    uint ctl = t.w, n = ctl & 3;
    cycle++;
    [loop] while (s < n) {
        uint w = pick(t, s);
        bool L = long_of(w);
        [branch] if (L && s + 1 >= n) { trap(GVM_CAUSE_ILLEGAL, base | (s * 4), w); return E_BUDGET; }
        uint imm = L ? pick(t, s + 1) : asuint((int)w >> 20);
        uint res = op_fast(w, imm, L, base);
        [branch] if (res == G_DONE) opsrun++;
        else if (res == G_FULL) { pc = base | (s * 4); return E_FULL; }
        else {
            uint g = op_general(w, imm, base, s, endw);
            [branch] if (g == S_TRAP) return E_BUDGET;
            [branch] if (g == S_END) {
                if (((pc >> 2) & 3) >= n) pc = base | 12;
                return endw;
            }
        }
        s += L ? 2 : 1;
    }
    uint cc = (ctl >> 2) & 15, next = base + 16;
    bool super = (st & GVM_ST_S) != 0;
    [branch] if (cc == GVM_CC_SPECIAL) {
        uint sub = (ctl >> 6) & 7, rs = (ctl >> 9) & 63;
        [branch] if (sub == GVM_SUB_NONE) {}
        else if (sub == GVM_SUB_JR) next = r[rs] & ~15u;
        else if (sub == GVM_SUB_CALLR) { next = r[rs] & ~15u; r[1] = base + 16; }
        else if (sub == GVM_SUB_ERET) {
            [branch] if (!super) { trap(GVM_CAUSE_PRIV, base | 12, ctl); return E_BUDGET; }
            next = epc;
            st = (st & ~(GVM_ST_IE | GVM_ST_S | GVM_ST_PS)) | ((st & GVM_ST_PIE) ? GVM_ST_IE : 0) | ((st & GVM_ST_PS) ? GVM_ST_S : 0) | GVM_ST_PIE;
            recheck_irq();
        } else if (sub == GVM_SUB_J) next = base + asuint((int)ctl >> 9) * 16;
        else if (sub == GVM_SUB_CALL) { next = base + asuint((int)ctl >> 9) * 16; r[1] = base + 16; }
        else { trap(GVM_CAUSE_ILLEGAL, base | 12, ctl); return E_BUDGET; }
    } else if (cc == 15) {
        trap(GVM_CAUSE_ILLEGAL, base | 12, ctl);
        return E_BUDGET;
    } else {
        uint ca = r[(ctl >> 6) & 63], bf = (ctl >> 12) & 63, cb = r[bf];
        int i6 = (int)(bf << 26) >> 26;
        bool tk = cc == GVM_CC_BEQ ? ca == cb : cc == GVM_CC_BNE ? ca != cb : cc == GVM_CC_BLT ? asint(ca) < asint(cb)
                : cc == GVM_CC_BGE ? asint(ca) >= asint(cb) : cc == GVM_CC_BLTU ? ca < cb : cc == GVM_CC_BGEU ? ca >= cb
                : cc == GVM_CC_BBS ? ((ca >> (bf & 31)) & 1) != 0 : cc == GVM_CC_BBC ? ((ca >> (bf & 31)) & 1) == 0
                : cc == GVM_CC_BEQI ? asint(ca) == i6 : cc == GVM_CC_BNEI ? asint(ca) != i6 : cc == GVM_CC_BLTI ? asint(ca) < i6
                : cc == GVM_CC_BGEI ? asint(ca) >= i6 : cc == GVM_CC_BLTUI ? ca < bf : ca >= bf;
        if (tk) next = base + asuint((int)ctl >> 18) * 16;
    }
    pc = next;
    return E_BUDGET;
}

// ---- The fast loop: bundles whose ops op_fast makes and whose control word is a branch, a jump or a call
// (or none); it leaves at pc's op for the general step otherwise. Returns E_*, or NONE for "the general
// step next".
uint fast() {
    uint4 cur = RAM(pc >> 4);
    uint res = NONE;
    [loop] while (left > 0) {
        uint base = pc & ~15u, s = (pc >> 2) & 3;
        uint4 t = cur;
        uint ctl = t.w, n = ctl & 3, cc = (ctl >> 2) & 15, sub = (ctl >> 6) & 7;
        bool special = cc == GVM_CC_SPECIAL;
        int off = special ? (int)ctl >> 9 : (int)ctl >> 18;
        uint tg = base + asuint(off) * 16;
        uint4 nx = RAM((base >> 4) + 1), tc = RAM(tg >> 4);
        // a control word the loop does not make, or a bundle not from its start: the general step
        [branch] if (s != 0 || cc == 15 || (special && (sub == GVM_SUB_ILLEGAL || sub == GVM_SUB_ERET || sub == 7))) break;
        left--;
        cycle++;
        uint p = 0, why = G_DONE;
#if OPS_UNROLL
        [unroll] for (uint k = 0; k < 3; k++) {
            [branch] if (p < n && why == G_DONE) {
#else
        [loop] for (uint k = 0; k < 3; k++) {
            [branch] if (p >= n || why != G_DONE) break;
            {
#endif
                uint w = pick(t, p);
                bool L = long_of(w);
                uint imm = L ? pick(t, p + 1) : asuint((int)w >> 20);
                why = (L && p + 1 >= n) ? G_GENERAL : op_fast(w, imm, L, base);
                [branch] if (why == G_DONE) { opsrun++; p += L ? 2 : 1; }
            }
        }
        [branch] if (why != G_DONE) {   // pc at the op not made; the bundle is counted where it goes on
            left++;
            cycle--;
            pc = base | (p * 4);
            [branch] if (why == G_FULL) { left--; cycle++; res = E_FULL; }
            break;
        }
        uint ca = r[(ctl >> 6) & 63], bf = (ctl >> 12) & 63, cb = r[bf];
        int i6 = (int)(bf << 26) >> 26;
        bool tk = cc == GVM_CC_BEQ ? ca == cb : cc == GVM_CC_BNE ? ca != cb : cc == GVM_CC_BLT ? asint(ca) < asint(cb)
                : cc == GVM_CC_BGE ? asint(ca) >= asint(cb) : cc == GVM_CC_BLTU ? ca < cb : cc == GVM_CC_BGEU ? ca >= cb
                : cc == GVM_CC_BBS ? ((ca >> (bf & 31)) & 1) != 0 : cc == GVM_CC_BBC ? ((ca >> (bf & 31)) & 1) == 0
                : cc == GVM_CC_BEQI ? asint(ca) == i6 : cc == GVM_CC_BNEI ? asint(ca) != i6 : cc == GVM_CC_BLTI ? asint(ca) < i6
                : cc == GVM_CC_BGEI ? asint(ca) >= i6 : cc == GVM_CC_BLTUI ? ca < bf : ca >= bf;
        tk = special ? (sub == GVM_SUB_J || sub == GVM_SUB_CALL) : tk;
        bool reg = special && (sub == GVM_SUB_JR || sub == GVM_SUB_CALLR);
        uint rt = r[(ctl >> 9) & 63] & ~15u;
        [branch] if (special && (sub == GVM_SUB_CALL || sub == GVM_SUB_CALLR)) r[1] = base + 16;
        pc = reg ? rt : tk ? tg : base + 16;
        cur = tk ? tc : nx;
        [branch] if (reg) cur = RAM(pc >> 4);
        [branch] if (pc >= _RamEnd) break;   // (the fetch fault is the general step's)
    }
    return res;
}

void take_irq() {
    [branch] if (irq_maybe && (pc & 15) == 0) {
        uint p = ipend() & ien;
        uint bit = (p & 4) ? 2u : (p & 2) ? 1u : 0u;
        trap(GVM_CAUSE_IRQ | bit, pc, 0);
    }
}

uint run(uint budget) {
    left = budget;
    endr = E_BUDGET;
    [branch] if (flags & F_HALTED) return E_HALT;
    [branch] if (flags & (F_OP_NEW | F_OP_RUN)) {   // the operation asked for: through, or wait for it
        uint4 os = _OpState.Load(int3(core, 0, 0));
        [branch] if (os.y == 0) return E_SYNC;   // still going
        opd0.x = os.y == 1 ? 0u : 0xffffffffu;
        flags &= ~(F_OP_NEW | F_OP_RUN);
    }
    [branch] if (flags & F_WAITING) {
        [branch] if ((ipend() & ien) == 0) return E_WAIT;
        flags &= ~F_WAITING;
    }
    recheck_irq();
    [loop] while (left > 0) {
        take_irq();
        uint e = fast();
        [branch] if (e != NONE) { endr = e; break; }
        [branch] if (left == 0) break;
        left--;
        e = bundle_general();
        [branch] if (e != E_BUDGET) { endr = e; break; }
        [branch] if (flags & F_OP_NEW) { }   // (a descriptor stored: the core goes on until its sync)
    }
    return endr;
}

// ---- The pass: state in, run, state out
uint4 state_in(uint t, uint c) {
    int3 at = int3(t >> 3, c, 0);
    switch (t & 7) {
    case 0: return _S0.Load(at); case 1: return _S1.Load(at); case 2: return _S2.Load(at); case 3: return _S3.Load(at);
    case 4: return _S4.Load(at); case 5: return _S5.Load(at); case 6: return _S6.Load(at); default: return _S7.Load(at);
    }
}
uint4 state_in_any(uint t, uint c) {
    int3 at = int3(t >> 3, c, 0);
    uint4 v0 = (t & 1) ? _S1.Load(at) : _S0.Load(at), v1 = (t & 1) ? _S3.Load(at) : _S2.Load(at);
    uint4 v2 = (t & 1) ? _S5.Load(at) : _S4.Load(at), v3 = (t & 1) ? _S7.Load(at) : _S6.Load(at);
    uint4 lo = (t & 2) ? v1 : v0, hi = (t & 2) ? v3 : v2;
    return (t & 4) ? hi : lo;
}
uint4 state_out(uint t) {
    uint4 v = 0;
    [branch] if (t < 16) v = uint4(r[4 * t], r[4 * t + 1], r[4 * t + 2], r[4 * t + 3]);
    else if (t == T_SYS0) v = uint4(pc, st, epc, cause);
    else if (t == T_SYS1) v = uint4(tval, tvec, scratch, ien);
    else if (t == T_SYS2) v = uint4(tcmp_lo, tcmp_hi, flags, rnd);
    else if (t == T_SYS3) v = uint4(cycle, opsrun, ran, endr);
    else if (t == T_OP0) v = opd0;
    else if (t == T_OP1) v = opd1;
    else if (t >= T_WC && t < T_WC + WCN / 2) {
        uint k = 2 * (t - T_WC);
        v = uint4(occ(k) ? wc[k] : uint2(0, 0), occ(k + 1) ? wc[k + 1] : uint2(0, 0));
    }
    return v;
}
struct v2f { float4 vertex : SV_POSITION; };
v2f vert(uint id : SV_VertexID) {
    v2f o;
    o.vertex = float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1);
    return o;
}
struct tick_out { uint4 t0 : SV_Target0; uint4 t1 : SV_Target1; uint4 t2 : SV_Target2; uint4 t3 : SV_Target3;
                  uint4 t4 : SV_Target4; uint4 t5 : SV_Target5; uint4 t6 : SV_Target6; uint4 t7 : SV_Target7; };
tick_out frag(v2f i) {
    uint2 p = uint2(i.vertex.xy);
    core = p.y;
    tick_out o;
    // a pixel of no core (the quad's second row, which the card runs as helpers): nothing to run
    [branch] if (core >= _NCores) { o.t0 = o.t1 = o.t2 = o.t3 = o.t4 = o.t5 = o.t6 = o.t7 = 0; return o; }
    uint k;
    [unroll] for (k = 0; k < 16; k++) {
        uint4 s = state_in(k, core);
        r[4 * k] = s.x; r[4 * k + 1] = s.y; r[4 * k + 2] = s.z; r[4 * k + 3] = s.w;
    }
    r[0] = 0;
    uint4 s0 = state_in(T_SYS0, core), s1 = state_in(T_SYS1, core), s2 = state_in(T_SYS2, core), s3 = state_in(T_SYS3, core);
    pc = s0.x; st = s0.y; epc = s0.z; cause = s0.w;
    tval = s1.x; tvec = s1.y; scratch = s1.z; ien = s1.w;
    tcmp_lo = s2.x; tcmp_hi = s2.y; flags = (s2.z & ~F_EXT) | (_Ext & (1u << (core & 31)) ? F_EXT : 0); rnd = s2.w;
    cycle = s3.x; opsrun = s3.y;
    opd0 = state_in(T_OP0, core); opd1 = state_in(T_OP1, core);
    // an operation stored last pass is in flight from this pass's commit on
    flags = (flags & F_OP_NEW) ? (flags & ~F_OP_NEW) | F_OP_RUN : flags;
    occv = 0;
    uint was = cycle;
    run(_Ticks);
    ran = cycle - was;
    o.t0 = state_out(p.x * 8 + 0); o.t1 = state_out(p.x * 8 + 1); o.t2 = state_out(p.x * 8 + 2); o.t3 = state_out(p.x * 8 + 3);
    o.t4 = state_out(p.x * 8 + 4); o.t5 = state_out(p.x * 8 + 5); o.t6 = state_out(p.x * 8 + 6); o.t7 = state_out(p.x * 8 + 7);
    return o;
}

// ---- The commit: a point a write cache pair, drawn into its texel of RAM in the draw of its word's component
cbuffer CommitParams : register(b1) { uint _Comp; uint _Cores; uint2 _cpad; };
struct c2f { float4 vertex : SV_POSITION; nointerpolation uint value : V; };
c2f commit_vert(uint id : SV_VertexID) {
    uint c = id / WCN, j = id % WCN;
    uint4 e = state_in_any(T_WC + (j >> 1), c);
    uint tag = (j & 1) ? e.z : e.x, word = tag - 1, tex = word >> 2;
    float2 at = (float2(tex & 2047, (tex >> 11) + RAM_ROW0) + 0.5) / float2(2048, _RamRows);
    c2f o;
    o.vertex = tag != 0 && (word & 3) == _Comp ? float4(at.x * 2 - 1, 1 - at.y * 2, 0, 1) : float4(-2, -2, 0, 1);
    o.value = (j & 1) ? e.w : e.y;
    return o;
}
uint4 commit_frag(c2f i) : SV_Target { return i.value; }

// ---- The operations (copy, fill, blit), a row of up to 256 KB at a time, up to 64 rows a pass:
// stage (the texels as they will be), write (into RAM), progress (rows done, in _OpState's other texture).
struct op_t { uint kind, dst, src, n, ds, ss, row, rows, fill; bool back, ok; };
bool op_range(uint a, uint n, bool dst) {
    uint e = a + n - 1;
    return n == 0 || (e >= a && (dst ? (a >= GVM_DEV && e < _RamEnd) : (e < _RamEnd || (a >= GVM_ROM && e < GVM_ROM_END))));
}
op_t op_of(uint4 d0, uint4 d1) {
    op_t o;
    o.kind = d0.x; o.dst = d0.y; o.src = d0.z; o.fill = d0.z; o.n = d0.w; o.back = false;
    [branch] if (o.kind == GVM_OP_BLIT) {
        o.ds = d1.x; o.ss = d1.y; o.row = d1.z; o.rows = d1.z == 0 ? 0 : d1.w;
        uint dn = o.rows ? o.ds * (o.rows - 1) + o.row : 0, sn = o.rows ? o.ss * (o.rows - 1) + o.row : 0;
        o.ok = o.row <= OP_ROW_MAX && op_range(o.dst, dn, true) && op_range(o.src, sn, false);
    } else {
        // rows between the destination's 256 KB boundaries, so that no two rows share a texel
        o.ds = OP_ROW_MAX; o.ss = OP_ROW_MAX; o.row = min(o.n, OP_ROW_MAX);
        o.rows = o.n == 0 ? 0 : ((o.dst + o.n - 1) >> 18) - (o.dst >> 18) + 1;
        o.back = o.kind == GVM_OP_COPY && o.dst > o.src && o.dst - o.src < o.n;
        o.ok = (o.kind == GVM_OP_COPY || o.kind == GVM_OP_FILL) && op_range(o.dst, o.n, true) && (o.kind == GVM_OP_FILL || op_range(o.src, o.n, false));
    }
    return o;
}
// row R's destination, source and bytes
void op_row(op_t o, uint R, out uint d0, out uint s0, out uint rb) {
    [branch] if (o.kind == GVM_OP_BLIT) { d0 = o.dst + R * o.ds; s0 = o.src + R * o.ss; rb = o.row; }
    else {
        uint A = o.dst & ~(OP_ROW_MAX - 1), e = min(o.dst + o.n, A + (R + 1) * OP_ROW_MAX);
        d0 = max(o.dst, A + R * OP_ROW_MAX); rb = e - d0; s0 = o.src + (d0 - o.dst);
    }
}
uint op_span(op_t o) { return ((o.row + 15) >> 4) + 1; }
uint op_per_pass(op_t o) { return min(OP_PER_PASS, max(1u, (OP_STAGE - 1) / op_span(o))); }
// the core's operation this pass: rows done before it and how many it makes; false when it has none
bool op_now(uint c, out op_t o, out uint done, out uint now) {
    uint4 s2 = state_in_any(T_SYS2, c);
    o = op_of(state_in_any(T_OP0, c), state_in_any(T_OP1, c));
    uint4 os = _OpState.Load(int3(c, 0, 0));
    bool fresh = (s2.z & F_OP_NEW) != 0, going = (s2.z & F_OP_RUN) != 0 && os.y == 0;
    done = fresh ? 0 : os.x;
    now = (fresh || going) && o.ok ? min(op_per_pass(o), o.rows - done) : 0;
    return (fresh || going) && o.ok && now > 0;
}
uint op_row_of(op_t o, uint done, uint q) { return o.back ? o.rows - 1 - (done + q) : done + q; }

struct o2f { float4 vertex : SV_POSITION; nointerpolation uint4 job : J; };   // core, q, the row, its first texel
o2f op_quad(uint vid, uint c, uint q, uint y0, uint y1, uint4 job, bool on, float h) {
    o2f o;
    float2 a = float2((vid & 1) ? 2048.0 : 0.0, (vid & 2) ? (float)y1 : (float)y0) / float2(2048.0, h);
    o.vertex = on ? float4(a.x * 2 - 1, 1 - a.y * 2, 0, 1) : float4(-2, -2, 0, 1);
    o.job = job;
    return o;
}
// stage: instance c * 64 + q, a quad over the stage rows its texels lie in
o2f op_stage_vert(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    uint c = inst / OP_PER_PASS, q = inst % OP_PER_PASS, done, now;
    op_t o;
    bool on = op_now(c, o, done, now) && q < now;
    uint rw = op_row_of(o, done, q), first = c * OP_STAGE + q * op_span(o);
    return op_quad(vid, c, q, first / 2048, (first + op_span(o) + 2047) / 2048, uint4(c, q, rw, first), on, (float)(_Cores * OP_STAGE / 2048));
}
uint src_byte(uint a) {
    uint v = a >= GVM_ROM ? pick(ROM((a - GVM_ROM) >> 4), (a >> 2) & 3) : pick(RAM(a >> 4), (a >> 2) & 3);
    return (v >> ((a & 3) * 8)) & 0xffu;
}
uint4 op_stage_frag(o2f i) : SV_Target {
    uint c = i.job.x, rw = i.job.z, first = i.job.w;
    uint2 px = uint2(i.vertex.xy);
    uint j = px.y * 2048 + px.x - first;
    op_t o = op_of(state_in_any(T_OP0, c), state_in_any(T_OP1, c));
    uint d0, s0, rb;
    op_row(o, rw, d0, s0, rb);
    uint cnt = rb == 0 ? 0 : ((d0 + rb - 1) >> 4) - (d0 >> 4) + 1;
    if (j >= cnt) discard;
    uint t = (d0 >> 4) + j;
    uint4 old = RAM(t), outv = old;
    [loop] for (uint b = 0; b < 16; b++) {
        uint a = t * 16 + b;
        [branch] if (a >= d0 && a - d0 < rb) {
            uint i2 = a - d0;
            uint v = o.kind == GVM_OP_FILL ? (o.fill >> (((a - o.dst) & 3) * 8)) & 0xffu : src_byte(s0 + i2);
            uint wi = b >> 2, sh = (b & 3) * 8;
            outv = put(outv, wi, (pick(outv, wi) & ~(0xffu << sh)) | (v << sh));
        }
    }
    return outv;
}
// write: a quad over the RAM rows of the row's texels, each from its stage texel
o2f op_write_vert(uint vid : SV_VertexID, uint inst : SV_InstanceID) {
    uint c = inst / OP_PER_PASS, q = inst % OP_PER_PASS, done, now;
    op_t o;
    bool on = op_now(c, o, done, now) && q < now;
    uint rw = op_row_of(o, done, q), first = c * OP_STAGE + q * op_span(o);
    uint d0, s0, rb;
    op_row(o, rw, d0, s0, rb);
    uint t0 = d0 >> 4, t1 = rb == 0 ? t0 : (d0 + rb - 1) >> 4;
    on = on && rb != 0;
    return op_quad(vid, c, q, (t0 >> 11) + RAM_ROW0, (t1 >> 11) + RAM_ROW0 + 1, uint4(t0, t1, first, 0), on, (float)_RamRows);
}
uint4 op_write_frag(o2f i) : SV_Target {
    uint2 px = uint2(i.vertex.xy);
    uint t = (px.y - RAM_ROW0) * 2048 + px.x;
    if (t < i.job.x || t > i.job.y) discard;
    uint s = i.job.z + (t - i.job.x);
    return _Stage.Load(int3(s & 2047, s >> 11, 0));
}
// progress: a point a core into the other _OpState: rows done, and 1 through / 2 refused / 0 going
struct p2f { float4 vertex : SV_POSITION; nointerpolation uint4 v : V; };
p2f op_prog_vert(uint c : SV_VertexID) {
    uint4 s2 = state_in_any(T_SYS2, c), os = _OpState.Load(int3(c, 0, 0));
    op_t o = op_of(state_in_any(T_OP0, c), state_in_any(T_OP1, c));
    bool fresh = (s2.z & F_OP_NEW) != 0, going = (s2.z & F_OP_RUN) != 0 && os.y == 0;
    uint done = fresh ? 0 : os.x, now = (fresh || going) && o.ok ? min(op_per_pass(o), o.rows - done) : 0;
    uint4 v = os;
    [branch] if (fresh || going) v = !o.ok ? uint4(0, 2, 0, 0) : done + now >= o.rows ? uint4(o.rows, 1, 0, 0) : uint4(done + now, 0, 0, 0);
    p2f p;
    p.vertex = float4(((float)c + 0.5) / (float)_Cores * 2 - 1, 0, 0, 1);
    p.v = v;
    return p;
}
uint4 op_prog_frag(p2f i) : SV_Target { return i.v; }
