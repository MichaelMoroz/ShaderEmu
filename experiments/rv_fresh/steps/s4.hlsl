// Step 4: the register file is cheap in a small loop and dear in a large one (NOTES.md, "The cliff"). Can the hot loop stay small
// with the rest of a tick round it? An inner loop of plain arithmetic, the rest in the outer loop (PAD groups of 8 operations, using the array too).
// SLOW: every instruction goes out to the outer loop, to price the way out and back. FLAT: no inner loop, the same code all in one.
#include "pre.hlsl"
#ifndef PAD
#define PAD 0
#endif
#define P1 v = (v ^ w) + 7; v = (v >> 1) ^ a; v = (v | b) + w; v = (v & w) ^ 5;
#define P4 P1 P1 P1 P1
Out frag(float4 at : SV_Position) {
    int3 me = int3(at.xy, 0);
    uint4 ctl = _S0.Load(int3(1, 0, 0));
    uint pc = ctl.x, n = 0;
    regs_in();
    [loop] while (n < _Budget) {
        uint w = 0;
#ifndef FLAT
        [loop] while (n < _Budget) {
#endif
            uint4 t = RAM(pc >> 4);
            w = t[(pc >> 2) & 3];
#ifdef SLOW
            [branch] if (n != 0xfffffff0) break;
#else
            [branch] if ((w & 0x7f) == 0x6f) break;   // (a jump: the one thing the test programs have but arithmetic)
#endif
            uint f3 = (w >> 12) & 7, rd = (w >> 7) & 31, a0 = x[(w >> 15) & 31], b0 = x[(w >> 20) & 31], v0;
            bool reg = (w & 0x20) != 0, alt = (w & 0x40000000) != 0;
            uint y = reg ? b0 : asuint(asint(w) >> 20), s = y & 31, turn = (f3 & 1) ? 0 : 0x80000000;
            uint r01 = (f3 & 1) ? a0 << s : ((reg && alt) ? a0 - y : a0 + y), r23 = (a0 ^ turn) < (y ^ turn) ? 1 : 0;
            uint r45 = (f3 & 1) ? (alt ? asuint(asint(a0) >> s) : a0 >> s) : a0 ^ y, r67 = (f3 & 1) ? a0 & y : a0 | y;
            v0 = (f3 & 4) ? ((f3 & 2) ? r67 : r45) : ((f3 & 2) ? r23 : r01);
            x[rd] = v0;
            pc = (pc + 4) & 0xfff;
            n++;
#ifndef FLAT
        }
#endif
        [branch] if (n >= _Budget) break;
#ifdef FLAT
        [branch] if (w == 0xdeadbeef) {
#else
        [branch] if ((w & 0x7f) == 0x6f) {
#endif
            uint a = x[(w >> 15) & 31], b = x[(w >> 20) & 31], v = pc;
#if PAD & 1
            P1
#endif
#if PAD & 8
            P4 P4
#endif
#if PAD & 16
            P4 P4 P4 P4
#endif
#if PAD & 32
            P4 P4 P4 P4 P4 P4 P4 P4
#endif
            x[1] = v;
            pc = 0; n++;
        }
#ifdef SLOW
        [branch] if (w != 0xdeadbeef) {
            uint f3 = (w >> 12) & 7, rd = (w >> 7) & 31, a = x[(w >> 15) & 31], b = x[(w >> 20) & 31];
            x[rd] = a + b; pc = (pc + 4) & 0xfff; n++;
        }
#endif
    }
    return state_out(me, pc, ctl.y + n);
}
