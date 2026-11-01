// Step 3 went from 42 to 150 ns with arithmetic's few lines: which of them? From step 2's loop, a piece at a time (LEVEL).
#include "pre.hlsl"
#ifndef LEVEL
#define LEVEL 0
#endif
Out frag(float4 at : SV_Position) {
    int3 me = int3(at.xy, 0);
    uint4 ctl = _S0.Load(int3(1, 0, 0));
    uint pc = ctl.x, n = 0, acc = 0, acc2 = 0;
    regs_in();
    [loop] while (n < _Budget) {
        uint4 t = RAM(pc >> 4);
        uint w = t[(pc >> 2) & 3];
        uint f3 = (w >> 12) & 7, rd = (w >> 7) & 31;
#if defined(NOARR) || defined(NORD)   // no reads of the register file
        uint a = acc ^ ((w >> 15) & 31), b = acc2 ^ ((w >> 20) & 31), next = pc + 4, v;
#else
        uint a = x[(w >> 15) & 31], b = x[(w >> 20) & 31], next = pc + 4, v;
#endif
#if LEVEL == 0     // the sum
        v = a + b;
#elif LEVEL == 1   // the second operand chosen
        bool reg = (w & 0x20) != 0;
        uint y = reg ? b : asuint(asint(w) >> 20);
        v = a + y;
#elif LEVEL == 2   // sum, xor, or, and by bits of f3
        bool reg = (w & 0x20) != 0;
        uint y = reg ? b : asuint(asint(w) >> 20);
        uint r0 = a + y, r4 = a ^ y, r6 = (f3 & 1) ? a & y : a | y;
        v = (f3 & 4) ? ((f3 & 2) ? r6 : r4) : r0;
#elif LEVEL == 3   // and shifts
        bool reg = (w & 0x20) != 0, alt = (w & 0x40000000) != 0;
        uint y = reg ? b : asuint(asint(w) >> 20), s = y & 31;
        uint r0 = (f3 & 1) ? a << s : a + y, r4 = (f3 & 1) ? (alt ? asuint(asint(a) >> s) : a >> s) : a ^ y, r6 = (f3 & 1) ? a & y : a | y;
        v = (f3 & 4) ? ((f3 & 2) ? r6 : r4) : r0;
#elif LEVEL >= 5 && LEVEL <= 9   // one piece of level 4 on level 3 at a time
        bool reg = (w & 0x20) != 0, alt = (w & 0x40000000) != 0;
        uint y = reg ? b : asuint(asint(w) >> 20), s = y & 31;
#if LEVEL == 5
        uint plus = (reg && alt) ? a - y : a + y;
#elif LEVEL == 6
        uint plus = alt ? a - y : a + y;
#else
        uint plus = a + y;
#endif
        uint r0 = (f3 & 1) ? a << s : plus, r4 = (f3 & 1) ? (alt ? asuint(asint(a) >> s) : a >> s) : a ^ y, r6 = (f3 & 1) ? a & y : a | y;
#if LEVEL == 7
        uint r2 = a < y ? 1 : 0;
#elif LEVEL == 8
        uint turn = (f3 & 1) ? 0 : 0x80000000, r2 = (a ^ turn) < (y ^ turn) ? 1 : 0;
#elif LEVEL == 9
        uint turn = (f3 & 1) ? 0 : 0x80000000, p = a ^ turn, q = y ^ turn, r2 = (((~p) & q) | (((~p) | q) & (p - q))) >> 31;
#else
        uint r2 = 0;
#endif
        v = (f3 & 4) ? ((f3 & 2) ? r6 : r4) : ((f3 & 2) ? r2 : r0);
#elif LEVEL == 10  // 5 and 8 together in level 3's form
        bool reg = (w & 0x20) != 0, alt = (w & 0x40000000) != 0;
        uint y = reg ? b : asuint(asint(w) >> 20), s = y & 31;
        uint plus = (reg && alt) ? a - y : a + y;
        uint r0 = (f3 & 1) ? a << s : plus, r4 = (f3 & 1) ? (alt ? asuint(asint(a) >> s) : a >> s) : a ^ y, r6 = (f3 & 1) ? a & y : a | y;
        uint turn = (f3 & 1) ? 0 : 0x80000000, r2 = (a ^ turn) < (y ^ turn) ? 1 : 0;
        v = (f3 & 4) ? ((f3 & 2) ? r6 : r4) : ((f3 & 2) ? r2 : r0);
#else              // and sub, the comparisons: all of it
        bool reg = (w & 0x20) != 0, alt = (w & 0x40000000) != 0;
        uint y = reg ? b : asuint(asint(w) >> 20), s = y & 31, turn = (f3 & 1) ? 0 : 0x80000000;
        uint r01 = (f3 & 1) ? a << s : ((reg && alt) ? a - y : a + y), r23 = (a ^ turn) < (y ^ turn) ? 1 : 0;
        uint r45 = (f3 & 1) ? (alt ? asuint(asint(a) >> s) : a >> s) : a ^ y, r67 = (f3 & 1) ? a & y : a | y;
        v = (f3 & 4) ? ((f3 & 2) ? r67 : r45) : ((f3 & 2) ? r23 : r01);
#endif
#ifdef PAD   // PAD groups of 8 plain operations that nothing can remove: is it how long the loop is?
#define P1 v = (v ^ w) + 7; v = (v >> 1) ^ a; v = (v | b) + w; v = (v & w) ^ 5;
#define P4 P1 P1 P1 P1
#ifdef SKIP   // the same code in the loop but never run: is it what runs, or what is there?
        [branch] if (w == 0xdeadbeef) {
#endif
#if PAD & 1
        P1
#endif
#if PAD & 2
        P1 P1
#endif
#if PAD & 4
        P4
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
#ifdef SKIP
        }
#endif
#endif
#if defined(NOARR) || defined(NOWR)
        acc2 = acc; acc = v + rd;
#else
        x[rd] = v;
#endif
        pc = next & 0xfff;
        n++;
    }
    x[5] = acc + acc2;
#ifdef AFTER   // the same code after the loop, one trip: is it the shader's size?
    uint w = pc, a = x[1], b = x[2], v = n;
    [branch] if (x[3] == 0xdeadbeef) {
#define P1 v = (v ^ w) + 7; v = (v >> 1) ^ a; v = (v | b) + w; v = (v & w) ^ 5;
#define P4 P1 P1 P1 P1
        P4 P4 P4 P4 P4 P4 P4 P4 P4 P4 P4 P4 P4 P4 P4 P4
        x[6] = v;
    }
#endif
    return state_out(me, pc, ctl.y + n);
}
