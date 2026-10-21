// isabench: the prototype ISA's tick (tick.hlsl) on D3D11, running hand-assembled programs, timed
// by the GPU's clock and checked against a reference interpreter on the host (TICK.md).
//
//   isabench [--warp] [--prog NAME[,NAME]] [--passes N] [--warm N] [--ticks N] [-D NAME=VALUE]...
#define NOMINMAX
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
#include <unordered_map>
#include <vector>

// ---- The ISA (README.md, "The control word")
enum { K_INT = 0, K_MEM = 1, K_FPU = 2, K_OTHER = 3 };
enum : uint32_t {
    B_NEG = 1u << 2, B_UNS = 1u << 6, B_EQ = 1u << 7, B_INV = 1u << 8, B_BR = 1u << 9, B_SELV = 1u << 10,
    B_STORE = 1u << 2, B_NC = 1u << 3, B_MUNS = 1u << 9, B_SFU = 1u << 2, B_DIV = 1u << 3, B_JALR = 1u << 4,
    B_HIGH = 1u << 5, B_SIGNA = 1u << 6, B_SIGNB = 1u << 7, B_SYS = 1u << 8,
};
enum { S_SUM, S_AND, S_OR, S_XOR, S_SLL, S_SRL, S_SRA, S_CMP };
static uint32_t SINK = 64;   // the register a result nobody wants goes to: --regs N makes it N - 1
static const uint32_t CODE_BASE = 0x80400000u, WC_N = 256, CODE_SHIFT = 12, RAM_SHIFT = 13;
static const uint32_t STATE_PIX = (20 + WC_N + 7) / 8, CTL_T = 17;

struct Slot { uint32_t x, ra, rdrb, imm; };
struct Asm {
    std::vector<Slot> code;
    std::vector<std::pair<uint32_t, uint32_t>> ram;   // (byte address, word) at the start
    uint32_t here() const { return (uint32_t)code.size(); }
    void emit(uint32_t x, uint32_t rd, uint32_t ra, uint32_t rb, uint32_t imm) { code.push_back({x, ra, rd | rb << 16, imm}); }
    void op(int sel, uint32_t flags, uint32_t rd, uint32_t ra, uint32_t rb, uint32_t imm) { emit(K_INT | sel << 3 | flags, rd, ra, rb, imm); }
    void add(uint32_t rd, uint32_t ra, uint32_t rb) { op(S_SUM, 0, rd, ra, rb, 0); }
    void addi(uint32_t rd, uint32_t ra, int32_t i) { op(S_SUM, 0, rd, ra, 0, (uint32_t)i); }
    void sub(uint32_t rd, uint32_t ra, uint32_t rb) { op(S_SUM, B_NEG, rd, ra, rb, 0); }
    void bne(uint32_t ra, uint32_t rb, uint32_t to) { op(S_SUM, B_BR | B_EQ | B_INV, SINK, ra, rb, 0); at_target(to); }
    void beq(uint32_t ra, uint32_t rb, uint32_t to) { op(S_SUM, B_BR | B_EQ, SINK, ra, rb, 0); at_target(to); }
    void jal(uint32_t rd, uint32_t to) {   // r0 + link, "not equal" to the link: always taken
        op(S_SUM, B_BR | B_EQ | B_INV | B_SELV, rd, 0, 0, CODE_BASE + 4 * (here() + 1)); at_target(to);
    }
    void jr(uint32_t ra) { emit(K_OTHER | B_JALR, SINK, ra, 0, 0); }
    void lw(uint32_t rd, uint32_t ra, int32_t off) { emit(K_MEM, rd, ra, 0, (uint32_t)off); }
    void lwx(uint32_t rd, uint32_t ra, uint32_t rb) { emit(K_MEM, rd, ra, rb, 0); }
    void sw(uint32_t rs, uint32_t ra, int32_t off) { emit(K_MEM | B_STORE, rs, ra, 0, (uint32_t)off); }
    void swx(uint32_t rs, uint32_t ra, uint32_t rb) { emit(K_MEM | B_STORE, rs, ra, rb, 0); }
    // bit 10 on a branch: predicted taken (steps/s9.hlsl): backward branches; a sum's bit 10 means nothing else
    void at_target(uint32_t to) { code.back().x |= to << 11 | (to < here() ? 1u << 10 : 0); }
};

// ---- Bundles (steps/s4.hlsl): two of step 1's sums a texel, (control, imm) each; control is rd | ra << 6 |
// rb << 12, then EQ, INV, BR, ONE (no second op), the target bundle from bit 22. Only the second branches.
enum : uint32_t { U_EQ = 1u << 18, U_INV = 1u << 19, U_BR = 1u << 20, U_ONE = 1u << 21 };
struct Bundles { std::vector<Slot> code; std::vector<uint32_t> first, imm2; };   // first: each bundle's first slot; imm2: its second op's immediate
static Bundles bundle(const std::vector<Slot>& in) {
    std::vector<bool> target(in.size() + 1, false);
    for (const Slot& s : in) if (s.x & B_BR) target[s.x >> 11] = true;
    auto ctl = [](const Slot& s) {
        if ((s.x & 3) != K_INT || ((s.x >> 3) & 7) != S_SUM || (s.x & (B_NEG | B_UNS | B_SELV))) { printf("bundle: not a sum\n"); exit(1); }
        return (s.rdrb & 63) | (s.ra & 63) << 6 | ((s.rdrb >> 16) & 63) << 12 | ((s.x & B_EQ) ? U_EQ : 0) |
               ((s.x & B_INV) ? U_INV : 0) | ((s.x & B_BR) ? U_BR : 0);
    };
    Bundles b;
    std::vector<uint32_t> at(in.size() + 1, 0);
    for (size_t i = 0; i < in.size();) {
        at[i] = (uint32_t)b.code.size();
        b.first.push_back((uint32_t)i);
        bool pair = !(in[i].x & B_BR) && i + 1 < in.size() && !target[i + 1];
        if (pair) { at[i + 1] = at[i]; b.code.push_back({ctl(in[i]), in[i].imm, ctl(in[i + 1]), in[i + 1].imm}); i += 2; }
        else { b.code.push_back({SINK, 0, ctl(in[i]) | U_ONE, in[i].imm}); i += 1; }   // the first op a no-op into the sink
    }
    for (Slot& s : b.code) if (s.rdrb & U_BR) s.rdrb |= at[in[b.first[&s - &b.code[0]] + ((s.rdrb & U_ONE) ? 0 : 1)].x >> 11] << 22;
    return b;
}

// ---- Pairs (steps/s5.hlsl): two step-1 slots a bundle, as they are; the second alone may branch (to a bundle),
// bit 15 of its third word says the first is none. With par, only ops the second does not depend on are paired.
static Bundles pairs(const std::vector<Slot>& in, bool par) {
    std::vector<bool> target(in.size() + 1, false);
    for (const Slot& s : in) if (s.x & B_BR) target[s.x >> 11] = true;
    Bundles b;
    std::vector<uint32_t> at(in.size() + 1, 0);
    for (size_t i = 0; i < in.size();) {
        at[i] = (uint32_t)(b.code.size() / 2);
        b.first.push_back((uint32_t)i);
        bool pair = !(in[i].x & B_BR) && i + 1 < in.size() && !target[i + 1];
        if (pair && par) {
            uint32_t rd = in[i].rdrb & 0xffff, x = in[i + 1].x;
            pair = in[i + 1].ra != rd && (in[i + 1].rdrb >> 16) != rd && (in[i + 1].rdrb & 0xffff) != rd && !((x & 3) == K_MEM && (x & B_STORE));
        }
        if (pair) { at[i + 1] = at[i]; b.code.push_back(in[i]); b.code.push_back(in[i + 1]); i += 2; }
        else { b.code.push_back({K_INT, 0, 0, 0}); b.code.push_back(in[i]); b.code.back().rdrb |= 1u << 15; i += 1; }
    }
    for (size_t k = 1; k < b.code.size(); k += 2) {
        Slot& s = b.code[k];
        if (s.x & B_BR) s.x = (s.x & 2047) | at[s.x >> 11] << 11;
    }
    return b;
}

// ---- Slots (steps/s7.hlsl): an integer op (which alone may branch, to a slot) and a memory op, read together and
// done side by side: neighbours are paired when the second reads nothing the first writes and they write apart.
static Bundles slots(const std::vector<Slot>& in) {
    std::vector<bool> target(in.size() + 1, false);
    for (const Slot& s : in) if (s.x & B_BR) target[s.x >> 11] = true;
    auto is_mem = [](const Slot& s) { return (s.x & 3) == K_MEM; };
    auto rd = [](const Slot& s) { return s.rdrb & 0xffff; };
    auto reads = [&](const Slot& s, uint32_t r) { return s.ra == r || (s.rdrb >> 16) == r || (is_mem(s) && (s.x & B_STORE) && rd(s) == r); };
    const Slot nopA = {K_INT, 0, 0, 0}, nopM = {K_INT, 0, SINK | 1u << 15, 0};
    Bundles b;
    std::vector<uint32_t> at(in.size() + 1, 0);
    for (size_t i = 0; i < in.size();) {
        at[i] = (uint32_t)(b.code.size() / 2);
        b.first.push_back((uint32_t)i);
        const Slot& u = in[i];
        bool two = i + 1 < in.size() && !target[i + 1] && !(u.x & B_BR) && is_mem(u) != is_mem(in[i + 1]) &&
                   !reads(in[i + 1], rd(u)) && rd(in[i + 1]) != rd(u);
        if (two) {
            const Slot& v = in[i + 1];
            at[i + 1] = at[i];
            b.code.push_back(is_mem(u) ? v : u);
            b.code.push_back(is_mem(u) ? u : v);
            i += 2;
        } else if (is_mem(u)) { b.code.push_back(nopA); b.code.push_back(u); b.code.back().rdrb |= 1u << 15; i += 1; }
        else { b.code.push_back(u); b.code.push_back(nopM); i += 1; }
    }
    for (size_t k = 0; k < b.code.size(); k += 2) {
        Slot& s = b.code[k];
        if (s.x & B_BR) s.x = (s.x & 2047) | at[s.x >> 11] << 11;
    }
    return b;
}

// ---- Groups (steps/s10.hlsl): K step-1 slots a group, one after the other; only the last may branch (to a group),
// a group starts at every branch target, empty places are r0 = r0 + r0. Bits 8-11 of the first slot's third
// word: the ops that count.
static Bundles groups(const std::vector<Slot>& in, uint32_t K) {
    std::vector<bool> target(in.size() + 1, false);
    for (const Slot& s : in) if (s.x & B_BR) target[s.x >> 11] = true;
    Bundles b;
    std::vector<uint32_t> at(in.size() + 1, 0);
    std::vector<uint32_t> last;   // each group's last real slot, for its branch
    for (size_t i = 0; i < in.size();) {
        uint32_t g = (uint32_t)(b.code.size() / K), m = 0;
        b.first.push_back((uint32_t)i);
        std::vector<Slot> ops;
        while (i < in.size() && m < K && (m == 0 || !target[i])) {
            at[i] = g;
            bool br = (in[i].x & B_BR) != 0;
            if (br && m < K - 1) { while (ops.size() < K - 1) ops.push_back({K_INT, 0, 0, 0}); }
            ops.push_back(in[i]); m++; i++;
            if (br) break;
        }
        while (ops.size() < K) ops.insert(ops.end(), {K_INT, 0, 0, 0});
        ops[0].rdrb |= m << 8;
        for (Slot& o : ops) b.code.push_back(o);
    }
    for (size_t k = K - 1; k < b.code.size(); k += K) {
        Slot& s = b.code[k];
        if (s.x & B_BR) s.x = (s.x & 2047) | at[s.x >> 11] << 11;
    }
    return b;
}

// ---- Decoded groups (steps/s11.hlsl): K ops a group, two words of four an op: (ra, rb, rd | target << 8, imm) and
// (negation mask, AND mask, XOR mask, control). A group starts at every target; only its last op branches; at most
// one store a group and no load after it. The first op's control holds the count of real ops in bits 16-19.
enum : uint32_t { D_SH = 1u << 0, D_SHR = 1u << 1, D_SRA = 1u << 2, D_CMP = 1u << 3, D_LG = 1u << 4, D_EQ = 1u << 5,
                  D_INV = 1u << 6, D_BR = 1u << 7, D_MEM = 1u << 8, D_ST = 1u << 9, D_SIGNED = 1u << 31 };
static Bundles decoded(const std::vector<Slot>& in, uint32_t K) {
    std::vector<bool> target(in.size() + 1, false);
    for (const Slot& s : in) if (s.x & B_BR) target[s.x >> 11] = true;
    auto dec = [](const Slot& s, Slot& q0, Slot& q1) {
        uint32_t x = s.x, sel = (x >> 3) & 7, c = 0, neg = 0, m1 = 0, m2 = 0, imm = s.imm;
        q0 = {s.ra, s.rdrb >> 16, s.rdrb & 0xffff, 0};
        if ((x & 3) == K_MEM) {
            if ((x >> 4) & 31) { printf("decoded: only word loads and stores\n"); exit(1); }
            c = D_MEM | ((x & B_STORE) ? D_ST : 0);
        } else if ((x & 3) == K_INT) {
            if (sel == S_SUM && (x & B_NEG)) { neg = ~0u; imm = 1u - imm; }
            if (sel == S_AND) { c |= D_LG; m1 = ~0u; }
            if (sel == S_OR) { c |= D_LG; m1 = m2 = ~0u; }
            if (sel == S_XOR) { c |= D_LG; m2 = ~0u; }
            if (sel == S_SLL) c |= D_SH;
            if (sel == S_SRL) c |= D_SH | D_SHR;
            if (sel == S_SRA) c |= D_SH | D_SHR | D_SRA;
            if (sel == S_CMP) c |= D_CMP;
            c |= ((x & B_EQ) ? D_EQ : 0) | ((x & B_INV) ? D_INV : 0) | ((x & B_UNS) ? 0 : D_SIGNED) | ((x & B_BR) ? D_BR : 0);
        } else { printf("decoded: no such instruction\n"); exit(1); }
        q0.imm = imm;
        q1 = {neg, m1, m2, c};
    };
    Bundles b;
    std::vector<uint32_t> at(in.size() + 1, 0), brAt;
    const Slot fill0 = {0, 0, 0, 0}, fill1 = {0, 0, 0, 0};
    for (size_t i = 0; i < in.size();) {
        uint32_t g = (uint32_t)(b.code.size() / (2 * K)), m = 0;
        b.first.push_back((uint32_t)i);
        std::vector<Slot> ops;
        bool stored = false;
        while (i < in.size() && m < K && (m == 0 || !target[i])) {
            const Slot& u = in[i];
            bool mem = (u.x & 3) == K_MEM, st = mem && (u.x & B_STORE), br = (u.x & B_BR) != 0;
            if (m > 0 && mem && stored) break;   // nothing of memory after the group's store
            at[i] = g;
            Slot q0, q1;
            dec(u, q0, q1);
            if (br) while (ops.size() < 2 * (K - 1)) { ops.push_back(fill0); ops.push_back(fill1); }
            ops.push_back(q0); ops.push_back(q1);
            m++; i++;
            stored = stored || st;
            if (br) break;
        }
        while (ops.size() < 2 * K) { ops.push_back(fill0); ops.push_back(fill1); }
        ops[1].imm |= m << 16;
        for (Slot& o : ops) b.code.push_back(o);
    }
    for (size_t k = 2 * (K - 1); k < b.code.size(); k += 2 * K) {
        Slot& q0 = b.code[k];
        const Slot& q1 = b.code[k + 1];
        if (q1.imm & D_BR) {
            uint32_t g = (uint32_t)(k / (2 * K)), src = 0;   // the input slot of this branch: the group's last real op
            for (size_t j = 0; j < in.size(); j++) if (at[j] == g && (in[j].x & B_BR)) src = (uint32_t)j;
            q0.rdrb |= at[in[src].x >> 11] << 8;
        }
    }
    return b;
}

// ---- Roles (steps/s12.hlsl): four step-1 slots a group, each place with its role: 0 the whole integer unit, 1 a
// sum, 2 a sum or a word load or store, 3 a sum or a branch. Ops keep their order, a place passed over is r0 = r0 +
// r0; a group starts at every target and ends at a branch. Bits 8-11 of the first slot's third word: the count.
static Bundles roles(const std::vector<Slot>& in) {
    std::vector<bool> target(in.size() + 1, false);
    for (const Slot& s : in) if (s.x & B_BR) target[s.x >> 11] = true;
    auto fits = [](const Slot& s, int p) {
        uint32_t k = s.x & 3, sel = (s.x >> 3) & 7;
        bool br = (s.x & B_BR) != 0, sum = k == K_INT && sel == S_SUM && !br;
        if (k == K_MEM) return p == 2 && ((s.x >> 4) & 31) == 0;
        if (k != K_INT) return false;
        if (br) return p == 3 && sel == S_SUM;
        return p == 0 || sum;
    };
    Bundles b;
    std::vector<uint32_t> at(in.size() + 1, 0);
    const Slot fill = {K_INT, 0, 0, 0};
    for (size_t i = 0; i < in.size();) {
        uint32_t g = (uint32_t)(b.code.size() / 4), m = 0;
        b.first.push_back((uint32_t)i);
        Slot ops[4] = {fill, fill, fill, fill};
        int p = 0;
        while (i < in.size() && (m == 0 || !target[i])) {
            int q = p;
            while (q < 4 && !fits(in[i], q)) q++;
            if (q == 4) { if (m == 0) { printf("roles: slot %zu fits nowhere\n", i); exit(1); } break; }
            at[i] = g;
            ops[q] = in[i];
            m++; p = q + 1;
            bool br = (in[i].x & B_BR) != 0;
            i++;
            if (br || p == 4) break;
        }
        ops[0].rdrb |= m << 8;
        for (Slot& o : ops) b.code.push_back(o);
    }
    for (size_t k = 3; k < b.code.size(); k += 4) {
        Slot& s = b.code[k];
        if (s.x & B_BR) s.x = (s.x & 2047) | at[s.x >> 11] << 11;
    }
    return b;
}

// ---- Fused (steps/s13.hlsl): an op and a sum beside it, (control, ra | re << 8 | rf << 16 | imm8 << 24,
// rd | count << 8 | rb << 16, imm). Neighbours in a block pair when one is a plain sum with a small immediate
// and they write apart: the op then the sum, or the sum first when the op comes second.
static const uint32_t F_FIRST = 1u << 23;
static Bundles fused(const std::vector<Slot>& in) {
    std::vector<bool> target(in.size() + 1, false);
    for (const Slot& s : in) if (s.x & B_BR) target[s.x >> 11] = true;
    auto rd = [](const Slot& s) { return s.rdrb & 0xffff; };
    auto plain = [](const Slot& s) { return (s.x & 3) != K_OTHER && !(s.x & B_SELV); };
    auto sum = [](const Slot& s) {
        int32_t i = (int32_t)s.imm;
        return (s.x & 3) == K_INT && ((s.x >> 3) & 7) == S_SUM && !(s.x & (B_BR | B_SELV | B_UNS | B_EQ | B_INV)) && i >= -128 && i < 128;
    };
    auto op = [&](const Slot& s, uint32_t count) { return Slot{s.x & 0x7fffffu, s.ra | SINK << 8, (rd(s) & 0xff) | count << 8 | (s.rdrb >> 16) << 16, s.imm}; };
    auto with = [&](Slot o, const Slot& m, bool first) {
        o.x |= (first ? F_FIRST : 0) | (m.rdrb >> 16) << 24 | ((m.x & B_NEG) ? 1u << 30 : 0);
        o.ra = (o.ra & 0xff) | rd(m) << 8 | m.ra << 16 | (m.imm & 0xff) << 24;
        return o;
    };
    Bundles b;
    std::vector<uint32_t> at(in.size() + 1, 0);
    for (size_t i = 0; i < in.size();) {
        at[i] = (uint32_t)b.code.size();
        b.first.push_back((uint32_t)i);
        const Slot& p = in[i];
        bool two = i + 1 < in.size() && !target[i + 1] && plain(p) && plain(in[i + 1]) && rd(p) != rd(in[i + 1]);
        if (two && !(p.x & B_BR) && sum(in[i + 1])) { b.code.push_back(with(op(p, 2), in[i + 1], false)); at[i + 1] = at[i]; i += 2; }
        else if (two && sum(p)) { b.code.push_back(with(op(in[i + 1], 2), p, true)); at[i + 1] = at[i]; i += 2; }
        else { b.code.push_back(op(p, 1)); i += 1; }
    }
    for (Slot& s : b.code) if (s.x & B_BR) s.x = (s.x & 2047) | at[(s.x >> 11) & 4095] << 11 | (s.x & 0x7f800000u);
    return b;
}

// ---- Fused, after only (steps/s14.hlsl): (control | rg << 12 | target << 20, ra | rf << 8 | re << 16 | imm8 << 24,
// rd | count << 8 | rb << 16, imm). An op and a sum after it; a sum and the branch on its result as one op that
// compares its sum with r[rg] (bit 10). 16 registers.
static bool g_single = false;   // --single: s14's layout, no op paired
static Bundles fused_after(const std::vector<Slot>& in) {
    std::vector<bool> target(in.size() + 1, false);
    for (const Slot& s : in) if (s.x & B_BR) target[s.x >> 11] = true;
    auto rd = [](const Slot& s) { return s.rdrb & 0xffff; };
    auto rb = [](const Slot& s) { return s.rdrb >> 16; };
    auto plain = [](const Slot& s) { return (s.x & 3) != K_OTHER; };
    auto is_sum = [](const Slot& s) { return (s.x & 3) == K_INT && ((s.x >> 3) & 7) == S_SUM && !(s.x & (B_BR | B_SELV | B_UNS | B_EQ | B_INV)); };
    auto fits8 = [](const Slot& s) { int32_t i = (int32_t)s.imm; return i >= -128 && i < 128 && !(s.x & B_NEG); };
    auto one = [&](const Slot& s, uint32_t count) { return Slot{s.x & 0x3ffu & ~(uint32_t)1024, s.ra | 15u << 16, rd(s) | count << 8 | rb(s) << 16, s.imm}; };
    Bundles b;
    std::vector<uint32_t> at(in.size() + 1, 0);
    for (size_t i = 0; i < in.size();) {
        at[i] = (uint32_t)b.code.size();
        b.first.push_back((uint32_t)i);
        const Slot& p = in[i];
        const Slot* q = !g_single && i + 1 < in.size() && !target[i + 1] && plain(p) && plain(in[i + 1]) ? &in[i + 1] : nullptr;
        uint32_t x = p.x & 0x3ff & ~(uint32_t)1024;
        if (q && !(p.x & B_BR) && is_sum(*q) && fits8(*q)) {   // the op, then the sum
            Slot o = one(p, 2);
            o.x |= rb(*q) << 12;
            o.ra = p.ra | q->ra << 8 | rd(*q) << 16 | (q->imm & 0xff) << 24;
            b.code.push_back(o); at[i + 1] = at[i]; i += 2;
        } else if (q && is_sum(p) && (q->x & B_BR) && !(q->x & B_SELV) && ((q->x >> 3) & 7) == S_SUM && q->ra == rd(p) && q->imm == 0) {
            Slot o = one(p, 2);   // the sum's operands and result, the branch's control, compared with r[rg]
            o.x = (q->x & 0x3ff & ~(uint32_t)1024) | (p.x & B_NEG) | 1024 | rb(*q) << 12 | (q->x >> 11) << 20;
            b.code.push_back(o); at[i + 1] = at[i]; i += 2;
        } else {
            Slot o = one(p, 1);
            o.x = x | (p.x & B_BR ? (p.x >> 11) << 20 : 0);
            b.code.push_back(o); i += 1;
        }
    }
    for (Slot& s : b.code) if (s.x & B_BR) s.x = (s.x & 0xfffff) | at[s.x >> 20] << 20;
    return b;
}

// ---- Wide (steps/s15.hlsl): s14's layout; the second slot is a sum after the op, a load (bit 11) or the store
// of the next word (bit 16). Neighbours in a block pair in this order: a loop's step and its branch (EQ or NE
// on the old value, the step after); an op and a sum; a sum and an op that has nothing to do with it, swapped;
// an op and a load that does not read its result; a store and the store of the word after it.
static const uint32_t W_LD2 = 1u << 11, W_ST2 = 1u << 16;
static bool g_nostep = false;   // --nostep: a loop's step and branch not paired
static bool g_stepsame = false;   // --stepsame: a loop's branch writes its step's register too (overwritten)
static bool g_nocommit = false;   // --nocommit: no commit pass (a probe; stores are lost)
static bool g_codert = false;   // --codert: the code texture made a render target too (a probe)
static bool g_ram4 = false;     // --ram4 (steps/s20.hlsl): RAM in 16-byte texels, code a slot a texel in it
static bool g_wordwc = false;   // --wordwc (steps/s21.hlsl): the commit in four draws, a component each
static bool g_ram = false;      // --ram (steps/s19.hlsl): words in RAM, whose immediates are 7 bits (an op) and 14 (a second)
static bool g_sched2 = false;   // --sched2 (steps/s18.hlsl): the scheduler with its fused forms
static bool g_alu2 = false;     // --wide2 (steps/s16.hlsl): the second slot any integer op but a compare (bits 17-19)
static Bundles wide(const std::vector<Slot>& in) {
    std::vector<bool> target(in.size() + 1, false);
    for (const Slot& s : in) if (s.x & B_BR) target[s.x >> 11] = true;
    auto rd = [](const Slot& s) { return s.rdrb & 0xffff; };
    auto rb = [](const Slot& s) { return s.rdrb >> 16; };
    // (bit 10 selects a value only in a compare; on a backward branch it is the prediction)
    auto plain = [](const Slot& s) { return (s.x & 3) != K_OTHER && !((s.x & B_SELV) && ((s.x >> 3) & 7) == S_CMP); };
    auto br = [](const Slot& s) { return (s.x & B_BR) != 0; };
    auto is_sum = [](const Slot& s) { return (s.x & 3) == K_INT && ((s.x >> 3) & 7) == S_SUM && !(s.x & (B_BR | B_SELV | B_UNS | B_EQ | B_INV)); };
    auto fits8 = [](uint32_t imm) { int32_t i = (int32_t)imm; return i >= -128 && i < 128; };
    auto word = [](const Slot& s, bool st) { return (s.x & 3) == K_MEM && ((s.x & B_STORE) != 0) == st && ((s.x >> 4) & 31) == 0; };
    auto reads = [&](const Slot& s, uint32_t r) { return s.ra == r || rb(s) == r || (word(s, true) && rd(s) == r); };
    auto ctl = [](const Slot& s) { return s.x & 0x3ff & ~(uint32_t)1024; };
    auto sec = [&](const Slot& s) {   // what the second slot does for it (a sum alone, without --wide2), or -1
        uint32_t sel = (s.x >> 3) & 7;
        if ((s.x & 3) != K_INT || (s.x & (B_BR | B_SELV | B_UNS | B_EQ | B_INV)) || sel == S_CMP || !fits8(s.imm)) return -1;
        if (sel == S_SUM) return (s.x & B_NEG) ? (g_alu2 ? 7 : -1) : 0;
        return g_alu2 ? (int)sel : -1;
    };
    auto one = [&](const Slot& s, uint32_t count) {   // the op alone; its second slot a sum into the sink
        return Slot{ctl(s) | (br(s) ? (s.x >> 11) << 20 : 0), s.ra | 15u << 16, rd(s) | count << 8 | rb(s) << 16, s.imm};
    };
    auto second = [&](Slot o, uint32_t flag, uint32_t re, uint32_t rf, uint32_t rg, uint32_t imm8) {
        o.x |= flag | rg << 12;
        o.ra = (o.ra & 0xff) | rf << 8 | re << 16 | (imm8 & 0xff) << 24;
        return o;
    };
    Bundles b;
    std::vector<uint32_t> at(in.size() + 1, 0);
    for (size_t i = 0; i < in.size();) {
        at[i] = (uint32_t)b.code.size();
        b.first.push_back((uint32_t)i);
        const Slot& p = in[i];
        const Slot* q = !g_single && i + 1 < in.size() && !target[i + 1] && plain(p) && plain(in[i + 1]) ? &in[i + 1] : nullptr;
        Slot o;
        if (q && !g_nostep && is_sum(p) && p.ra == rd(p) && rb(p) == 0 && fits8(p.imm) && br(*q) && ((q->x >> 3) & 7) == S_SUM &&
            (q->x & B_EQ) && q->ra == rd(p) && rb(*q) != rd(p) && q->imm == 0) {
            o = one(*q, 2);
            o.imm = 0u - p.imm;
            if (g_stepsame) o.rdrb = (o.rdrb & ~0xffu) | rd(p);
            o = second(o, 0, rd(p), rd(p), 0, p.imm);
        } else if (q && !br(p) && sec(*q) >= 0) {
            o = second(one(p, 2), (uint32_t)sec(*q) << 17, rd(*q), q->ra, rb(*q), q->imm);
        } else if (q && sec(p) >= 0 && !br(*q) && !reads(*q, rd(p)) && rd(*q) != rd(p) &&
                   rd(*q) != p.ra && rd(*q) != rb(p)) {
            o = second(one(*q, 2), (uint32_t)sec(p) << 17, rd(p), p.ra, rb(p), p.imm);
        } else if (q && !br(p) && word(*q, false) && rb(*q) == 0 && fits8(q->imm) && q->ra != rd(p)) {
            o = second(one(p, 2), W_LD2, rd(*q), q->ra, 0, q->imm);
        } else if (q && word(p, true) && word(*q, true) && rb(p) == 0 && rb(*q) == 0 && q->ra == p.ra && q->imm == p.imm + 4) {
            o = second(one(p, 2), W_ST2, 15, 0, rd(*q), 0);
        } else {
            b.code.push_back(one(p, 1)); i += 1;
            continue;
        }
        b.code.push_back(o); at[i + 1] = at[i]; i += 2;
    }
    for (Slot& s : b.code) if (s.x & B_BR) s.x = (s.x & 0xfffff) | at[s.x >> 20] << 20;
    return b;
}

// ---- Scheduled (steps/s17.hlsl): s16's slots chosen over each block's dependences, the fewest trips by search
// over what is done. A pass may end only before a slot whose ops before it are a prefix of the block (bit 4 of
// the second word); bit 5 gives the second slot the op's immediate word when the op's own is 0. With fuse
// (steps/s18.hlsl) a loop's step and branch are one op and an add then a mask one second slot ("units").
static const uint32_t S_ENDOK = 16, S_BIG = 32, S_ADDAND = 4096, S_STEP = 1024;
enum { U_OP, U_ADDAND, U_BRSTEP };
static Bundles scheduled(const std::vector<Slot>& in, bool fuse) {
    const size_t n = in.size();
    auto rd = [](const Slot& s) { return s.rdrb & 0xffff; };
    auto rb = [](const Slot& s) { return s.rdrb >> 16; };
    auto br = [](const Slot& s) { return (s.x & B_BR) != 0 || (s.x & 3) == K_OTHER; };
    auto fits8 = [](uint32_t imm) { int32_t i = (int32_t)imm; return i >= -128 && i < 128; };
    auto word = [](const Slot& s, bool st) { return (s.x & 3) == K_MEM && ((s.x & B_STORE) != 0) == st && ((s.x >> 4) & 31) == 0; };
    auto mem = [](const Slot& s) { return (s.x & 3) == K_MEM; };
    auto store = [](const Slot& s) { return (s.x & 3) == K_MEM && (s.x & B_STORE); };
    auto W = [&](const Slot& s) { uint32_t r = rd(s); return store(s) || r == 0 || r == SINK ? -1 : (int)r; };
    auto R = [&](const Slot& s, int r) { return r > 0 && (s.ra == (uint32_t)r || rb(s) == (uint32_t)r || (store(s) && rd(s) == (uint32_t)r)); };
    auto raw = [&](const Slot& a, const Slot& b) { return W(a) >= 0 && R(b, W(a)); };
    auto dep = [&](const Slot& a, const Slot& b) {
        return raw(a, b) || (W(b) >= 0 && R(a, W(b))) || (W(a) >= 0 && W(a) == W(b)) || (mem(a) && mem(b) && (store(a) || store(b)));
    };
    auto alu = [&](const Slot& s) {   // the second slot's unit for s (sel, sub as 7), or -1
        uint32_t sel = (s.x >> 3) & 7;
        if ((s.x & 3) != K_INT || (s.x & (B_BR | B_SELV | B_UNS | B_EQ | B_INV)) || sel == S_CMP) return -1;
        return sel == S_SUM && (s.x & B_NEG) ? 7 : (int)sel;
    };
    auto step = [&](const Slot& p, const Slot& q) {   // q a branch on p's step: the branch on the old value
        return alu(p) == 0 && p.ra == rd(p) && rb(p) == 0 && fits8(p.imm) && (q.x & B_BR) && !(q.x & 3) && ((q.x >> 3) & 7) == S_SUM &&
               (q.x & B_EQ) && q.ra == rd(p) && rb(q) != rd(p) && q.imm == 0;
    };
    Bundles b;
    std::vector<uint32_t> at(n + 1, 0);
    std::vector<bool> leader(n + 1, false);
    leader[0] = true;
    for (size_t i = 0; i < n; i++) if (br(in[i])) { if ((in[i].x & 3) != K_OTHER) leader[in[i].x >> 11] = true; leader[i + 1] = true; }
    for (size_t s = 0; s < n;) {
        size_t e = s + 1;
        while (e < n && !leader[e]) e++;
        // the block's units: an op, or with fuse an add then a mask of the same register, or a step and its branch
        std::vector<Slot> U;
        std::vector<int> kind, len, orig;
        for (size_t i = s; i < e;) {
            const Slot& p = in[i];
            if (fuse && i + 1 < e && alu(p) == 0 && p.ra == rd(p) && rb(p) == 0 && W(p) >= 0 && alu(in[i + 1]) == S_AND &&
                in[i + 1].ra == rd(p) && rd(in[i + 1]) == rd(p) && rb(in[i + 1]) != rd(p) && rb(in[i + 1]) != 0 && in[i + 1].imm == 0) {
                U.push_back({K_INT | S_AND << 3, p.ra, rd(p) | rb(in[i + 1]) << 16, p.imm}); kind.push_back(U_ADDAND);
                len.push_back(2); orig.push_back((int)i); i += 2; continue;
            }
            if (fuse && i + 1 < e && step(p, in[i + 1]) && (int32_t)p.imm >= -8 && (int32_t)p.imm < 8) {
                const Slot& q = in[i + 1];
                U.push_back({q.x, q.ra, rd(p) | rb(q) << 16, p.imm}); kind.push_back(U_BRSTEP);
                len.push_back(2); orig.push_back((int)i); i += 2; continue;
            }
            U.push_back(p); kind.push_back(U_OP); len.push_back(1); orig.push_back((int)i); i += 1;
        }
        // how c goes in m's second slot: 0 no, 1 the unit, 2 the unit on m's immediate word, 3 a load,
        // 4 the next word's store, 5 a step, 6 an add then a mask, 7 the same on m's immediate word
        auto how = [&](int mi, int ci, bool after) {
            const Slot &m = U[mi], &c = U[ci];
            if (br(c) || kind[mi] == U_ADDAND) return 0;
            bool d = dep(m, c);
            uint32_t mimm = kind[mi] == U_BRSTEP ? 0u - m.imm : m.imm;
            if (br(m)) {
                if ((m.x & 3) == K_OTHER || (kind[mi] == U_OP && W(m) >= 0)) return 0;   // (a jal writes its link)
                if (kind[mi] == U_OP && kind[ci] == U_OP && step(c, m)) return 5;
                if (d || mem(c)) return 0;
            } else if (!after && d) return 0;
            if (g_ram && (int32_t)mimm != ((int32_t)(mimm << 25) >> 25)) return 0;   // (an op's literal takes its second word)
            auto fits2 = [&](uint32_t i) { return g_ram ? (int32_t)i == ((int32_t)(i << 18) >> 18) : fits8(i); };
            if (kind[ci] == U_ADDAND) return fits2(c.imm) ? 6 : (mimm == 0 && !br(m) && !g_ram ? 7 : 0);
            if (alu(c) >= 0) return fits2(c.imm) ? 1 : (mimm == 0 && !br(m) && !g_ram ? 2 : 0);
            if (br(m)) return 0;
            if (word(c, false) && fits2(c.imm) && !raw(m, c)) return 3;
            if (after && word(m, true) && word(c, true) && rb(m) == 0 && rb(c) == 0 && c.ra == m.ra && c.imm == m.imm + 4) return 4;
            return 0;
        };
        const int k = (int)U.size(), full = (1 << k) - 1;
        // f(done): the fewest trips for the rest, and the choice (m, c or -1)
        std::unordered_map<uint32_t, std::pair<int, int>> memo;
        std::function<int(uint32_t)> f = [&](uint32_t done) -> int {
            if ((int)done == full) return 0;
            auto it = memo.find(done);
            if (it != memo.end()) return it->second.first;
            int best = 1 << 20, choice = 0;
            for (int m = 0; m < k; m++) {
                if (done >> m & 1) continue;
                const Slot& M = U[m];
                bool ready = true;
                for (int i = 0; i < m && ready; i++) if (!(done >> i & 1) && (dep(U[i], M) || br(M))) ready = false;
                for (int c = -1; c < k; c++) {
                    if (c == m || (c >= 0 && (done >> c & 1))) continue;
                    uint32_t nd = done | 1u << m | (c >= 0 ? 1u << c : 0);
                    if (br(M)) {   // the branch last: every other op done or its second slot
                        bool ok = true;
                        for (int i = 0; i < k; i++) if (!(nd >> i & 1)) ok = false;
                        if (!ok) continue;
                    } else if (!ready) break;
                    if (c >= 0) {
                        if (!how(m, c, c > m)) continue;
                        bool ok = true;
                        for (int i = 0; i < c && ok; i++) if (i != m && !(done >> i & 1) && dep(U[i], U[c])) ok = false;
                        if (!ok) continue;
                    }
                    int t = 1 + f(nd);
                    if (t < best) { best = t; choice = m << 8 | (c + 1); }
                }
            }
            memo[done] = {best, choice};
            return best;
        };
        f(0);
        at[s] = (uint32_t)b.code.size();
        for (uint32_t done = 0; (int)done != full;) {
            int ch = memo[done].second, m = ch >> 8, c = (ch & 255) - 1;
            const Slot& M = U[m];
            uint32_t count = len[m] + (c >= 0 ? len[c] : 0);
            Slot o{(M.x & 0x3ff & ~(uint32_t)1024) | (br(M) ? (M.x >> 11) << 20 : 0), M.ra | 15u << 16, rd(M) | count << 8 | rb(M) << 16, M.imm};
            uint32_t h = c >= 0 ? how(m, c, c > m) : 0;
            if (kind[m] == U_BRSTEP) { o.x |= S_STEP; o.imm = 0u - M.imm; o.rdrb |= (M.imm & 15) << 4; }
            int sc = c;
            if (kind[m] == U_ADDAND) { o = Slot{0x80000000u, 15u << 16, 15 | count << 8, 0}; sc = m; h = fits8(M.imm) || g_ram ? 6 : 7; }
            if (sc >= 0) {
                const Slot& C = U[sc];
                uint32_t flag = 0, re = rd(C), rf = C.ra, rg = rb(C), imm8 = C.imm;
                if (h == 1 || h == 2) flag = (uint32_t)alu(C) << 17;
                if (h == 2 || h == 7) { o.ra |= S_BIG; o.imm = C.imm; imm8 = 0; }
                if (h == 3) flag = W_LD2;
                if (h == 4) { flag = W_ST2; re = 15; rf = 0; rg = rd(C); imm8 = 0; }
                if (h == 5) { o.imm = 0u - C.imm; rf = rd(C); rg = 0; }
                if (h == 6 || h == 7) o.ra |= S_ADDAND;
                o.x |= flag | rg << 12;
                o.ra = (o.ra & 0xf0ff) | rf << 8 | re << 16 | (imm8 & 0xff) << 24;
            }
            uint32_t j = 0;   // a prefix of the block done before this slot: a pass may end here
            while ((int)j < k && (done >> j & 1)) j++;
            if ((done >> j) == 0) o.ra |= S_ENDOK;
            b.first.push_back((uint32_t)(j < (uint32_t)k ? orig[j] : (int)e));
            b.imm2.push_back(sc >= 0 && h != 4 ? U[sc].imm : 0);
            b.code.push_back(o);
            done |= 1u << m | (c >= 0 ? 1u << c : 0);
        }
        s = e;
    }
    for (Slot& s : b.code) if ((s.x & B_BR) && !(s.x & 2)) s.x = (s.x & 0xfffff) | at[s.x >> 20] << 20;
    return b;
}

// ---- Words in RAM (steps/s19.hlsl): a scheduled bundle as one or two words. The op: ra (bits 0-3), rd (4-7) or on
// a branch a signed word offset (4-11), sel (8-10), neg or store (11), uns, eq, inv (12-14), branch (15), memory
// (16), none (17), step (18), what the next word is (19-20: none, a second op, the op's literal), a 7-bit
// immediate (21-27), rb (28-31). A second op: rf, re, sel, load, store pair, add-and, a 14-bit immediate, rg.
static std::vector<uint32_t> ram_words(const Bundles& b, std::vector<uint32_t>& at) {
    const size_t n = b.code.size();
    auto fits = [](uint32_t v, int bits) { return (int32_t)v == ((int32_t)(v << (32 - bits)) >> (32 - bits)); };
    std::vector<uint32_t> next(n, 0), words;
    for (int round = 0; round < 8; round++) {   // lengths, then branch offsets; again while an offset needs a literal
        at.assign(n + 1, 0);
        for (size_t i = 0; i < n; i++) {
            const Slot& o = b.code[i];
            bool two = ((o.ra >> 16) & 15) != 15 || (o.x & (W_LD2 | W_ST2));
            if (two) next[i] = 1;
            else if (!(o.x & B_BR) && !fits(o.imm, 7)) next[i] = 2;
            at[i + 1] = at[i] + (next[i] ? 2 : 1);
        }
        bool again = false;
        for (size_t i = 0; i < n; i++) {
            const Slot& o = b.code[i];
            if ((o.x & B_BR) && next[i] != 2 && !fits(at[o.x >> 20] - at[i], 8)) {
                if (next[i] == 1) { printf("ram_words: a branch with a second op too far\n"); exit(1); }
                next[i] = 2; again = true;
            }
        }
        if (!again) break;
    }
    for (size_t i = 0; i < n; i++) {
        const Slot& o = b.code[i];
        uint32_t x = o.x, br = (x & B_BR) != 0, step = (x & S_STEP) != 0, imm = (o.ra & S_BIG) ? 0 : o.imm;
        uint32_t w = (o.ra & 15) | ((x >> 3) & 7) << 8 | ((x >> 2) & 1) << 11 | ((x >> 6) & 7) << 12 | br << 15 |
                     ((x & 3) == K_MEM) << 16 | (x >> 31) << 17 | step << 18 | next[i] << 19 | (o.rdrb >> 16 & 15) << 28;
        uint32_t off = br ? at[x >> 20] - at[i] : 0;
        if (br) w = (w & ~0xff0u) | (next[i] == 2 ? 0 : (off & 0xff) << 4) | (imm & 0x7f) << 21;
        else w |= (o.rdrb & 15) << 4 | (next[i] == 2 ? 0 : (imm & 0x7f) << 21);
        if (br && !step && (o.rdrb & 15) != 15) { printf("ram_words: a branch that writes r%u\n", o.rdrb & 15); exit(1); }
        words.push_back(w);
        if (next[i] == 2) words.push_back(br ? off : imm);
        if (next[i] == 1)
            words.push_back((o.ra >> 8 & 15) | (o.ra >> 16 & 15) << 4 | ((x >> 17) & 7) << 8 | ((x & W_LD2) != 0) << 11 |
                            ((x & W_ST2) != 0) << 12 | ((o.ra & S_ADDAND) != 0) << 13 | (b.imm2[i] & 0x3fff) << 14 | ((x >> 12) & 15) << 28);
    }
    return words;
}

// ---- The reference: the same instructions on the host
struct Machine {
    uint32_t r[68] = {}, pc = 0;
    std::vector<uint32_t> ram;
    void run(const std::vector<Slot>& code, uint64_t n) {
        for (uint64_t k = 0; k < n; k++) {
            const Slot& s = code[pc];
            uint32_t x = s.x, rd = s.rdrb & 0xffff, a = r[s.ra], b = r[s.rdrb >> 16] + s.imm, v = 0, next = pc + 1;
            int32_t sa = (int32_t)a, sb = (int32_t)b;
            switch (x & 3) {
            case K_INT: {
                bool c = (x & B_EQ) ? a == b : (x & B_UNS) ? a < b : sa < sb;
                if (x & B_INV) c = !c;
                uint32_t cv = (x & B_SELV) ? (c ? a : b) : (c ? 1 : 0);
                uint32_t vals[8] = {a + ((x & B_NEG) ? 0 - b : b), a & b, a | b, a ^ b, a << (b & 31), a >> (b & 31),
                                    (uint32_t)(sa >> (b & 31)), cv};
                v = vals[(x >> 3) & 7];
                if ((x & B_BR) && c) next = x >> 11;
                break;
            }
            case K_MEM: {
                uint32_t ad = a + b, w = ad >> 2, up = (x >> 4) & 31, sh = (ad & 3) << 3;
                if (x & B_STORE) {
                    v = r[rd];
                    uint32_t m = 0xffffffffu >> up << sh;
                    ram[w] = up ? (ram[w] & ~m) | ((v << sh) & m) : v;
                } else {
                    uint32_t y = ram[w] << (up - sh);
                    v = (x & B_MUNS) ? y >> up : (uint32_t)((int32_t)y >> up);
                }
                break;
            }
            case K_OTHER:
                if (x & B_JALR) { v = (pc << 2) + CODE_BASE + 4; next = (a + b - CODE_BASE) >> 2; }
                else if (!(x & (B_SFU | B_DIV | B_SYS))) v = a * b;
                else { printf("reference: no such instruction at %u\n", pc); exit(1); }
                break;
            default: printf("reference: floats are not modelled (slot %u)\n", pc); exit(1);
            }
            r[rd] = v;
            r[0] = 0;
            pc = next;
        }
    }
};

// ---- The programs: programs/classes' loops, hand-assembled. Registers as RISC-V's numbers.
// the lowest registers first, so that few will do (--regs): tight and adds need r1 and r2
enum { N = 1, T1 = 2, T2 = 3, T3 = 4, T4 = 5, T5 = 6, T6 = 7, P = 8, SUM = 9, AT = 10, X = 11, RA = 12, B = 13, B2 = 14 };
static const uint32_t DATA = 0x10000, LOOPS = 0x7fffffff, CODE_W = 0x80000;   // CODE_W: the word --ram puts code at
struct Program { const char* name; int rv_per_trip, per_trip; std::function<void(Asm&)> build; };   // instructions a trip: RISC-V's, ours
static std::vector<Program> programs() {
    return {
        {"arith", 17, 17, [](Asm& s) {   // 15 register operations and the loop's two
             s.addi(N, 0, LOOPS);
             uint32_t top = s.here();
             s.addi(T1, T1, 1); s.op(S_XOR, 0, T2, T2, T1, 0); s.add(T3, T3, T2); s.op(S_SLL, 0, T4, T3, 0, 3);
             s.sub(T5, T4, T1); s.op(S_OR, 0, T6, T5, T2, 0); s.op(S_AND, 0, T1, T1, 0, 0x7ff); s.op(S_SRL, 0, T2, T6, 0, 2);
             s.add(T3, T3, T5); s.op(S_XOR, 0, T4, T4, 0, 0x55); s.op(S_CMP, B_UNS, T5, T3, T4, 0); s.add(T6, T6, T5);
             s.op(S_SLL, 0, T1, T1, 0, 1); s.op(S_SRA, 0, T2, T2, 0, 1); s.op(S_AND, 0, T3, T3, T6, 0);
             s.addi(N, N, -1); s.bne(N, 0, top);
         }},
        {"loads", 16, 16, [](Asm& s) {   // 14 lw from one page
             s.addi(N, 0, LOOPS); s.addi(P, 0, DATA);
             for (int i = 0; i < 16; i++) s.ram.push_back({DATA + 4 * i, 0x1000u * i + 7});
             uint32_t top = s.here();
             int regs[6] = {T1, T2, T3, T4, T5, T6};
             for (int i = 0; i < 14; i++) s.lw(regs[i % 6], P, 4 * i);
             s.addi(N, N, -1); s.bne(N, 0, top);
         }},
        {"stores", 16, 16, [](Asm& s) {   // 14 sw to four texels, a new value each trip
             s.addi(N, 0, LOOPS); s.addi(P, 0, DATA);
             uint32_t top = s.here();
             for (int i = 0; i < 14; i++) s.sw(N, P, 4 * i);
             s.addi(N, N, -1); s.bne(N, 0, top);
         }},
        {"adds", 16, 16, [](Asm& s) {   // 14 dependent adds and the loop's two (r1 and r2 alone)
             s.addi(N, 0, LOOPS);
             uint32_t top = s.here();
             for (int i = 0; i < 14; i++) s.add(T1, T1, N);
             s.addi(N, N, -1); s.bne(N, 0, top);
         }},
        {"adds2", 16, 16, [](Asm& s) {   // two chains of 7 adds, interleaved, and the loop's two (r1 to r3)
             s.addi(N, 0, LOOPS);
             uint32_t top = s.here();
             for (int i = 0; i < 7; i++) { s.add(T1, T1, N); s.add(T2, T2, N); }
             s.addi(N, N, -1); s.bne(N, 0, top);
         }},
        {"branchy", 1, 1, [](Asm& s) {   // a forward branch taken every other trip, against its prediction
             s.addi(N, 0, LOOPS); s.addi(T1, 0, 2);
             uint32_t top = s.here();
             s.addi(T1, T1, -1); s.bne(T1, 0, 0);
             uint32_t br = s.here() - 1;
             s.addi(T1, 0, 2);
             uint32_t skip = s.here();
             s.addi(N, N, -1); s.bne(N, 0, top);
             s.code[br].x |= skip << 11;
         }},
        {"mix", 12, 10, [](Asm& s) {   // ordinary code: a load, a store, ALU work and the loop's branch
             s.addi(N, 0, LOOPS); s.addi(P, 0, 0); s.addi(B, 0, DATA); s.addi(B2, 0, DATA + 0x1000);   // the bases in registers, as compiled
             for (int i = 0; i < 32; i++) s.ram.push_back({DATA + 4 * i, 0x9e3779b9u * (i + 1)});
             uint32_t top = s.here();
             s.lwx(T1, B, P); s.add(SUM, SUM, T1); s.addi(P, P, 4); s.op(S_XOR, 0, T2, T1, SUM, 0);
             s.op(S_AND, 0, P, P, 0, 0x7c); s.swx(T2, B2, P); s.op(S_SLL, 0, T3, T2, 0, 1);
             s.add(SUM, SUM, T3); s.addi(N, N, -1); s.bne(N, 0, top);
         }},
        {"tight", 2, 2, [](Asm& s) {   // a two-instruction loop
             s.addi(N, 0, LOOPS);
             uint32_t top = s.here();
             s.addi(N, N, -1); s.bne(N, 0, top);
         }},
        {"tight7", 2, 2, [](Asm& s) {   // tight with its count in r7 (where the register lies)
             s.addi(T6, 0, LOOPS);
             uint32_t top = s.here();
             s.addi(T6, T6, -1); s.bne(T6, 0, top);
         }},
        {"call", 5, 5, [](Asm& s) {   // jal to a leaf, addi, ret, the loop's two
             s.addi(N, 0, LOOPS);
             uint32_t top = s.here();
             s.jal(RA, 0);   // patched below
             uint32_t call = s.here() - 1;
             s.addi(N, N, -1); s.bne(N, 0, top);
             uint32_t leaf = s.here();
             s.addi(X, X, 1); s.jr(RA);
             s.code[call].x |= leaf << 11;
         }},
        {"stride", 7, 6, [](Asm& s) {   // loads striding through 4 MB; add + lw are one lw of a + b here; the mask in X as RISC-V has it
             s.addi(N, 0, LOOPS); s.addi(P, 0, 0); s.addi(X, 0, 0x3ffffc);
             uint32_t top = s.here();
             s.lwx(T2, P, AT); s.add(SUM, SUM, T2); s.addi(AT, AT, 1040);
             s.op(S_AND, 0, AT, AT, X, 0); s.addi(N, N, -1); s.bne(N, 0, top);
         }},
    };
}

// ---- D3D11
static ID3D11Device* dev;
static ID3D11DeviceContext* ctx;
static void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) { printf("%s failed: %08lx\n", what, (unsigned long)hr); exit(1); }
}
struct Tex { ID3D11Texture2D* t = nullptr; ID3D11ShaderResourceView* srv = nullptr; ID3D11RenderTargetView* rtv = nullptr; UINT w, h; DXGI_FORMAT f; };
static Tex make_tex(UINT w, UINT h, DXGI_FORMAT f, bool rt, const void* init, UINT pitch) {
    Tex t; t.w = w; t.h = h; t.f = f;
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, f, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | (rt ? D3D11_BIND_RENDER_TARGET : 0u), 0, 0};
    D3D11_SUBRESOURCE_DATA sd = {init, pitch, 0};
    check(dev->CreateTexture2D(&d, init ? &sd : nullptr, &t.t), "CreateTexture2D");
    check(dev->CreateShaderResourceView(t.t, nullptr, &t.srv), "CreateShaderResourceView");
    if (rt) check(dev->CreateRenderTargetView(t.t, nullptr, &t.rtv), "CreateRenderTargetView");
    return t;
}
static std::vector<uint32_t> read_tex(const Tex& t, UINT words_per_texel) {
    D3D11_TEXTURE2D_DESC d = {t.w, t.h, 1, 1, t.f, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    ID3D11Texture2D* st;
    check(dev->CreateTexture2D(&d, nullptr, &st), "staging");
    ctx->CopyResource(st, t.t);
    D3D11_MAPPED_SUBRESOURCE m;
    check(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m), "Map");
    std::vector<uint32_t> out((size_t)t.w * t.h * words_per_texel);
    for (UINT y = 0; y < t.h; y++) memcpy(&out[(size_t)y * t.w * words_per_texel], (char*)m.pData + (size_t)y * m.RowPitch, t.w * 4 * words_per_texel);
    ctx->Unmap(st, 0);
    st->Release();
    return out;
}
static ID3DBlob* compile(const char* path, const char* entry, const char* profile, const std::vector<D3D_SHADER_MACRO>& defs, bool must = true) {
    std::vector<wchar_t> wpath(strlen(path) + 1);
    mbstowcs(wpath.data(), path, wpath.size());
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompileFromFile(wpath.data(), defs.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, profile,
                                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (!must && FAILED(hr)) return nullptr;
    if (err) printf("%s", (const char*)err->GetBufferPointer());
    check(hr, entry);
    return code;
}

int main(int argc, char** argv) {
    bool warp = false, bundled = false, paired = false, par = false, halfpc = false, vliw = false;
    bool present = false, roled = false, fuse = false, fuse2 = false, wid = false, sched = false;
    uint32_t group = 0, dgroup = 0, drawPix = STATE_PIX;   // --pix: the pixels a core draws (the rest of the row is never written)
    std::string only, shader;
    int passes = 2000, warm = 300;
    uint32_t ticks = 8192;
    std::vector<std::string> defStore;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--warp") warp = true;
        else if (a == "--bundle") bundled = true;
        else if (a == "--pairs") paired = true;
        else if (a == "--par") paired = par = true;
        else if (a == "--halfpc") paired = halfpc = true;
        else if (a == "--vliw") vliw = true;
        else if (a == "--present") present = true;
        else if (a == "--roles") roled = true;
        else if (a == "--fuse") fuse = true;
        else if (a == "--after") fuse2 = true;
        else if (a == "--single") fuse2 = g_single = true;
        else if (a == "--wide") wid = true;
        else if (a == "--nostep") wid = g_nostep = true;
        else if (a == "--wide2") wid = g_alu2 = true;
        else if (a == "--sched") sched = true;
        else if (a == "--sched2") sched = g_sched2 = true;
        else if (a == "--ram") sched = g_sched2 = g_ram = true;
        else if (a == "--ram4") sched = g_sched2 = g_ram4 = true;
        else if (a == "--wordwc") sched = g_sched2 = g_ram4 = g_wordwc = true;
        else if (a == "--codert") g_codert = true;
        else if (a == "--nocommit") g_nocommit = true;
        else if (a == "--stepsame") g_stepsame = true;
        else if (a == "--decoded" && i + 1 < argc) { dgroup = (uint32_t)atoi(argv[++i]); defStore.push_back("GROUP=" + std::to_string(dgroup)); }
        else if (a == "--pix" && i + 1 < argc) drawPix = (uint32_t)atoi(argv[++i]);
        else if (a == "--group" && i + 1 < argc) { group = (uint32_t)atoi(argv[++i]); defStore.push_back("GROUP=" + std::to_string(group)); }
        else if (a == "--prog" && i + 1 < argc) only = argv[++i];
        else if (a == "--shader" && i + 1 < argc) shader = argv[++i];
        else if (a == "--regs" && i + 1 < argc) { SINK = (uint32_t)atoi(argv[++i]) - 1; defStore.push_back("NREG=" + std::to_string(SINK + 1)); }
        else if (a == "--passes" && i + 1 < argc) passes = atoi(argv[++i]);
        else if (a == "--warm" && i + 1 < argc) warm = atoi(argv[++i]);
        else if (a == "--ticks" && i + 1 < argc) ticks = (uint32_t)atoi(argv[++i]);
        else if (a == "-D" && i + 1 < argc) defStore.push_back(argv[++i]);
        else { printf("isabench [--warp] [--prog A,B] [--passes N] [--warm N] [--ticks N] [-D NAME=V]\n"); return 1; }
    }
    std::vector<std::string> names, values;
    for (auto& d : defStore) { size_t e = d.find('='); names.push_back(d.substr(0, e)); values.push_back(e == std::string::npos ? "1" : d.substr(e + 1)); }
    std::vector<D3D_SHADER_MACRO> defs;
    for (size_t i = 0; i < names.size(); i++) defs.push_back({names[i].c_str(), values[i].c_str()});
    defs.push_back({nullptr, nullptr});

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    IDXGISwapChain* swap = nullptr;
    if (present) {   // a hidden window's swap chain, presented now and then, so that a frame profiler can attach
        WNDCLASSA wc = {0, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "isabench"};
        RegisterClassA(&wc);
        HWND w = CreateWindowExA(0, "isabench", "isabench", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
        DXGI_SWAP_CHAIN_DESC sd = {{64, 64, {0, 1}, DXGI_FORMAT_R8G8B8A8_UNORM}, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2, w, TRUE,
                                   DXGI_SWAP_EFFECT_FLIP_DISCARD};
        check(D3D11CreateDeviceAndSwapChain(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                                            D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx), "D3D11CreateDeviceAndSwapChain");
    } else
    check(D3D11CreateDevice(nullptr, warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1,
                            D3D11_SDK_VERSION, &dev, nullptr, &ctx), "D3D11CreateDevice");
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe);
    dir = !shader.empty() ? shader : dir.substr(0, dir.find_last_of("\\/")) + "\\..\\experiments\\isa_proto\\tick.hlsl";
    LARGE_INTEGER f0, f1, fq;
    QueryPerformanceCounter(&f0);
    ID3DBlob *vsb = compile(dir.c_str(), "vert", "vs_5_0", defs), *psb = compile(dir.c_str(), "frag", "ps_5_0", defs);
    // (a shader without commit_vert, a step before stores, has no commit pass)
    ID3DBlob *cvb = compile(dir.c_str(), "commit_vert", "vs_5_0", defs, false), *cpb = cvb ? compile(dir.c_str(), "commit_frag", "ps_5_0", defs) : nullptr;
    QueryPerformanceCounter(&f1);
    QueryPerformanceFrequency(&fq);
    ID3D11VertexShader *vs, *cvs = nullptr;
    ID3D11PixelShader *ps, *cps = nullptr;
    check(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs), "vs");
    check(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps), "ps");
    if (cvb) check(dev->CreateVertexShader(cvb->GetBufferPointer(), cvb->GetBufferSize(), nullptr, &cvs), "commit vs");
    if (cpb) check(dev->CreatePixelShader(cpb->GetBufferPointer(), cpb->GetBufferSize(), nullptr, &cps), "commit ps");
    ID3D11BlendState* masks[4] = {};   // --wordwc: a component each, told to the commit in b1
    ID3D11Buffer* compCb[4] = {};
    for (int c = 0; c < 4 && g_wordwc; c++) {
        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)(1 << c);
        check(dev->CreateBlendState(&bd, &masks[c]), "blend");
        uint32_t comp[4] = {(uint32_t)c, 0, 0, 0};
        D3D11_BUFFER_DESC cd = {16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA ci = {comp, 0, 0};
        check(dev->CreateBuffer(&cd, &ci, &compCb[c]), "component cbuffer");
    }
    printf("isabench: %s, %s, %u instructions a pass, compiled in %.1f s%s\n", warp ? "WARP" : "the card", dir.c_str(), ticks,
           double(f1.QuadPart - f0.QuadPart) / fq.QuadPart, cvs ? "" : ", no commit pass");

    const UINT RAM_W = 1u << RAM_SHIFT, RAM_H = 128;
    struct { uint32_t ticks, ramH, pad[2]; } cbData = {ticks, RAM_H, {0, 0}};
    D3D11_BUFFER_DESC bd = {16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA cbInit = {&cbData, 0, 0};
    ID3D11Buffer* cb;
    check(dev->CreateBuffer(&bd, &cbInit, &cb), "cbuffer");
    D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    ID3D11Query *qDis, *q0, *q1;
    dev->CreateQuery(&qd, &qDis);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    dev->CreateQuery(&qd, &q0);
    dev->CreateQuery(&qd, &q1);

    bool all_ok = true;
    for (const Program& prog : programs()) {
        if (!only.empty() && ("," + only + ",").find("," + std::string(prog.name) + ",") == std::string::npos) continue;
        Asm s;
        prog.build(s);
        uint32_t entry = 0;
        Bundles bun;
        if (bundled) bun = bundle(s.code);
        if (paired) bun = pairs(s.code, par);
        if (vliw) bun = slots(s.code);
        if (group) bun = groups(s.code, group);
        if (dgroup) bun = decoded(s.code, dgroup);
        if (roled) bun = roles(s.code);
        if (fuse) bun = fused(s.code);
        if (fuse2) bun = fused_after(s.code);
        if (wid) bun = wide(s.code);
        if (sched) bun = scheduled(s.code, g_sched2);
        std::vector<uint32_t> wordAt;   // --ram: each bundle's word from CODE_W, the code in RAM there
        if (g_ram) {
            std::vector<uint32_t> words = ram_words(bun, wordAt);
            for (size_t j = 0; j < words.size(); j++) s.ram.push_back({(CODE_W + (uint32_t)j) * 4, words[j]});
            entry = CODE_W;
        }
        if (g_ram4) {   // the slots at texel CODE_W / 4, a branch's target relative to its own
            for (size_t j = 0; j < bun.code.size(); j++) {
                Slot& o = bun.code[j];   // (relative in the code texture too, for s20's CODE_COPY probe)
                if (o.x & B_BR) o.x = (o.x & 0xfffff) | ((o.x >> 20) - (uint32_t)j) << 20;
                uint32_t w[4] = {o.x, o.ra, o.rdrb, o.imm};
                for (int c = 0; c < 4; c++) s.ram.push_back({(CODE_W + 4 * (uint32_t)j + c) * 4, w[c]});
            }
            entry = CODE_W / 4;
            if (getenv("ISA_DUMP"))
                for (size_t j = 0; j < bun.code.size(); j++)
                    printf("  slot %zu: %08x %08x %08x %08x count %u\n", j, bun.code[j].x, bun.code[j].ra, bun.code[j].rdrb, bun.code[j].imm, (bun.code[j].rdrb >> 8) & 15);
        }
        auto bundle_at = [&](uint32_t pc) {   // the bundle a pc names, or ~0
            if (g_ram4) return pc - CODE_W / 4 < bun.code.size() ? pc - CODE_W / 4 : ~0u;
            if (!g_ram) return pc;
            for (size_t j = 0; j < bun.code.size(); j++) if (CODE_W + wordAt[j] == pc) return (uint32_t)j;
            return ~0u;
        };
        const std::vector<Slot>& gpuCode = bundled || paired || vliw || group || dgroup || roled || fuse || fuse2 || wid || sched ? bun.code : s.code;
        // the code, a slot a texel
        UINT codeH = (UINT)((gpuCode.size() + (1u << CODE_SHIFT) - 1) >> CODE_SHIFT);
        std::vector<Slot> codeImg((size_t)codeH << CODE_SHIFT, Slot{K_OTHER | B_SYS, 0, SINK, 0});
        std::copy(gpuCode.begin(), gpuCode.end(), codeImg.begin());
        Tex code = make_tex(1u << CODE_SHIFT, codeH, DXGI_FORMAT_R32G32B32A32_UINT, g_codert, codeImg.data(), (1u << CODE_SHIFT) * 16);
        // the same slots as a structured buffer (t10) and as a constant buffer of up to 4,096 (b1)
        ID3D11Buffer *codeBuf, *codeCb;
        ID3D11ShaderResourceView* codeBufSrv;
        D3D11_BUFFER_DESC sb = {(UINT)(codeImg.size() * 16), D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16};
        D3D11_SUBRESOURCE_DATA sbInit = {codeImg.data(), 0, 0};
        check(dev->CreateBuffer(&sb, &sbInit, &codeBuf), "code buffer");
        check(dev->CreateShaderResourceView(codeBuf, nullptr, &codeBufSrv), "code buffer view");
        std::vector<Slot> cbImg(4096, Slot{K_OTHER | B_SYS, 0, SINK, 0});
        std::copy(codeImg.begin(), codeImg.begin() + std::min<size_t>(codeImg.size(), 4096), cbImg.begin());
        D3D11_BUFFER_DESC cbd = {4096 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
        D3D11_SUBRESOURCE_DATA cbdInit = {cbImg.data(), 0, 0};
        check(dev->CreateBuffer(&cbd, &cbdInit, &codeCb), "code cbuffer");
        Machine ref;
        ref.ram.assign((size_t)RAM_W * RAM_H, 0);
        for (auto& w : s.ram) ref.ram[w.first >> 2] = w.second;
        Tex ram = g_ram4 ? make_tex(RAM_W / 4, RAM_H, DXGI_FORMAT_R32G32B32A32_UINT, true, ref.ram.data(), RAM_W * 4)
                         : make_tex(RAM_W, RAM_H, DXGI_FORMAT_R32_UINT, true, ref.ram.data(), RAM_W * 4);
        // the state: two sets of eight targets; the first pass reads set 1
        std::vector<uint32_t> init[8];
        for (auto& v : init) v.assign((size_t)STATE_PIX * 4, 0);
        init[CTL_T & 7][(CTL_T >> 3) * 4] = entry;
        Tex set[2][8];
        for (int k = 0; k < 2; k++)
            for (int t = 0; t < 8; t++) set[k][t] = make_tex(STATE_PIX, 1, DXGI_FORMAT_R32G32B32A32_UINT, true, init[t].data(), STATE_PIX * 16);
        int cur = 0;
        auto pass = [&]() {
            ID3D11ShaderResourceView* none[11] = {};
            ID3D11RenderTargetView* rtvs[8];
            for (int t = 0; t < 8; t++) rtvs[t] = set[cur][t].rtv;
            ctx->PSSetShaderResources(0, 11, none);
            ctx->VSSetShaderResources(0, 11, none);
            ctx->OMSetRenderTargets(8, rtvs, nullptr);
            D3D11_VIEWPORT vp = {0, 0, (float)drawPix, 1, 0, 1};
            ctx->RSSetViewports(1, &vp);
            ID3D11ShaderResourceView* srvs[11] = {code.srv, ram.srv};
            for (int t = 0; t < 8; t++) srvs[2 + t] = set[cur ^ 1][t].srv;
            srvs[10] = codeBufSrv;
            ctx->PSSetShaderResources(0, 11, srvs);
            ID3D11Buffer* cbs[2] = {cb, codeCb};
            ctx->PSSetConstantBuffers(0, 2, cbs);
            ctx->VSSetShader(vs, nullptr, 0);
            ctx->PSSetShader(ps, nullptr, 0);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->Draw(3, 0);
            if (!cvs || g_nocommit) { cur ^= 1; return; }
            // the commit: this pass's buckets into RAM
            ctx->PSSetShaderResources(0, 10, none);
            ctx->OMSetRenderTargets(1, &ram.rtv, nullptr);
            D3D11_VIEWPORT rv = {0, 0, (float)(g_ram4 ? RAM_W / 4 : RAM_W), (float)RAM_H, 0, 1};
            ctx->RSSetViewports(1, &rv);
            for (int t = 0; t < 8; t++) srvs[2 + t] = set[cur][t].srv;
            ctx->VSSetShaderResources(0, 10, srvs);
            ctx->VSSetConstantBuffers(0, 1, &cb);
            ctx->VSSetShader(cvs, nullptr, 0);
            ctx->PSSetShader(cps, nullptr, 0);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
            if (g_wordwc) {
                for (int c = 0; c < 4; c++) { ctx->OMSetBlendState(masks[c], nullptr, 0xffffffff); ctx->VSSetConstantBuffers(1, 1, &compCb[c]); ctx->Draw(2 * WC_N, 0); }
                ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            } else ctx->Draw(2 * WC_N, 0);
            cur ^= 1;
        };
        auto release = [&]() { codeBufSrv->Release(); codeBuf->Release(); codeCb->Release(); };
        auto total = [&]() {   // the count of all passes, from the last set written
            ctx->OMSetRenderTargets(0, nullptr, nullptr);
            std::vector<uint32_t> v = read_tex(set[cur ^ 1][CTL_T & 7], 4);
            return v[(CTL_T >> 3) * 4 + 3];
        };
        for (int k = 0; k < warm; k++) pass();
        uint32_t n0 = total();
        ctx->Begin(qDis);
        ctx->End(q0);
        for (int k = 0; k < passes; k++) { pass(); if (swap && k % 50 == 49) swap->Present(0, 0); }
        ctx->End(q1);
        ctx->End(qDis);
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        while (ctx->GetData(qDis, &dj, sizeof dj, 0) != S_OK) Sleep(1);
        UINT64 t0, t1;
        while (ctx->GetData(q0, &t0, sizeof t0, 0) != S_OK) Sleep(1);
        while (ctx->GetData(q1, &t1, sizeof t1, 0) != S_OK) Sleep(1);
        uint32_t n1 = total();
        if (sched) {   // for the check, passes of one slot until one ends where a prefix of the program has run
            cbData.ticks = 1;
            ctx->UpdateSubresource(cb, 0, nullptr, &cbData, 0, 0);
            for (int k = 0; k < 64; k++) {
                ctx->OMSetRenderTargets(0, nullptr, nullptr);
                std::vector<uint32_t> c = read_tex(set[cur ^ 1][CTL_T & 7], 4);
                uint32_t at = bundle_at(c[(CTL_T >> 3) * 4]);
                if (!c[(CTL_T >> 3) * 4 + 2] && at < gpuCode.size() && (gpuCode[at].ra & S_ENDOK)) break;
                pass();
            }
            n1 = total();
            cbData.ticks = ticks;
            ctx->UpdateSubresource(cb, 0, nullptr, &cbData, 0, 0);
        }
        double sec = double(t1 - t0) / dj.Frequency;
        double ips = (n1 - n0) / sec;
        // the check: the reference runs as many instructions; registers, pc and RAM must agree
        ref.run(s.code, n1);
        std::vector<uint32_t> regs;
        for (int t = 0; t < 17; t++) {
            std::vector<uint32_t> v = read_tex(set[cur ^ 1][t & 7], 4);
            for (int c = 0; c < 4; c++) regs.push_back(v[(t >> 3) * 4 + c]);
        }
        uint32_t gpc = read_tex(set[cur ^ 1][CTL_T & 7], 4)[(CTL_T >> 3) * 4];
        std::vector<uint32_t> gram = read_tex(ram, g_ram4 ? 4 : 1);
        int bad = 0;
        for (int k = 1; k < 64; k++) if (k != (int)SINK && regs[k] != ref.r[k]) { if (bad++ < 4) printf("  r%d %08x, the reference %08x\n", k, regs[k], ref.r[k]); }
        if (g_ram || g_ram4) gpc = bundle_at(gpc);
        if (halfpc) gpc = (gpc >> 1) < bun.first.size() ? bun.first[gpc >> 1] + (gpc & 1) : ~0u;
        else if (bundled || paired || vliw || group || dgroup || roled || fuse || fuse2 || wid || sched) gpc = gpc < bun.first.size() ? bun.first[gpc] : ~0u;
        if (gpc != ref.pc) { bad++; printf("  pc %u, the reference %u\n", gpc, ref.pc); }
        size_t ramBad = 0;
        for (size_t k = 0; k < gram.size(); k++) if (gram[k] != ref.ram[k] && ramBad++ < 4) printf("  word %zx %08x, the reference %08x\n", k, gram[k], ref.ram[k]);
        bool ok = !dj.Disjoint && bad == 0 && ramBad == 0;
        all_ok = all_ok && ok;
        printf("%3zu slots ", gpuCode.size());
        printf("%-7s %10u instructions in %8.2f ms: %7.2f M a second, %6.1f ns each; as the RISC-V loop's work %7.2f M; %s\n",
               prog.name, n1 - n0, sec * 1e3, ips / 1e6, 1e9 / ips,
               ips * prog.rv_per_trip / prog.per_trip / 1e6,
               ok ? "same as the reference" : "WRONG");
        release();
    }
    return all_ok ? 0 : 2;
}
