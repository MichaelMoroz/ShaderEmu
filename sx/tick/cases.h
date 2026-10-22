// The cases of SX's tick bench (sxbench.cpp) and of its D3D12 runner for Nsight (sxtrace12.cpp): programs for
// both machines, assembled with sx/ref/sx_asm.h, and the lean tick's numbers they are held against.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "../ref/sx_ref.h"
#include "../ref/sx_asm.h"

// ---- The bench's machine: RAM to 48 MB (16 MB of it RAM proper), 64 KB of ROM
static const uint32_t RAM_ROWS = 1536, RAM_END = RAM_ROWS * 2048 * 16, ROM_ROWS = 2, ROM_SIZE = ROM_ROWS * 2048 * 16;
static const uint32_t DATA = SX_RAM + 0x100000;   // a case's data
static uint32_t g_wcn = 128;
static int g_loop_n = 3000000;   // a timed loop's iterations (a check runs a few thousand)

struct Case {
    const char* name;
    uint32_t cores;
    bool timed;
    std::function<void(std::vector<SxAsm>&, std::vector<std::pair<uint32_t, uint32_t>>&, uint32_t seed)> build;
    double ops_per_iter = 0;   // for a timed case: SX ops an iteration of its loop
    double rv_per_iter = 0;    // and the RISC-V instructions isaloops.c spends on the same iteration
    double tol = 0;            // registers that may differ by this much, relative (the card's special functions)
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
        if (g.below(4) == 0 && f != SX_F_MAD && f != SX_F_MSB && f != SX_F_NMSB) { uint32_t v = rfloat(g); float fv; memcpy(&fv, &v, 4); a.fopi(f, rd, ra, fv); }
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
    v.push_back({"fpaths", 1, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        // floats in every path: float with float, float with an integer op, loads and stores beside float sums
        Rng g{seed * 7727 + 3};
        for (uint32_t k = 0; k < 1024; k++) { uint32_t u = rfloat(g); data.push_back({DATA + 4 * k, u}); }
        static const uint32_t lite_f[] = {SX_F_ADD, SX_F_MUL, SX_F_MAD, SX_F_MSB, SX_F_SUB, SX_F_NMSB, SX_F_MIN, SX_F_MAX};
        static const uint32_t any_f[] = {SX_F_ADD, SX_F_MUL, SX_F_MAD, SX_F_SUB, SX_F_MIN, SX_F_MAX, SX_F_EQ, SX_F_LT, SX_F_LE,
                                         SX_F_UNORD, SX_F_SGNJ, SX_F_SGNJN, SX_F_SGNJX, SX_F_FRAC, SX_F_ITOF, SX_F_UTOF, SX_F_FTOI,
                                         SX_F_FTOU, SX_F_FLOOR, SX_F_CEIL, SX_F_TRUNC, SX_F_ROUND};
        SxAsm& a = p[0];
        for (int round = 0; round < 30; round++) {
            seed_regs(a, g, true);
            a.li(29, (int32_t)DATA);
            for (int i = 0; i < 40; i++) {
                switch (g.below(5)) {
                case 0: a.fop(any_f[g.below(sizeof any_f / 4)], rreg(g), rreg(g), rreg(g), rreg(g)); break;
                case 1: a.fop(lite_f[g.below(8)], rreg(g), rreg(g), rreg(g), rreg(g)); break;
                case 2: a.lw(rreg(g), 29, (int32_t)(4 * g.below(1024))); break;
                case 3: a.sw(rreg(g), 29, (int32_t)(4 * g.below(1024))); break;
                default: a.addi(rreg(g), rreg(g), rimm(g)); break;
                }
            }
        }
        a.halt();
    }});
    v.push_back({"sfu", 1, false, [](std::vector<SxAsm>& p, auto& data, uint32_t seed) {
        // division and the special functions on r5-r16 into r17-r28 (no result is used again): within 2e-5
        Rng g{seed * 104729 + 7};
        SxAsm& a = p[0];
        for (int round = 0; round < 30; round++) {
            for (uint32_t k = 5; k <= 16; k++) {
                float f = (float)((int32_t)g.below(20001) - 10000) / 100.0f;
                if (g.below(8) == 0) f = (float)(g.below(1000)) * 1e-3f;
                uint32_t u; memcpy(&u, &f, 4);
                a.li(k, (int32_t)u);
            }
            for (int i = 0; i < 24; i++) a.fop(SX_F_DIV + g.below(8), 17 + g.below(12), 5 + g.below(12), 5 + g.below(12));
        }
        a.halt();
    }, 0, 0, 2e-5});
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
    v.push_back({"arith", 1, true, [](std::vector<SxAsm>& p, auto& data, uint32_t) {
        SxAsm& a = p[0];
        a.li(5, g_loop_n);
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
    v.push_back({"loads", 1, true, [](std::vector<SxAsm>& p, auto& data, uint32_t) {
        for (uint32_t k = 0; k < 64; k++) data.push_back({DATA + 4 * k, k * 7 + 1});
        SxAsm& a = p[0];
        a.li(5, g_loop_n); a.li(4, (int32_t)DATA);
        int top = a.here();
        for (int i = 0; i < 14; i++) a.lw(6 + i % 6, 4, 4 * i);
        a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 15, 16});
    v.push_back({"stores", 1, true, [](std::vector<SxAsm>& p, auto& data, uint32_t) {
        SxAsm& a = p[0];
        a.li(5, g_loop_n); a.li(4, (int32_t)DATA);
        int top = a.here();
        for (int i = 0; i < 14; i++) a.sw(5, 4, 4 * i);
        a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 15, 16});
    v.push_back({"adds", 1, true, [](std::vector<SxAsm>& p, auto& data, uint32_t) {
        SxAsm& a = p[0];
        a.li(5, g_loop_n);
        int top = a.here();
        for (int i = 0; i < 14; i++) a.add(6, 6, 5);
        a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 15, 16});
    v.push_back({"tight", 1, true, [](std::vector<SxAsm>& p, auto& data, uint32_t) {
        SxAsm& a = p[0];
        a.li(5, g_loop_n * 8);
        int top = a.here();
        a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 1, 2});
    v.push_back({"mix", 1, true, [](std::vector<SxAsm>& p, auto& data, uint32_t) {
        for (uint32_t k = 0; k < 2048; k++) data.push_back({DATA + 4 * k, 0x9e3779b9u * (k + 1)});
        SxAsm& a = p[0];
        a.li(5, g_loop_n); a.li(6, 0); a.li(14, 0); a.li(7, (int32_t)DATA); a.li(8, (int32_t)(DATA + 4096));
        int top = a.here();
        a.R(SX_OP_M(SX_M_LOADX, SX_W_W), 10, 7, 6, 0); a.add(14, 14, 10); a.addi(6, 6, 4);
        a.op(SX_I_XOR, 12, 10, 14); a.opi(SX_I_AND, 6, 6, 0x7c); a.R(SX_OP_M(SX_M_STOREX, SX_W_W), 12, 8, 6, 0);
        a.opi(SX_I_SLL, 13, 12, 1); a.add(14, 14, 13); a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 9, 12});
    v.push_back({"stride", 1, true, [](std::vector<SxAsm>& p, auto& data, uint32_t) {
        SxAsm& a = p[0];
        a.li(5, g_loop_n); a.li(6, 0); a.li(14, 0); a.li(7, (int32_t)(SX_RAM + 0x400000)); a.li(8, 0x3ffffc);
        int top = a.here();
        a.R(SX_OP_M(SX_M_LOADX, SX_W_W), 10, 7, 6, 0); a.add(14, 14, 10); a.addi(6, 6, 1040);
        a.op(SX_I_AND, 6, 6, 8); a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 5, 7});
    v.push_back({"floats", 1, true, [](std::vector<SxAsm>& p, auto& data, uint32_t) {
        SxAsm& a = p[0];
        a.li(5, g_loop_n);
        for (int k = 6; k <= 13; k++) a.li(k, 0);
        int top = a.here();
        a.fop(SX_F_ADD, 6, 6, 7); a.fop(SX_F_MUL, 8, 6, 9); a.fop(SX_F_SUB, 10, 8, 7); a.fop(SX_F_ADD, 11, 10, 6);
        a.fop(SX_F_MUL, 12, 11, 9); a.fop(SX_F_MIN, 13, 12, 8); a.fop(SX_F_ADD, 7, 7, 13); a.fop(SX_F_MUL, 6, 6, 9);
        a.fop(SX_F_ADD, 8, 8, 11); a.fop(SX_F_SUB, 9, 9, 10); a.fop(SX_F_MAX, 10, 10, 12); a.fop(SX_F_ADD, 12, 12, 7);
        a.fop(SX_F_MUL, 13, 13, 6); a.fop(SX_F_ADD, 11, 11, 8);
        a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 15, 16});
    v.push_back({"fmix", 1, true, [](std::vector<SxAsm>& p, auto& data, uint32_t) {
        for (uint32_t k = 0; k < 4; k++) data.push_back({DATA + 4 * k, 0x3f800000u + k});
        SxAsm& a = p[0];
        a.li(5, g_loop_n); a.li(4, (int32_t)DATA);
        int top = a.here();
        a.lw(6, 4, 0); a.lw(7, 4, 4); a.fop(SX_F_MUL, 8, 6, 7); a.fop(SX_F_ADD, 9, 9, 8);
        a.sw(9, 4, 8); a.lw(10, 4, 12); a.fop(SX_F_MUL, 11, 10, 8); a.fop(SX_F_ADD, 12, 12, 11);
        a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 9, 10});
    v.push_back({"fmath", 1, true, [](std::vector<SxAsm>& p, auto& data, uint32_t) {
        SxAsm& a = p[0];
        a.li(5, g_loop_n); a.li(6, 0x3f800000); a.li(7, 0x3f000000); a.li(8, 0);
        int top = a.here();
        a.fop(SX_F_MAD, 8, 6, 7, 8); a.fop(SX_F_MUL, 9, 8, 7); a.fop(SX_F_ADD, 10, 9, 6);
        a.fop(SX_F_MIN, 11, 10, 8); a.fop(SX_F_SQRT, 12, 10, 0); a.fop(SX_F_SUB, 8, 8, 12);
        a.addi(5, 5, -1);
        a.bnei(5, 0, top);
        a.halt();
    }, 7, 0});
    return v;
}

// ---- A case laid out for both machines
struct Prepared {
    std::vector<uint32_t> ramImg;
    std::vector<std::vector<uint32_t>> state;
    std::vector<uint32_t> bases;
};
static const uint32_t HALT_STUB = SX_RAM + 0xff0000;
static Prepared prepare(const Case& cs, uint32_t seed) {
    Prepared pr;
    std::vector<SxAsm> progs(cs.cores, SxAsm(SX_RAM));
    std::vector<std::pair<uint32_t, uint32_t>> data;
    for (uint32_t c = 0; c < cs.cores; c++) progs[c].base = SX_RAM + c * 0x40000;
    cs.build(progs, data, seed);
    pr.ramImg.assign((size_t)RAM_END / 4, 0);
    for (uint32_t c = 0; c < cs.cores; c++) {
        std::vector<uint32_t> w = progs[c].finish();
        memcpy(&pr.ramImg[progs[c].base / 4], w.data(), w.size() * 4);
        pr.bases.push_back(progs[c].base);
    }
    for (auto& d : data) pr.ramImg[d.first / 4] = d.second;
    // a trap nobody asked for halts the core where its handler would be: {break} at HALT_STUB
    pr.ramImg[HALT_STUB / 4] = SX_X_BREAK; pr.ramImg[HALT_STUB / 4 + 3] = SX_CTRL_NONE | 1;
    pr.state.assign(cs.cores, std::vector<uint32_t>((size_t)(24 + g_wcn / 2 + 8) * 4, 0));
    for (uint32_t c = 0; c < cs.cores; c++) {
        pr.state[c][16 * 4 + 0] = pr.bases[c];
        pr.state[c][16 * 4 + 1] = SX_ST_S;
        pr.state[c][18 * 4 + 3] = 0x9e3779b9u ^ (c * 0x85ebca6bu);
        pr.state[c][17 * 4 + 1] = HALT_STUB;
    }
    return pr;
}
// the lean tick on the RTX 5070 Laptop (isaloops, 22 October 2026, sx/tick/steps/README.md)
static double lean_of(const char* name) {
    static const struct { const char* n; double v; } L[] = {{"arith", 8.26}, {"loads", 5.70}, {"stores", 4.91}, {"adds", 8.18},
                                                            {"tight", 7.28}, {"mix", 7.22}, {"stride", 6.37}, {"floats", 6.11},
                                                            {"fmix", 5.31}};
    for (auto& l : L) if (!strcmp(l.n, name)) return l.v;
    return 0;
}

