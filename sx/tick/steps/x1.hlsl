// Step x1 (sx/tick/steps/README.md): SX's bundle at s21's speed: three op slots of one op, a sum (add, addi,
// rd = ra + B), and the control word (none, j, the fourteen branches). Every operand is read at the bundle's
// start and a result reaches the ops after it by selects (s14); the writes are made last, in order. An op it
// does not make (break among them) halts the core. No memory, no traps.
#include "../../isa.h"

Texture2D<uint4> _Ram : register(t0);
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
cbuffer PassParams : register(b0) {
    uint _Ticks, _RamEnd, _TimeLo, _TimeHi, _Player, _Flags, _RamRows, _Ext, _NCores; uint3 _ppad;
};
#define RAM(t) _Ram.Load(int3((t) & 2047, (t) >> 11, 0))
#ifndef ARR
#define ARR 64
#endif
static uint r[ARR];
static uint pc, cycle, opsrun, flags, ran;
static uint4 sys1, sys2, sys3x;   // texels the step carries and does not use

bool long_of(uint w) { return (w >> 20) == 0x800u && ((w >> 6) & 1) != 0 && ((w >> 5) & 1) != 0; }   // (integer and float I forms)
uint sx12(uint w) { return asuint((int)w >> 20); }
// an op this step makes: add or addi
bool known(uint w) { uint opc = w & 255; return opc == SX_OP_I(SX_I_ADD) || opc == SX_OP_II(SX_I_ADD); }

void run(uint budget) {
    uint4 cur = RAM(pc >> 4);
    [loop] while (budget > 0) {
        uint base = pc & ~15u;
        uint4 t = cur;
        uint ctl = t.w, n = ctl & 3, cc = (ctl >> 2) & 15, sub = (ctl >> 6) & 7;
        bool special = cc == SX_CC_SPECIAL;
        uint tg = base + asuint(special ? (int)ctl >> 9 : (int)ctl >> 18) * 16;
        uint4 nx = RAM((base >> 4) + 1), tc = RAM(tg >> 4);
        // the slots: an op, or the long immediate of the one before
        bool a0 = n > 0, L0 = a0 && long_of(t.x), a1 = n > 1 && !L0, L1 = a1 && long_of(t.y), a2 = n > 2 && !L1;
        uint i0 = L0 ? t.y : sx12(t.x), i1 = L1 ? t.z : sx12(t.y), i2 = sx12(t.z);
        bool ok = (!a0 || known(t.x)) && (!a1 || known(t.y)) && (!a2 || known(t.z)) && (special ? (sub == SX_SUB_NONE || sub == SX_SUB_J) : cc != 15);
        [branch] if (!ok) { flags |= 4; break; }   // halted
        uint d0 = a0 ? (t.x >> 8) & 63 : 0, d1 = a1 ? (t.y >> 8) & 63 : 0, d2 = a2 ? (t.z >> 8) & 63 : 0;
        uint ra0 = (t.x >> 14) & 63, rb0 = (t.x >> 20) & 63, ra1 = (t.y >> 14) & 63, rb1 = (t.y >> 20) & 63;
        uint ra2 = (t.z >> 14) & 63, rb2 = (t.z >> 20) & 63, ca = (ctl >> 6) & 63, cb = (ctl >> 12) & 63;
        // every read first; a result reaches the ops after it by selects; the writes last, in order
        uint A0 = r[ra0], B0 = r[rb0], A1 = r[ra1], B1 = r[rb1], A2 = r[ra2], B2 = r[rb2], CA = r[ca], CB = r[cb];
        uint v0 = A0 + ((t.x & 32) ? i0 : B0);
        A1 = (d0 != 0 && ra1 == d0) ? v0 : A1; B1 = (d0 != 0 && rb1 == d0) ? v0 : B1;
        uint v1 = A1 + ((t.y & 32) ? i1 : B1);
        A2 = (d1 != 0 && ra2 == d1) ? v1 : (d0 != 0 && ra2 == d0) ? v0 : A2;
        B2 = (d1 != 0 && rb2 == d1) ? v1 : (d0 != 0 && rb2 == d0) ? v0 : B2;
        uint v2 = A2 + ((t.z & 32) ? i2 : B2);
        CA = (d2 != 0 && ca == d2) ? v2 : (d1 != 0 && ca == d1) ? v1 : (d0 != 0 && ca == d0) ? v0 : CA;
        CB = (d2 != 0 && cb == d2) ? v2 : (d1 != 0 && cb == d1) ? v1 : (d0 != 0 && cb == d0) ? v0 : CB;
        r[d0] = v0; r[d1] = v1; r[d2] = v2;
        r[0] = 0;
        // the control word
        // cc 1-8 against a register, 9-14 against imm6: a test (equal, less, unsigned less, a bit) and its inverse
        bool imm = cc >= SX_CC_BEQI;
        uint kk = imm ? cc - SX_CC_BEQI : cc - 1, kind = kk >> 1;
        uint B = imm ? (kind == 2 ? cb : asuint((int)(cb << 26) >> 26)) : CB, F = kind == 1 ? 0x80000000u : 0;
        bool cond = kind == 0 ? CA == B : kind == 3 ? ((CA >> (cb & 31)) & 1) != 0 : (CA ^ F) < (B ^ F);
        bool tk = cond != ((kk & 1) != 0);
        tk = special ? sub == SX_SUB_J : tk;
        pc = tk ? tg : base + 16;
        cur = tk ? tc : nx;
        budget--;
        cycle++;
        opsrun += (a0 ? 1u : 0u) + (a1 ? 1u : 0u) + (a2 ? 1u : 0u);
    }
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
