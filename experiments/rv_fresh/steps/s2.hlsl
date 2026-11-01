// Step 2: registers, one op. A: a fetch every instruction, RV's add by its fields: x[rd] = x[rs1] + x[rs2], nothing asked.
// KEEP: the texel kept, a fetch only when pc enters a new one.  NOREG: no register file (the fields decoded, the sum in a temporary)
#include "pre.hlsl"
Out frag(float4 at : SV_Position) {
    int3 me = int3(at.xy, 0);
    uint4 ctl = _S0.Load(int3(1, 0, 0));
    uint pc = ctl.x, n = 0, acc = 0;
    regs_in();
    uint4 cur = 0;
#ifdef KEEP
    cur = RAM(pc >> 4); cur = (pc & 8) ? ((pc & 4) ? cur.wxyz : cur.zwxy) : ((pc & 4) ? cur.yzwx : cur);
#endif
    [loop] while (n < _Budget) {
#ifdef KEEP
        [branch] if ((pc & 12) == 0) cur = RAM(pc >> 4);
        uint w = cur.x; cur = cur.yzwx;
#else
        uint4 t = RAM(pc >> 4);
        uint w = t[(pc >> 2) & 3];
#endif
#ifdef NOREG
        acc += ((w >> 15) & 31) + ((w >> 20) & 31) + ((w >> 7) & 31);
#else
        uint rd = (w >> 7) & 31;
        x[rd] = x[(w >> 15) & 31] + x[(w >> 20) & 31];
#endif
        pc = (pc + 4) & 0xfff;
        n++;
    }
    x[3] ^= acc;
    return state_out(me, pc, ctl.y + n);
}
