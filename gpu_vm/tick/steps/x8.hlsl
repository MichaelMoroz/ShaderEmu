// Step x8 (gpu_vm/tick/steps/README.md): x7 and path 3, two memory ops a bundle: op 1 a load or a store as op 0 is.
// When op 1's store finds no room, op 0's write cache entry is put back as it was and the pass ends before the
// bundle. Anything else halts the core.
#include "../../isa.h"

Texture2D<uint4> _Ram : register(t0);
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
cbuffer PassParams : register(b0) {
    uint _Ticks, _RamEnd, _TimeLo, _TimeHi, _Player, _Flags, _RamRows, _Ext, _NCores; uint3 _ppad;
};
#define RAM(t) _Ram.Load(int3((t) & 2047, (t) >> 11, 0))
static uint r[65];
#define WCN 128
#define HALF (WCN / 2)
#define NONE 0xffffffffu
static uint2 wc[WCN];
static uint4 occv;   // a bit a slot: taken this pass
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

uint pick(uint4 q, uint i) { return (i & 2) ? ((i & 1) ? q.w : q.z) : ((i & 1) ? q.y : q.x); }
uint4 put(uint4 v, uint i, uint x) { return uint4(i == 0 ? x : v.x, i == 1 ? x : v.y, i == 2 ? x : v.z, i == 3 ? x : v.w); }
// the write cache: word w in slot h or h + HALF, filled in that order and never emptied in a pass
uint wc_h(uint w) { return (w ^ (w >> 6)) & (HALF - 1); }
bool occ(uint s) { return ((pick(occv, s >> 5) >> (s & 31)) & 1) != 0; }
uint wc_get(uint w, uint def) {   // word w as this core sees it, def being RAM's
    uint h = wc_h(w), v = def;
    [branch] if (occ(h)) {
        uint2 e = wc[h];
        [branch] if (e.x == w + 1) v = e.y;
        else if (occ(h + HALF)) { uint2 e2 = wc[h + HALF]; if (e2.x == w + 1) v = e2.y; }
    }
    return v;
}
uint wc_slot(uint w) {   // where word w is or would go, or NONE when both slots hold others
    uint h = wc_h(w), s = NONE;
    [branch] if (!occ(h)) s = h;
    else if (wc[h].x == w + 1) s = h;
    else if (!occ(h + HALF)) s = h + HALF;
    else if (wc[h + HALF].x == w + 1) s = h + HALF;
    return s;
}
uint mem_addr(uint w, uint imm, uint a, uint b) { return (w & 16) ? a + (b << (w >> 26)) : a + imm; }
// a memory op of paths 2 and 3 at addr, q its RAM texel: a load's value in v; a store's slot in sl (else NONE) and
// what it held in was; false: a store found no room (nothing done)
bool mem(uint w, uint addr, uint4 q, out uint v, out uint sl, out uint2 was) {
    uint mode = (w >> 3) & 7, wd = w & 7, rd = (w >> 8) & 63;
    uint wa = addr >> 2, sh = (addr & 3) * 8;
    uint word = pick(q, wa & 3);
    bool ok = true;
    v = 0; sl = NONE; was = 0;
    [branch] if ((mode & 1) == 0) {   // a load
        v = wc_get(wa, word) >> sh;
        v = wd == GVM_W_W ? v : wd == GVM_W_B ? asuint((int)(v << 24) >> 24) : wd == GVM_W_H ? asuint((int)(v << 16) >> 16)
          : wd == GVM_W_BU ? (v & 0xffu) : (v & 0xffffu);
    } else {   // a store: a byte or a half into the word as this core sees it
        uint s = wc_slot(wa);
        ok = s != NONE;
        [branch] if (ok) {
            uint x = r[rd], m = wd == GVM_W_W ? 0xffffffffu : ((wd & 1) ? 0xffffu : 0xffu) << sh;
            was = wc[s]; sl = s;
            uint old = occ(s) ? was.y : word;
            wc[s] = uint2(wa + 1, (old & ~m) | ((x << sh) & m));
            occv = put(occv, s >> 5, pick(occv, s >> 5) | (1u << (s & 31)));
        }
    }
    return ok;
}

void run(uint budget) {
    uint4 cur = RAM(pc >> 4);
    [loop] while (budget > 0) {
        uint base = pc & ~15u;
        uint4 t = cur;
        uint ctl = t.w, n = ctl & 3, cc = (ctl >> 6) & 15, sub = (ctl >> 10) & 7;
        bool special = cc == GVM_CC_SPECIAL;
        uint tg = base + asuint(special ? (int)ctl >> 13 : (int)ctl >> 22) * 16;
        uint4 nx = RAM((base >> 4) + 1), tc = RAM(tg >> 4);
        uint path = (ctl >> 4) & 3;
        bool ok = path != GVM_PATH_GENERAL && n < 3 && (special ? (sub == GVM_SUB_NONE || sub == GVM_SUB_J) : cc != 15);
        [branch] if (!ok) { flags |= 4; break; }   // halted
        bool a1 = n > 1, L = (ctl & 4) != 0, L1 = (ctl & 8) != 0;
        uint i0 = L && !L1 ? t.z : simm12(t.x), i1 = L && L1 ? t.z : simm12(t.y);
        uint d0 = n > 0 ? (t.x >> 8) & 63 : 0, d1 = a1 ? (t.y >> 8) & 63 : 0;
        uint ra0 = (t.x >> 14) & 63, rb0 = (t.x >> 20) & 63, ra1 = (t.y >> 14) & 63, rb1 = (t.y >> 20) & 63;
        uint ca = (ctl >> 10) & 63, cb = (ctl >> 16) & 63;
        // every read first: the branch's as the bundle found them; op 0's result reaches op 1 by selects
        uint A0 = r[ra0], B0 = r[rb0], A1 = r[ra1], B1 = r[rb1], CA = r[ca], CB = r[cb];
        uint v0, v1, sl0, sl1;
        uint2 was0, was1;
        [branch] if (path == GVM_PATH_MEM2) {   // two loads or stores; op 1's store with no room undoes op 0
            // both texels fetched at once from the registers as read; again only when op 1 needs op 0's load
            uint ad0 = mem_addr(t.x, i0, A0, B0), ad1 = mem_addr(t.y, i1, A1, B1);
            uint4 q0 = RAM(ad0 >> 4), q1 = RAM(ad1 >> 4);
            uint4 occ0 = occv;
            uint old0 = r[d0 == 0 ? 64 : d0];
            [branch] if (!mem(t.x, ad0, q0, v0, sl0, was0)) break;
            d0 = (t.x & 8) ? 0 : d0;
            r[d0 == 0 ? 64 : d0] = v0;
            [branch] if (d0 != 0 && (ra1 == d0 || rb1 == d0)) {
                A1 = ra1 == d0 ? v0 : A1; B1 = rb1 == d0 ? v0 : B1;
                ad1 = mem_addr(t.y, i1, A1, B1);
                q1 = RAM(ad1 >> 4);
            }
            [branch] if (!mem(t.y, ad1, q1, v1, sl1, was1)) {
                [branch] if (sl0 != NONE) wc[sl0] = was0;
                occv = occ0;
                r[d0 == 0 ? 64 : d0] = old0;
                break;
            }
            d1 = (t.y & 8) ? 0 : d1;
        } else {   // op 0 an integer op or a load or store, op 1 a cheap integer op
            [branch] if (path == GVM_PATH_MEM) {
                uint ad0 = mem_addr(t.x, i0, A0, B0);
                [branch] if (!mem(t.x, ad0, RAM(ad0 >> 4), v0, sl0, was0)) break;   // no room: the bundle again next pass
                d0 = (t.x & 8) ? 0 : d0;   // a store's rd is its value, not a result
            } else v0 = alu(t.x, i0, A0, B0);
            r[d0 == 0 ? 64 : d0] = v0;
            A1 = (d0 != 0 && ra1 == d0) ? v0 : A1; B1 = (d0 != 0 && rb1 == d0) ? v0 : B1;
            v1 = alu_lite(t.y, i1, A1, B1);
        }
        r[d1 == 0 ? 64 : d1] = v1;
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
    [branch] if (t >= 24 && t < 24 + WCN / 2) {   // the write cache: two pairs a texel, a pair with no word as 0
        uint k = 2 * (t - 24);
        return uint4(occ(k) ? wc[k] : uint2(0, 0), occ(k + 1) ? wc[k + 1] : uint2(0, 0));
    }
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
    occv = 0;
    [branch] if ((flags & 4) == 0) run(_Ticks);
    ran = cycle - sys3x.x;
    o.t0 = state_out(p.x * 8 + 0, s0); o.t1 = state_out(p.x * 8 + 1, s0); o.t2 = state_out(p.x * 8 + 2, s0); o.t3 = state_out(p.x * 8 + 3, s0);
    o.t4 = state_out(p.x * 8 + 4, s0); o.t5 = state_out(p.x * 8 + 5, s0); o.t6 = state_out(p.x * 8 + 6, s0); o.t7 = state_out(p.x * 8 + 7, s0);
    return o;
}

// ---- The commit: a point a write cache pair, drawn into its texel of RAM in the draw of its word's component
cbuffer CommitParams : register(b1) { uint _Comp; uint _Cores; uint2 _cpad; };
uint4 state_any(uint t, uint c) {
    int3 at = int3(t >> 3, c, 0);
    uint4 v0 = (t & 1) ? _S1.Load(at) : _S0.Load(at), v1 = (t & 1) ? _S3.Load(at) : _S2.Load(at);
    uint4 v2 = (t & 1) ? _S5.Load(at) : _S4.Load(at), v3 = (t & 1) ? _S7.Load(at) : _S6.Load(at);
    return (t & 4) ? ((t & 2) ? v3 : v2) : ((t & 2) ? v1 : v0);
}
struct c2f { float4 vertex : SV_POSITION; nointerpolation uint value : V; };
c2f commit_vert(uint id : SV_VertexID) {
    uint c = id / WCN, j = id % WCN;
    uint4 e = state_any(24 + (j >> 1), c);
    uint tag = (j & 1) ? e.z : e.x, word = tag - 1, tex = word >> 2;
    float2 at = (float2(tex & 2047, tex >> 11) + 0.5) / float2(2048, _RamRows);
    c2f o;
    o.vertex = tag != 0 && (word & 3) == _Comp ? float4(at.x * 2 - 1, 1 - at.y * 2, 0, 1) : float4(-2, -2, 0, 1);
    o.value = (j & 1) ? e.w : e.y;
    return o;
}
uint4 commit_frag(c2f i) : SV_Target { return i.value; }
