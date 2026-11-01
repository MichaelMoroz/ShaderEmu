// Step 16: a new form. Each RV word has a predecoded texel of its own in `_Code` (xlate.py); arithmetic, lui, auipc, jal, branches and loads on one path with no decoding of immediates; the rest, by the old decode from the RV word, a way out.
#include "mem.hlsl"
Texture2D<uint4> _Ram : register(t0);
Texture2D<uint4> _S0 : register(t1); Texture2D<uint4> _S1 : register(t2); Texture2D<uint4> _S2 : register(t3); Texture2D<uint4> _S3 : register(t4);
Texture2D<uint4> _S4 : register(t5); Texture2D<uint4> _S5 : register(t6); Texture2D<uint4> _S6 : register(t7); Texture2D<uint4> _S7 : register(t8);
Texture2D<uint4> _Code : register(t9);
cbuffer C : register(b0) { uint _Budget; };
float4 vert(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1); }
struct Out { uint4 o0 : SV_Target0; uint4 o1 : SV_Target1; uint4 o2 : SV_Target2; uint4 o3 : SV_Target3; uint4 o4 : SV_Target4; uint4 o5 : SV_Target5; uint4 o6 : SV_Target6; uint4 o7 : SV_Target7; };
#define RAM(t) _Ram.Load(int3((t) & 2047, (t) >> 11, 0))
#define TEXEL_OF(a) (((a) & 0x7fffffff) >> 4)
#define RAM_LO 0x82000000u
#define RAM_HI 0x92000000u
#ifndef PROBES
#define PROBES 8
#endif
#define SINK 32
#define HASH(u) ((((u) * 40503u) & 0xffff) * N_ENT >> 16)
static uint fl = 0, fh = 0;
static bool res_on = false; static uint res_at = 0;
uint mulhu_(uint a, uint b) {
    uint al = a & 0xffff, ah = a >> 16, bl = b & 0xffff, bh = b >> 16, t = al * bl, m1 = ah * bl + (t >> 16), m2 = al * bh + (m1 & 0xffff);
    return ah * bh + (m1 >> 16) + (m2 >> 16);
}
uint op_m(uint f3, uint a, uint b) {
    [branch] if (f3 == 0) return a * b;
    [branch] if (f3 < 4) {
        uint h = mulhu_(a, b);
        h -= (f3 != 3 && (a >> 31) != 0) ? b : 0;
        h -= (f3 == 1 && (b >> 31) != 0) ? a : 0;
        return h;
    }
    bool sgn = (f3 & 1) == 0, rem = (f3 & 2) != 0, sa = sgn && (a >> 31) != 0, sb = sgn && (b >> 31) != 0;
    uint ua = sa ? 0u - a : a, ub = sb ? 0u - b : b;
    [branch] if (b == 0) return rem ? a : 0xffffffffu;
    uint q = ua / ub, r = ua % ub;
    return rem ? (sa ? 0u - r : r) : (sa != sb ? 0u - q : q);
}
uint lookup(inout uint m[2048], uint u) {
    uint i = HASH(u), got = EMPTY, k = 0;
    [loop] for (k = 0; k < PROBES; k++) {
        uint g = m[TAG0 + i];
        [branch] if (g == u) { got = i; break; }
        [branch] if (g == EMPTY) break;
        i = i + 1 == N_ENT ? 0 : i + 1;
    }
    return got;
}
bool put(inout uint m[2048], uint va_, uint val, uint f3_) {
    uint u_ = TEXEL_OF(va_), ws_ = (va_ >> 2) & 3, i = HASH(u_), sl = EMPTY, k;
    [loop] for (k = 0; k < PROBES; k++) {
        uint g = m[TAG0 + i];
        [branch] if (g == u_) { sl = i; break; }
        [branch] if (g == EMPTY) {
            uint4 q_ = RAM(u_); uint o = DATA0 + 4 * i;
            m[o] = q_.x; m[o + 1] = q_.y; m[o + 2] = q_.z; m[o + 3] = q_.w; m[TAG0 + i] = u_; sl = i;
            uint h6 = u_ & 63;
            fl |= (h6 & 32) ? 0 : 1u << (h6 & 31); fh |= (h6 & 32) ? 1u << (h6 & 31) : 0;
            break;
        }
        i = i + 1 == N_ENT ? 0 : i + 1;
    }
    [branch] if (sl == EMPTY) return false;
    uint sh = (va_ & 3) * 8, mask = (f3_ == 2 ? 0xffffffffu : f3_ == 1 ? 0xffffu : 0xffu) << sh, o = DATA0 + 4 * sl + ws_;
    m[o] = (m[o] & ~mask) | ((val << sh) & mask);
    return true;
}
Out frag(float4 at : SV_Position) {
    uint m[2048];
    int3 me = int3(at.xy, 0);
    uint lin = (me.x + 8 * me.y) * 8;
    uint4 ctl = _S0.Load(int3(1, 0, 0));
    uint n = 0, budget = ctl.z != 0 ? 0 : _Budget, reason = ctl.z, outs = 0, full = 0, tp = (ctl.x - CODE_LO) >> 2, lastw = 0;
    { int3 a = int3(0, 0, 0); uint4 q; uint i;
      q = _S0.Load(a); m[0] = q.x; m[1] = q.y; m[2] = q.z; m[3] = q.w; q = _S1.Load(a); m[4] = q.x; m[5] = q.y; m[6] = q.z; m[7] = q.w;
      q = _S2.Load(a); m[8] = q.x; m[9] = q.y; m[10] = q.z; m[11] = q.w; q = _S3.Load(a); m[12] = q.x; m[13] = q.y; m[14] = q.z; m[15] = q.w;
      q = _S4.Load(a); m[16] = q.x; m[17] = q.y; m[18] = q.z; m[19] = q.w; q = _S5.Load(a); m[20] = q.x; m[21] = q.y; m[22] = q.z; m[23] = q.w;
      q = _S6.Load(a); m[24] = q.x; m[25] = q.y; m[26] = q.z; m[27] = q.w; q = _S7.Load(a); m[28] = q.x; m[29] = q.y; m[30] = q.z; m[31] = q.w;
      [loop] for (i = 0; i < N_ENT; i++) m[TAG0 + i] = EMPTY;
    }
    [loop] while (n < budget) {
        uint4 c = _Code.Load(int3(tp & 2047, tp >> 11, 0));
        uint k = c.x, f3 = (k >> 18) & 7;
        uint a0 = m[(k >> 6) & 63], b0 = m[(k >> 12) & 63], y = (k & (1u << 21)) ? b0 : c.y;
        uint nx = tp + 1;
        [branch] if (k & (1u << 25)) {
            uint w = c.w, op = w & 0x7f, pc = CODE_LO + 4 * tp;
            bool ok = true;
            lastw = w; outs++;
            [branch] if (op == 0x23) {
                uint f3_ = (w >> 12) & 7, va_ = m[(w >> 15) & 31] + (asuint(asint(w) >> 25 << 5) | ((w >> 7) & 31));
                [branch] if (va_ - RAM_LO >= RAM_HI - RAM_LO || (va_ & 3) + (1u << f3_) > 4 || f3_ > 2) { reason = 3; budget = n; ok = false; }
                else { ok = put(m, va_, m[(w >> 20) & 31], f3_); [branch] if (!ok) { budget = n; full = 1; } }
            } else [branch] if (op == 0x67) {
                uint rd = (w >> 7) & 31; nx = (m[(w >> 15) & 31] + asuint(asint(w) >> 20) - CODE_LO) >> 2; m[rd == 0 ? SINK : rd] = pc + 4;
            } else [branch] if (op == 0x33 && (w >> 25) == 1) {
                uint f3_ = (w >> 12) & 7, rd_ = (w >> 7) & 31, a_ = m[(w >> 15) & 31], b_ = m[(w >> 20) & 31];
                m[rd_ == 0 ? SINK : rd_] = op_m(f3_, a_, b_);
            } else [branch] if (op == 0x2f && ((w >> 12) & 7) == 2) {
                uint f5 = w >> 27, rd_ = (w >> 7) & 31, a_ = m[(w >> 15) & 31], b_ = m[(w >> 20) & 31];
                [branch] if (a_ - RAM_LO >= RAM_HI - RAM_LO || (a_ & 3) != 0 || f5 > 12 || ((0x111fu >> f5) & 1) == 0) { reason = 4; budget = n; ok = false; }
                else {
                    uint old = 0, sl = lookup(m, TEXEL_OF(a_));
                    [branch] if (sl != EMPTY) old = m[DATA0 + 4 * sl + ((a_ >> 2) & 3)]; else { uint4 q_ = RAM(TEXEL_OF(a_)); old = q_[(a_ >> 2) & 3]; }
                    bool sc_ok = res_on && res_at == a_;
                    [branch] if (f5 != 2 && (f5 != 3 || sc_ok)) ok = put(m, a_, f5 == 0 ? old + b_ : f5 == 4 ? old ^ b_ : f5 == 8 ? old | b_ : f5 == 12 ? old & b_ : b_, 2);
                    [branch] if (ok) { m[rd_ == 0 ? SINK : rd_] = f5 == 3 ? (sc_ok ? 0 : 1) : old; res_on = f5 == 2; res_at = a_; }
                    else { budget = n; full = 1; }
                }
            } else [branch] if (op == 0x0f) { }
            else [branch] if ((w & 0xfffff07f) == 0xc0002073 || (w & 0xfffff07f) == 0xc8102073 || (w & 0xfffff07f) == 0xc0102073) {
                uint rd_ = (w >> 7) & 31; m[rd_ == 0 ? SINK : rd_] = 0;
            } else { reason = 1; budget = n; ok = false; }
            [branch] if (ok) { tp = nx; n++; }
        } else {
            uint s = y & 31, turn = (k & (1u << 27)) ? 0 : 0x80000000;
            bool ltb = (a0 ^ turn) < (y ^ turn);
            uint va = a0 + y;
            uint r01 = (f3 & 1) ? a0 << s : ((k & (3u << 21)) == (3u << 21) ? a0 - y : va), r23 = ltb ? 1 : 0;
            uint r45 = (f3 & 1) ? ((k & (1u << 22)) ? asuint(asint(a0) >> s) : a0 >> s) : a0 ^ y, r67 = (f3 & 1) ? a0 & y : a0 | y;
            uint v0 = (f3 & 4) ? ((f3 & 2) ? r67 : r45) : ((f3 & 2) ? r23 : r01);
            bool take = (k & (1u << 26)) != 0 || ((k & (1u << 24)) != 0 && ((((f3 & 4) ? ltb : a0 == y) != ((f3 & 1) != 0))));
            nx = take ? c.z : nx;
            bool go = true;
            [branch] if (k & (1u << 23)) {
                uint u = TEXEL_OF(va), ws = (va >> 2) & 3, sz = f3 & 3, h6 = u & 63, z0;
                [branch] if (va - RAM_LO >= RAM_HI - RAM_LO || (va & 3) + (1u << sz) > 4) { reason = 2; budget = n; go = false; lastw = c.w; }
                else {
                    uint4 q = RAM(u); z0 = q[ws];
                    [branch] if ((((h6 & 32) ? fh : fl) >> (h6 & 31)) & 1) {
                        uint sl = lookup(m, u);
                        [branch] if (sl != EMPTY) z0 = m[DATA0 + 4 * sl + ws];
                    }
                    uint up = 24 - 8 * sz - (sz == 2 ? 8 : 0), z = z0 >> ((va & 3) * 8) << up;
                    v0 = (f3 & 4) ? z >> up : asuint(asint(z) >> up);
                }
            }
            [branch] if (go) { m[k & 63] = v0; tp = nx; n++; }
        }
    }
    m[32] = CODE_LO + 4 * tp; m[33] = ctl.y + n; m[34] = reason; m[35] = reason ? lastw : 0; m[36] = outs; m[37] = full; m[38] = 0; m[39] = 0;
    Out r;
    uint e = 4 * lin;
    r.o0 = uint4(m[e], m[e + 1], m[e + 2], m[e + 3]); r.o1 = uint4(m[e + 4], m[e + 5], m[e + 6], m[e + 7]);
    r.o2 = uint4(m[e + 8], m[e + 9], m[e + 10], m[e + 11]); r.o3 = uint4(m[e + 12], m[e + 13], m[e + 14], m[e + 15]);
    r.o4 = uint4(m[e + 16], m[e + 17], m[e + 18], m[e + 19]); r.o5 = uint4(m[e + 20], m[e + 21], m[e + 22], m[e + 23]);
    r.o6 = uint4(m[e + 24], m[e + 25], m[e + 26], m[e + 27]); r.o7 = uint4(m[e + 28], m[e + 29], m[e + 30], m[e + 31]);
    return r;
}
