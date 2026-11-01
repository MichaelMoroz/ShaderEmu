// Step 3: all of RV32I's arithmetic and control. A fetch every instruction (step 2: keeping the texel was slower than the fetch).
// TREE: a branch between arithmetic and control. Without it, one path: every result made and one chosen, control too.
// NOSPLIT8: arithmetic's eight results chosen by comparisons of f3 instead of by its bits (nothing else)
#include "pre.hlsl"
Out frag(float4 at : SV_Position) {
    int3 me = int3(at.xy, 0);
    uint4 ctl = _S0.Load(int3(1, 0, 0));
    uint pc = ctl.x, n = 0;
    regs_in();
    [loop] while (n < _Budget) {
        uint4 t = RAM(pc >> 4);
        uint w = t[(pc >> 2) & 3];
        uint opc = w & 0x7f, f3 = (w >> 12) & 7, rd = (w >> 7) & 31, dst = rd == 0 ? 32 : rd;
        uint a = x[(w >> 15) & 31], b = x[(w >> 20) & 31], next = pc + 4, v;
        bool isalu = (w & 0x5f) == 0x13;
#ifdef TREE
        [branch] if (isalu) {
#endif
        bool reg = (w & 0x20) != 0, alt = (w & 0x40000000) != 0;
        uint y = reg ? b : asuint(asint(w) >> 20), s = y & 31, turn = (f3 & 1) ? 0 : 0x80000000;
        uint r01 = (f3 & 1) ? a << s : ((reg && alt) ? a - y : a + y), r23 = (a ^ turn) < (y ^ turn) ? 1 : 0;
        uint r45 = (f3 & 1) ? (alt ? asuint(asint(a) >> s) : a >> s) : a ^ y, r67 = (f3 & 1) ? a & y : a | y;
        v = (f3 & 4) ? ((f3 & 2) ? r67 : r45) : ((f3 & 2) ? r23 : r01);
#ifdef TREE
        } else {
            v = 0;
#else
        {
#endif
            uint u = (w & 0xfffff000) + (opc == 0x17 ? pc : 0);
            bool isbr = opc == 0x63, isjal = opc == 0x6f, isjalr = opc == 0x67;
            bool lt = (f3 & 2) ? a < b : asint(a) < asint(b);
            bool tk = isbr && (((f3 & 4) ? lt : a == b) != ((f3 & 1) != 0));
            uint ib = asuint((asint(w) >> 19) & ~0xfffu) | ((w << 4) & 0x800) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e);
            uint ij = asuint((asint(w) >> 11) & ~0xfffffu) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe);
            next = tk ? pc + ib : isjal ? pc + ij : isjalr ? a + asuint(asint(w) >> 20) : next;
            v = (isjal || isjalr) ? pc + 4 : isalu ? v : u;
            dst = isbr ? 32 : dst;
        }
        x[dst] = v;
        pc = next;
        n++;
    }
    return state_out(me, pc, ctl.y + n);
}
