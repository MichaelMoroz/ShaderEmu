// Step 15: step 12 with the array local.
// Step 12: what real code needs (snapsim.py): RAM at its addresses, multiplication and division, atomics, fence, the time CSR read as 0; anything else stops the machine,
// reason in texel 8's third word, the word in its fourth. Control before stores in the way out, as real code has more of it. Counts in texel 9: ways out, a pass ended full.
// From step 8: stores and the write cache. One array of words (mem.hlsl). An entry is a whole texel, read from RAM when it is made; slots by open
// probing from a hash, at most PROBES; a filter of 64 bits by the hash's low bits lets a load that no store touched go straight to RAM.
// A store that finds no slot ends the pass with that instruction not done. NOFILTER: every load looks in the table.
#include "mem.hlsl"
Texture2D<uint4> _Ram : register(t0);
Texture2D<uint4> _S0 : register(t1); Texture2D<uint4> _S1 : register(t2); Texture2D<uint4> _S2 : register(t3); Texture2D<uint4> _S3 : register(t4);
Texture2D<uint4> _S4 : register(t5); Texture2D<uint4> _S5 : register(t6); Texture2D<uint4> _S6 : register(t7); Texture2D<uint4> _S7 : register(t8);
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
#define FETCH(pc) t = RAM(TEXEL_OF(pc)); w = t[((pc) >> 2) & 3]
#define HASH(u) ((((u) * 40503u) & 0xffff) * N_ENT >> 16)
#define ALUV uint f3 = (w >> 12) & 7, rd = (w >> 7) & 31, a0 = m[(w >> 15) & 31], b0 = m[(w >> 20) & 31], dst = rd == 0 ? SINK : rd, v0; \
    bool reg = (w & 0x20) != 0, alt = (w & 0x40000000) != 0; \
    uint y = reg ? b0 : asuint(asint(w) >> 20), s = y & 31, turn = (f3 & 1) ? 0 : 0x80000000; \
    uint r01 = (f3 & 1) ? a0 << s : ((reg && alt) ? a0 - y : a0 + y), r23 = (a0 ^ turn) < (y ^ turn) ? 1 : 0; \
    uint r45 = (f3 & 1) ? (alt ? asuint(asint(a0) >> s) : a0 >> s) : a0 ^ y, r67 = (f3 & 1) ? a0 & y : a0 | y; \
    v0 = (f3 & 4) ? ((f3 & 2) ? r67 : r45) : ((f3 & 2) ? r23 : r01);
#define CTRL(v, nx, dst) { uint opc_ = w & 0x7f, f3_ = (w >> 12) & 7; uint a_ = m[(w >> 15) & 31], b_ = m[(w >> 20) & 31]; \
    uint u_ = (w & 0xfffff000) + (opc_ == 0x17 ? pc : 0); \
    bool isbr_ = opc_ == 0x63, isjal_ = opc_ == 0x6f, isjalr_ = opc_ == 0x67; \
    bool lt_ = (f3_ & 2) ? a_ < b_ : asint(a_) < asint(b_); \
    bool tk_ = isbr_ && (((f3_ & 4) ? lt_ : a_ == b_) != ((f3_ & 1) != 0)); \
    uint ib_ = asuint((asint(w) >> 19) & ~0xfffu) | ((w << 4) & 0x800) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e); \
    uint ij_ = asuint((asint(w) >> 11) & ~0xfffffu) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe); \
    nx = tk_ ? pc + ib_ : isjal_ ? pc + ij_ : isjalr_ ? a_ + asuint(asint(w) >> 20) : pc + 4; \
    v = (isjal_ || isjalr_) ? pc + 4 : u_; \
    dst = isbr_ ? SINK : dst; }
static uint fl = 0, fh = 0;   // the filter
static bool res_on = false; static uint res_at = 0;   // (lr's reservation: not kept across passes yet; sc then fails, as it may)
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
// the slot of texel u, or EMPTY if it has no entry
uint lookup(inout uint m[2048], uint u) {
    uint i = HASH(u), got = EMPTY, k = 0;
#ifndef NOFILTER
    uint h6 = u & 63;
    [branch] if ((((h6 & 32) ? fh : fl) >> (h6 & 31)) & 1) {
#endif
    [loop] for (k = 0; k < PROBES; k++) {
        uint g = m[TAG0 + i];
        [branch] if (g == u) { got = i; break; }
        [branch] if (g == EMPTY) break;
        i = i + 1 == N_ENT ? 0 : i + 1;
    }
#ifndef NOFILTER
    }
#endif
    return got;
}
bool put(inout uint m[2048], uint va_, uint val, uint f3_) {   // a store into the write cache, false when it has no room
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
    uint m[2048];   // a local array: HLSL does not zero one, as it zeroed the static (2,018 stores at every pass in step 12: SASS)
    int3 me = int3(at.xy, 0);
    uint lin = (me.x + 8 * me.y) * 8;
    uint4 ctl = _S0.Load(int3(1, 0, 0));
    uint pc = ctl.x, n = 0, w, budget = ctl.z != 0 ? 0 : _Budget, reason = ctl.z, outs = 0, full = 0;
    uint4 t;
    { int3 a = int3(0, 0, 0); uint4 q; uint i;
      q = _S0.Load(a); m[0] = q.x; m[1] = q.y; m[2] = q.z; m[3] = q.w; q = _S1.Load(a); m[4] = q.x; m[5] = q.y; m[6] = q.z; m[7] = q.w;
      q = _S2.Load(a); m[8] = q.x; m[9] = q.y; m[10] = q.z; m[11] = q.w; q = _S3.Load(a); m[12] = q.x; m[13] = q.y; m[14] = q.z; m[15] = q.w;
      q = _S4.Load(a); m[16] = q.x; m[17] = q.y; m[18] = q.z; m[19] = q.w; q = _S5.Load(a); m[20] = q.x; m[21] = q.y; m[22] = q.z; m[23] = q.w;
      q = _S6.Load(a); m[24] = q.x; m[25] = q.y; m[26] = q.z; m[27] = q.w; q = _S7.Load(a); m[28] = q.x; m[29] = q.y; m[30] = q.z; m[31] = q.w;
      [loop] for (i = 0; i < N_ENT; i++) m[TAG0 + i] = EMPTY;
    }
    [loop] while (n < budget) {
        FETCH(pc);
        [loop] while (n < budget && (w & 0x5f) == 0x13 && (w & 0xfe00007f) != 0x02000033) {
            ALUV;
            m[dst] = v0;
            pc += 4; n++;
            FETCH(pc);
        }
        [branch] if (n < budget) {
            uint op = w & 0x7f;
            outs++;
            [branch] if (op == 0x03) {
                uint f3_ = (w >> 12) & 7, rd_ = (w >> 7) & 31, va_ = m[(w >> 15) & 31] + asuint(asint(w) >> 20), u_ = TEXEL_OF(va_), ws_ = (va_ >> 2) & 3;
                [branch] if (va_ - RAM_LO >= RAM_HI - RAM_LO || (va_ & 3) + (1u << (f3_ & 3)) > 4 || (f3_ & 3) == 3 || f3_ > 5) { reason = 2; budget = n; }
                else {
                    uint sl = lookup(m, u_), z0;
                    [branch] if (sl != EMPTY) z0 = m[DATA0 + 4 * sl + ws_]; else { uint4 q_ = RAM(u_); z0 = q_[ws_]; }
                    uint up_ = 24 - 8 * (f3_ & 3) - ((f3_ & 3) == 2 ? 8 : 0), z_ = z0 >> ((va_ & 3) * 8) << up_;
                    m[rd_ == 0 ? SINK : rd_] = (f3_ & 4) ? z_ >> up_ : asuint(asint(z_) >> up_);
                    pc += 4; n++;
                }
            } else [branch] if (op == 0x63 || op == 0x6f || op == 0x67 || op == 0x37 || op == 0x17) {
                uint rd = (w >> 7) & 31, dst = rd == 0 ? SINK : rd, vc, nx; CTRL(vc, nx, dst);
                m[dst] = vc; pc = nx; n++;
            } else [branch] if (op == 0x23) {
                uint f3_ = (w >> 12) & 7, va_ = m[(w >> 15) & 31] + (asuint(asint(w) >> 25 << 5) | ((w >> 7) & 31));
                [branch] if (va_ - RAM_LO >= RAM_HI - RAM_LO || (va_ & 3) + (1u << f3_) > 4 || f3_ > 2) { reason = 3; budget = n; }
                else {
                    bool ok = put(m, va_, m[(w >> 20) & 31], f3_);
                    [branch] if (ok) { pc += 4; n++; } else { budget = n; full = 1; }
                }
            } else [branch] if (op == 0x33 && (w >> 25) == 1) {
                uint f3_ = (w >> 12) & 7, rd_ = (w >> 7) & 31, a_ = m[(w >> 15) & 31], b_ = m[(w >> 20) & 31];
                m[rd_ == 0 ? SINK : rd_] = op_m(f3_, a_, b_); pc += 4; n++;
            } else [branch] if (op == 0x2f && ((w >> 12) & 7) == 2) {
                uint f5 = w >> 27, rd_ = (w >> 7) & 31, a_ = m[(w >> 15) & 31], b_ = m[(w >> 20) & 31];
                [branch] if (a_ - RAM_LO >= RAM_HI - RAM_LO || (a_ & 3) != 0 || f5 > 12 || ((0x111fu >> f5) & 1) == 0) { reason = 4; budget = n; }
                else {
                    uint old = 0, sl = lookup(m, TEXEL_OF(a_));
                    [branch] if (sl != EMPTY) old = m[DATA0 + 4 * sl + ((a_ >> 2) & 3)]; else { uint4 q_ = RAM(TEXEL_OF(a_)); old = q_[(a_ >> 2) & 3]; }
                    bool sc_ok = res_on && res_at == a_, ok = true;
                    [branch] if (f5 != 2 && (f5 != 3 || sc_ok)) ok = put(m, a_, f5 == 0 ? old + b_ : f5 == 4 ? old ^ b_ : f5 == 8 ? old | b_ : f5 == 12 ? old & b_ : b_, 2);
                    [branch] if (ok) {
                        m[rd_ == 0 ? SINK : rd_] = f5 == 3 ? (sc_ok ? 0 : 1) : old;
                        res_on = f5 == 2; res_at = a_;
                        pc += 4; n++;
                    } else { budget = n; full = 1; }
                }
            } else [branch] if (op == 0x0f) { pc += 4; n++; }
            else [branch] if ((w & 0xfffff07f) == 0xc0002073 || (w & 0xfffff07f) == 0xc8102073 || (w & 0xfffff07f) == 0xc0102073) {   // rdtime, rdtimeh, read as 0 (the model does the same)
                uint rd_ = (w >> 7) & 31; m[rd_ == 0 ? SINK : rd_] = 0; pc += 4; n++;
            } else { reason = 1; budget = n; }
        }
    }
    m[32] = pc; m[33] = ctl.y + n; m[34] = reason; m[35] = reason ? w : 0; m[36] = outs; m[37] = full; m[38] = 0; m[39] = 0;   // texel 8 is the sink's and these: the bench reads pixel (1,0)'s first texel
    Out r;
    uint e = 4 * lin;
    r.o0 = uint4(m[e], m[e + 1], m[e + 2], m[e + 3]); r.o1 = uint4(m[e + 4], m[e + 5], m[e + 6], m[e + 7]);
    r.o2 = uint4(m[e + 8], m[e + 9], m[e + 10], m[e + 11]); r.o3 = uint4(m[e + 12], m[e + 13], m[e + 14], m[e + 15]);
    r.o4 = uint4(m[e + 16], m[e + 17], m[e + 18], m[e + 19]); r.o5 = uint4(m[e + 20], m[e + 21], m[e + 22], m[e + 23]);
    r.o6 = uint4(m[e + 24], m[e + 25], m[e + 26], m[e + 27]); r.o7 = uint4(m[e + 28], m[e + 29], m[e + 30], m[e + 31]);
    return r;
}
