// Step 6: control with step 5's loop (the next word asked in the loop's condition).
// IN: control on the hot path, every result made and one chosen (a jump or branch stays in the loop).
// else: control in the outer loop: the hot loop is arithmetic alone, each control instruction a way out and back.
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
Out frag(float4 at : SV_Position) {
    int3 me = int3(at.xy, 0);
    uint4 ctl = _S0.Load(int3(1, 0, 0));
    uint pc = ctl.x, n = 0, w;
    uint4 t;
    regs_in();
#ifdef IN
    FETCH(pc);
    #define KNOWN(w) ((w & 0x5f) == 0x13 || (w & 0x5f) == 0x17 || (w & 0x7f) == 0x63 || (w & 0x7f) == 0x6f || (w & 0x7f) == 0x67)
    [loop] while (n < _Budget && KNOWN(w)) {
        ALUV;
        uint vc, nx; CTRL(vc, nx, dst);
        x[dst] = (w & 0x5f) == 0x13 ? v0 : vc;
        pc = nx; n++;
        FETCH(pc);
    }
#else
    [loop] while (n < _Budget) {
        FETCH(pc);
        [loop] while (n < _Budget && (w & 0x5f) == 0x13) {
            ALUV;
            x[dst] = v0;
            pc += 4; n++;
            FETCH(pc);
        }
        [branch] if (n < _Budget) {
            uint rd = (w >> 7) & 31, dst = rd == 0 ? 32 : rd, vc, nx; CTRL(vc, nx, dst);
            x[dst] = vc; pc = nx; n++;
        }
    }
#endif
    return state_out(me, pc, ctl.y + n);
}
