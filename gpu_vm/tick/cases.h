// The cases of gpu_vm's tick bench (gvmbench.cpp) and of its D3D12 runner for Nsight (gvmtrace12.cpp): programs for
// both machines, assembled with gpu_vm/ref/gvm_asm.h, and the lean tick's numbers they are held against.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "../ref/gvm_ref.h"
#include "../ref/gvm_asm.h"

// ---- The bench's machine: RAM to 64 MB (32 MB of it RAM proper: the real programs' runtime has a static heap of 16 MB),
// 1 MB of ROM
static uint32_t RAM_END = 2048 * 2048 * 16;   // (128 MB for the many-core cases: RAM_MC)
static const uint32_t ROM_SIZE = 1u << 20;
static uint32_t g_texw = 2048;   // the RAM's and ROM's textures' width: a step's RAM_W (x16 on: 8192)
static uint32_t ram_rows() { return RAM_END / 16 / g_texw; }
static uint32_t rom_rows() { return ROM_SIZE / 16 / g_texw; }
static const uint32_t DATA = GVM_RAM + 0x100000;   // a case's data
static uint32_t g_wcn = 128;
static uint32_t g_twc = 24;    // the state's first write cache texel: a step's T_WC (x17 on: 40)
static int g_loop_n = 3000000;   // a timed loop's iterations (a check runs a few thousand)
// The timed loops with the fused control words (gpu_vm/isa.h): 1 addbnz, 2 addbnz and the control word's add; GVM_FUSED=N
// in the environment, or gvmbench --fused N. 0, every loop is as it was. The score stays the RISC-V loop's
// instructions; the loops are counted in bundles then (a loop of addbnz alone has no ops).
static int env_int(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr; size_t len = 0;
    if (_dupenv_s(&v, &len, name) || !v) return 0;
    int r = atoi(v); free(v); return r;
#else
    const char* v = getenv(name); return v ? atoi(v) : 0;
#endif
}
static int g_fused = env_int("GVM_FUSED");

struct Case {
    const char* name;
    uint32_t cores;
    bool timed;
    std::function<void(std::vector<GvmAsm>&, std::vector<std::pair<uint32_t, uint32_t>>&, uint32_t seed)> build;
    double ops_per_iter = 0;   // for a timed case: gpu_vm ops an iteration of its loop
    double rv_per_iter = 0;    // and the RISC-V instructions isaloops.c spends on the same iteration
    double tol = 0;            // registers that may differ by this much, relative (the card's special functions)
    double bundles_fused = 0;  // a timed case under g_fused: bundles an iteration of its loop
    bool named = false;        // run only when --case names it (the fused ops: a tick without them is not wrong)
    int ext_from = -1, ext_to = -1;   // core 0's external interrupt line is on in passes [ext_from, ext_to) of a check
    uint32_t check_ticks = 61;        // a check's bundles a pass (x19: long ones to fill the write cache)
    // a real program (an ELF of build/tick_real, gpu_vm/tick/real/build.sh) in place of build: timed in bundles a second,
    // its words `pokes` set in the image ("symbol=check,timed;..."): a check runs a short piece of it to its first call to
    // the host (a trap, which halts), a timing a long one
    const char* elf = nullptr;
    const char* pokes = "";
    bool ns = false;   // timed in ns a bundle (a real program, a probe of one kind of bundle)
    // many cores (x19): core c runs a copy of the case named by the c-th name of this list (round robin), each copy
    // at a place of its own (the real programs built -fPIE, the probes' code), g_mc_cores cores; timed in ns a
    // bundle of all cores together
    const char* mc = nullptr;
    bool us_pass = false;   // timed in us a pass (the p_ cases: how a pass's time grows with the cores that run in it)
    // phase 2 (isa.h GVM_M_FREAD): the case uses the core's file (at least xr_need words), bindings, pairs (at least
    // pair_need a pass); a tick without them skips it
    uint32_t xr_need = 0, pair_need = 0;
    bool bind_need = false;
    bool vec_need = false;   // phase 3: the 128-bit registers' vector ops (isa.h GVM_X_VADD ...)

    // the game bench (gamebench.h): a set's name (gvmbench --set NAME), 128 MB of RAM, registers a check does not
    // compare (time-dependent: polls, sinks) and the ops count left out, timed in units of core 0's register units_reg
    // a second, unit_div of them a frame, over `passes` passes (0: gvmbench's --passes)
    const char* set = nullptr;
    bool big = false;
    uint64_t skip_regs = 0;
    bool skip_ops = false;
    int units_reg = 0;
    double unit_div = 1;
    uint32_t passes = 0;
};
static uint32_t g_mc_cores = 32;   // (gvmbench --cores N)

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
static void seed_regs(GvmAsm& a, Rng& g, bool floats) {
    for (uint32_t k = 5; k <= 28; k++) a.li(k, (int32_t)(floats && (k & 1) ? rfloat(g) : rval(g)));
}
// the integer ops, every one of them
static void fuzz_int(GvmAsm& a, Rng& g, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t rd = rreg(g), ra = rreg(g), rb = rreg(g), rc = rreg(g);
        switch (g.below(7)) {
        case 0: case 1: a.op(g.below(GVM_I_COMMON_LAST + 1), rd, ra, rb); break;
        case 2: a.opi(g.below(GVM_I_COMMON_LAST + 1), rd, ra, rimm(g)); break;
        case 3: {
            uint32_t f = GVM_I_SHADD + g.below(6);
            a.R(GVM_OP_I(f), rd, ra, rb, f == GVM_I_SHADD ? g.below(64) : rc);
            break;
        }
        case 4: a.bfe(rd, ra, g.below(32), 1 + g.below(32), g.below(2) != 0); break;
        case 5: a.x(GVM_X_MULH + g.below(GVM_X_BREV - GVM_X_MULH + 1), rd, ra, rb); break;
        default: a.xi(GVM_X_BFI, rd, ra, (int32_t)(g.below(32) | g.below(32) << 5)); break;
        }
    }
}
// the floats a check can hold to the bit (the special functions and division are the card's own)
static void fuzz_float(GvmAsm& a, Rng& g, int n) {
    static const uint32_t exact[] = {GVM_F_ADD, GVM_F_SUB, GVM_F_MUL, GVM_F_MIN, GVM_F_MAX, GVM_F_MAD, GVM_F_MSB, GVM_F_NMSB, GVM_F_EQ, GVM_F_LT,
                                     GVM_F_LE, GVM_F_SGNJ, GVM_F_SGNJN, GVM_F_SGNJX, GVM_F_UNORD, GVM_F_ITOF, GVM_F_UTOF, GVM_F_FTOI, GVM_F_FTOU,
                                     GVM_F_FLOOR, GVM_F_CEIL, GVM_F_TRUNC, GVM_F_ROUND};
    for (int i = 0; i < n; i++) {
        uint32_t f = exact[g.below(sizeof exact / 4)], rd = rreg(g), ra = rreg(g), rb = rreg(g), rc = rreg(g);
        if (g.below(4) == 0 && f != GVM_F_MAD && f != GVM_F_MSB && f != GVM_F_NMSB) { uint32_t v = rfloat(g); float fv; memcpy(&fv, &v, 4); a.fopi(f, rd, ra, fv); }
        else a.fop(f, rd, ra, rb, rc);
    }
}
// a width's bytes (gpu_vm/isa.h), and a store's width for a load's (the unsigned forms are the signed ones' stores)
static uint32_t wbytes(uint32_t wd) { static const uint32_t b[8] = {4, 8, 2, 1, 16, 0, 2, 1}; return b[wd & 7]; }
static uint32_t wstore(uint32_t wd) { return wd == GVM_W_BU ? GVM_W_B : wd == GVM_W_HU ? GVM_W_H : wd; }
static void fuzz_mem(GvmAsm& a, Rng& g, int n, uint32_t window) {   // r29 is the window's base
    static const uint32_t wds[7] = {GVM_W_B, GVM_W_H, GVM_W_W, GVM_W_D, GVM_W_BU, GVM_W_HU, GVM_W_Q};
    for (int i = 0; i < n; i++) {
        uint32_t rd = rreg(g), wd = wds[g.below(7)], by = wbytes(wd);
        uint32_t off = g.below(window - 16);
        if (g.below(3)) off &= ~(by - 1);   // mostly aligned: the fast path's
        if (by >= 8) rd = 5 + g.below(20);
        switch (g.below(5)) {
        case 0: case 1: a.ld(wd, rd, 29, (int32_t)off); break;
        case 2: case 3: if (wd != GVM_W_BU && wd != GVM_W_HU) a.st(wd, rd, 29, (int32_t)off); else a.ld(wd, rd, 29, (int32_t)off); break;
        default: {   // an atomic on an aligned word
            a.li(30, (int32_t)(off & ~3u));
            a.add(30, 30, 29);
            a.amo(g.below(8), rd, 30, rreg(g));
        }
        }
    }
}

// a timed loop's count in r5, and its end: `addi r5, -1; bnei r5, 0` (the branch sees r5 as the bundle found it: n + 1
// iterations), or fused one addbnz (the sum: n + 1 iterations from n + 1)
static int32_t loop_count(int n) { return g_fused ? n + 1 : n; }
static void loop_end(GvmAsm& a, int top) {
    if (g_fused) a.addbnz(5, -1, top);
    else { a.addi(5, 5, -1); a.bnei(5, 0, top); }
}
#include "gamebench.h"
static std::vector<Case> cases() {
    std::vector<Case> v;
    v.push_back({"int", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        Rng g{seed * 2654435761ull + 1};
        GvmAsm& a = p[0];
        for (int round = 0; round < 40; round++) { seed_regs(a, g, false); fuzz_int(a, g, 60); }
        a.halt();
    }});
    v.push_back({"float", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        Rng g{seed * 0x9e3779b97f4a7c15ull + 3};
        GvmAsm& a = p[0];
        for (int round = 0; round < 40; round++) { seed_regs(a, g, true); fuzz_float(a, g, 60); }
        a.halt();
    }});
    v.push_back({"mem", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        Rng g{seed * 0x2545F4914F6CDD1Dull + 5};
        for (uint32_t k = 0; k < 2048; k++) data.push_back({DATA + 4 * k, g.next()});
        GvmAsm& a = p[0];
        a.li(29, (int32_t)DATA);
        for (int round = 0; round < 30; round++) { seed_regs(a, g, false); a.li(29, (int32_t)DATA); fuzz_mem(a, g, 50, 8192); }
        a.halt();
    }});
    v.push_back({"memw", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // loads and stores of bytes, halves and words, aligned, in a window, between sums (step x7 on)
        Rng g{seed * 31337 + 9};
        for (uint32_t k = 0; k < 1024; k++) data.push_back({DATA + 4 * k, g.next()});
        GvmAsm& a = p[0];
        for (int round = 0; round < 30; round++) {
            seed_regs(a, g, false);
            a.li(29, (int32_t)DATA);
            for (int i = 0; i < 60; i++) {
                static const uint32_t wds[6] = {GVM_W_B, GVM_W_H, GVM_W_W, GVM_W_W, GVM_W_BU, GVM_W_HU};
                uint32_t wd = wds[g.below(6)], by = wbytes(wd);
                uint32_t off = g.below(4096 - 4) & ~(by - 1), rd = rreg(g);
                if (g.below(2)) a.ld(wd, rd, 29, (int32_t)off); else a.st(wstore(wd), rd, 29, (int32_t)off);
                if (g.below(2)) a.addi(rreg(g), rreg(g), rimm(g));
            }
        }
        a.halt();
    }});
    v.push_back({"rom", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // loads of every width from the ROM (r29 its base) beside loads and stores of RAM (r28), in pairs (step x16 on)
        Rng g{seed * 6151 + 13};
        for (uint32_t k = 0; k < 1024; k++) data.push_back({DATA + 4 * k, g.next()});
        static const uint32_t wds[] = {GVM_W_W, GVM_W_H, GVM_W_B, GVM_W_HU, GVM_W_BU};
        GvmAsm& a = p[0];
        for (int round = 0; round < 30; round++) {
            seed_regs(a, g, false);
            a.li(29, (int32_t)GVM_ROM); a.li(28, (int32_t)DATA);
            for (int i = 0; i < 60; i++) {
                uint32_t wd = wds[g.below(5)], by = wd == GVM_W_W ? 4 : (wd == GVM_W_B || wd == GVM_W_BU) ? 1 : 2;
                uint32_t rom = g.below(ROM_SIZE / 2) & ~(by - 1);   // (past imm12: through a register)
                a.li(27, (int32_t)rom); a.add(27, 27, 29);
                a.ld(wd, 5 + g.below(22), 27, (int32_t)(g.below(64) & ~(by - 1)));
                switch (g.below(3)) {
                case 0: a.lw(5 + g.below(22), 28, (int32_t)(4 * g.below(1024))); break;
                case 1: a.sw(5 + g.below(22), 28, (int32_t)(4 * g.below(1024))); break;
                default: a.ld(wd, 5 + g.below(22), 29, (int32_t)(g.below(2047) & ~(by - 1))); break;
                }
            }
        }
        a.halt();
    }});
    v.push_back({"spillw", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // more distinct words stored than the write cache holds, words, halves and bytes (step x7 on)
        GvmAsm& a = p[0];
        a.li(5, (int32_t)DATA); a.li(6, 0); a.li(7, 3000);
        int top = a.here();
        a.R(GVM_OP_M(GVM_M_STOREX, GVM_W_W), 6, 5, 6, 2); a.addi(6, 6, 1);
        a.bne(6, 7, top);
        a.li(6, 0); a.li(8, 0);
        int sum = a.here();
        a.R(GVM_OP_M(GVM_M_LOADX, GVM_W_W), 9, 5, 6, 2); a.addi(6, 6, 1);
        a.add(8, 8, 9);
        a.bne(6, 7, sum);
        a.li(6, 0); a.li(10, (int32_t)(DATA + 0x10000));
        int bytes = a.here();
        a.R(GVM_OP_M(GVM_M_STOREX, GVM_W_B), 6, 10, 6, 0); a.addi(11, 6, 3);
        a.R(GVM_OP_M(GVM_M_STOREX, GVM_W_H), 11, 10, 6, 2); a.addi(6, 6, 1);
        a.bne(6, 7, bytes);
        a.halt();
    }});
    v.push_back({"fpaths", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // floats in every path: float with float, float with an integer op, loads and stores beside float sums
        Rng g{seed * 7727 + 3};
        for (uint32_t k = 0; k < 1024; k++) { uint32_t u = rfloat(g); data.push_back({DATA + 4 * k, u}); }
        static const uint32_t lite_f[] = {GVM_F_ADD, GVM_F_MUL, GVM_F_MAD, GVM_F_MSB, GVM_F_SUB, GVM_F_NMSB, GVM_F_MIN, GVM_F_MAX};
        static const uint32_t any_f[] = {GVM_F_ADD, GVM_F_MUL, GVM_F_MAD, GVM_F_SUB, GVM_F_MIN, GVM_F_MAX, GVM_F_EQ, GVM_F_LT, GVM_F_LE,
                                         GVM_F_UNORD, GVM_F_SGNJ, GVM_F_SGNJN, GVM_F_SGNJX, GVM_F_FRAC, GVM_F_ITOF, GVM_F_UTOF, GVM_F_FTOI,
                                         GVM_F_FTOU, GVM_F_FLOOR, GVM_F_CEIL, GVM_F_TRUNC, GVM_F_ROUND};
        GvmAsm& a = p[0];
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
    v.push_back({"sfu", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // division and the special functions on r5-r16 into r17-r28 (no result is used again): within 2e-5
        Rng g{seed * 104729 + 7};
        GvmAsm& a = p[0];
        for (int round = 0; round < 30; round++) {
            for (uint32_t k = 5; k <= 16; k++) {
                float f = (float)((int32_t)g.below(20001) - 10000) / 100.0f;
                if (g.below(8) == 0) f = (float)(g.below(1000)) * 1e-3f;
                uint32_t u; memcpy(&u, &f, 4);
                a.li(k, (int32_t)u);
            }
            for (int i = 0; i < 24; i++) a.fop(GVM_F_DIV + g.below(8), 17 + g.below(12), 5 + g.below(12), 5 + g.below(12));
        }
        a.halt();
    }, 0, 0, 2e-5});
    v.push_back({"spill", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // more distinct words stored than the write cache holds: passes end on a full cache, and go on
        GvmAsm& a = p[0];
        a.li(5, (int32_t)DATA); a.li(6, 0); a.li(7, 3000);
        int top = a.here();
        a.R(GVM_OP_M(GVM_M_STOREX, GVM_W_W), 6, 5, 6, 2); a.addi(6, 6, 1);
        a.bne(6, 7, top);
        a.li(6, 0); a.li(8, 0);
        int sum = a.here();
        a.R(GVM_OP_M(GVM_M_LOADX, GVM_W_W), 9, 5, 6, 2); a.add(8, 8, 9); a.addi(6, 6, 1);
        a.bne(6, 7, sum);
        // and bytes, halves and texels, as many again
        a.li(6, 0); a.li(10, (int32_t)(DATA + 0x10000));
        int bytes = a.here();
        a.R(GVM_OP_M(GVM_M_STOREX, GVM_W_B), 6, 10, 6, 0); a.R(GVM_OP_M(GVM_M_STOREX, GVM_W_H), 6, 10, 6, 2);
        a.st(GVM_W_Q, 5, 10, 0x8000); a.addi(10, 10, 16); a.addi(6, 6, 1);
        a.bne(6, 7, bytes);
        a.halt();
    }});
    v.push_back({"conds", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // every condition, on random values, both ways, with sums alone (step x1 on): r20 counts what was taken
        Rng g{seed + 177};
        GvmAsm& a = p[0];
        for (int i = 0; i < 300; i++) {
            a.li(5, (int32_t)rval(g)); a.li(6, (int32_t)(g.below(2) ? rval(g) : 0));
            if (g.below(2)) a.mv(6, 5);
            uint32_t cc = GVM_CC_BLTU + g.below(14);   // (2-15: every condition)
            int skip = a.label();
            a.br(cc, 5, cc < GVM_CC_BBS ? 6 : g.below(64), skip);
            a.addi(20, 20, 1); a.addi(21, 21, i);
            a.bind(skip);
            a.addi(22, 22, 3);
        }
        a.halt();
    }});
    v.push_back({"ints", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // the integer unit's 24 functions, R and I forms, on random values (step x2 on)
        Rng g{seed * 7919 + 11};
        GvmAsm& a = p[0];
        for (int round = 0; round < 40; round++) {
            seed_regs(a, g, false);
            for (int i = 0; i < 60; i++) {
                if (g.below(2)) a.op(g.below(GVM_I_COMMON_LAST + 1), rreg(g), rreg(g), rreg(g));
                else a.opi(g.below(GVM_I_COMMON_LAST + 1), rreg(g), rreg(g), rimm(g));
            }
        }
        a.halt();
    }});
    v.push_back({"branch", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        Rng g{seed + 77};
        GvmAsm& a = p[0];
        // every condition, on random values, both ways: r20 counts what was taken
        for (int i = 0; i < 200; i++) {
            a.li(5, (int32_t)rval(g)); a.li(6, (int32_t)(g.below(2) ? rval(g) : 0));
            if (g.below(2)) a.mv(6, 5);
            uint32_t cc = GVM_CC_BLTU + g.below(14);
            int skip = a.label();
            uint32_t bfield = cc < GVM_CC_BBS ? 6 : g.below(64);
            a.br(cc, 5, bfield, skip);
            a.addi(20, 20, 1);
            a.opi(GVM_I_XOR, 21, 21, (int32_t)i);
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
        a.R(GVM_OP_I(GVM_I_ADD), 11, 9, 0);
        a.callr(9);
        a.j(done);
        a.bind(after);
        a.addi(23, 23, 100);
        a.ret();
        a.bind(fn);
        a.op(GVM_I_ADD, 24, 24, 7); a.opi(GVM_I_SLL, 25, 24, 1);
        a.ret();
        a.bind(done);
        a.halt();
    }});
    v.push_back({"trap", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // syscalls, an illegal op, a load fault, a store to the boot area, and the timer's interrupt; the
        // handler counts causes in r40-r47 and returns past the op (a syscall, a fault) or to it (an interrupt)
        GvmAsm& a = p[0];
        int handler = a.label(), go = a.label();
        a.lpc(5, handler); a.mtsr(GVM_SR_TVEC, 5);
        a.j(go);
        a.bind(handler);
        a.mfsr(50, GVM_SR_CAUSE); a.mfsr(51, GVM_SR_EPC); a.mfsr(52, GVM_SR_TVAL);
        a.opi(GVM_I_AND, 53, 50, 7); a.shadd(54, 0, 53, 0);   // r54 = cause & 7
        a.addi(40, 40, 1);
        int irq = a.label(), back = a.label();
        a.br(GVM_CC_BBS, 50, 31, irq);
        a.addi(51, 51, 4); a.mtsr(GVM_SR_EPC, 51);           // past the op
        a.op(GVM_I_ADD, 41, 41, 54);
        a.j(back);
        a.bind(irq);
        a.li(55, 0); a.mtsr(GVM_SR_TCMP_LO, 55); a.mtsr(GVM_SR_TCMP_HI, 55);   // the timer off
        a.addi(42, 42, 1);
        a.bind(back);
        a.eret();
        a.bind(go);
        a.li(6, 1); a.x(GVM_X_SYSCALL); a.addi(6, 6, 10);
        a.x(GVM_X_SYSCALL);
        a.put(0x3fu << GVM_OPC_SHIFT); a.addi(7, 7, 1);      // opc 0x3f: illegal
        a.li(8, 0x7000000); a.lw(9, 8, 0); a.addi(10, 10, 1); // a load past RAM's end
        a.li(8, 0x100); a.sw(9, 8, 0);                        // a store to the boot area
        a.li(11, 1); a.mtsr(GVM_SR_TCMP_LO, 11); a.li(11, 2); a.mtsr(GVM_SR_IEN, 11);
        a.li(11, GVM_ST_IE | GVM_ST_PIE); a.mtsr(GVM_SR_STATUS, 11);    // the timer is due: taken at the next bundle
        a.addi(12, 12, 1); a.close();
        a.addi(12, 12, 1); a.close();
        a.halt();
    }});
    v.push_back({"ops", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // copy, fill and blit through the operation region, then a sum of the destination
        Rng g{seed + 991};
        for (uint32_t k = 0; k < 0x20000; k++) data.push_back({DATA + 4 * k, g.next()});
        GvmAsm& a = p[0];
        a.li(20, (int32_t)GVM_OPS);
        auto op = [&](uint32_t kind, uint32_t dst, uint32_t src, uint32_t n) {
            a.li(5, (int32_t)kind); a.li(6, (int32_t)dst); a.li(7, (int32_t)src); a.li(8, (int32_t)n);
            a.st(GVM_W_Q, 5, 20, 0); a.x(GVM_X_SYNC);
            a.lw(9, 20, 4); a.op(GVM_I_ADD, 30, 30, 9);   // the refused, so far
        };
        op(GVM_OP_COPY, DATA + 0x40003, DATA + 0x1001, 5000);
        op(GVM_OP_COPY, DATA + 0x2008, DATA + 0x2000, 3000);   // overlapping, the destination after the source
        op(GVM_OP_COPY, DATA + 0x3000, DATA + 0x3005, 3000);   // the destination before
        op(GVM_OP_FILL, DATA + 0x50001, 0xa1b2c3d4u, 777);
        op(GVM_OP_COPY, DATA + 0x60000, GVM_ROM + 0x10, 4000);  // from the ROM
        op(GVM_OP_COPY, GVM_RAM + 0x400007, DATA + 1, 600000);  // more than a row of 256 KB
        op(GVM_OP_COPY, 0x100, DATA, 16);                      // refused: the boot area
        // a blit: 37 rows of 50 bytes, strides 64 and 80
        a.li(5, GVM_OP_BLIT); a.li(6, (int32_t)(DATA + 0x70003)); a.li(7, (int32_t)(DATA + 0x8000)); a.li(8, 0);
        a.li(10, 64); a.li(11, 80); a.li(12, 50); a.li(13, 37);
        a.st(GVM_W_Q, 10, 20, 16); a.st(GVM_W_Q, 5, 20, 0); a.x(GVM_X_SYNC);
        a.lw(9, 20, 0); a.op(GVM_I_ADD, 30, 30, 9);
        a.halt();
    }});
    for (int adds = 0; adds < 2; adds++) v.push_back({adds ? "fused" : "addbnz", 1, false, [adds](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // the fused control words (gpu_vm/isa.h): the control word's add (rs += imm12 after the ops; "fused" only, an
        // addi in "addbnz") beside integer, memory and float ops, addbnz both ways on random values, counted loops of
        // loads and stores through bumped pointers, and stores through one past the write cache (passes end inside)
        Rng g{seed * 6700417 + 13};
        for (uint32_t k = 0; k < 2048; k++) data.push_back({DATA + 4 * k, g.next()});
        GvmAsm& a = p[0];
        auto bump = [&](uint32_t r, int32_t i) { if (adds) a.bump(r, i); else a.addi(r, r, i); };
        for (int round = 0; round < 30; round++) {
            seed_regs(a, g, round & 1);
            a.li(29, (int32_t)DATA); a.li(30, (int32_t)(DATA + 4 * g.below(1024)));
            for (int i = 0; i < 40; i++) {
                switch (g.below(8)) {
                case 0: a.op(g.below(GVM_I_COMMON_LAST + 1), rreg(g), rreg(g), rreg(g)); break;
                case 1: a.opi(g.below(GVM_I_SRA + 1), rreg(g), rreg(g), rimm(g)); break;
                case 2: a.lw(rreg(g), 30, 4 * (int32_t)g.below(64)); break;
                case 3: {   // bytes, halves, words, aligned
                    static const uint32_t wds[] = {GVM_W_B, GVM_W_H, GVM_W_W, GVM_W_BU, GVM_W_HU};
                    uint32_t wd = wds[g.below(5)], by = wbytes(wd);
                    a.ld(wd, rreg(g), 29, (int32_t)(g.below(4000) & ~(by - 1)));
                    break;
                }
                case 4: a.sw(rreg(g), 30, 4 * (int32_t)g.below(64)); break;
                case 5: a.fop(g.below(GVM_F_MAX + 1), rreg(g), rreg(g), rreg(g), rreg(g)); break;
                case 6: {   // addbnz forward, taken or not: the register sometimes made -imm
                    uint32_t r = rreg(g);
                    int32_t im = (int32_t)g.below(64) - 32;
                    if (g.below(2)) a.li(r, -im + (int32_t)g.below(3) - 1);
                    int skip = a.label();
                    a.addbnz(r, im, skip);
                    a.addi(20, 20, 1); a.opi(GVM_I_XOR, 21, 21, i);
                    a.bind(skip);
                    break;
                }
                default: {   // the control word's add: a pointer, a counter, r0 (an addi: the add has no r0)
                    uint32_t r = g.below(3) == 0 ? 30 : g.below(10) == 0 ? 0 : rreg(g);
                    bump(r, r == 30 ? 4 * ((int32_t)g.below(5) - 2) : rimm(g) % 2048);
                    break;
                }
                }
            }
            a.li(30, (int32_t)DATA);   // (r30 back in the window)
        }
        // a counted loop: sum and copy through two bumped pointers, by addbnz -1 and -3
        a.li(30, (int32_t)DATA); a.li(31, (int32_t)(DATA + 0x2000)); a.li(32, 37); a.li(21, 0);
        int l1 = a.here();
        a.lw(6, 30, 0); a.add(21, 21, 6); bump(30, 4);
        a.sw(21, 31, 0); a.opi(GVM_I_XOR, 22, 22, 6); bump(31, 4);
        a.addbnz(32, -1, l1);
        a.li(32, 3 * 25);
        int l2 = a.here();
        a.lw(7, 30, 0); a.lw(8, 30, 4); bump(30, 8);
        a.add(23, 23, 7); a.sub(24, 24, 8);
        a.addbnz(32, -3, l2);
        // stores through a bumped pointer, two a bundle and one, past the write cache
        a.li(33, (int32_t)(DATA + 0x10000)); a.li(34, 1500); a.li(6, 0);
        int l3 = a.here();
        a.sw(6, 33, 0); a.sw(34, 33, 4); bump(33, 8);
        a.addi(6, 6, 3); a.st(GVM_W_B, 6, 33, 0x4000);
        a.addbnz(34, -1, l3);
        a.li(34, 700);
        int l4 = a.here();
        a.sw(34, 33, 0); bump(33, 4);
        a.addbnz(34, -1, l4);   // (a bundle of no ops)
        a.halt();
    }});
    v[v.size() - 2].named = v[v.size() - 1].named = true;
    // ---- x19: stores at random words of 1 MB in long passes, so that a set's two ways fill and words go to the overflow
    // list; loads read back the last stores (a store and a load in one bundle, path 6), a byte into a stored word
    // random stores (wcov's loop; core c in a megabyte of its own from DATA + c MB, n iterations)
    auto wcov_core = [](GvmAsm& a, uint32_t c, uint32_t n, uint32_t seed) {
        a.li(5, (int32_t)n); a.li(4, (int32_t)(DATA + c * 0x100000)); a.li(9, 1103515245); a.li(10, 0xffffc); a.li(6, (int32_t)((seed + c) * 2654435761u));
        a.li(11, (int32_t)(DATA + c * 0x100000)); a.li(13, (int32_t)(DATA + c * 0x100000)); a.li(12, 0);
        int top = a.here();
        a.op(GVM_I_MUL, 6, 6, 9); a.close();
        a.addi(6, 6, 12345); a.close();
        a.opi(GVM_I_SRL, 7, 6, 9); a.close();
        a.op(GVM_I_AND, 7, 7, 10); a.close();
        a.add(7, 7, 4); a.close();
        a.sw(6, 7, 0); a.lw(8, 11, 0); a.close();      // (path 6: a store, then a load of the last one's word)
        a.add(12, 12, 8); a.close();
        a.st(GVM_W_B, 12, 7, 1); a.lw(14, 13, 0); a.close();   // a byte into this word; a load of the word before the last
        a.add(12, 12, 14); a.mv(13, 11); a.close();
        a.mv(11, 7); a.close();
        a.lw(15, 7, 0); a.close();                     // (path 3: this word, as just stored)
        a.add(12, 12, 15); a.close();
        a.addbnz(5, -1, top);
        a.halt();
    };
    v.push_back({"wcov", 1, false, [wcov_core](std::vector<GvmAsm>& p, auto& data, uint32_t seed) { wcov_core(p[0], 0, 1500, seed); }});
    v.back().check_ticks = 3000;
    // x25: core 0's cache full in long passes, and four cores' (a worker's cache full, its move on insertion)
    v.push_back({"wcovl", 1, false, [wcov_core](std::vector<GvmAsm>& p, auto& data, uint32_t seed) { wcov_core(p[0], 0, 6000, seed); }});
    v.back().check_ticks = 8192;
    v.push_back({"wcov4", 4, false, [wcov_core](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        for (uint32_t c = 0; c < 4; c++) wcov_core(p[c], c, 2000, seed);
    }});
    v.back().check_ticks = 3000;
    // ---- x17's: the whole ISA (gpu_vm/tick/steps/README.md, "x17")
    v.push_back({"memx", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // d and q at any alignment, bytes, halves and words off their alignment (each stays in its word), atomics and
        // cas, d and q from the ROM, faults (past RAM's end, 0xfffffffc, the boot area, an atomic off its word: the
        // handler counts them in r40-r42 and goes on past the op), the operations' region read by d and q and latched
        Rng g{seed * 2246822519ull + 21};
        for (uint32_t k = 0; k < 2048; k++) data.push_back({DATA + 4 * k, g.next()});
        GvmAsm& a = p[0];
        int handler = a.label(), go = a.label();
        a.lpc(5, handler); a.mtsr(GVM_SR_TVEC, 5);
        a.j(go);
        a.bind(handler);
        a.mfsr(50, GVM_SR_CAUSE); a.mfsr(51, GVM_SR_EPC); a.mfsr(52, GVM_SR_TVAL);
        a.addi(40, 40, 1); a.add(41, 41, 50); a.op(GVM_I_XOR, 42, 42, 52);
        a.addi(51, 51, 4); a.mtsr(GVM_SR_EPC, 51);
        a.eret();
        a.bind(go);
        static const uint32_t wds[] = {GVM_W_B, GVM_W_H, GVM_W_W, GVM_W_D, GVM_W_Q, GVM_W_BU, GVM_W_HU};
        for (int round = 0; round < 24; round++) {
            seed_regs(a, g, false);
            a.li(60, (int32_t)DATA); a.li(61, (int32_t)GVM_ROM); a.li(62, (int32_t)GVM_OPS);
            for (int i = 0; i < 40; i++) {
                uint32_t wd = wds[g.below(7)], by = wbytes(wd), rd = by >= 8 ? 5 + g.below(16) : rreg(g);
                int32_t off = (int32_t)g.below(8000);
                switch (g.below(12)) {
                case 0: case 1: case 2: a.ld(wd, rd, 60, off); break;
                case 3: case 4: case 5: a.st(wstore(wd), rd, 60, off); break;
                case 6:   // indexed, r0 the index: an address off its alignment now and then
                    a.R(GVM_OP_M(g.below(2) ? GVM_M_LOADX : GVM_M_STOREX, wstore(wd)), rd, 60, 0, 0);
                    a.addi(60, 60, (int32_t)g.below(7) - 3);
                    break;
                case 7: {   // an atomic on a word (now and then off it: a store fault)
                    a.li(30, (int32_t)((uint32_t)off & (g.below(6) ? ~3u : ~0u))); a.add(30, 30, 60);
                    if (g.below(4)) a.amo(g.below(8), rd, 30, rreg(g));
                    else a.R(GVM_OP_M(GVM_M_AMO2, g.below(2)), rd, 30, rreg(g));
                    break;
                }
                case 8: a.ld(g.below(2) ? GVM_W_D : GVM_W_Q, 5 + g.below(16), 61, (int32_t)g.below(2000)); break;
                case 9: {   // faults
                    static const uint32_t bad[] = {0x7000000u, 0xfffffffcu, 0x100u, 0x0fffcu, 0x2fffff0u, 0x60000000u};
                    a.li(30, (int32_t)bad[g.below(6)]);
                    if (g.below(2)) a.ld(wds[g.below(7)], 5 + g.below(16), 30, 0); else a.st(wstore(wds[g.below(5)]), 5 + g.below(16), 30, 0);
                    break;
                }
                case 10: a.ld(g.below(2) ? GVM_W_D : GVM_W_Q, 5 + g.below(16), 62, 0); break;   // in flight, refused, latched
                default: a.st(g.below(2) ? GVM_W_D : GVM_W_W, 5 + g.below(16), 62, 8 + 4 * (int32_t)g.below(5)); break;   // latched
                }
            }
        }
        a.ld(GVM_W_Q, 5, 62, 0); a.ld(GVM_W_Q, 9, 62, 16);
        a.halt();
    }});
    v.push_back({"irq", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // TIME is 1000 + the pass. The timer (TCMP a few passes on), the software interrupt, the external line (on in
        // passes 50-51), a supervisor's wait woken by the timer, the special registers, then user mode (eret with PS
        // clear): its privileged ops trap, a user's wait ends the pass, a syscall traps, eret traps, and its break halts
        GvmAsm& a = p[0];
        int handler = a.label(), go = a.label(), irq = a.label(), back = a.label(), notimer = a.label(), nosoft = a.label();
        a.lpc(5, handler); a.mtsr(GVM_SR_TVEC, 5);
        a.j(go);
        a.bind(handler);   // r40 traps, r41 causes summed, r42-r44 timer, soft, external
        a.mfsr(50, GVM_SR_CAUSE); a.mfsr(51, GVM_SR_EPC);
        a.addi(40, 40, 1); a.add(41, 41, 50); a.op(GVM_I_XOR, 45, 45, 51);
        a.br(GVM_CC_BBS, 50, 31, irq);
        a.addi(51, 51, 4); a.mtsr(GVM_SR_EPC, 51);
        a.j(back);
        a.bind(irq);
        a.opi(GVM_I_AND, 52, 50, 7);
        a.bnei(52, 1, notimer);
        a.addi(42, 42, 1); a.li(53, 0); a.mtsr(GVM_SR_TCMP_LO, 53);   // the timer off
        a.j(back);
        a.bind(notimer);
        a.bnei(52, 0, nosoft);
        a.addi(43, 43, 1); a.li(53, 0); a.mtsr(GVM_SR_IPEND, 53);    // the software bit cleared
        a.j(back);
        a.bind(nosoft);
        a.addi(44, 44, 1); a.li(53, 4); a.I(GVM_X_SRCLR, 54, 53, GVM_SR_IEN);   // the external line masked
        a.bind(back);
        a.eret();
        a.bind(go);
        a.li(5, 7); a.mtsr(GVM_SR_IEN, 5); a.li(5, GVM_ST_IE); a.mtsr(GVM_SR_STATUS, 5);
        // the special registers, into r20-r36
        a.mfsr(20, GVM_SR_STATUS); a.mfsr(21, GVM_SR_COREID); a.mfsr(22, GVM_SR_PLAYER); a.mfsr(23, GVM_SR_RANDOM);
        a.mfsr(24, GVM_SR_RANDOM); a.mfsr(25, GVM_SR_CYCLE); a.mfsr(26, GVM_SR_OPS); a.mfsr(27, GVM_SR_LEFT);
        a.li(6, 0x1234); a.I(GVM_X_XSR, 28, 6, GVM_SR_SCRATCH); a.li(6, 0xf0); a.I(GVM_X_SRSET, 29, 6, GVM_SR_SCRATCH);
        a.li(6, 0x30); a.I(GVM_X_SRCLR, 30, 6, GVM_SR_SCRATCH); a.mfsr(31, GVM_SR_SCRATCH);
        a.li(6, 0xffff); a.mtsr(GVM_SR_STATUS, 6); a.mfsr(32, GVM_SR_STATUS);   // (S stays, the rest as written)
        a.li(6, GVM_ST_IE); a.mtsr(GVM_SR_STATUS, 6);
        a.mfsr(33, GVM_SR_TIME_LO); a.mfsr(34, GVM_SR_TIME_HI); a.mfsr(35, GVM_SR_IPEND); a.mfsr(36, GVM_SR_IEN);
        // the timer, 3 passes on, while a loop sums
        a.addi(7, 33, 3); a.mtsr(GVM_SR_TCMP_LO, 7);
        a.li(8, 400);
        int l1 = a.here();
        a.addi(9, 9, 3); a.op(GVM_I_XOR, 10, 10, 9); a.mfsr(11, GVM_SR_CYCLE); a.add(12, 12, 11);
        a.addbnz(8, -1, l1);
        // the software interrupt: taken before the next whole bundle
        a.li(6, 1); a.mtsr(GVM_SR_IPEND, 6); a.addi(13, 13, 1); a.close();
        a.addi(13, 13, 1);
        // a supervisor's wait woken by the timer, 2 passes on
        a.mfsr(7, GVM_SR_TIME_LO); a.addi(7, 7, 2); a.mtsr(GVM_SR_TCMP_LO, 7);
        a.x(GVM_X_WAIT); a.mfsr(14, GVM_SR_TIME_LO);
        // spin until the external line (passes 50-51) has been taken
        int l2 = a.here();
        a.addi(15, 15, 1); a.mfsr(16, GVM_SR_LEFT); a.add(17, 17, 16);
        a.br(GVM_CC_BEQI, 44, 0, l2);
        // a wait goes on at once with an interrupt pending and enabled, IE clear
        a.li(6, 0); a.mtsr(GVM_SR_STATUS, 6); a.li(6, 1); a.mtsr(GVM_SR_IPEND, 6); a.x(GVM_X_WAIT); a.addi(18, 18, 1);
        a.li(6, 0); a.mtsr(GVM_SR_IPEND, 6);
        // user mode: EPC the user code, PS clear, IE on after eret
        int user = a.label();
        a.lpc(6, user); a.mtsr(GVM_SR_EPC, 6); a.li(6, GVM_ST_PIE); a.mtsr(GVM_SR_STATUS, 6);
        a.eret();
        a.bind(user);
        a.mfsr(19, GVM_SR_TIME_LO); a.mfsr(37, GVM_SR_STATUS);   // TIME: user-readable; STATUS: a privilege trap
        a.li(6, 5); a.mtsr(GVM_SR_SCRATCH, 6);                    // a privilege trap
        a.x(GVM_X_WAIT); a.addi(38, 38, 1);                         // a user's wait: the pass's end
        a.x(GVM_X_SYSCALL); a.addi(38, 38, 2);
        a.eret();                                                   // in user mode: a privilege trap (EPC + 4: the next bundle)
        a.mfsr(39, GVM_SR_OPS);
        a.halt();
    }, 0, 0, 0, 0, false, 50, 52});
    v.push_back({"opq", 2, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // two cores, each with operations in flight and no sync: a fill and a copy of what it fills, other work, a load
        // of the copy's destination and a store to its source (both wait for them), the count in flight and refused
        // by ld.d, nine asked at once (the ninth waits), the latched words by ld.q, a blit whose rows overlap (a piece
        // a round), a copy backwards (its destination after its source), a refused one, and a copy longer than a pass
        Rng g{seed * 40503 + 17};
        for (uint32_t k = 0; k < 0x10000; k++) data.push_back({DATA + 4 * k, g.next()});
        for (uint32_t c = 0; c < 2; c++) {
            GvmAsm& a = p[c];
            uint32_t B = DATA + 0x100000 + c * 0x200000;   // this core's region
            auto ask = [&](uint32_t kind, uint32_t dst, uint32_t src, uint32_t n) {
                a.li(5, (int32_t)kind); a.li(6, (int32_t)dst); a.li(7, (int32_t)src); a.li(8, (int32_t)n);
                a.st(GVM_W_Q, 5, 20, 0);
            };
            a.li(20, (int32_t)GVM_OPS); a.li(21, (int32_t)B);
            ask(GVM_OP_FILL, B, 0x01020304u + c, 0x10000);
            ask(GVM_OP_COPY, B + 0x10003, B + 5, 0x8000);
            a.ld(GVM_W_D, 22, 20, 0);                     // in flight: 2, refused
            a.li(9, 300);
            int l1 = a.here();
            a.addi(10, 10, 7); a.op(GVM_I_XOR, 11, 11, 10);
            a.addbnz(9, -1, l1);
            a.lw(12, 21, 0x10100);                       // the copy's destination: waits for it
            a.li(13, 77); a.sw(13, 21, 0x40);           // the copy's source (made by now)
            a.ld(GVM_W_D, 24, 20, 0);
            for (int k = 0; k < 9; k++) ask(GVM_OP_FILL, B + 0x30000 + 0x1000 * k, 0x11111111u * (k + 1), 0x800 + 3 * k);
            a.ld(GVM_W_D, 26, 20, 0); a.ld(GVM_W_Q, 28, 20, 0);
            // a blit of 20 rows of 70 bytes, each row's source the row before's destination (strides 64)
            a.li(10, 64); a.li(11, 64); a.li(12, 70); a.li(13, 20);
            a.st(GVM_W_Q, 10, 20, 16);
            ask(GVM_OP_BLIT, B + 0x40040 + 3, B + 0x40000 + 3, 0);
            a.x(GVM_X_SYNC);
            ask(GVM_OP_COPY, B + 0x50000 + 100, B + 0x50000, 10000);   // backwards
            ask(GVM_OP_COPY, 0x200, B, 16);                              // refused: the boot area
            a.x(GVM_X_SYNC);
            a.ld(GVM_W_D, 32, 20, 0);
            ask(GVM_OP_COPY, B + 0x80000 + 1, DATA + 2, 0xa0000);         // more than a pass's rounds make
            a.li(9, 100);
            int l2 = a.here();
            a.addi(14, 14, 3); a.addbnz(9, -1, l2);
            a.lw(15, 21, 0x80000 + 0x9fff0);              // waits for all of it
            a.ld(GVM_W_D, 34, 20, 0);
            a.halt();
        }
    }});
    v.push_back({"cores", 4, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
        // four cores: each fills a region of its own, and all store to one word (the highest core's lands)
        for (uint32_t c = 0; c < 4; c++) {
            GvmAsm& a = p[c];
            a.mfsr(5, GVM_SR_COREID);
            a.li(6, (int32_t)DATA); a.sw(5, 6, 0);
            a.opi(GVM_I_SLL, 7, 5, 12); a.add(7, 7, 6); a.addi(7, 7, 0x1000);
            a.li(8, 0); a.li(9, 600);
            int top = a.here();
            a.op(GVM_I_MUL, 10, 8, 5); a.R(GVM_OP_M(GVM_M_STOREX, GVM_W_W), 10, 7, 8, 2); a.addi(8, 8, 1);
            a.bne(8, 9, top);
            a.halt();
        }
    }});
    // ---- the loops of experiments/isa_proto/lean/isaloops.c, as gpu_vm writes them; timed, scored in the RISC-V
    // loop's instructions (rv_per_iter), the lean tick's measure
    v.push_back({"arith", 1, true, [](std::vector<GvmAsm>& p, auto& data, uint32_t) {
        GvmAsm& a = p[0];
        a.li(5, loop_count(g_loop_n));
        int top = a.here();
        a.addi(6, 6, 1); a.op(GVM_I_XOR, 7, 7, 6); a.add(8, 8, 7);
        a.opi(GVM_I_SLL, 9, 8, 3); a.sub(10, 9, 6); a.op(GVM_I_OR, 11, 10, 7);
        a.opi(GVM_I_AND, 6, 6, 0x7ff); a.opi(GVM_I_SRL, 7, 11, 2); a.add(8, 8, 10);
        a.opi(GVM_I_XOR, 9, 9, 0x55); a.op(GVM_I_SLTU, 10, 8, 9); a.add(11, 11, 10);
        a.opi(GVM_I_SLL, 6, 6, 1); a.opi(GVM_I_SRA, 7, 7, 1); a.op(GVM_I_AND, 8, 8, 11);
        loop_end(a, top);
        a.halt();
    }, 16, 17, 0, 8});
    v.push_back({"loads", 1, true, [](std::vector<GvmAsm>& p, auto& data, uint32_t) {
        for (uint32_t k = 0; k < 64; k++) data.push_back({DATA + 4 * k, k * 7 + 1});
        GvmAsm& a = p[0];
        a.li(5, loop_count(g_loop_n)); a.li(4, (int32_t)DATA);
        int top = a.here();
        for (int i = 0; i < 14; i++) a.lw(6 + i % 6, 4, 4 * i);
        loop_end(a, top);
        a.halt();
    }, 15, 16, 0, 7});
    v.push_back({"stores", 1, true, [](std::vector<GvmAsm>& p, auto& data, uint32_t) {
        GvmAsm& a = p[0];
        a.li(5, loop_count(g_loop_n)); a.li(4, (int32_t)DATA);
        int top = a.here();
        for (int i = 0; i < 14; i++) a.sw(5, 4, 4 * i);
        loop_end(a, top);
        a.halt();
    }, 15, 16, 0, 7});
    v.push_back({"adds", 1, true, [](std::vector<GvmAsm>& p, auto& data, uint32_t) {
        GvmAsm& a = p[0];
        a.li(5, loop_count(g_loop_n));
        int top = a.here();
        for (int i = 0; i < 14; i++) a.add(6, 6, 5);
        loop_end(a, top);
        a.halt();
    }, 15, 16, 0, 7});
    v.push_back({"tight", 1, true, [](std::vector<GvmAsm>& p, auto& data, uint32_t) {
        GvmAsm& a = p[0];
        a.li(5, loop_count(g_loop_n * 8));
        int top = a.here();
        loop_end(a, top);
        a.halt();
    }, 1, 2, 0, 1});
    v.push_back({"mix", 1, true, [](std::vector<GvmAsm>& p, auto& data, uint32_t) {
        for (uint32_t k = 0; k < 2048; k++) data.push_back({DATA + 4 * k, 0x9e3779b9u * (k + 1)});
        GvmAsm& a = p[0];
        a.li(5, loop_count(g_loop_n)); a.li(6, 0); a.li(14, 0); a.li(7, (int32_t)DATA); a.li(8, (int32_t)(DATA + 4096));
        int top = a.here();
        a.R(GVM_OP_M(GVM_M_LOADX, GVM_W_W), 10, 7, 6, 0); a.add(14, 14, 10);
        if (g_fused >= 2) a.bump(6, 4); else a.addi(6, 6, 4);   // (the load read r6 before the add)
        a.op(GVM_I_XOR, 12, 10, 14); a.opi(GVM_I_AND, 6, 6, 0x7c); a.R(GVM_OP_M(GVM_M_STOREX, GVM_W_W), 12, 8, 6, 0);
        a.opi(GVM_I_SLL, 13, 12, 1); a.add(14, 14, 13);
        loop_end(a, top);
        a.halt();
    }, 9, 12, 0, g_fused >= 2 ? 4.0 : 5.0});
    v.push_back({"stride", 1, true, [](std::vector<GvmAsm>& p, auto& data, uint32_t) {
        GvmAsm& a = p[0];
        a.li(5, loop_count(g_loop_n)); a.li(6, 0); a.li(14, 0); a.li(7, (int32_t)(GVM_RAM + 0x400000)); a.li(8, 0x3ffffc);
        int top = a.here();
        a.R(GVM_OP_M(GVM_M_LOADX, GVM_W_W), 10, 7, 6, 0); a.add(14, 14, 10); a.addi(6, 6, 1040);
        a.op(GVM_I_AND, 6, 6, 8);
        loop_end(a, top);
        a.halt();
    }, 5, 7, 0, 2});
    v.push_back({"floats", 1, true, [](std::vector<GvmAsm>& p, auto& data, uint32_t) {
        GvmAsm& a = p[0];
        a.li(5, loop_count(g_loop_n));
        for (int k = 6; k <= 13; k++) a.li(k, 0);
        int top = a.here();
        a.fop(GVM_F_ADD, 6, 6, 7); a.fop(GVM_F_MUL, 8, 6, 9); a.fop(GVM_F_SUB, 10, 8, 7); a.fop(GVM_F_ADD, 11, 10, 6);
        a.fop(GVM_F_MUL, 12, 11, 9); a.fop(GVM_F_MIN, 13, 12, 8); a.fop(GVM_F_ADD, 7, 7, 13); a.fop(GVM_F_MUL, 6, 6, 9);
        a.fop(GVM_F_ADD, 8, 8, 11); a.fop(GVM_F_SUB, 9, 9, 10); a.fop(GVM_F_MAX, 10, 10, 12); a.fop(GVM_F_ADD, 12, 12, 7);
        a.fop(GVM_F_MUL, 13, 13, 6); a.fop(GVM_F_ADD, 11, 11, 8);
        loop_end(a, top);
        a.halt();
    }, 15, 16, 0, 7});
    v.push_back({"fmix", 1, true, [](std::vector<GvmAsm>& p, auto& data, uint32_t) {
        for (uint32_t k = 0; k < 4; k++) data.push_back({DATA + 4 * k, 0x3f800000u + k});
        GvmAsm& a = p[0];
        a.li(5, loop_count(g_loop_n)); a.li(4, (int32_t)DATA);
        int top = a.here();
        a.lw(6, 4, 0); a.lw(7, 4, 4); a.fop(GVM_F_MUL, 8, 6, 7); a.fop(GVM_F_ADD, 9, 9, 8);
        a.sw(9, 4, 8); a.lw(10, 4, 12); a.fop(GVM_F_MUL, 11, 10, 8); a.fop(GVM_F_ADD, 12, 12, 11);
        loop_end(a, top);
        a.halt();
    }, 9, 10, 0, 4});
    v.push_back({"fmath", 1, true, [](std::vector<GvmAsm>& p, auto& data, uint32_t) {
        GvmAsm& a = p[0];
        a.li(5, loop_count(g_loop_n)); a.li(6, 0x3f800000); a.li(7, 0x3f000000); a.li(8, 0);
        int top = a.here();
        a.fop(GVM_F_MAD, 8, 6, 7, 8); a.fop(GVM_F_MUL, 9, 8, 7); a.fop(GVM_F_ADD, 10, 9, 6);
        a.fop(GVM_F_MIN, 11, 10, 8); a.fop(GVM_F_SQRT, 12, 10, 0); a.fop(GVM_F_SUB, 8, 8, 12);
        loop_end(a, top);
        a.halt();
    }, 7, 0, 0, 3});
    // ---- real programs, compiled by gpu_vm's clang -O2 (gpu_vm/tick/real/build.sh): timed in ns a bundle
    auto real = [&](const char* name, const char* elf, const char* pokes) {
        Case c{name, 1, true, [](std::vector<GvmAsm>&, std::vector<std::pair<uint32_t, uint32_t>>&, uint32_t) {}};
        c.elf = elf; c.pokes = pokes; c.named = true; c.ns = true;
        v.push_back(c);
    };
    // ---- probes of one kind of bundle, in ns a bundle (named): the general step's (sel, path 0), calls and returns
    auto probe = [&](const char* name, std::function<void(GvmAsm&)> body) {
        Case c{name, 1, true, [body](std::vector<GvmAsm>& p, auto& data, uint32_t) {
            GvmAsm& a = p[0];
            a.li(5, g_loop_n + 1); a.li(4, (int32_t)DATA);
            int top = a.here();
            body(a);
            a.addbnz(5, -1, top);
            a.halt();
        }};
        c.named = true; c.ns = true;
        v.push_back(c);
    };
    probe("p_int", [](GvmAsm& a) { for (int i = 0; i < 16; i++) { a.add(6 + i % 4, 6 + i % 4, 5); a.close(); } });
    // x19: a bundle's fixed cost and an op's: no op (a branch never taken), two ops apart, op 1 on op 0's result
    probe("p_nop", [](GvmAsm& a) { for (int i = 0; i < 16; i++) { int l = a.label(); a.br(GVM_CC_BEQI, 5, 63, l); a.bind(l); } });
    probe("p_int2", [](GvmAsm& a) { for (int i = 0; i < 16; i++) { a.add(6 + i % 4, 6 + i % 4, 5); a.add(10 + i % 4, 10 + i % 4, 5); a.close(); } });
    probe("p_dep2", [](GvmAsm& a) { for (int i = 0; i < 16; i++) { a.add(6 + i % 4, 6 + i % 4, 5); a.add(10 + i % 4, 6 + i % 4, 5); a.close(); } });
    probe("p_sel", [](GvmAsm& a) { for (int i = 0; i < 16; i++) { a.sel(6 + i % 4, 6 + i % 4, 5, 4); a.close(); } });
    probe("p_load", [](GvmAsm& a) { for (int i = 0; i < 16; i++) { a.lw(6 + i % 4, 4, 4 * i); a.close(); } });
    probe("p_store", [](GvmAsm& a) { for (int i = 0; i < 16; i++) { a.sw(5, 4, 4 * i); a.close(); } });
    probe("p_call", [](GvmAsm& a) {   // 16 bundles: a call, a return, 14 integer bundles
        int f = a.label(), back = a.label();
        a.call(f); a.bind(back);
        for (int i = 0; i < 14; i++) { a.add(6 + i % 4, 6 + i % 4, 5); a.close(); }
        int over = a.label();
        a.j(over);   // (the function after the loop's body: jumped over)
        a.bind(f); a.ret();
        a.bind(over);
    });
    // scattered stores (binfmt_flat's relocations, gzip's hash chains): words at random in 2 MB, and words 4 KB apart
    probe("p_scat", [](GvmAsm& a) {
        a.li(9, 1103515245); a.li(10, 0x1ffffc);   // (in the loop: the constants each trip, two bundles)
        a.op(GVM_I_MUL, 6, 6, 9); a.addi(6, 6, 12345);
        a.opi(GVM_I_SRL, 7, 6, 9); a.op(GVM_I_AND, 7, 7, 10);
        a.add(7, 7, 4); a.close();
        a.sw(5, 7, 0); a.close();
    });
    probe("p_page", [](GvmAsm& a) {
        a.li(10, 0x1fffff);
        a.addi(7, 7, 4096 + 4); a.op(GVM_I_AND, 7, 7, 10); a.close();
        a.add(8, 7, 4); a.close();
        a.sw(5, 8, 0); a.close();
    });
    real("coremark", "build/tick_real/coremark.elf", "seed4_volatile=1,1000000");
    real("coremark_pie", "build/tick_real/coremark_pie.elf", "seed4_volatile=1,1000000");   // (-fPIE: globals by lpc; the compiler of 834f68d)
    real("gzip", "build/tick_real/gzloop.elf", "gz_mode=1,1;gz_bytes=6000,262144;gz_rounds=1,1000");
    real("gunzip", "build/tick_real/gzloop.elf", "gz_mode=2,2;gz_rounds=1,1000");
    // ---- many cores at once (x19): throughput, in ns a bundle of all cores (gvmbench --cores N, 32 by default)
    auto mc = [&](const char* name, const char* list) {
        Case c{name, g_mc_cores, true, [](std::vector<GvmAsm>&, std::vector<std::pair<uint32_t, uint32_t>>&, uint32_t) {}};
        c.mc = list; c.named = true; c.ns = true;
        v.push_back(c);
    };
    mc("mc_int", "p_int");                  // the same integer loop on every core (no divergence)
    mc("mc_load", "p_load");
    mc("mc_cm", "coremark");                // CoreMark on every core: the same path (lockstep)
    mc("mc_cm1", "coremark@");              // the same, one copy (code and data shared: as threads, in lockstep)
    mc("mc_mix", "coremark,gzip,coremark,gunzip");   // different programs on neighbouring cores
    // ---- a pass with K busy workers and M awake idle ones (p_kK_mM, named, in us a pass of 8,192 bundles): core 0 and
    // workers 1..K each run a long serial loop of their own (loads, stores to a ring of four words, integer chains: the write
    // cache stays small, so every busy core runs its whole budget); workers K+1..K+M poll a word and `sync` (a few bundles
    // and the general step a pass, as a game's idle worker). With overlap a pass costs what core 0's alone does (p_k0_m0).
    // Core 0 stores the word when its loop ends (a check's 3,000 trips), and the idle workers halt.
    static const uint32_t P_DATA = GVM_RAM + 0x1000000, P_DONE = P_DATA - 16;
    auto pmc = [&](const char* name, int K, int M) {
        Case c{name, (uint32_t)(1 + K + M), true, [K, M](std::vector<GvmAsm>& p, auto& data, uint32_t) {
            for (int k = 0; k <= K; k++) {
                GvmAsm& a = p[k];
                a.li(5, loop_count(g_loop_n)); a.li(4, (int32_t)(P_DATA + k * 0x10000)); a.li(20, (int32_t)(P_DATA + k * 0x10000 + 0x8000));
                a.li(10, k + 1); a.li(11, 3 * k + 7);
                int top = a.here();
                for (int i = 0; i < 4; i++) {
                    a.lw(6 + i, 4, 64 * i + 4 * ((k + i) & 3)); a.add(10, 10, 6 + i); a.close();
                    a.op(GVM_I_XOR, 11, 11, 10); a.opi(GVM_I_SLL, 12, 11, 1 + (k & 3)); a.close();
                    a.sw(11, 20, 4 * i); a.addi(13, 13, 1); a.close();
                    a.add(14, 14, 12); a.op(GVM_I_AND, 15, 14, 11); a.close();
                }
                loop_end(a, top);
                if (k == 0) { a.li(6, 1); a.li(7, (int32_t)P_DONE); a.close(); a.sw(6, 7, 0); a.close(); }
                a.halt();
            }
            for (int k = K + 1; k <= K + M; k++) {
                GvmAsm& a = p[k];
                a.li(4, (int32_t)P_DONE); a.close();
                int top = a.here(), out = a.label();
                a.lw(6, 4, 0); a.close();
                a.bnei(6, 0, out);
                a.x(GVM_X_SYNC); a.close();
                a.j(top);
                a.bind(out);
                a.halt();
            }
            for (int k = 0; k < 64; k++) data.push_back({P_DATA + 4 * k, 0x9e3779b9u * (k + 1)});
        }};
        c.named = true; c.us_pass = true;
        v.push_back(c);
    };
    // ---- phase 2 (isa.h GVM_M_FREAD): the core's file, pairs and bindings checked against the reference (two cores, random
    // slots and values; a pair's words are never loaded, a bound word only after its unbind; a tick without them skips these)
    auto xcase = [&](const char* name, int kind) {   // 0 lf/sf, 1 pairs, 2 bindings
        Case c{name, 2, false, [kind](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
            for (uint32_t k = 0; k < 2; k++) {
                GvmAsm& a = p[k];
                Rng g{seed * 977u + k * 131u + 7u};
                auto rr = [&]() { return 6 + g.below(19); };   // r6-r24
                a.li(29, (int32_t)(DATA + 0x10000 + k * 0x4000)); a.li(28, (int32_t)(DATA + 0x20000 + k * 0x4000));
                for (int i = 6; i < 25; i++) a.li(i, (int32_t)g.next());
                a.li(5, 40);
                int top = a.here();
                for (int i = 0; i < 12; i++) {
                    switch (g.below(kind == 2 ? 3 : 4)) {
                    case 0: if (kind == 1) a.sub(rr(), rr(), rr()); else a.lf(rr(), 0, (int32_t)g.below(64)); break;
                    case 1: if (kind == 1) a.opi(GVM_I_SLL, rr(), rr(), 3); else a.sf(rr(), 0, (int32_t)g.below(64)); break;
                    case 2: a.op(GVM_I_XOR, rr(), rr(), rr()); a.addi(rr(), rr(), (int32_t)g.below(100)); break;
                    default: if (kind == 1) a.fpair(rr(), 29, (int32_t)(4 * g.below(64))); else a.add(rr(), rr(), 5); break;
                    }
                    if (kind == 1 && g.below(2)) a.fpair(5, 29, (int32_t)(4 * g.below(64)));
                }
                if (kind == 2) {   // a slot bound, read, changed, unbound (dirty: the commit writes it), the word then loaded
                    for (int j = 0; j < 2; j++) {
                        int s = 64 + j + 2 * (int)g.below(4);
                        a.addi(27, 28, (int32_t)(4 * g.below(16))); a.close();
                        a.fbind(27, 0, s); a.close();
                        a.lf(26, 0, s); a.close();
                        a.add(26, 26, 5); a.close();
                        a.sf(26, 0, s); a.close();
                        if (g.below(2)) { a.lf(25, 0, s); a.op(GVM_I_XOR, 24, 24, 25); a.close(); }
                        a.funbind(0, s); a.close();
                        a.lw(25, 27, 0); a.close();
                        a.add(23, 23, 25); a.close();
                    }
                }
                loop_end(a, top);
                a.halt();
            }
        }};
        if (kind == 1) c.pair_need = 2; else c.xr_need = 64;
        c.bind_need = kind == 2;
        v.push_back(c);
    };
    xcase("xfile", 0); xcase("xpair", 1); xcase("xbind", 2);
    // spills (named, ns a bundle of core 0; its budget used goes to --json's util): a frame of 8 words stored and loaded
    // again, the frame moving through 4 KB of stack (256 texels a pass: x23's cache ends passes early); p_xspill the same
    // in the core's file (slots by the frame's word index, masked by GVM_SR_XR), no write cache
    auto spill = [&](const char* name, bool file) {
        Case c{name, 1, true, [file](std::vector<GvmAsm>& p, auto& data, uint32_t) {
            GvmAsm& a = p[0];
            a.li(5, loop_count(g_loop_n)); a.li(4, (int32_t)(DATA + 0x8000)); a.li(20, 0);
            if (file) { a.mfsr(23, GVM_SR_XR); a.addi(23, 23, -1); }
            for (int i = 6; i < 14; i++) a.li(i, i * 0x01010101);
            int top = a.here();
            a.addi(20, 20, 64 * 13); a.opi(GVM_I_AND, 20, 20, 4095); a.close();
            if (file) { a.opi(GVM_I_SRL, 21, 20, 2); a.close(); a.op(GVM_I_AND, 21, 21, 23); a.close(); }
            else { a.add(21, 4, 20); a.close(); }
            for (int k = 0; k < 8; k++) { if (file) a.sf(6 + k, 21, k); else a.sw(6 + k, 21, 4 * k); a.addi(14 + k % 4, 14 + k % 4, k); a.close(); }
            for (int k = 0; k < 8; k++) { if (file) a.lf(6 + k, 21, k); else a.lw(6 + k, 21, 4 * k); a.add(18, 18, 14 + k % 4); a.close(); }
            for (int k = 0; k < 8; k++) { a.add(6 + k, 6 + k, 18); a.close(); }
            loop_end(a, top);
            a.halt();
        }};
        c.named = true; c.ns = true;
        if (file) c.xr_need = 64;
        v.push_back(c);
    };
    spill("p_spill", false); spill("p_xspill", true);
    // read-only loads (ns a bundle): 8 loads of a 1 KB constant table beside 4 stores a trip to a moving 4 KB ring (the bloom
    // word busy); p_rdnc the same loads as lw_nc (GVM_W_NC: no look at this pass's stores; a plain lw on a tick without it)
    auto rd = [&](const char* name, bool nc) {
        Case c{name, 1, true, [nc](std::vector<GvmAsm>& p, auto& data, uint32_t) {
            GvmAsm& a = p[0];
            const uint32_t TAB = DATA + 0x10000;
            for (uint32_t k = 0; k < 256; k++) data.push_back({TAB + 4 * k, k * 0x9e3779b9u});
            a.li(5, loop_count(g_loop_n)); a.li(4, (int32_t)(DATA + 0x8000)); a.li(7, (int32_t)TAB); a.li(20, 0); a.li(22, 0);
            int top = a.here();
            a.addi(20, 20, 16 * 5); a.opi(GVM_I_AND, 20, 20, 4095); a.addi(22, 22, 4 * 37); a.close();
            a.add(21, 4, 20); a.opi(GVM_I_AND, 22, 22, 1023 - 31); a.close();
            a.add(23, 7, 22); a.close();
            for (int k = 0; k < 4; k++) { a.sw(10 + k, 21, 4 * k); a.addi(10 + k, 10 + k, 1); a.close(); }
            for (int k = 0; k < 8; k++) { if (nc) a.lw_nc(14 + k, 23, 4 * k); else a.lw(14 + k, 23, 4 * k); a.add(10 + (k & 3), 10 + (k & 3), 14 + k); a.close(); }
            loop_end(a, top);
            a.halt();
        }};
        c.named = true; c.ns = true;
        v.push_back(c);
    };
    rd("p_rd", false); rd("p_rdnc", true);
    {   // xvec (phase 3): random vector ops on r5-r28 (ints and floats), vld and vst of texels, masks and vsel, vdot,
        // swizzles, vext, vins and vmask, beside scalar ops (which write lane 0 only)
        Case c{"xvec", 1, false, [](std::vector<GvmAsm>& p, auto& data, uint32_t seed) {
            Rng g{seed * 6151 + 11};
            for (uint32_t k = 0; k < 1024; k++) data.push_back({DATA + 4 * k, (k & 1) ? rfloat(g) : g.next()});
            GvmAsm& a = p[0];
            static const uint32_t vo[] = {GVM_X_VADD, GVM_X_VSUB, GVM_X_VMUL, GVM_X_VAND, GVM_X_VOR, GVM_X_VXOR, GVM_X_VFADD, GVM_X_VFSUB,
                                          GVM_X_VFMUL, GVM_X_VFMA, GVM_X_VFMIN, GVM_X_VFMAX, GVM_X_VFLT, GVM_X_VFLE, GVM_X_VEQ, GVM_X_VSEL,
                                          GVM_X_VDOT, GVM_X_VSPLAT};
            for (int round = 0; round < 20; round++) {
                seed_regs(a, g, true);
                a.li(29, (int32_t)DATA);
                for (int i = 0; i < 40; i++) {
                    switch (g.below(7)) {
                    case 0: case 1: a.vop(vo[g.below(sizeof vo / 4)], rreg(g), rreg(g), rreg(g), rreg(g)); break;
                    case 2: a.vld(rreg(g), 29, (int32_t)(16 * g.below(256))); break;
                    case 3: a.vst(rreg(g), 29, (int32_t)(16 * g.below(256))); break;
                    case 4: { uint32_t k = g.below(4); if (k == 0) a.vswz(rreg(g), rreg(g), g.below(256)); else if (k == 1) a.vext(rreg(g), rreg(g), g.below(4)); else if (k == 2) a.vins(rreg(g), rreg(g), g.below(4)); else a.vop(GVM_X_VMASK, rreg(g), rreg(g), 0); break; }
                    case 5: a.lw(rreg(g), 29, (int32_t)(4 * g.below(1024))); break;
                    default: a.addi(rreg(g), rreg(g), rimm(g)); break;
                    }
                }
            }
            a.halt();
        }};
        c.named = true; c.vec_need = true; c.tol = 2e-5;
        v.push_back(c);
    }
    pmc("p_k0_m0", 0, 0); pmc("p_k1_m0", 1, 0); pmc("p_k2_m0", 2, 0); pmc("p_k4_m0", 4, 0); pmc("p_k8_m0", 8, 0);
    pmc("p_k0_m40", 0, 40); pmc("p_k4_m40", 4, 40); pmc("p_k8_m40", 8, 40);
    game_cases(v);   // the game bench's g_ cases (gamebench.h)
    return v;
}

// ---- A case laid out for both machines
struct Prepared {
    std::vector<uint32_t> ramImg;
    std::vector<std::vector<uint32_t>> state;
    std::vector<uint32_t> bases;
};
static const uint32_t HALT_STUB = GVM_RAM + 0xff0000;
// a real program's ELF: its segments into the image, its words poked; the entry, or 0 (and why printed)
static std::vector<uint8_t> slurp_file(const char* path) {
    std::vector<uint8_t> b;
    FILE* f = fopen(path, "rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    b.resize((size_t)n);
    if (n > 0 && fread(b.data(), 1, (size_t)n, f) != (size_t)n) b.clear();
    fclose(f);
    return b;
}
// The real programs of build/tick_real were compiled before x18's paths (gpu_vm/isa.h): cc 0's bundles, and those whose op 0 is
// sel or lpc, were path 0. Loading gives them x18's paths, as the compiler will (and as gvm_asm.h does).
static int g_x17_paths = 0;   // (a variant -D X17PATHS=1: the real programs as compiled, for x17)
static void x18_paths(uint32_t* wv, uint32_t bundles) {
    if (g_x17_paths) return;
    for (uint32_t b = 0; b < bundles; b++) {
        uint32_t* w = wv + 4 * b, ctl = w[3];
        if (ctl == 0 || GVM_N(ctl) == 3 || GVM_PATH(ctl) != GVM_PATH_GENERAL) continue;
        uint32_t np;
        if (GVM_CC(ctl) == GVM_CC_SPECIAL) np = GvmAsm::path12_of(ctl, w[0], w[1]);
        else {
            GvmAsm a(0);
            for (uint32_t k = 0; k < GVM_N(ctl); k++) a.cur.push_back(w[k]);
            np = a.path();
        }
        w[3] = (ctl & ~7u) | np;
    }
}
static uint32_t load_real(const Case& cs, std::vector<uint32_t>& img) {
    std::string path = cs.elf;
    // GVM_CC_ELF: another build of the game_cc kernels; a tick with late branches takes its _late.elf (clang -mllvm -gpuvm-late-br)
    if (path == "build/gamebench/gvm_lone.elf" && getenv("GVM_CC_ELF")) path = getenv("GVM_CC_ELF");
    if (path.size() > 4 && GvmAsm::late_br && !GvmAsm::late_float && path.rfind("gvm_lone", std::string::npos) != std::string::npos && getenv("GVM_CC_ELF"))
        path = path.substr(0, path.size() - 4) + "_late.elf";
    std::vector<uint8_t> e = slurp_file(path.c_str());
    for (int up = 0; up < 3 && e.empty(); up++) { path = "../" + path; e = slurp_file(path.c_str()); }   // (run from bin or gpu_vm)
    auto rd16 = [&](size_t o) { return (uint32_t)e[o] | (uint32_t)e[o + 1] << 8; };
    auto rd32 = [&](size_t o) { return rd16(o) | rd16(o + 2) << 16; };
    if (e.size() < 52 || memcmp(e.data(), "\177ELF", 4)) { printf("  %s: no ELF at %s (gpu_vm/tick/real/build.sh)\n", cs.name, cs.elf); return 0; }
    uint8_t* bytes = (uint8_t*)img.data();
    for (uint32_t i = 0, ph = rd32(28); i < rd16(44); i++, ph += rd16(42)) {
        if (rd32(ph) != 1) continue;
        uint32_t off = rd32(ph + 4), va = rd32(ph + 8), fs = rd32(ph + 16), ms = rd32(ph + 20);
        if (va + ms > RAM_END) { printf("  %s: a segment past RAM's end\n", cs.name); return 0; }
        memcpy(bytes + va, e.data() + off, fs);
        memset(bytes + va + fs, 0, ms - fs);
        if ((rd32(ph + 24) & 1) && !(va & 15)) x18_paths(&img[va / 4], fs / 16);   // (PF_X)
    }
    bool timing = g_loop_n > 100000;
    std::string pk = cs.pokes;
    for (size_t at = 0; at < pk.size();) {   // symbol=check,timed
        size_t end = pk.find(';', at), eq = pk.find('=', at), cm = pk.find(',', at);
        if (end == std::string::npos) end = pk.size();
        std::string sym = pk.substr(at, eq - at);
        uint32_t val = (uint32_t)strtoul(pk.c_str() + (timing ? cm + 1 : eq + 1), nullptr, 0);
        bool found = false;
        for (uint32_t i = 0, sh = rd32(32); i < rd16(48) && !found; i++, sh += rd16(46)) {
            if (rd32(sh + 4) != 2) continue;   // SHT_SYMTAB
            uint32_t so = rd32(sh + 16), sn = rd32(sh + 20), str = rd32(rd32(32) + rd32(sh + 24) * rd16(46) + 16);
            for (uint32_t k = so; k + 16 <= so + sn; k += 16)
                if (!strcmp((const char*)e.data() + str + rd32(k), sym.c_str())) { img[rd32(k + 4) / 4] = val; found = true; break; }
        }
        if (!found) printf("  %s: no symbol %s\n", cs.name, sym.c_str());
        at = end + 1;
    }
    return rd32(24);
}
static std::vector<Case> cases();
static Prepared prepare(const Case& cs, uint32_t seed);
// Many cores (Case::mc): core c runs a copy of the case named c-th in the list (round robin), each at a place of its
// own: a real program's copy k is its ELF linked there (build/tick_real/mc, gpu_vm/tick/real/build.sh: CoreMark's
// cm<k>.elf, gzloop's gz<k>.elf for gzip and gunzip alike), poked as the case; a probe's copy is its code moved (its
// data at DATA is shared). Each core's stack is 32 KB of its own below RAM's end (RAM_MC: 128 MB).
static const uint32_t RAM_MC = 128u << 20;
static Prepared prepare_mc(const Case& cs, uint32_t seed) {
    Prepared pr;
    pr.ramImg.assign((size_t)RAM_END / 4, 0);
    std::vector<std::string> names;
    for (const char *p = cs.mc, *q; *p; p = *q ? q + 1 : q) { q = strchr(p, ','); if (!q) q = p + strlen(p); names.push_back(std::string(p, q)); }
    std::vector<Case> all = cases();
    uint32_t asm_at = GVM_RAM + 0x200000, ncm = 0, ngz = 0;
    pr.state.assign(cs.cores, {});
    for (uint32_t c = 0; c < cs.cores; c++) {
        const Case* sub = nullptr;
        std::string nm = names[c % names.size()];
        bool shared = nm.back() == '@';   // name@: every core the first copy (its code and data shared, as threads)
        if (shared) nm.pop_back();
        for (auto& x : all) if (nm == x.name) sub = &x;
        if (!sub) { printf("  %s: no case %s\n", cs.name, nm.c_str()); exit(1); }
        std::vector<uint32_t> st((size_t)(g_twc + g_wcn / 2 + 8) * 4, 0);
        uint32_t entry;
        if (sub->elf) {
            bool gz = strstr(sub->elf, "gzloop") != nullptr;
            uint32_t k = shared ? 0 : gz ? ngz++ : ncm++;
            if (k >= (gz ? 16u : 64u)) { printf("  %s: too many copies of %s\n", cs.name, sub->name); exit(1); }
            std::string path = std::string("build/tick_real/mc/") + (gz ? "gz" : "cm") + std::to_string(k) + ".elf";
            Case one = *sub;
            one.elf = path.c_str();
            entry = load_real(one, pr.ramImg);
            if (!entry) exit(1);
            st[2] = RAM_END - 64 - c * 0x8000;
            st[17 * 4 + 1] = HALT_STUB;
        } else {
            Prepared one = prepare(*sub, seed);
            if (c == 0) for (size_t k = 0; k < one.ramImg.size(); k++) if (one.ramImg[k]) pr.ramImg[k] = one.ramImg[k];   // (the probe's data)
            memcpy(&pr.ramImg[asm_at / 4], &one.ramImg[one.bases[0] / 4], 0x10000);
            st = one.state[0];
            entry = asm_at;
            asm_at += 0x10000;
        }
        st[16 * 4 + 0] = entry;
        st[16 * 4 + 1] = GVM_ST_S;
        st[18 * 4 + 3] = 0x9e3779b9u ^ (c * 0x85ebca6bu);
        pr.state[c] = st;
        pr.bases.push_back(entry);
    }
    pr.ramImg[HALT_STUB / 4] = GVM_WORD_R(GVM_X_BREAK, 0u, 0u, 0u, 0u); pr.ramImg[HALT_STUB / 4 + 3] = GVM_CTRL_NONE | 1u << 3 | GVM_PATH_GENERAL; if (GvmAsm::thin8) pr.ramImg[HALT_STUB / 4 + 1] = pr.ramImg[HALT_STUB / 4 + 3];
    return pr;
}
static Prepared prepare(const Case& cs, uint32_t seed) {
    if (cs.mc) return prepare_mc(cs, seed);
    Prepared pr;
    if (cs.elf) {   // a real program: its image, sp at RAM's end, a trap halts
        pr.ramImg.assign((size_t)RAM_END / 4, 0);
        uint32_t entry = load_real(cs, pr.ramImg);
        pr.bases.push_back(entry ? entry : HALT_STUB);
        pr.ramImg[HALT_STUB / 4] = GVM_WORD_R(GVM_X_BREAK, 0u, 0u, 0u, 0u); pr.ramImg[HALT_STUB / 4 + 3] = GVM_CTRL_NONE | 1u << 3 | GVM_PATH_GENERAL; if (GvmAsm::thin8) pr.ramImg[HALT_STUB / 4 + 1] = pr.ramImg[HALT_STUB / 4 + 3];
        pr.state.assign(1, std::vector<uint32_t>((size_t)(g_twc + g_wcn / 2 + 8) * 4, 0));
        pr.state[0][2] = RAM_END - 64;
        pr.state[0][16 * 4 + 0] = pr.bases[0];
        pr.state[0][16 * 4 + 1] = GVM_ST_S;
        pr.state[0][18 * 4 + 3] = 0x9e3779b9u;
        pr.state[0][17 * 4 + 1] = HALT_STUB;
        return pr;
    }
    std::vector<GvmAsm> progs(cs.cores, GvmAsm(GVM_RAM));
    std::vector<std::pair<uint32_t, uint32_t>> data;
    for (uint32_t c = 0; c < cs.cores; c++) progs[c].base = GVM_RAM + c * 0x40000;
    cs.build(progs, data, seed);
    pr.ramImg.assign((size_t)RAM_END / 4, 0);
    for (uint32_t c = 0; c < cs.cores; c++) {
        std::vector<uint32_t> w = progs[c].finish();
        memcpy(&pr.ramImg[progs[c].base / 4], w.data(), w.size() * 4);
        pr.bases.push_back(progs[c].base);
    }
    for (auto& d : data) pr.ramImg[d.first / 4] = d.second;
    // a trap nobody asked for halts the core where its handler would be: {break} at HALT_STUB
    pr.ramImg[HALT_STUB / 4] = GVM_WORD_R(GVM_X_BREAK, 0u, 0u, 0u, 0u); pr.ramImg[HALT_STUB / 4 + 3] = GVM_CTRL_NONE | 1u << 3 | GVM_PATH_GENERAL; if (GvmAsm::thin8) pr.ramImg[HALT_STUB / 4 + 1] = pr.ramImg[HALT_STUB / 4 + 3];
    pr.state.assign(cs.cores, std::vector<uint32_t>((size_t)(g_twc + g_wcn / 2 + 8) * 4, 0));
    for (uint32_t c = 0; c < cs.cores; c++) {
        pr.state[c][16 * 4 + 0] = pr.bases[c];
        pr.state[c][16 * 4 + 1] = GVM_ST_S;
        pr.state[c][18 * 4 + 3] = 0x9e3779b9u ^ (c * 0x85ebca6bu);
        pr.state[c][17 * 4 + 1] = HALT_STUB;
    }
    return pr;
}
// the lean tick on the RTX 5070 Laptop (isaloops, 22 October 2026, gpu_vm/tick/steps/README.md)
static double lean_of(const char* name) {
    if (double g = g_rv_of(name)) return g;   // (the game bench: the RISC-V machine's frames a second)
    static const struct { const char* n; double v; } L[] = {{"arith", 8.26}, {"loads", 5.70}, {"stores", 4.91}, {"adds", 8.18},
                                                            {"tight", 7.28}, {"mix", 7.22}, {"stride", 6.37}, {"floats", 6.11},
                                                            {"fmix", 5.31}};
    for (auto& l : L) if (!strcmp(l.n, name)) return l.v;
    return 0;
}

