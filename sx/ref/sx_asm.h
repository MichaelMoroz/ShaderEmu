// A small assembler for SX (sx/SX.md) in C++, for the tick's bench and the reference's tests until
// LLVM's is there: ops in order, two a bundle and one long immediate; a label or a control op closes one. The
// control word says whose the long immediate is, and its path is the body that makes its ops (isa.h, x15's paths).
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../isa.h"

struct SxAsm {
    uint32_t base;                    // the first bundle's address
    std::vector<uint32_t> w;          // four words a bundle
    std::vector<uint32_t> cur;        // the open bundle's ops (two at most)
    int long_of = -1;                 // the op whose immediate is word 2, or -1
    uint32_t long_imm = 0;
    int bump_reg = -1;                // the open bundle's control-word add (SX_SUB_ADD): rs += imm12 after its ops
    int32_t bump_imm = 0;
    std::vector<int64_t> labels;      // bundle index of each label, or -1
    struct Fix { size_t bundle; int label; int kind; int word; };   // kind 0: off9, 1: off18 (call), 2: lpc's long immediate, 3: j (near or far)
    std::vector<Fix> fixes;
    explicit SxAsm(uint32_t at) : base(at) {}

    uint32_t addr_of(int label) const { return base + (uint32_t)labels[label] * 16; }
    int label() { labels.push_back(-1); return (int)labels.size() - 1; }
    void bind(int l) { close(); labels[l] = (int64_t)(w.size() / 4); }
    int here() { int l = label(); bind(l); return l; }

    // the paths (sx/isa.h): mid is the integer functions 0-19, lite 0-7 and 16-18, float lite the float functions 0-7
    static uint32_t fn(uint32_t op) { return SX_OPC(op) & 31; }
    static bool int_op(uint32_t op) { return SX_CLASS(SX_OPC(op)) == 1 && fn(op) <= SX_I_COMMON_LAST; }
    static bool mid(uint32_t op) { return SX_CLASS(SX_OPC(op)) == 1 && fn(op) <= SX_I_MID_LAST; }
    static bool lite(uint32_t op) { return SX_CLASS(SX_OPC(op)) == 1 && (fn(op) <= SX_I_XNOR || (fn(op) >= SX_I_SLL && fn(op) <= SX_I_SRA)); }
    static bool mem_op(uint32_t op) {
        uint32_t o = SX_OPC(op), wd = o & 7;
        return SX_CLASS(o) == 2 && ((o >> 3) & 7) <= 3 && (wd == SX_W_W || wd == SX_W_H || wd == SX_W_B || wd == SX_W_HU || wd == SX_W_BU);
    }
    static bool madlike(uint32_t f) { return f == SX_F_MAD || f == SX_F_MSB || f == SX_F_NMSB; }
    static bool float_op(uint32_t op) { uint32_t o = SX_OPC(op); return SX_CLASS(o) == 3 && !(SX_IFORM(o) && madlike(o & 31)); }
    static bool flite(uint32_t op) { return float_op(op) && fn(op) <= SX_F_MAX; }
    uint32_t path() const {
        if (cur.empty()) return SX_PATH_INT;
        uint32_t a = cur[0];
        bool one = cur.size() == 1;
        uint32_t b = one ? 0 : cur[1];
        if (mid(a) && (one || lite(b))) return SX_PATH_INT;
        if ((int_op(a) || float_op(a)) && (one || lite(b) || flite(b))) return SX_PATH_ALU;
        if (mem_op(a) && (one || lite(b))) return store_op(a) ? SX_PATH_STORE : SX_PATH_LOAD;
        if (mem_op(a) && mem_op(b)) return store_op(a) ? SX_PATH_MEM2 : store_op(b) ? SX_PATH_GENERAL : SX_PATH_LOAD2;
        if (mem_op(a)) return flite(b) ? SX_PATH_MEMF : SX_PATH_GENERAL;
        return SX_PATH_GENERAL;
    }
    void close(uint32_t ctl = SX_CTRL_NONE) {
        if (cur.empty() && ctl == SX_CTRL_NONE && bump_reg < 0) return;
        if (bump_reg >= 0) {   // (a control op flushes a pending add first: see ctl_op)
            ctl = SX_CTRL_SPECIAL(SX_SUB_ADD) | (uint32_t)bump_reg << 14 | (uint32_t)(bump_imm & 0xfff) << 20;
            bump_reg = -1;
        }
        uint32_t n = (uint32_t)cur.size();
        uint32_t L = long_of < 0 ? 0u : long_of == 1 ? 0x40u : 0x20u;
        // cc 0 (jr, callr, call, eret, a far j, the add): the general step's (isa.h). A j's path is the ops' until
        // finish() finds it far.
        uint32_t p = SX_CC(ctl) != SX_CC_SPECIAL ? path() : SX_PATH_GENERAL;
        w.push_back(n > 0 ? cur[0] : 0);
        w.push_back(n > 1 ? cur[1] : 0);
        w.push_back(long_of < 0 ? 0 : long_imm);
        w.push_back(ctl | n << 3 | L | p);
        cur.clear();
        long_of = -1;
    }
    // op 1 takes only path 1's cheap ops: anything else opens a bundle
    bool fits1(uint32_t op) const {   // op 1's place: what some path has after op 0
        // a memory pair reads nothing op 0 writes, and a store's is op 0 (isa.h's paths 5 and 7)
        if (mem_op(op) && mem_op(cur[0])) return !reads_rd0(op) && (store_op(cur[0]) || !store_op(op));
        return lite(op) || (flite(op) && (float_op(cur[0]) || mem_op(cur[0])));
    }
    bool reads_rd0(uint32_t op) const {   // op names op 0's result in a field it reads (a store's rd is its value)
        uint32_t d = SX_RD(cur[0]);
        return !store_op(cur[0]) && d != 0 && (SX_RA(op) == d || SX_RB(op) == d || (store_op(op) && SX_RD(op) == d));
    }
    // an op that names a register in any field (conservatively: rd, ra, rb, rc)
    static bool touches(uint32_t op, uint32_t r) { return SX_RD(op) == r || SX_RA(op) == r || SX_RB(op) == r || SX_RC(op) == r; }
    static bool store_op(uint32_t op) { uint32_t o = SX_OPC(op); return SX_CLASS(o) == 2 && (((o >> 3) & 7) == SX_M_STORE || ((o >> 3) & 7) == SX_M_STOREX); }
    bool writes(uint32_t r) const {   // an op of the open bundle that writes r (a store's rd is its value: read); wide ones by rd's group
        for (uint32_t op : cur) if (!store_op(op) && (SX_RD(op) == r || (SX_CLASS(SX_OPC(op)) == 2 && (r - SX_RD(op)) < 4))) return true;
        return false;
    }
    void put(uint32_t op) {
        if (bump_reg >= 0 && touches(op, (uint32_t)bump_reg)) close();   // it comes after the add in program order
        if (cur.size() == 2 || (!cur.empty() && !fits1(op))) close();
        cur.push_back(op);
    }
    void put2(uint32_t op, uint32_t imm) {
        if (bump_reg >= 0 && touches(op, (uint32_t)bump_reg)) close();
        if (cur.size() == 2 || (!cur.empty() && !fits1(op)) || long_of >= 0) close();
        long_of = (int)cur.size();
        long_imm = imm;
        cur.push_back(op);
    }

    static bool fits12(int32_t v) { return v >= -2048 && v <= 2047; }
    void R(uint32_t opc, uint32_t rd, uint32_t ra, uint32_t rb, uint32_t rc = 0) { put(SX_WORD_R(opc, rd, ra, rb, rc)); }
    void I(uint32_t opc, uint32_t rd, uint32_t ra, int32_t imm, bool force_long = false) {
        uint32_t base_w = SX_WORD_I(opc, rd, ra, 0u);
        if (fits12(imm) && !force_long) put(base_w | (uint32_t)(imm & 0xfff) << 20);
        else put2(base_w, (uint32_t)imm);
    }

    // integer
    void op(uint32_t f, uint32_t rd, uint32_t ra, uint32_t rb) { R(SX_OP_I(f), rd, ra, rb); }
    void opi(uint32_t f, uint32_t rd, uint32_t ra, int32_t imm) { I(SX_OP_II(f), rd, ra, imm); }
    void add(uint32_t rd, uint32_t ra, uint32_t rb) { op(SX_I_ADD, rd, ra, rb); }
    void sub(uint32_t rd, uint32_t ra, uint32_t rb) { op(SX_I_SUB, rd, ra, rb); }
    void addi(uint32_t rd, uint32_t ra, int32_t imm) { opi(SX_I_ADD, rd, ra, imm); }
    void li(uint32_t rd, int32_t imm) { opi(SX_I_ADD, rd, 0, imm); }
    void mv(uint32_t rd, uint32_t ra) { opi(SX_I_ADD, rd, ra, 0); }
    void shadd(uint32_t rd, uint32_t ra, uint32_t rb, uint32_t sh) { R(SX_OP_I(SX_I_SHADD), rd, ra, rb, sh); }
    void sel(uint32_t rd, uint32_t ra, uint32_t rb, uint32_t rc) { R(SX_OP_I(SX_I_SEL), rd, ra, rb, rc); }
    void bfe(uint32_t rd, uint32_t ra, uint32_t pos, uint32_t len, bool sgn) {
        I(SX_OP_II(SX_I_BFE), rd, ra, (int32_t)(pos | (len - 1) << 5 | (sgn ? 0x400u : 0)));
    }
    // memory
    void ld(uint32_t wd, uint32_t rd, uint32_t ra, int32_t off) { I(SX_OP_M(SX_M_LOAD, wd), rd, ra, off); }
    void st(uint32_t wd, uint32_t rs, uint32_t ra, int32_t off) { I(SX_OP_M(SX_M_STORE, wd), rs, ra, off); }
    void ldx(uint32_t wd, uint32_t rd, uint32_t ra, uint32_t rb, uint32_t sh) { R(SX_OP_M(SX_M_LOADX, wd), rd, ra, rb, sh); }
    void stx(uint32_t wd, uint32_t rs, uint32_t ra, uint32_t rb, uint32_t sh) { R(SX_OP_M(SX_M_STOREX, wd), rs, ra, rb, sh); }
    void lw(uint32_t rd, uint32_t ra, int32_t off) { ld(SX_W_W, rd, ra, off); }
    void sw(uint32_t rs, uint32_t ra, int32_t off) { st(SX_W_W, rs, ra, off); }
    void amo(uint32_t f, uint32_t rd, uint32_t ra, uint32_t rb) { R(SX_OP_M(SX_M_AMO, f), rd, ra, rb); }
    // float: the I form's short immediate is a float's top 12 bits
    void fop(uint32_t f, uint32_t rd, uint32_t ra, uint32_t rb, uint32_t rc = 0) { R(SX_OP_F(f), rd, ra, rb, rc); }
    void fopi(uint32_t f, uint32_t rd, uint32_t ra, float v) {
        uint32_t bits; memcpy(&bits, &v, 4);
        uint32_t base_w = SX_WORD_I(SX_OP_FI(f), rd, ra, 0u);
        if ((bits & 0xfffff) == 0) put(base_w | (bits >> 20) << 20);
        else put2(base_w, bits);
    }
    // extended
    void x(uint32_t opc, uint32_t rd = 0, uint32_t ra = 0, uint32_t rb = 0) { R(opc, rd, ra, rb); }
    void xi(uint32_t opc, uint32_t rd, uint32_t ra, int32_t imm) { I(opc, rd, ra, imm); }
    void mfsr(uint32_t rd, uint32_t n) { I(SX_X_MFSR, rd, 0, (int32_t)n); }
    void mtsr(uint32_t n, uint32_t ra) { I(SX_X_MTSR, 0, ra, (int32_t)n); }
    void lpc(uint32_t rd, int l) {   // rd = the label's address (a long immediate, fixed up)
        put2(SX_WORD_I(SX_X_LPC, rd, 0u, 0u), 0);
        fixes.push_back({w.size() / 4, l, 2, 2});   // (after put2, which may close a bundle: the open one, its immediate word 2)
    }
    void halt() { close(); x(SX_X_BREAK); close(); }   // break in a bundle of its own

    // fused (sx/isa.h): rd += imm12 in the open bundle's control word, after its ops (a post-increment: ops already in
    // the bundle read rd as it was). An op put later that names rd opens a new bundle; so does a control op.
    void bump(uint32_t rd, int32_t imm) {
        if (!fits12(imm) || (rd & 63) == 0) { addi(rd, rd, imm); return; }
        if (bump_reg >= 0 || writes(rd & 63)) close();
        bump_reg = (int)(rd & 63);
        bump_imm = imm;
    }
    void ctl_op() { if (bump_reg >= 0) close(); }   // a control op's bundle has no room for the add
    // control: closes the bundle
    void br(uint32_t cc, uint32_t ra, uint32_t rb_or_imm, int l) {
        ctl_op();
        fixes.push_back({w.size() / 4, l, 0, 0});
        close(cc << 7 | (ra & 63) << 11 | (rb_or_imm & 63) << 26);
    }
    void beq(uint32_t a, uint32_t b, int l) { br(SX_CC_BEQ, a, b, l); }
    void bne(uint32_t a, uint32_t b, int l) { br(SX_CC_BNE, a, b, l); }
    void blt(uint32_t a, uint32_t b, int l) { br(SX_CC_BLT, a, b, l); }
    void bnei(uint32_t a, int32_t i, int l) { br(SX_CC_BNEI, a, (uint32_t)i & 63, l); }
    // fused: ra += imm6 (-32..31) after the bundle's ops, and to l when the sum is not 0 (`addi ra, ra, i; bnez ra`)
    void addbnz(uint32_t a, int32_t i, int l) {
        if (i < -32 || i > 31 || (a & 63) == 0) { fprintf(stderr, "sx_asm: addbnz r%u by %d\n", a & 63, i); exit(1); }
        if (writes(a & 63)) close();
        br(SX_CC_ADDBNZ, a, (uint32_t)i & 63, l);
    }
    // j: beq r0, r0 (+-4 KB, the ops' path) when finish() finds the label near, else cc 0's far j (the general step)
    void j(int l) { ctl_op(); fixes.push_back({w.size() / 4, l, 3, 0}); close(SX_CTRL_J); }
    void call(int l) { ctl_op(); fixes.push_back({w.size() / 4, l, 1, 0}); close(SX_CTRL_SPECIAL(SX_SUB_CALL)); }
    void jr(uint32_t rs) { ctl_op(); close(SX_CTRL_SPECIAL(SX_SUB_JR) | (rs & 63) << 14); }
    void callr(uint32_t rs) { ctl_op(); close(SX_CTRL_SPECIAL(SX_SUB_CALLR) | (rs & 63) << 14); }
    void eret() { ctl_op(); close(SX_CTRL_SPECIAL(SX_SUB_ERET)); }
    void ret() { jr(1); }

    std::vector<uint32_t> finish() {
        close();
        for (const Fix& f : fixes) {
            if (labels[f.label] < 0) { fprintf(stderr, "sx_asm: label %d not bound\n", f.label); exit(1); }
            int64_t off = labels[f.label] - (int64_t)f.bundle;
            uint32_t& ctl = w[f.bundle * 4 + 3];
            if (f.kind == 3 && (off < -256 || off > 255))   // a far j: cc 0, the general step's
                ctl = (ctl & 0x78u) | SX_PATH_GENERAL | SX_CTRL_SPECIAL(SX_SUB_J) | (uint32_t)(off << 14);
            else if (f.kind == 0 || f.kind == 3) {
                if (off < -256 || off > 255) { fprintf(stderr, "sx_asm: a branch of %lld bundles\n", (long long)off); exit(1); }
                ctl = (ctl & ~(0x1ffu << 17)) | ((uint32_t)off & 0x1ffu) << 17;
            } else if (f.kind == 1) ctl = (ctl & 0x3fffu) | (uint32_t)(off << 14);
            else w[f.bundle * 4 + f.word] = (uint32_t)(off * 16);   // lpc: its immediate's word
        }
        return w;
    }
};
