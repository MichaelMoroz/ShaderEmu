// Writes test/MC/GPUVM/encoding.s: every op alone in a bundle (-gpuvm-no-pack), its 16 bytes as gvm_asm.h makes them
// (isa.h's macros), and its disassembly.
//   g++ -std=c++17 gen_encoding.cpp -o gen_encoding && ./gen_encoding > ../MC/GPUVM/encoding.s
#include "../../../ref/gvm_asm.h"
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

struct Case {
    std::string src, dis;
    std::vector<uint32_t> w;
};
static std::vector<Case> cases;

static void op(const std::string &src, const std::string &dis, const std::function<void(GvmAsm &)> &f) {
    GvmAsm a(0);
    f(a);
    cases.push_back({src, dis, a.finish()});
}
static void op(const std::string &src, const std::function<void(GvmAsm &)> &f) { op(src, src, f); }
// a control op alone: its word with n 0 and the given path
static void ctl(const std::string &src, const std::string &dis, uint32_t c, uint32_t path) {
    if (GVM_CC(c) == GVM_CC_SPECIAL) path = GvmAsm::path12_of(c, 0, 0);   // x18: cc 0 by its ops (none here)
    cases.push_back({src, dis, {0, 0, 0, c | path}});
}
static uint32_t br(uint32_t cc, uint32_t ra, uint32_t rb, int off) {
    return cc << 7 | ra << 11 | (rb & 63) << 26 | ((uint32_t)(off / 16) & 0x1ffu) << 17;
}

enum { ZERO, RA, SP, GP, TP, A0, A1, A2, A3, A7 = 12, T0 = 13, T1 = 14, S0 = 32 };

int main() {
    const char *names[] = {"add", "and", "or", "xor", "sub", "andn", "orn", "xnor", "slt", "sltu", "min", "minu",
                           "sge", "sgeu", "max", "maxu", "sll", "srl", "sra", "mul", "ror", "rol", "seq", "sne"};
    for (uint32_t f = 0; f < 24; f++) {
        op(std::string(names[f]) + " a0, a1, a2", [f](GvmAsm &a) { a.op(f, A0, A1, A2); });
        op(std::string(names[f]) + "i a0, a1, -5", [f](GvmAsm &a) { a.opi(f, A0, A1, -5); });
    }
    op("addi a0, a1, 2047", [](GvmAsm &a) { a.addi(A0, A1, 2047); });
    op("addi a0, a1, 2048", [](GvmAsm &a) { a.addi(A0, A1, 2048); });
    op("addi a0, a1, -2048", [](GvmAsm &a) { a.addi(A0, A1, -2048); });
    op("addi a0, a1, -2049", [](GvmAsm &a) { a.addi(A0, A1, -2049); });
    op("li t0, 0x12345678", "li t0, 305419896", [](GvmAsm &a) { a.li(T0, 0x12345678); });
    op("li t0, -1", [](GvmAsm &a) { a.li(T0, -1); });
    op("mv s0, sp", [](GvmAsm &a) { a.mv(S0, SP); });
    op("nop", [](GvmAsm &a) { a.add(ZERO, ZERO, ZERO); });
    op("add r63, r62, r61", "add t26, t25, t24", [](GvmAsm &a) { a.add(63, 62, 61); });
    op("shadd a0, a1, a2, 3", [](GvmAsm &a) { a.shadd(A0, A1, A2, 3); });
    op("sel a0, a1, a2, a3", [](GvmAsm &a) { a.sel(A0, A1, A2, A3); });
    op("addcy a0, a1, a2, a3", [](GvmAsm &a) { a.R(GVM_OP_I(GVM_I_ADDCY), A0, A1, A2, A3); });
    op("carry a0, a1, a2, a3", [](GvmAsm &a) { a.R(GVM_OP_I(GVM_I_CARRY), A0, A1, A2, A3); });
    op("subby a0, a1, a2, a3", [](GvmAsm &a) { a.R(GVM_OP_I(GVM_I_SUBBY), A0, A1, A2, A3); });
    op("borrow a0, a1, a2, a3", [](GvmAsm &a) { a.R(GVM_OP_I(GVM_I_BORROW), A0, A1, A2, A3); });
    op("bfe a0, a1, 4, 8", [](GvmAsm &a) { a.bfe(A0, A1, 4, 8, false); });
    op("bfes a0, a1, 31, 1", [](GvmAsm &a) { a.bfe(A0, A1, 31, 1, true); });
    op("bfe a0, a1, 0, 32", [](GvmAsm &a) { a.bfe(A0, A1, 0, 32, false); });

    struct W { const char *n; uint32_t w; } ws[] = {{"w", GVM_W_W}, {"d", GVM_W_D}, {"h", GVM_W_H}, {"b", GVM_W_B},
                                                   {"q", GVM_W_Q}, {"hu", GVM_W_HU}, {"bu", GVM_W_BU}};
    for (W w : ws) {
        uint32_t wd = w.w;
        op(std::string("l") + w.n + " a0, 8(sp)", [wd](GvmAsm &a) { a.ld(wd, A0, SP, 8); });
        op(std::string("l") + w.n + "x a0, a1, a2, 2", [wd](GvmAsm &a) { a.ldx(wd, A0, A1, A2, 2); });
        if (wd == GVM_W_HU || wd == GVM_W_BU)
            continue;
        op(std::string("s") + w.n + " a0, -4(sp)", [wd](GvmAsm &a) { a.st(wd, A0, SP, -4); });
        op(std::string("s") + w.n + "x a0, a1, a2, 4", [wd](GvmAsm &a) { a.stx(wd, A0, A1, A2, 4); });
    }
    op("lw a0, 0x10000(sp)", "lw a0, 65536(sp)", [](GvmAsm &a) { a.lw(A0, SP, 0x10000); });
    op("lw a0, (a1)", "lw a0, 0(a1)", [](GvmAsm &a) { a.lw(A0, A1, 0); });
    op("lw a0, 0x100", "lw a0, 256(zero)", [](GvmAsm &a) { a.lw(A0, ZERO, 0x100); });
    op("sq t0, 0(a0)", [](GvmAsm &a) { a.st(GVM_W_Q, T0, A0, 0); });
    const char *amos[] = {"amoswap", "amoadd", "amoand", "amoor", "amoxor", "amomin", "amomax", "amocas"};
    for (uint32_t f = 0; f < 8; f++)
        op(std::string(amos[f]) + " a0, a2, (a1)", [f](GvmAsm &a) { a.amo(f, A0, A1, A2); });
    op("amominu a0, a2, (a1)", [](GvmAsm &a) { a.R(GVM_OP_M(GVM_M_AMO2, 0), A0, A1, A2); });
    op("amomaxu a0, a2, (a1)", [](GvmAsm &a) { a.R(GVM_OP_M(GVM_M_AMO2, 1), A0, A1, A2); });

    struct F { const char *n; uint32_t f; int kind; };   // 0 R and I, 1 with rc, 2 unary
    F fs[] = {{"fadd", 0, 0}, {"fmul", 1, 0}, {"fmad", 2, 1}, {"fmsb", 3, 1}, {"fsub", 4, 0}, {"fnmsb", 5, 1},
              {"fmin", 6, 0}, {"fmax", 7, 0}, {"feq", 8, 0}, {"flt", 9, 0}, {"fle", 10, 0}, {"funord", 11, 0},
              {"fsgnj", 12, 0}, {"fsgnjn", 13, 0}, {"fsgnjx", 14, 0}, {"ffrac", 15, 2}, {"itof", 16, 2},
              {"utof", 17, 2}, {"ftoi", 18, 2}, {"ftou", 19, 2}, {"ffloor", 20, 2}, {"fceil", 21, 2},
              {"ftrunc", 22, 2}, {"fround", 23, 2}, {"fdiv", 24, 0}, {"fsqrt", 25, 2}, {"frsqrt", 26, 2},
              {"frcp", 27, 2}, {"fsin", 28, 2}, {"fcos", 29, 2}, {"fexp2", 30, 2}, {"flog2", 31, 2}};
    for (F f : fs) {
        uint32_t fn = f.f;
        if (f.kind == 0) {
            op(std::string(f.n) + " a0, a1, a2", [fn](GvmAsm &a) { a.fop(fn, A0, A1, A2); });
            op(std::string(f.n) + "i a0, a1, 1.5", [fn](GvmAsm &a) { a.fopi(fn, A0, A1, 1.5f); });
        } else if (f.kind == 1) {
            op(std::string(f.n) + " a0, a1, a2, a3", [fn](GvmAsm &a) { a.fop(fn, A0, A1, A2, A3); });
        } else {
            op(std::string(f.n) + " a0, a1", [fn](GvmAsm &a) { a.fop(fn, A0, A1, 0); });
        }
    }
    op("fmuli a0, a1, 0.1", "fmuli a0, a1, 0.100000001", [](GvmAsm &a) { a.fopi(GVM_F_MUL, A0, A1, 0.1f); });
    op("faddi a0, a1, -2.0", [](GvmAsm &a) { a.fopi(GVM_F_ADD, A0, A1, -2.0f); });
    op("faddi a0, a1, 0x3f800000", "faddi a0, a1, 1.0", [](GvmAsm &a) { a.fopi(GVM_F_ADD, A0, A1, 1.0f); });

    struct X { const char *n; uint32_t o; int kind; } xs[] = {
        {"mulh", GVM_X_MULH, 0}, {"mulhu", GVM_X_MULHU, 0}, {"mulhsu", GVM_X_MULHSU, 0}, {"div", GVM_X_DIV, 0},
        {"divu", GVM_X_DIVU, 0}, {"rem", GVM_X_REM, 0}, {"remu", GVM_X_REMU, 0}, {"clz", GVM_X_CLZ, 1},
        {"ctz", GVM_X_CTZ, 1}, {"popc", GVM_X_POPC, 1}, {"bswap", GVM_X_BSWAP, 1}, {"brev", GVM_X_BREV, 1},
        {"syscall", GVM_X_SYSCALL, 2}, {"break", GVM_X_BREAK, 2}, {"wait", GVM_X_WAIT, 2}, {"sync", GVM_X_SYNC, 2},
        {"fence", GVM_X_FENCE, 2}};
    for (X x : xs) {
        uint32_t o = x.o;
        std::string s = std::string(x.n) + (x.kind == 0 ? " a0, a1, a2" : x.kind == 1 ? " a0, a1" : "");
        if (x.kind == 0) op(s, [o](GvmAsm &a) { a.x(o, A0, A1, A2); });
        else if (x.kind == 1) op(s, [o](GvmAsm &a) { a.x(o, A0, A1); });
        else op(s, [o](GvmAsm &a) { a.x(o); });
    }
    op("unimp", [](GvmAsm &a) { a.put(0); });
    op("bfi a0, a1, 8, 4", [](GvmAsm &a) { a.xi(GVM_X_BFI, A0, A1, 8 | 3 << 5); });
    op("lpc a0, 32", "lpc a0, 0x{{[0-9a-f]+}}", [](GvmAsm &a) { a.xi(GVM_X_LPC, A0, 0, 32); });
    op("mfsr a0, cycle", [](GvmAsm &a) { a.mfsr(A0, GVM_SR_CYCLE); });
    op("mtsr tvec, a1", [](GvmAsm &a) { a.mtsr(GVM_SR_TVEC, A1); });
    op("xsr a0, scratch, a1", [](GvmAsm &a) { a.xi(GVM_X_XSR, A0, A1, GVM_SR_SCRATCH); });
    op("srset a0, status, a1", [](GvmAsm &a) { a.xi(GVM_X_SRSET, A0, A1, GVM_SR_STATUS); });
    op("srclr a0, ien, a1", [](GvmAsm &a) { a.xi(GVM_X_SRCLR, A0, A1, GVM_SR_IEN); });
    op("mfsr a0, 100", [](GvmAsm &a) { a.mfsr(A0, 100); });

    // control ops alone (n 0; the integer path, or 0 for cc 0); a target is a byte offset from the bundle (at address 0)
    ctl("beq a0, a1, 32", "beq a0, a1, 0x{{[0-9a-f]+}}", br(GVM_CC_BEQ, A0, A1, 32), GVM_PATH_INT);
    ctl("bne a0, zero, -16", "bne a0, zero, 0x{{[0-9a-f]+}}", br(GVM_CC_BNE, A0, 0, -16), GVM_PATH_INT);
    ctl("blt a0, a1, 16", "blt a0, a1, 0x{{[0-9a-f]+}}", br(GVM_CC_BLT, A0, A1, 16), GVM_PATH_INT);
    ctl("bge a0, a1, 16", "bge a0, a1, 0x{{[0-9a-f]+}}", br(GVM_CC_BGE, A0, A1, 16), GVM_PATH_INT);
    ctl("bltu a0, a1, 16", "bltu a0, a1, 0x{{[0-9a-f]+}}", br(GVM_CC_BLTU, A0, A1, 16), GVM_PATH_INT);
    ctl("bgeu a0, a1, 4080", "bgeu a0, a1, 0x{{[0-9a-f]+}}", br(GVM_CC_BGEU, A0, A1, 4080), GVM_PATH_INT);
    ctl("bbs a0, 31, 16", "bbs a0, 31, 0x{{[0-9a-f]+}}", br(GVM_CC_BBS, A0, 31, 16), GVM_PATH_INT);
    ctl("bbc a0, 0, 16", "bbc a0, 0, 0x{{[0-9a-f]+}}", br(GVM_CC_BBC, A0, 0, 16), GVM_PATH_INT);
    ctl("beqi a0, -3, 16", "beqi a0, -3, 0x{{[0-9a-f]+}}", br(GVM_CC_BEQI, A0, (uint32_t)-3, 16), GVM_PATH_INT);
    ctl("bnei a0, 31, 16", "bnei a0, 31, 0x{{[0-9a-f]+}}", br(GVM_CC_BNEI, A0, 31, 16), GVM_PATH_INT);
    ctl("blti a0, -32, 16", "blti a0, -32, 0x{{[0-9a-f]+}}", br(GVM_CC_BLTI, A0, (uint32_t)-32, 16), GVM_PATH_INT);
    ctl("bgei a0, 0, 16", "bgei a0, 0, 0x{{[0-9a-f]+}}", br(GVM_CC_BGEI, A0, 0, 16), GVM_PATH_INT);
    ctl("bltui a0, 5, 16", "bltui a0, 5, 0x{{[0-9a-f]+}}", br(GVM_CC_BLTUI, A0, 5, 16), GVM_PATH_INT);
    ctl("bgeui a0, 5, 16", "bgeui a0, 5, 0x{{[0-9a-f]+}}", br(GVM_CC_BGEUI, A0, 5, 16), GVM_PATH_INT);
    ctl("addbnz t1, -1, -4096", "addbnz t1, -1, 0x{{[0-9a-f]+}}", br(GVM_CC_ADDBNZ, T1, (uint32_t)-1, -4096), GVM_PATH_INT);
    ctl("j 32", "j 0x{{[0-9a-f]+}}", GVM_CTRL_J | 2u << 17, GVM_PATH_INT);
    ctl("j 8192", "j 0x{{[0-9a-f]+}}", GVM_CTRL_SPECIAL(GVM_SUB_J) | 512u << 14, 0);
    ctl("call -64", "call 0x{{[0-9a-f]+}}", GVM_CTRL_SPECIAL(GVM_SUB_CALL) | (uint32_t)(-4 * (1 << 14)), 0);
    ctl("jr a0", "jr a0", GVM_CTRL_SPECIAL(GVM_SUB_JR) | A0 << 14, 0);
    ctl("callr t0", "callr t0", GVM_CTRL_SPECIAL(GVM_SUB_CALLR) | T0 << 14, 0);
    ctl("ret", "ret", GVM_CTRL_SPECIAL(GVM_SUB_JR) | RA << 14, 0);
    ctl("eret", "eret", GVM_CTRL_SPECIAL(GVM_SUB_ERET), 0);
    ctl("bump sp, -16", "bump sp, -16", GVM_CTRL_SPECIAL(GVM_SUB_ADD) | SP << 14 | (uint32_t)(-16 & 0xfff) << 20, 0);

    printf("# Generated by gpu_vm/llvm/test/gen/gen_encoding.cpp from gvm_asm.h (isa.h's macros): every op alone in a\n"
           "# bundle, its 16 bytes and its disassembly. A branch's target is a byte offset from its bundle.\n"
           "# RUN: llvm-mc -triple=gpuvm -filetype=obj -gpuvm-no-pack %%s -o %%t.o\n"
           "# RUN: llvm-objdump -d -z %%t.o | FileCheck %%s\n\n");
    for (const Case &c : cases) {
        if (c.w.size() != 4) fprintf(stderr, "%s: %zu words\n", c.src.c_str(), c.w.size());
        printf("%s ;;\n# CHECK: ", c.src.c_str());
        for (int i = 0; i < 4; i++)
            for (int b = 0; b < 4; b++) printf("%02x%s", (c.w[i] >> (8 * b)) & 255, i == 3 && b == 3 ? "" : " ");
        printf(" %s ;;\n", c.dis.c_str());
    }
}
