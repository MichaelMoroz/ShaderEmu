// Step 5: the way out of the hot loop. s4's inner loop cost 43 ns over a plain one: is it the `if (...) break`, or the nesting?
// MODE 0 one loop, no way out. 1 one loop, `if (jump) break`. 2 one loop, the next word fetched at a trip's end and asked in the loop's own
// condition (gpu_vm's shape). 3 that inside an outer loop that does the jump. 4 `if break` inside an outer loop that does the jump (s4).
#include "pre.hlsl"
#ifndef MODE
#define MODE 0
#endif
#define FETCH(pc) t = RAM((pc) >> 4); w = t[((pc) >> 2) & 3]
#define ALU uint f3 = (w >> 12) & 7, rd = (w >> 7) & 31, a0 = x[(w >> 15) & 31], b0 = x[(w >> 20) & 31], v0; \
    bool reg = (w & 0x20) != 0, alt = (w & 0x40000000) != 0; \
    uint y = reg ? b0 : asuint(asint(w) >> 20), s = y & 31, turn = (f3 & 1) ? 0 : 0x80000000; \
    uint r01 = (f3 & 1) ? a0 << s : ((reg && alt) ? a0 - y : a0 + y), r23 = (a0 ^ turn) < (y ^ turn) ? 1 : 0; \
    uint r45 = (f3 & 1) ? (alt ? asuint(asint(a0) >> s) : a0 >> s) : a0 ^ y, r67 = (f3 & 1) ? a0 & y : a0 | y; \
    v0 = (f3 & 4) ? ((f3 & 2) ? r67 : r45) : ((f3 & 2) ? r23 : r01); \
    x[rd] = v0; pc = pc + 4; n++;
Out frag(float4 at : SV_Position) {
    int3 me = int3(at.xy, 0);
    uint4 ctl = _S0.Load(int3(1, 0, 0));
    uint pc = ctl.x, n = 0, w;
    uint4 t;
    regs_in();
#if MODE == 0
    [loop] while (n < _Budget) { FETCH(pc); ALU; pc &= 0xfff; }
#elif MODE == 1
    [loop] while (n < _Budget) { FETCH(pc); [branch] if ((w & 0x7f) == 0x6f) break; ALU; }
#elif MODE == 2
    FETCH(pc);
    [loop] while (n < _Budget && (w & 0x7f) != 0x6f) { ALU; FETCH(pc); }
#elif MODE == 3
    [loop] while (n < _Budget) {
        FETCH(pc);
        [loop] while (n < _Budget && (w & 0x7f) != 0x6f) { ALU; FETCH(pc); }
        [branch] if ((w & 0x7f) == 0x6f) { pc = 4; n++; }
    }
#else
    [loop] while (n < _Budget) {
        [loop] while (n < _Budget) { FETCH(pc); [branch] if ((w & 0x7f) == 0x6f) break; ALU; }
        [branch] if ((w & 0x7f) == 0x6f && n < _Budget) { pc = 4; n++; }
    }
#endif
    return state_out(me, pc, ctl.y + n);
}
