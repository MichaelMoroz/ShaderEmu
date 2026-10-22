// Step x6 (gpu_vm/tick/steps/README.md): gpu_vm's two-slot bundle (gpu_vm/isa.h): op 0 with the whole integer unit, op 1 with
// sums, logic and the three shifts (path 1), word 2 the long immediate of the op the control word names. The
// control word compares the registers as the bundle found them, so the next bundle's fetch does not wait for its
// ops. A write to r0 lands in r64. A bundle of another path halts the core.
#include "../../isa.h"

Texture2D<uint4> _Ram : register(t0);
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
cbuffer PassParams : register(b0) {
    uint _Ticks, _RamEnd, _TimeLo, _TimeHi, _Player, _Flags, _RamRows, _Ext, _NCores; uint3 _ppad;
};
#define RAM(t) _Ram.Load(int3((t) & 2047, (t) >> 11, 0))
static uint r[65];
static uint pc, cycle, opsrun, flags, ran;
static uint4 sys1, sys2, sys3x;   // texels the step carries and does not use

uint simm12(uint w) { return asuint((int)w >> 20); }
uint alu(uint w, uint imm, uint a, uint b) {   // every result in parallel, then a tree of selects on f's bits
    uint f = w & 31;
    bool I = (w & 32) != 0, sw = I && f == GVM_I_SUB;   // rsub: imm - ra
    uint B = I ? imm : b, x = sw ? B : a, y = sw ? a : B;
    bool f0 = (f & 1) != 0, f1 = (f & 2) != 0, f2 = (f & 4) != 0;
    uint yi = f2 ? ~y : y, sum = x + yi + (f2 ? 1u : 0u);   // sub is x + ~y + 1
    uint g0 = f1 ? (f0 ? x ^ yi : x | yi) : (f0 ? x & yi : sum);
    uint s = y & 31, sl = x << s, sr = x >> s;   // (a shift by 32 - 0 is by 0: the rotates need no test)
    uint lo = f1 ? (f0 ? sr | (x << (32 - s)) : asuint(asint(x) >> s)) : (f0 ? sr : sl);
    uint hi = f1 ? ((x == y) != f0 ? 1u : 0u) : (f0 ? x * y : sl | (x >> (32 - s)));
    uint F = f0 ? 0u : 0x80000000u, xs = x ^ F, ys = y ^ F;
    bool lt = f1 ? ys < xs : xs < ys;
    uint g2 = f2 ? (lt ? x : y) : (lt ? 1u : 0u);
    return (f & 16) ? g2 : (f & 8) ? (f2 ? hi : lo) : g0;
}
uint alu_lite(uint w, uint imm, uint a, uint b) {   // op 1: sums, logic and the three shifts (f 0-10)
    uint f = w & 31;
    bool I = (w & 32) != 0, sw = I && f == GVM_I_SUB;
    uint B = I ? imm : b, x = sw ? B : a, y = sw ? a : B;
    bool f0 = (f & 1) != 0, f1 = (f & 2) != 0, f2 = (f & 4) != 0;
    uint yi = f2 ? ~y : y, sum = x + yi + (f2 ? 1u : 0u);
    uint g0 = f1 ? (f0 ? x ^ yi : x | yi) : (f0 ? x & yi : sum);
    uint s = y & 31;
    return (f & 8) ? (f1 ? asuint(asint(x) >> s) : (f0 ? x >> s : x << s)) : g0;
}

void run(uint budget) {
    uint4 cur = RAM(pc >> 4);
#if LATE   // (probe: a bundle's results written after the next bundle's reads, which take them by selects)
    uint pd0 = 64, pv0 = 0, pd1 = 64, pv1 = 0;
#define LATE_FWD(X, rX) X = (rX) == pd1 ? pv1 : (rX) == pd0 ? pv0 : X
#endif
    [loop] while (budget > 0) {
        uint base = pc & ~15u;
        uint4 t = cur;
        uint ctl = t.w, n = ctl & 3, cc = (ctl >> 6) & 15, sub = (ctl >> 10) & 7;
        bool special = cc == GVM_CC_SPECIAL;
        uint tg = base + asuint(special ? (int)ctl >> 13 : (int)ctl >> 22) * 16;
        uint4 nx = RAM((base >> 4) + 1), tc = RAM(tg >> 4);
        bool ok = ((ctl >> 4) & 3) == GVM_PATH_INT && n < 3 && (special ? (sub == GVM_SUB_NONE || sub == GVM_SUB_J) : cc != 15);
        [branch] if (!ok) { flags |= 4; break; }   // halted
        bool a1 = n > 1, L = (ctl & 4) != 0, L1 = (ctl & 8) != 0;
        uint i0 = L && !L1 ? t.z : simm12(t.x), i1 = L && L1 ? t.z : simm12(t.y);
        uint d0 = n > 0 ? (t.x >> 8) & 63 : 0, d1 = a1 ? (t.y >> 8) & 63 : 0;
        uint ra0 = (t.x >> 14) & 63, rb0 = (t.x >> 20) & 63, ra1 = (t.y >> 14) & 63, rb1 = (t.y >> 20) & 63;
        uint ca = (ctl >> 10) & 63, cb = (ctl >> 16) & 63;
        // every read first: the branch's as the bundle found them; op 0's result reaches op 1 by selects
        uint A0 = r[ra0], B0 = r[rb0], A1 = r[ra1], B1 = r[rb1], CA = r[ca], CB = r[cb];
#if LATE
        LATE_FWD(A0, ra0); LATE_FWD(B0, rb0); LATE_FWD(A1, ra1); LATE_FWD(B1, rb1); LATE_FWD(CA, ca); LATE_FWD(CB, cb);
#endif
#if BR_PROBE   // (probe: the integer unit on one side of a branch whose other side is trivial)
        uint v0, v1;
        [branch] if (((ctl >> 4) & 3) >= GVM_PATH_MEM) { v0 = A0 ^ B0; v1 = A1 + B1; }
        else {
            v0 = alu(t.x, i0, A0, B0);
            A1 = (d0 != 0 && ra1 == d0) ? v0 : A1; B1 = (d0 != 0 && rb1 == d0) ? v0 : B1;
            v1 = alu_lite(t.y, i1, A1, B1);
        }
#else
        uint v0 = alu(t.x, i0, A0, B0);
        A1 = (d0 != 0 && ra1 == d0) ? v0 : A1; B1 = (d0 != 0 && rb1 == d0) ? v0 : B1;
        uint v1 = alu_lite(t.y, i1, A1, B1);
#endif
#if LATE
        r[pd0] = pv0; r[pd1] = pv1;
        pd0 = d0 == 0 ? 64 : d0; pv0 = v0; pd1 = d1 == 0 ? 64 : d1; pv1 = v1;
#else
        r[d0 == 0 ? 64 : d0] = v0; r[d1 == 0 ? 64 : d1] = v1;
#endif
        // the control word: cc 1-8 against a register, 9-14 against imm6, a test and its inverse
        bool imm = cc >= GVM_CC_BEQI;
        uint kk = imm ? cc - GVM_CC_BEQI : cc - 1, kind = kk >> 1;
        uint B = imm ? (kind == 2 ? cb : asuint((int)(cb << 26) >> 26)) : CB, F = kind == 1 ? 0x80000000u : 0;
        bool cond = kind == 0 ? CA == B : kind == 3 ? ((CA >> (cb & 31)) & 1) != 0 : (CA ^ F) < (B ^ F);
        bool tk = special ? sub == GVM_SUB_J : cond != ((kk & 1) != 0);
        pc = tk ? tg : base + 16;
        cur = tk ? tc : nx;
        budget--;
        cycle++;
        opsrun += n;
    }
#if LATE
    r[pd0] = pv0; r[pd1] = pv1;
#endif
}

uint4 state_in(uint t, uint c) {
    int3 at = int3(t >> 3, c, 0);
    switch (t & 7) {
    case 0: return _S0.Load(at); case 1: return _S1.Load(at); case 2: return _S2.Load(at); case 3: return _S3.Load(at);
    case 4: return _S4.Load(at); case 5: return _S5.Load(at); case 6: return _S6.Load(at); default: return _S7.Load(at);
    }
}
uint4 state_out(uint t, uint4 s0) {
    [branch] if (t < 16) return uint4(r[4 * t], r[4 * t + 1], r[4 * t + 2], r[4 * t + 3]);
    return t == 16 ? uint4(pc, s0.yzw) : t == 17 ? sys1 : t == 18 ? uint4(sys2.xy, flags, sys2.w) : t == 19 ? uint4(cycle, opsrun, ran, 0) : 0;
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
    uint core = p.y, k;
    tick_out o;
    [branch] if (core >= _NCores) { o.t0 = o.t1 = o.t2 = o.t3 = o.t4 = o.t5 = o.t6 = o.t7 = 0; return o; }
    [unroll] for (k = 0; k < 16; k++) {
        uint4 s = state_in(k, core);
        r[4 * k] = s.x; r[4 * k + 1] = s.y; r[4 * k + 2] = s.z; r[4 * k + 3] = s.w;
    }
    r[0] = 0;
    uint4 s0 = state_in(16, core);
    sys1 = state_in(17, core); sys2 = state_in(18, core); sys3x = state_in(19, core);
    pc = s0.x; flags = sys2.z; cycle = sys3x.x; opsrun = sys3x.y;
    [branch] if ((flags & 4) == 0) run(_Ticks);
    ran = cycle - sys3x.x;
    o.t0 = state_out(p.x * 8 + 0, s0); o.t1 = state_out(p.x * 8 + 1, s0); o.t2 = state_out(p.x * 8 + 2, s0); o.t3 = state_out(p.x * 8 + 3, s0);
    o.t4 = state_out(p.x * 8 + 4, s0); o.t5 = state_out(p.x * 8 + 5, s0); o.t6 = state_out(p.x * 8 + 6, s0); o.t7 = state_out(p.x * 8 + 7, s0);
    return o;
}
