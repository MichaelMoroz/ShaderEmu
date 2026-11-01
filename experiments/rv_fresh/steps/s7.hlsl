// Step 7: a load (no stores yet, so RAM alone). The hot loop is arithmetic; control is a way out (step 6).
// (nothing): the load in the outer loop, after the arithmetic loop.  LDLOOP: a loop of its own for loads in a row beside the arithmetic one.
// LDIN: the load on the hot path by a [branch] inside the arithmetic loop.
#include "pre.hlsl"
#define FETCH(pc) t = RAM((pc) >> 4); w = t[((pc) >> 2) & 3]
#define ALUV uint f3 = (w >> 12) & 7, rd = (w >> 7) & 31, a0 = x[(w >> 15) & 31], b0 = x[(w >> 20) & 31], dst = rd == 0 ? 32 : rd, v0; \
    bool reg = (w & 0x20) != 0, alt = (w & 0x40000000) != 0; \
    uint y = reg ? b0 : asuint(asint(w) >> 20), s = y & 31, turn = (f3 & 1) ? 0 : 0x80000000; \
    uint r01 = (f3 & 1) ? a0 << s : ((reg && alt) ? a0 - y : a0 + y), r23 = (a0 ^ turn) < (y ^ turn) ? 1 : 0; \
    uint r45 = (f3 & 1) ? (alt ? asuint(asint(a0) >> s) : a0 >> s) : a0 ^ y, r67 = (f3 & 1) ? a0 & y : a0 | y; \
    v0 = (f3 & 4) ? ((f3 & 2) ? r67 : r45) : ((f3 & 2) ? r23 : r01);
#define CTRL(v, nx, dst) { uint opc_ = w & 0x7f, f3_ = (w >> 12) & 7; uint a_ = x[(w >> 15) & 31], b_ = x[(w >> 20) & 31]; \
    uint u_ = (w & 0xfffff000) + (opc_ == 0x17 ? pc : 0); \
    bool isbr_ = opc_ == 0x63, isjal_ = opc_ == 0x6f, isjalr_ = opc_ == 0x67; \
    bool lt_ = (f3_ & 2) ? a_ < b_ : asint(a_) < asint(b_); \
    bool tk_ = isbr_ && (((f3_ & 4) ? lt_ : a_ == b_) != ((f3_ & 1) != 0)); \
    uint ib_ = asuint((asint(w) >> 19) & ~0xfffu) | ((w << 4) & 0x800) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e); \
    uint ij_ = asuint((asint(w) >> 11) & ~0xfffffu) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe); \
    nx = tk_ ? pc + ib_ : isjal_ ? pc + ij_ : isjalr_ ? a_ + asuint(asint(w) >> 20) : pc + 4; \
    v = (isjal_ || isjalr_) ? pc + 4 : u_; \
    dst = isbr_ ? 32 : dst; }
// within one word of RAM; lb lh lw lbu lhu by f3. (An unaligned one is not done yet.)
#define LOAD { uint f3_ = (w >> 12) & 7, rd_ = (w >> 7) & 31; uint va_ = x[(w >> 15) & 31] + asuint(asint(w) >> 20); \
    uint4 m_ = RAM(va_ >> 4); uint up_ = 24 - 8 * (f3_ & 3) - ((f3_ & 3) == 2 ? 8 : 0); \
    uint z_ = m_[(va_ >> 2) & 3] >> ((va_ & 3) * 8) << up_; \
    x[rd_ == 0 ? 32 : rd_] = (f3_ & 4) ? z_ >> up_ : asuint(asint(z_) >> up_); pc += 4; n++; }
Out frag(float4 at : SV_Position) {
    int3 me = int3(at.xy, 0);
    uint4 ctl = _S0.Load(int3(1, 0, 0));
    uint pc = ctl.x, n = 0, w;
    uint4 t;
    regs_in();
    [loop] while (n < _Budget) {
        FETCH(pc);
#ifdef LDIN
        [loop] while (n < _Budget && ((w & 0x5f) == 0x13 || (w & 0x7f) == 0x03)) {
            [branch] if ((w & 0x7f) == 0x03) LOAD
            else { ALUV; x[dst] = v0; pc += 4; n++; }
            FETCH(pc);
        }
#else
        [loop] while (n < _Budget && (w & 0x5f) == 0x13) {
            ALUV;
            x[dst] = v0;
            pc += 4; n++;
            FETCH(pc);
        }
#ifdef LDLOOP
        [loop] while (n < _Budget && (w & 0x7f) == 0x03) { LOAD; FETCH(pc); }
#endif
#endif
        [branch] if (n < _Budget) {
#ifndef LDIN
#ifndef LDLOOP
            [branch] if ((w & 0x7f) == 0x03) LOAD
            else
#endif
#endif
            [branch] if ((w & 0x5f) != 0x13 && (w & 0x7f) != 0x03) {
                uint rd = (w >> 7) & 31, dst = rd == 0 ? 32 : rd, vc, nx; CTRL(vc, nx, dst);
                x[dst] = vc; pc = nx; n++;
            }
        }
    }
    return state_out(me, pc, ctl.y + n);
}
