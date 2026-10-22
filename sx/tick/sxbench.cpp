// sxbench: SX's tick (sx/tick/tick.hlsl) on D3D11, every case checked against the reference machine
// (sx/ref/sx_ref.h), and the loops timed by the GPU's clock.
//
//   sxbench [--warp] [--case NAME[,NAME]] [--ticks N] [--passes N] [--seed N] [-D NAME=VALUE]...
//
// A case is a program for each of its cores, assembled with sx/ref/sx_asm.h at SX_RAM, and data. Both
// machines run it in passes of --ticks bundles (a check runs it in short passes too, so that passes end
// inside bundles and on a full write cache) until every core has halted (break), and then registers, pc
// and every word of RAM must agree. A case marked timed is timed first: --passes passes of 8,192 bundles.
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#define SX_REF_IMPL
#include "../ref/sx_ref.h"
#include "../ref/sx_asm.h"

// ---- The bench's machine: RAM to 48 MB (16 MB of it RAM proper), 64 KB of ROM
static const uint32_t RAM_ROWS = 1536, RAM_END = RAM_ROWS * 2048 * 16, ROM_ROWS = 2, ROM_SIZE = ROM_ROWS * 2048 * 16;
static const uint32_t DATA = SX_RAM + 0x100000;   // a case's data
static uint32_t g_wcn = 128;

struct Case {
    const char* name;
    uint32_t cores;
    bool timed;
    std::function<void(std::vector<SxAsm>&, std::vector<std::pair<uint32_t, uint32_t>>&, uint32_t seed)> build;
    double ops_per_iter = 0;   // for a timed case: SX ops an iteration of its loop
    double rv_per_iter = 0;    // and the RISC-V instructions isaloops.c spends on the same iteration
};

// ---- Random programs: the fuzzer's ops (registers r5-r28 carry values; r29 a base for memory)
struct Rng { uint64_t s; uint32_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (uint32_t)s; } uint32_t below(uint32_t n) { return next() % n; } };
static uint32_t rreg(Rng& g) { return 5 + g.below(24); }
static int32_t rimm(Rng& g) {
    switch (g.below(5)) {
    case 0: return (int32_t)g.below(64) - 32;
    case 1: return (int32_t)g.below(4095) - 2047;
    case 2: return (int32_t)g.next();
    case 3: return 1 << g.below(32);
    default: return (int32_t)g.below(33);
    }
}
static uint32_t rval(Rng& g) {
    static const uint32_t edge[] = {0, 1, 2, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0xfffffffeu, 31, 32, 0x80000001u};
    return g.below(3) == 0 ? edge[g.below(10)] : g.next();
}
static uint32_t rfloat(Rng& g) {
    static const uint32_t edge[] = {0, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0x00000001u,
                                    0x80400000u, 0x4f000000u, 0x4f800000u, 0xcf000000u, 0x3f000000u, 0x7f7fffffu, 0x00800000u, 0x3fc00000u};
    switch (g.below(4)) {
    case 0: return edge[g.below(16)];
    case 1: { float f = (float)((int32_t)g.below(2001) - 1000) / 8.0f; uint32_t u; memcpy(&u, &f, 4); return u; }
    case 2: { float f = (float)((int32_t)g.next()) * 1e-3f; uint32_t u; memcpy(&u, &f, 4); return u; }
    default: return (g.next() & 0x8fffffffu) | 0x30000000u;   // within a few hundred powers of two of 1
    }
}
static void seed_regs(SxAsm& a, Rng& g, bool floats) {
    for (uint32_t k = 5; k <= 28; k++) a.li(k, (int32_t)(floats && (k & 1) ? rfloat(g) : rval(g)));
}
// the integer ops, every one of them
static void fuzz_int(SxAsm& a, Rng& g, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t rd = rreg(g), ra = rreg(g), rb = rreg(g), rc = rreg(g);
        switch (g.below(7)) {
        case 0: case 1: a.op(g.below(SX_I_COMMON_LAST + 1), rd, ra, rb); break;
        case 2: a.opi(g.below(SX_I_COMMON_LAST + 1), rd, ra, rimm(g)); break;
        case 3: {
            uint32_t f = SX_I_SHADD + g.below(6);
            a.R(SX_OP_I(f), rd, ra, rb, f == SX_I_SHADD ? g.below(64) : rc);
            break;
        }
        case 4: a.bfe(rd, ra, g.below(32), 1 + g.below(32), g.below(2) != 0); break;
        case 5: a.x(SX_X_MULH + g.below(SX_X_BREV - SX_X_MULH + 1), rd, ra, rb); break;
        default: a.xi(SX_X_BFI, rd, ra, (int32_t)(g.below(32) | g.below(32) << 5)); break;
        }
    }
}
// the floats a check can hold to the bit (the special functions and division are the card's own)
static void fuzz_float(SxAsm& a, Rng& g, int n) {
    static const uint32_t exact[] = {SX_F_ADD, SX_F_SUB, SX_F_MUL, SX_F_MIN, SX_F_MAX, SX_F_MAD, SX_F_MSB, SX_F_NMSB, SX_F_EQ, SX_F_LT,
                                     SX_F_LE, SX_F_SGNJ, SX_F_SGNJN, SX_F_SGNJX, SX_F_UNORD, SX_F_ITOF, SX_F_UTOF, SX_F_FTOI, SX_F_FTOU,
                                     SX_F_FLOOR, SX_F_CEIL, SX_F_TRUNC, SX_F_ROUND};
    for (int i = 0; i < n; i++) {
        uint32_t f = exact[g.below(sizeof exact / 4)], rd = rreg(g), ra = rreg(g), rb = rreg(g), rc = rreg(g);
        if (g.below(4) == 0 && (f < SX_F_MAD || f > SX_F_NMSB)) { uint32_t v = rfloat(g); float fv; memcpy(&fv, &v, 4); a.fopi(f, rd, ra, fv); }
        else a.fop(f, rd, ra, rb, rc);
    }
}
static void fuzz_mem(SxAsm& a, Rng& g, int n, uint32_t window) {   // r29 is the window's base
    for (int i = 0; i < n; i++) {
        uint32_t rd = rreg(g), wd = g.below(7), by = wd == SX_W_D ? 8 : wd == SX_W_Q ? 16 : (wd & 1) ? 2 : (wd == 2 ? 4 : 1);
        if (wd == SX_W_BU || wd == SX_W_HU) by = wd == SX_W_BU ? 1 : 2;
        uint32_t off = g.below(window - 16);
        if (g.below(3)) off &= ~(by - 1);   // mostly aligned: the fast path's
        if (by >= 8) rd = 5 + g.below(20);
        switch (g.below(5)) {
        case 0: case 1: a.ld(wd, rd, 29, (int32_t)off); break;
        case 2: case 3: if (wd <= SX_W_D || wd == SX_W_Q) a.st(wd == SX_W_D || wd == SX_W_Q ? wd : (wd & 3), rd, 29, (int32_t)off); else a.ld(wd, rd, 29, (int32_t)off); break;
        default: {   // an atomic on an aligned word
            a.li(30, (int32_t)(off & ~3u));
            a.add(30, 30, 29);
            a.amo(g.below(8), rd, 30, rreg(g));
        }
        }
    }
}

static std::vector<Case> cases() {
    std::vector<Case> v;
    v.push_back({"int", 1, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        Rng g{seed * 2654435761ull + 1};
        SxAsm& a = p[0];
        for (int round = 0; round < 40; round++) { seed_regs(a, g, false); fuzz_int(a, g, 60); }
        a.halt();
    }});
    v.push_back({"float", 1, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        Rng g{seed * 0x9e3779b97f4a7c15ull + 3};
        SxAsm& a = p[0];
        for (int round = 0; round < 40; round++) { seed_regs(a, g, true); fuzz_float(a, g, 60); }
        a.halt();
    }});
    v.push_back({"mem", 1, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        Rng g{seed * 0x2545F4914F6CDD1Dull + 5};
        for (uint32_t k = 0; k < 2048; k++) data.push_back({DATA + 4 * k, g.next()});
        SxAsm& a = p[0];
        a.li(29, (int32_t)DATA);
        for (int round = 0; round < 30; round++) { seed_regs(a, g, false); a.li(29, (int32_t)DATA); fuzz_mem(a, g, 50, 8192); }
        a.halt();
    }});
    v.push_back({"memw", 1, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        // loads and stores of bytes, halves and words, aligned, in a window, between sums (step x7 on)
        Rng g{seed * 31337 + 9};
        for (uint32_t k = 0; k < 1024; k++) data.push_back({DATA + 4 * k, g.next()});
        SxAsm& a = p[0];
        for (int round = 0; round < 30; round++) {
            seed_regs(a, g, false);
            a.li(29, (int32_t)DATA);
            for (int i = 0; i < 60; i++) {
                uint32_t wd = g.below(6), by = (wd & 3) == 0 ? 1 : (wd & 3) == 1 ? 2 : 4;
                if (wd == SX_W_D) { wd = SX_W_W; by = 4; }
                uint32_t off = g.below(4096 - 4) & ~(by - 1), rd = rreg(g);
                if (g.below(2)) a.ld(wd, rd, 29, (int32_t)off); else a.st(wd & 3, rd, 29, (int32_t)off);
                if (g.below(2)) a.addi(rreg(g), rreg(g), rimm(g));
            }
        }
        a.halt();
    }});
    v.push_back({"spillw", 1, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        // more distinct words stored than the write cache holds, words, halves and bytes (step x7 on)
        SxAsm& a = p[0];
        a.li(5, (int32_t)DATA); a.li(6, 0); a.li(7, 3000);
        int top = a.here();
        a.R(SX_OP_M(SX_M_STOREX, SX_W_W), 6, 5, 6, 2); a.addi(6, 6, 1);
        a.bne(6, 7, top);
        a.li(6, 0); a.li(8, 0);
        int sum = a.here();
        a.R(SX_OP_M(SX_M_LOADX, SX_W_W), 9, 5, 6, 2); a.addi(6, 6, 1);
        a.add(8, 8, 9);
        a.bne(6, 7, sum);
        a.li(6, 0); a.li(10, (int32_t)(DATA + 0x10000));
        int bytes = a.here();
        a.R(SX_OP_M(SX_M_STOREX, SX_W_B), 6, 10, 6, 0); a.addi(11, 6, 3);
        a.R(SX_OP_M(SX_M_STOREX, SX_W_H), 11, 10, 6, 2); a.addi(6, 6, 1);
        a.bne(6, 7, bytes);
        a.halt();
    }});
    v.push_back({"spill", 1, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        // more distinct words stored than the write cache holds: passes end on a full cache, and go on
        SxAsm& a = p[0];
        a.li(5, (int32_t)DATA); a.li(6, 0); a.li(7, 3000);
        int top = a.here();
        a.R(SX_OP_M(SX_M_STOREX, SX_W_W), 6, 5, 6, 2); a.addi(6, 6, 1);
        a.bne(6, 7, top);
        a.li(6, 0); a.li(8, 0);
        int sum = a.here();
        a.R(SX_OP_M(SX_M_LOADX, SX_W_W), 9, 5, 6, 2); a.add(8, 8, 9); a.addi(6, 6, 1);
        a.bne(6, 7, sum);
        // and bytes, halves and texels, as many again
        a.li(6, 0); a.li(10, (int32_t)(DATA + 0x10000));
        int bytes = a.here();
        a.R(SX_OP_M(SX_M_STOREX, SX_W_B), 6, 10, 6, 0); a.R(SX_OP_M(SX_M_STOREX, SX_W_H), 6, 10, 6, 2);
        a.st(SX_W_Q, 5, 10, 0x8000); a.addi(10, 10, 16); a.addi(6, 6, 1);
        a.bne(6, 7, bytes);
        a.halt();
    }});
    v.push_back({"conds", 1, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        // every condition, on random values, both ways, with sums alone (step x1 on): r20 counts what was taken
        Rng g{seed + 177};
        SxAsm& a = p[0];
        for (int i = 0; i < 300; i++) {
            a.li(5, (int32_t)rval(g)); a.li(6, (int32_t)(g.below(2) ? rval(g) : 0));
            if (g.below(2)) a.mv(6, 5);
            uint32_t cc = 1 + g.below(14);
            int skip = a.label();
            a.br(cc, 5, cc <= SX_CC_BGEU ? 6 : g.below(64), skip);
            a.addi(20, 20, 1); a.addi(21, 21, i);
            a.bind(skip);
            a.addi(22, 22, 3);
        }
        a.halt();
    }});
    v.push_back({"ints", 1, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        // the integer unit's 24 functions, R and I forms, on random values (step x2 on)
        Rng g{seed * 7919 + 11};
        SxAsm& a = p[0];
        for (int round = 0; round < 40; round++) {
            seed_regs(a, g, false);
            for (int i = 0; i < 60; i++) {
                if (g.below(2)) a.op(g.below(SX_I_COMMON_LAST + 1), rreg(g), rreg(g), rreg(g));
                else a.opi(g.below(SX_I_COMMON_LAST + 1), rreg(g), rreg(g), rimm(g));
            }
        }
        a.halt();
    }});
    v.push_back({"branch", 1, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        Rng g{seed + 77};
        SxAsm& a = p[0];
        // every condition, on random values, both ways: r20 counts what was taken
        for (int i = 0; i < 200; i++) {
            a.li(5, (int32_t)rval(g)); a.li(6, (int32_t)(g.below(2) ? rval(g) : 0));
            if (g.below(2)) a.mv(6, 5);
            uint32_t cc = 1 + g.below(14);
            int skip = a.label();
            uint32_t bfield = cc <= SX_CC_BGEU ? 6 : g.below(64);
            a.br(cc, 5, bfield, skip);
            a.addi(20, 20, 1);
            a.opi(SX_I_XOR, 21, 21, (int32_t)i);
            a.bind(skip);
            a.addi(22, 22, 3);
        }
        // calls and returns, a loop, a jump table through jr and callr
        int fn = a.label(), done = a.label(), after = a.label();
        a.li(7, 0); a.li(8, 25);
        int loop = a.here();
        a.call(fn);
        a.addi(7, 7, 1);
        a.bne(7, 8, loop);
        a.lpc(9, after);
        a.R(SX_OP_I(SX_I_ADD), 11, 9, 0);
        a.callr(9);
        a.j(done);
        a.bind(after);
        a.addi(23, 23, 100);
        a.ret();
        a.bind(fn);
        a.op(SX_I_ADD, 24, 24, 7); a.opi(SX_I_SLL, 25, 24, 1);
        a.ret();
        a.bind(done);
        a.halt();
    }});
    v.push_back({"trap", 1, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        // syscalls, an illegal op, a load fault, a store to the boot area, and the timer's interrupt; the
        // handler counts causes in r40-r47 and returns past the op (a syscall, a fault) or to it (an interrupt)
        SxAsm& a = p[0];
        int handler = a.label(), go = a.label();
        a.lpc(5, handler); a.mtsr(SX_SR_TVEC, 5);
        a.j(go);
        a.bind(handler);
        a.mfsr(50, SX_SR_CAUSE); a.mfsr(51, SX_SR_EPC); a.mfsr(52, SX_SR_TVAL);
        a.opi(SX_I_AND, 53, 50, 7); a.shadd(54, 0, 53, 0);   // r54 = cause & 7
        a.addi(40, 40, 1);
        int irq = a.label(), back = a.label();
        a.br(SX_CC_BBS, 50, 31, irq);
        a.addi(51, 51, 4); a.mtsr(SX_SR_EPC, 51);           // past the op
        a.op(SX_I_ADD, 41, 41, 54);
        a.j(back);
        a.bind(irq);
        a.li(55, 0); a.mtsr(SX_SR_TCMP_LO, 55); a.mtsr(SX_SR_TCMP_HI, 55);   // the timer off
        a.addi(42, 42, 1);
        a.bind(back);
        a.eret();
        a.bind(go);
        a.li(6, 1); a.x(SX_X_SYSCALL); a.addi(6, 6, 10);
        a.x(SX_X_SYSCALL);
        a.put(0x3fu << 0 | 0); a.addi(7, 7, 1);              // opc 0x3f: illegal
        a.li(8, 0x7000000); a.lw(9, 8, 0); a.addi(10, 10, 1); // a load past RAM's end
        a.li(8, 0x100); a.sw(9, 8, 0);                        // a store to the boot area
        a.li(11, 1); a.mtsr(SX_SR_TCMP_LO, 11); a.li(11, 2); a.mtsr(SX_SR_IEN, 11);
        a.li(11, SX_ST_IE | SX_ST_PIE); a.mtsr(SX_SR_STATUS, 11);    // the timer is due: taken at the next bundle
        a.addi(12, 12, 1); a.close();
        a.addi(12, 12, 1); a.close();
        a.halt();
    }});
    v.push_back({"ops", 1, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        // copy, fill and blit through the operation region, then a sum of the destination
        Rng g{seed + 991};
        for (uint32_t k = 0; k < 0x20000; k++) data.push_back({DATA + 4 * k, g.next()});
        SxAsm& a = p[0];
        a.li(20, (int32_t)SX_OPS);
        auto op = [&](uint32_t kind, uint32_t dst, uint32_t src, uint32_t n) {
            a.li(5, (int32_t)kind); a.li(6, (int32_t)dst); a.li(7, (int32_t)src); a.li(8, (int32_t)n);
            a.st(SX_W_Q, 5, 20, 0); a.x(SX_X_SYNC);
            a.lw(9, 20, 0); a.op(SX_I_ADD, 30, 30, 9);   // 0 through, ~0 refused
        };
        op(SX_OP_COPY, DATA + 0x40003, DATA + 0x1001, 5000);
        op(SX_OP_COPY, DATA + 0x2008, DATA + 0x2000, 3000);   // overlapping, the destination after the source
        op(SX_OP_COPY, DATA + 0x3000, DATA + 0x3005, 3000);   // the destination before
        op(SX_OP_FILL, DATA + 0x50001, 0xa1b2c3d4u, 777);
        op(SX_OP_COPY, DATA + 0x60000, SX_ROM + 0x10, 4000);  // from the ROM
        op(SX_OP_COPY, SX_RAM + 0x400007, DATA + 1, 600000);  // more than a row of 256 KB
        op(SX_OP_COPY, 0x100, DATA, 16);                      // refused: the boot area
        // a blit: 37 rows of 50 bytes, strides 64 and 80
        a.li(5, SX_OP_BLIT); a.li(6, (int32_t)(DATA + 0x70003)); a.li(7, (int32_t)(DATA + 0x8000)); a.li(8, 0);
        a.li(10, 64); a.li(11, 80); a.li(12, 50); a.li(13, 37);
        a.st(SX_W_Q, 10, 20, 16); a.st(SX_W_Q, 5, 20, 0); a.x(SX_X_SYNC);
        a.lw(9, 20, 0); a.op(SX_I_ADD, 30, 30, 9);
        a.halt();
    }});
    v.push_back({"cores", 4, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        // four cores: each fills a region of its own, and all store to one word (the highest core's lands)
        for (uint32_t c = 0; c < 4; c++) {
            SxAsm& a = p[c];
            a.mfsr(5, SX_SR_COREID);
            a.li(6, (int32_t)DATA); a.sw(5, 6, 0);
            a.opi(SX_I_SLL, 7, 5, 12); a.add(7, 7, 6); a.addi(7, 7, 0x1000);
            a.li(8, 0); a.li(9, 600);
            int top = a.here();
            a.op(SX_I_MUL, 10, 8, 5); a.R(SX_OP_M(SX_M_STOREX, SX_W_W), 10, 7, 8, 2); a.addi(8, 8, 1);
            a.bne(8, 9, top);
            a.halt();
        }
    }});
    // ---- the loops of experiments/isa_proto/lean/isaloops.c, as SX writes them; timed, scored in the RISC-V
    // loop's instructions (rv_per_iter), the lean tick's measure
    const int N = 3000000;
    v.push_back({"arith", 1, true, [N](std::vector<SxAsm>& p, auto& data, uint32_t) {
        SxAsm& a = p[0];
        a.li(5, N);
        int top = a.here();
        a.addi(6, 6, 1); a.op(SX_I_XOR, 7, 7, 6); a.add(8, 8, 7);
        a.opi(SX_I_SLL, 9, 8, 3); a.sub(10, 9, 6); a.op(SX_I_OR, 11, 10, 7);
        a.opi(SX_I_AND, 6, 6, 0x7ff); a.opi(SX_I_SRL, 7, 11, 2); a.add(8, 8, 10);
        a.opi(SX_I_XOR, 9, 9, 0x55); a.op(SX_I_SLTU, 10, 8, 9); a.add(11, 11, 10);
        a.opi(SX_I_SLL, 6, 6, 1); a.opi(SX_I_SRA, 7, 7, 1); a.op(SX_I_AND, 8, 8, 11);
        a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 16, 17});
    v.push_back({"loads", 1, true, [N](std::vector<SxAsm>& p, auto& data, uint32_t) {
        for (uint32_t k = 0; k < 64; k++) data.push_back({DATA + 4 * k, k * 7 + 1});
        SxAsm& a = p[0];
        a.li(5, N); a.li(4, (int32_t)DATA);
        int top = a.here();
        for (int i = 0; i < 14; i++) a.lw(6 + i % 6, 4, 4 * i);
        a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 15, 16});
    v.push_back({"stores", 1, true, [N](std::vector<SxAsm>& p, auto& data, uint32_t) {
        SxAsm& a = p[0];
        a.li(5, N); a.li(4, (int32_t)DATA);
        int top = a.here();
        for (int i = 0; i < 14; i++) a.sw(5, 4, 4 * i);
        a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 15, 16});
    v.push_back({"adds", 1, true, [N](std::vector<SxAsm>& p, auto& data, uint32_t) {
        SxAsm& a = p[0];
        a.li(5, N);
        int top = a.here();
        for (int i = 0; i < 14; i++) a.add(6, 6, 5);
        a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 15, 16});
    v.push_back({"tight", 1, true, [N](std::vector<SxAsm>& p, auto& data, uint32_t) {
        SxAsm& a = p[0];
        a.li(5, N * 8);
        int top = a.here();
        a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 1, 2});
    v.push_back({"mix", 1, true, [N](std::vector<SxAsm>& p, auto& data, uint32_t) {
        for (uint32_t k = 0; k < 2048; k++) data.push_back({DATA + 4 * k, 0x9e3779b9u * (k + 1)});
        SxAsm& a = p[0];
        a.li(5, N); a.li(6, 0); a.li(14, 0); a.li(7, (int32_t)DATA); a.li(8, (int32_t)(DATA + 4096));
        int top = a.here();
        a.R(SX_OP_M(SX_M_LOADX, SX_W_W), 10, 7, 6, 0); a.add(14, 14, 10); a.addi(6, 6, 4);
        a.op(SX_I_XOR, 12, 10, 14); a.opi(SX_I_AND, 6, 6, 0x7c); a.R(SX_OP_M(SX_M_STOREX, SX_W_W), 12, 8, 6, 0);
        a.opi(SX_I_SLL, 13, 12, 1); a.add(14, 14, 13); a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 9, 12});
    v.push_back({"stride", 1, true, [N](std::vector<SxAsm>& p, auto& data, uint32_t) {
        SxAsm& a = p[0];
        a.li(5, N); a.li(6, 0); a.li(14, 0); a.li(7, (int32_t)(SX_RAM + 0x400000)); a.li(8, 0x3ffffc);
        int top = a.here();
        a.R(SX_OP_M(SX_M_LOADX, SX_W_W), 10, 7, 6, 0); a.add(14, 14, 10); a.addi(6, 6, 1040);
        a.op(SX_I_AND, 6, 6, 8); a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 5, 7});
    v.push_back({"fmath", 1, true, [N](std::vector<SxAsm>& p, auto& data, uint32_t) {
        SxAsm& a = p[0];
        a.li(5, N); a.li(6, 0x3f800000); a.li(7, 0x3f000000); a.li(8, 0);
        int top = a.here();
        a.fop(SX_F_MAD, 8, 6, 7, 8); a.fop(SX_F_MUL, 9, 8, 7); a.fop(SX_F_ADD, 10, 9, 6);
        a.fop(SX_F_MIN, 11, 10, 8); a.fop(SX_F_SQRT, 12, 10, 0); a.fop(SX_F_SUB, 8, 8, 12);
        a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 7, 0});
    return v;
}

// ---- D3D11
static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
static void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) { printf("%s failed: %08lx\n", what, (unsigned long)hr); exit(1); }
}
struct Tex { ID3D11Texture2D* t = nullptr; ID3D11ShaderResourceView* srv = nullptr; ID3D11RenderTargetView* rtv = nullptr; UINT w = 0, h = 0; };
static Tex make_tex(UINT w, UINT h, const void* init) {
    Tex t; t.w = w; t.h = h;
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {init, w * 16, 0};
    check(dev->CreateTexture2D(&d, init ? &sd : nullptr, &t.t), "CreateTexture2D");
    check(dev->CreateShaderResourceView(t.t, nullptr, &t.srv), "SRV");
    check(dev->CreateRenderTargetView(t.t, nullptr, &t.rtv), "RTV");
    return t;
}
static void free_tex(Tex& t) { if (t.t) { t.srv->Release(); t.rtv->Release(); t.t->Release(); t = Tex(); } }
static std::vector<uint32_t> read_tex(const Tex& t) {
    D3D11_TEXTURE2D_DESC d = {t.w, t.h, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    ID3D11Texture2D* st;
    check(dev->CreateTexture2D(&d, nullptr, &st), "staging");
    ctx->CopyResource(st, t.t);
    D3D11_MAPPED_SUBRESOURCE m;
    check(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "Map");
    std::vector<uint32_t> out((size_t)t.w * t.h * 4);
    for (UINT y = 0; y < t.h; y++) memcpy(&out[(size_t)y * t.w * 4], (char*)m.pData + (size_t)y * m.RowPitch, t.w * 16);
    ctx->Unmap(st, 0);
    st->Release();
    return out;
}
static std::string slurp(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}
// compiled shaders are kept in bin\sxcache by a hash of the tick, isa.h, the defines and the entry
static ID3DBlob* compile(const char* path, const char* entry, const char* profile, const std::vector<D3D_SHADER_MACRO>& defs) {
    std::string dir(path);
    dir = dir.substr(0, dir.find_last_of("\\/"));
    std::string key = slurp(path) + slurp(dir + "/../isa.h") + entry + profile;
    char dll[MAX_PATH] = "";   // the compiler that makes it (fxc2 or Microsoft's FXC)
    GetModuleFileNameA(GetModuleHandleA("d3dcompiler_47.dll"), dll, MAX_PATH);
    key += std::to_string(slurp(dll).size());
    for (auto& d : defs) if (d.Name) key += std::string(d.Name) + "=" + d.Definition + ";";
    uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : key) h = (h ^ ch) * 1099511628211ull;
    char name[64];
    snprintf(name, sizeof name, "%016llx.cso", (unsigned long long)h);
    std::string cdir = dir + "/../../bin/sxcache";
    CreateDirectoryA(cdir.c_str(), nullptr);
    std::string cfile = cdir + "/" + name, got = slurp(cfile);
    ID3DBlob* code = nullptr;
    if (!got.empty()) {
        check(D3DCreateBlob(got.size(), &code), "D3DCreateBlob");
        memcpy(code->GetBufferPointer(), got.data(), got.size());
        return code;
    }
    std::vector<wchar_t> wpath(strlen(path) + 1);
    mbstowcs(wpath.data(), path, wpath.size());
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompileFromFile(wpath.data(), defs.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile,
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (err && (FAILED(hr) || getenv("SX_WARNINGS"))) printf("%s", (const char*)err->GetBufferPointer());
    check(hr, entry);
    if (FILE* f = fopen(cfile.c_str(), "wb")) { fwrite(code->GetBufferPointer(), 1, code->GetBufferSize(), f); fclose(f); }
    return code;
}
struct Prog { ID3D11VertexShader* vs; ID3D11PixelShader* ps; };
static Prog program(const char* path, const char* v, const char* p, const std::vector<D3D_SHADER_MACRO>& defs) {
    Prog r;
    ID3DBlob *vb = compile(path, v, "vs_5_0", defs), *pb = compile(path, p, "ps_5_0", defs);
    check(dev->CreateVertexShader(vb->GetBufferPointer(), vb->GetBufferSize(), nullptr, &r.vs), v);
    check(dev->CreatePixelShader(pb->GetBufferPointer(), pb->GetBufferSize(), nullptr, &r.ps), p);
    if (!strcmp(p, "frag")) {
        printf("sxbench: the tick's fragment shader is %zu bytes\n", pb->GetBufferSize());
        if (const char* out = getenv("SX_DISASM")) {   // its DXBC, for counting the loop's instructions
            ID3DBlob* text = nullptr;   // (fxc2 has no disassembler: Windows' own compiler's)
            HMODULE sys = LoadLibraryA("C:\\Windows\\System32\\d3dcompiler_47.dll");
            auto dis = sys ? (decltype(&D3DDisassemble))GetProcAddress(sys, "D3DDisassemble") : nullptr;
            if (dis && SUCCEEDED(dis(pb->GetBufferPointer(), pb->GetBufferSize(), 0, nullptr, &text))) {
                if (FILE* f = fopen(out, "wb")) { fwrite(text->GetBufferPointer(), 1, text->GetBufferSize() - 1, f); fclose(f); }
                text->Release();
            } else printf("sxbench: no disassembly\n");
        }
    }
    vb->Release(); pb->Release();
    return r;
}

// ---- The GPU's machine
struct Gpu {
    uint32_t cores, statePix;
    Tex ram, rom, stage, ops[2], set[2][8];
    int cur = 0, oc = 0;
    Prog tick, commit, stg, wrt, prog;
    bool hasCommit = true, hasOps = true;
    ID3D11Buffer *cb, *ccb[4];
    ID3D11BlendState* mask[4];
    struct { uint32_t ticks, ramEnd, timeLo, timeHi, player, flags, ramRows, ext, ncores, pad[3]; } pass;
    void make(uint32_t ncores, const std::vector<uint32_t>& ramImg, const std::vector<uint32_t>& romImg, const std::vector<std::vector<uint32_t>>& state) {
        cores = ncores;
        statePix = (24 + g_wcn / 2 + 7) / 8;
        ram = make_tex(2048, RAM_ROWS, ramImg.data());
        rom = make_tex(2048, ROM_ROWS, romImg.data());
        std::vector<uint32_t> z((size_t)2048 * cores * 9 * 4, 0);
        stage = make_tex(2048, cores * 9, z.data());
        std::vector<uint32_t> oz((size_t)cores * 4, 0);
        for (int k = 0; k < 2; k++) ops[k] = make_tex(cores, 1, oz.data());
        for (int k = 0; k < 2; k++)
            for (int t = 0; t < 8; t++) {
                std::vector<uint32_t> img((size_t)statePix * cores * 4, 0);
                for (uint32_t c = 0; c < cores; c++)
                    for (uint32_t tx = t; tx < statePix * 8 && tx < state[c].size() / 4; tx += 8)
                        memcpy(&img[((size_t)c * statePix + tx / 8) * 4], &state[c][tx * 4], 16);
                set[k][t] = make_tex(statePix, cores, img.data());
            }
        cur = 0; oc = 0;
    }
    void release() {
        free_tex(ram); free_tex(rom); free_tex(stage);
        for (auto& t : ops) free_tex(t);
        for (auto& s : set) for (auto& t : s) free_tex(t);
    }
    void clear_srv() {
        ID3D11ShaderResourceView* none[12] = {};
        ctx->PSSetShaderResources(0, 12, none);
        ctx->VSSetShaderResources(0, 12, none);
        ctx->OMSetRenderTargets(0, nullptr, nullptr);
    }
    void bind(const Tex& state0, int which, bool withRam, bool withStage) {   // t0 RAM, t1 ROM, t2-t9 a state set, t10 ops, t11 stage
        ID3D11ShaderResourceView* s[12] = {};
        if (withRam) s[0] = ram.srv;
        s[1] = rom.srv;
        for (int t = 0; t < 8; t++) s[2 + t] = set[which][t].srv;
        s[10] = ops[oc].srv;
        if (withStage) s[11] = stage.srv;
        ctx->PSSetShaderResources(0, 12, s);
        ctx->VSSetShaderResources(0, 12, s);
        (void)state0;
    }
    void viewport(UINT w, UINT h) { D3D11_VIEWPORT v = {0, 0, (float)w, (float)h, 0, 1}; ctx->RSSetViewports(1, &v); }
    void run_pass() {
        ctx->UpdateSubresource(cb, 0, nullptr, &pass, 0, 0);
        ID3D11Buffer* cbs[2] = {cb, ccb[0]};
        ctx->VSSetConstantBuffers(0, 2, cbs);
        ctx->PSSetConstantBuffers(0, 2, cbs);
        ctx->IASetInputLayout(nullptr);
        // the tick: set cur from set cur ^ 1
        clear_srv();
        ID3D11RenderTargetView* rt[8];
        for (int t = 0; t < 8; t++) rt[t] = set[cur][t].rtv;
        ctx->OMSetRenderTargets(8, rt, nullptr);
        bind(set[cur ^ 1][0], cur ^ 1, true, false);
        viewport(statePix, cores);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(tick.vs, nullptr, 0); ctx->PSSetShader(tick.ps, nullptr, 0);
        ctx->Draw(3, 0);
        if (!hasCommit) { clear_srv(); cur ^= 1; oc ^= 1; return; }
        // the commit: four draws, a component each
        clear_srv();
        ctx->OMSetRenderTargets(1, &ram.rtv, nullptr);
        bind(set[cur][0], cur, false, false);
        viewport(2048, RAM_ROWS);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        ctx->VSSetShader(commit.vs, nullptr, 0); ctx->PSSetShader(commit.ps, nullptr, 0);
        for (int c = 0; c < 4; c++) {
            ID3D11Buffer* b2[2] = {cb, ccb[c]};
            ctx->VSSetConstantBuffers(0, 2, b2);
            ctx->OMSetBlendState(mask[c], nullptr, 0xffffffff);
            ctx->Draw(cores * g_wcn, 0);
        }
        ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        if (!hasOps) { clear_srv(); cur ^= 1; oc ^= 1; return; }
        // the operations: stage, write, progress
        clear_srv();
        ctx->OMSetRenderTargets(1, &stage.rtv, nullptr);
        bind(set[cur][0], cur, true, false);
        viewport(2048, cores * 9);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(stg.vs, nullptr, 0); ctx->PSSetShader(stg.ps, nullptr, 0);
        ctx->DrawInstanced(4, cores * 64, 0, 0);
        clear_srv();
        ctx->OMSetRenderTargets(1, &ram.rtv, nullptr);
        bind(set[cur][0], cur, false, true);
        viewport(2048, RAM_ROWS);
        ctx->VSSetShader(wrt.vs, nullptr, 0); ctx->PSSetShader(wrt.ps, nullptr, 0);
        ctx->DrawInstanced(4, cores * 64, 0, 0);
        clear_srv();
        ctx->OMSetRenderTargets(1, &ops[oc ^ 1].rtv, nullptr);
        bind(set[cur][0], cur, false, false);
        viewport(cores, 1);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
        ctx->VSSetShader(prog.vs, nullptr, 0); ctx->PSSetShader(prog.ps, nullptr, 0);
        ctx->Draw(cores, 0);
        clear_srv();
        cur ^= 1; oc ^= 1;
    }
    std::vector<uint32_t> core_state(uint32_t c) {   // the last pass's state of core c, texel by texel
        std::vector<uint32_t> out((size_t)statePix * 8 * 4);
        for (int t = 0; t < 8; t++) {
            std::vector<uint32_t> v = read_tex(set[cur ^ 1][t]);
            for (uint32_t p = 0; p < statePix; p++) memcpy(&out[((size_t)p * 8 + t) * 4], &v[((size_t)c * statePix + p) * 4], 16);
        }
        return out;
    }
};

int main(int argc, char** argv) {
    bool warp = false;
    std::string only, shader;
    uint32_t ticks = 8192, seed = 1, passes = 400;
    std::vector<std::string> defStore;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--warp") warp = true;
        else if (a == "--shader" && i + 1 < argc) shader = argv[++i];
        else if (a == "--case" && i + 1 < argc) only = argv[++i];
        else if (a == "--ticks" && i + 1 < argc) ticks = (uint32_t)atoi(argv[++i]);
        else if (a == "--passes" && i + 1 < argc) passes = (uint32_t)atoi(argv[++i]);
        else if (a == "--seed" && i + 1 < argc) seed = (uint32_t)atoi(argv[++i]);
        else if (a == "-D" && i + 1 < argc) defStore.push_back(argv[++i]);
        else { printf("sxbench [--warp] [--case A,B] [--ticks N] [--passes N] [--seed N] [-D NAME=V]\n"); return 1; }
    }
    std::vector<std::string> names, values;
    for (auto& d : defStore) {
        size_t e = d.find('=');
        names.push_back(d.substr(0, e));
        values.push_back(e == std::string::npos ? "1" : d.substr(e + 1));
        if (names.back() == "WCN") g_wcn = (uint32_t)atoi(values.back().c_str());
    }
    std::vector<D3D_SHADER_MACRO> defs;
    for (size_t i = 0; i < names.size(); i++) defs.push_back({names[i].c_str(), values[i].c_str()});
    defs.push_back({nullptr, nullptr});

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx), "D3D11CreateDevice");
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string path(exe);
    path = shader.empty() ? path.substr(0, path.find_last_of("\\/")) + "\\..\\sx\\tick\\tick.hlsl" : shader;
    LARGE_INTEGER f0, f1, fq;
    QueryPerformanceCounter(&f0);
    Gpu gpu;
    gpu.tick = program(path.c_str(), "vert", "frag", defs);
    // a step without stores has no commit, one without the operation region no operations' passes
    std::string src = slurp(path);
    gpu.hasCommit = src.find("commit_vert") != std::string::npos;
    gpu.hasOps = src.find("op_stage_vert") != std::string::npos;
    if (gpu.hasCommit) gpu.commit = program(path.c_str(), "commit_vert", "commit_frag", defs);
    if (gpu.hasOps) {
        gpu.stg = program(path.c_str(), "op_stage_vert", "op_stage_frag", defs);
        gpu.wrt = program(path.c_str(), "op_write_vert", "op_write_frag", defs);
        gpu.prog = program(path.c_str(), "op_prog_vert", "op_prog_frag", defs);
    }
    QueryPerformanceCounter(&f1);
    QueryPerformanceFrequency(&fq);
    printf("sxbench: %s, %s compiled in %.1f s, write cache %u words\n", warp ? "WARP" : "the card", path.c_str(),
           double(f1.QuadPart - f0.QuadPart) / fq.QuadPart, g_wcn);
    {
        D3D11_BUFFER_DESC bd = {48, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        check(dev->CreateBuffer(&bd, nullptr, &gpu.cb), "cbuffer");
        for (int c = 0; c < 4; c++) {
            D3D11_BLEND_DESC b = {};
            b.RenderTarget[0].SrcBlend = b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            b.RenderTarget[0].DestBlend = b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            b.RenderTarget[0].BlendOp = b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            b.RenderTarget[0].RenderTargetWriteMask = (UINT8)(1 << c);
            check(dev->CreateBlendState(&b, &gpu.mask[c]), "blend");
            gpu.ccb[c] = nullptr;
        }
    }
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ID3D11Query *qDis, *q0, *q1;
    dev->CreateQuery(&qd, &qDis);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    dev->CreateQuery(&qd, &q0);
    dev->CreateQuery(&qd, &q1);

    std::vector<uint32_t> romImg((size_t)ROM_SIZE / 4);
    for (size_t k = 0; k < romImg.size(); k++) romImg[k] = (uint32_t)(k * 0x01000193u) ^ 0x5bd1e995u;
    bool all_ok = true;
    for (const Case& cs : cases()) {
        if (!only.empty() && ("," + only + ",").find("," + std::string(cs.name) + ",") == std::string::npos) continue;
        std::vector<SxAsm> progs(cs.cores, SxAsm(SX_RAM));
        std::vector<std::pair<uint32_t, uint32_t>> data;
        for (uint32_t c = 0; c < cs.cores; c++) progs[c].base = SX_RAM + c * 0x40000;
        cs.build(progs, data, seed);
        std::vector<uint32_t> ramImg((size_t)RAM_END / 4, 0);
        for (uint32_t c = 0; c < cs.cores; c++) {
            std::vector<uint32_t> w = progs[c].finish();
            memcpy(&ramImg[progs[c].base / 4], w.data(), w.size() * 4);
        }
        for (auto& d : data) ramImg[d.first / 4] = d.second;
        // a trap nobody asked for halts the core where its handler would be: {break} at HALT_STUB
        const uint32_t HALT_STUB = SX_RAM + 0xff0000;
        ramImg[HALT_STUB / 4] = SX_X_BREAK; ramImg[HALT_STUB / 4 + 3] = SX_CTRL_NONE | 1;
        // the state: pc, STATUS supervisor; the commit's constant: the cores
        std::vector<std::vector<uint32_t>> state(cs.cores, std::vector<uint32_t>((size_t)(24 + g_wcn / 2 + 8) * 4, 0));
        for (uint32_t c = 0; c < cs.cores; c++) {
            state[c][16 * 4 + 0] = progs[c].base;
            state[c][16 * 4 + 1] = SX_ST_S;
            state[c][18 * 4 + 3] = 0x9e3779b9u ^ (c * 0x85ebca6bu);
            state[c][17 * 4 + 1] = HALT_STUB;
        }
        uint32_t cdata[4][4];
        for (int c = 0; c < 4; c++) {
            cdata[c][0] = (uint32_t)c; cdata[c][1] = cs.cores; cdata[c][2] = cdata[c][3] = 0;
            if (gpu.ccb[c]) gpu.ccb[c]->Release();
            D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            D3D11_SUBRESOURCE_DATA ci = {cdata[c], 0, 0};
            check(dev->CreateBuffer(&cd, &ci, &gpu.ccb[c]), "commit cbuffer");
        }
        // the reference
        sx_machine m = {};
        std::vector<uint32_t> refRam = ramImg;
        std::vector<uint8_t> romBytes(ROM_SIZE);
        memcpy(romBytes.data(), romImg.data(), ROM_SIZE);
        m.ram = refRam.data(); m.ram_end = RAM_END; m.rom = romBytes.data(); m.rom_size = ROM_SIZE;
        m.flags = SX_REF_BREAK_HALTS;
        std::vector<sx_core> rc(cs.cores);
        for (uint32_t c = 0; c < cs.cores; c++) { sx_core_reset(&rc[c], c, progs[c].base); rc[c].sr[SX_SR_TVEC] = HALT_STUB; }
        m.cores = rc.data(); m.ncores = cs.cores;
        uint32_t checkTicks = cs.timed ? ticks : 61;   // a check's passes are short: they end inside bundles
        gpu.make(cs.cores, ramImg, romImg, state);
        gpu.pass = {checkTicks, RAM_END, 1000, 0, 0, 1, RAM_ROWS, 0, cs.cores, {0, 0, 0}};
        double mops = 0;
        if (cs.timed) {
            gpu.pass.ticks = ticks;
            for (int k = 0; k < 20; k++) gpu.run_pass();
            uint32_t o0 = gpu.core_state(0)[19 * 4 + 1];
            ctx->Begin(qDis);
            ctx->End(q0);
            for (uint32_t k = 0; k < passes; k++) gpu.run_pass();
            ctx->End(q1);
            ctx->End(qDis);
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
            while (ctx->GetData(qDis, &dj, sizeof dj, 0) != S_OK) Sleep(1);
            UINT64 t0, t1;
            while (ctx->GetData(q0, &t0, sizeof t0, 0) != S_OK) Sleep(1);
            while (ctx->GetData(q1, &t1, sizeof t1, 0) != S_OK) Sleep(1);
            std::vector<uint32_t> s = gpu.core_state(0);
            uint32_t o1 = s[19 * 4 + 1];
            double sec = double(t1 - t0) / dj.Frequency;
            mops = (o1 - o0) / sec / 1e6;
            double iters = mops / cs.ops_per_iter;
            printf("%-7s %8.2f M ops a second, %6.2f M iterations; as RISC-V's work %6.2f M a second; %.3f ms a pass%s\n", cs.name, mops,
                   iters, iters * cs.rv_per_iter, sec * 1e3 / passes, dj.Disjoint ? " (disjoint)" : "");
        }
        // both to the end
        int refPasses = 0, gpuPasses = 0;
        for (;;) {
            bool any = false;
            m.time = 1000;
            for (uint32_t c = 0; c < cs.cores; c++) { if (!rc[c].halted) { sx_run(&m, &rc[c], checkTicks); any = true; } }
            sx_commit(&m);
            refPasses++;
            if (!any || refPasses > 400000) break;
        }
        bool halted = false;
        ULONGLONG checkStart = GetTickCount64();   // a check gives up after 60 s: every run stays short
        for (int round = 0; round < 4000 && !halted && GetTickCount64() - checkStart < 60000; round++) {
            for (int k = 0; k < 50; k++) { gpu.run_pass(); gpuPasses++; }
            halted = true;
            for (uint32_t c = 0; c < cs.cores; c++) halted = halted && (gpu.core_state(c)[18 * 4 + 2] & 4);
        }
        int bad = 0;
        if (!halted) { printf("  the tick did not halt in %d passes\n", gpuPasses); bad++; }
        for (uint32_t c = 0; c < cs.cores; c++) {
            std::vector<uint32_t> s = gpu.core_state(c);
            for (int k = 1; k < 64; k++)
                if (s[k] != rc[c].x[k] && bad++ < 8) printf("  core %u r%d %08x, the reference %08x\n", c, k, s[k], rc[c].x[k]);
            if (s[16 * 4] != rc[c].pc && bad++ < 8) printf("  core %u pc %08x, the reference %08x\n", c, s[16 * 4], rc[c].pc);
            if (s[19 * 4 + 1] != (uint32_t)rc[c].ops_run && bad++ < 8) printf("  core %u ops %u, the reference %u\n", c, s[19 * 4 + 1], (uint32_t)rc[c].ops_run);
            if (bad) {
                uint32_t e = rc[c].sr[SX_SR_EPC];
                printf("  core %u: the tick's EPC %08x CAUSE %08x TVAL %08x; the reference's %08x %08x %08x, its op there %08x\n", c,
                       s[16 * 4 + 2], s[16 * 4 + 3], s[17 * 4], e, rc[c].sr[SX_SR_CAUSE], rc[c].sr[SX_SR_TVAL],
                       e < RAM_END ? refRam[e / 4] : 0);
            }
        }
        std::vector<uint32_t> gram = gpu.ram.t ? read_tex(gpu.ram) : std::vector<uint32_t>();
        size_t ramBad = 0;
        for (size_t k = 0; k < gram.size(); k++)
            if (gram[k] != refRam[k] && ramBad++ < 6) printf("  word %08zx %08x, the reference %08x\n", k * 4, gram[k], refRam[k]);
        bool ok = bad == 0 && ramBad == 0;
        all_ok = all_ok && ok;
        printf("%-7s %s (%d passes, the reference %d; %llu ops)%s\n", cs.name, ok ? "same as the reference" : "WRONG", gpuPasses, refPasses,
               (unsigned long long)rc[0].ops_run, ramBad ? (" " + std::to_string(ramBad) + " words differ").c_str() : "");
        gpu.release();
    }
    return all_ok ? 0 : 2;
}
