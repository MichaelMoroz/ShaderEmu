// A small assembler for gpu_vm (gpu_vm/GPU_VM.md) in C++, for the tick's bench and the reference's tests until
// LLVM's is there: ops in order, two a bundle and one long immediate; a label or a control op closes one. The
// control word says whose the long immediate is, and its path is the body that makes its ops (isa.h, x15's paths).
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../isa.h"

struct GvmAsm {
    uint32_t base;                    // the first bundle's address
    std::vector<uint32_t> w;          // four words a bundle
    std::vector<uint32_t> cur;        // the open bundle's ops (two at most)
    int long_of = -1;                 // the op whose immediate is word 2, or -1
    uint32_t long_imm = 0;
    int bump_reg = -1;                // the open bundle's control-word add (GVM_SUB_ADD): rs += imm12 after its ops
    int32_t bump_imm = 0;
    std::vector<int64_t> labels;      // bundle index of each label, or -1
    struct Fix { size_t bundle; int label; int kind; int word; };   // kind 0: off9, 1: off18 (call), 2: lpc's long immediate, 3: j (near or far)
    std::vector<Fix> fixes;
    explicit GvmAsm(uint32_t at) : base(at) {}

    uint32_t addr_of(int label) const { return base + (uint32_t)labels[label] * 16; }
    int label() { labels.push_back(-1); return (int)labels.size() - 1; }
    void bind(int l) { close(); labels[l] = (int64_t)(w.size() / 4); }
    int here() { int l = label(); bind(l); return l; }

    // the paths (gpu_vm/isa.h): mid is the integer functions 0-19, lite 0-7 and 16-18, float lite the float functions 0-7
    static uint32_t fn(uint32_t op) { return GVM_OPC(op) & 31; }
    static bool int_op(uint32_t op) { return GVM_CLASS(GVM_OPC(op)) == 1 && fn(op) <= GVM_I_COMMON_LAST; }
    static bool mid(uint32_t op) { return GVM_CLASS(GVM_OPC(op)) == 1 && fn(op) <= GVM_I_MID_LAST; }
    static bool lite(uint32_t op) { return GVM_CLASS(GVM_OPC(op)) == 1 && (fn(op) <= GVM_I_XNOR || (fn(op) >= GVM_I_SLL && fn(op) <= GVM_I_SRA)); }
    static bool mem_op(uint32_t op) {
        uint32_t o = GVM_OPC(op), wd = o & 7;
        return (GVM_CLASS(o) == 2 && ((o >> 3) & 7) <= 3 && (wd == GVM_W_W || wd == GVM_W_H || wd == GVM_W_B || wd == GVM_W_HU || wd == GVM_W_BU || (nc_loads && wd == GVM_W_NC))) || file_fast(op);
    }
    // phase 2 (isa.h, GVM_M_FREAD): with file_paths a tick that makes the file's lf, sf and fpair in its fast loop takes them
    // as a load (lf) and stores (sf, fpair) in paths 3-6; without it they are the general step's
    static inline bool file_paths = false;
    static inline bool nc_loads = false;   // the tick has GVM_W_NC (its NCLOAD): lw_nc is a plain lw without it
    static inline bool thin8 = false;   // phase 3: finish() lays the bundles out as 8-byte instructions (gvm_machine.thin; max_ops 1)
    static inline bool late_br = false;   // gvm_machine.late: a branch reads its registers after the bundle's ops
    static inline int max_ops = 2;   // ops a bundle (the tick's THIN: 1, a bundle of one op and its control word; WIDE3: 3, op 2 in word 2)
    static bool file_fast(uint32_t op) {
        uint32_t o = GVM_OPC(op), mode = (o >> 3) & 7, k = o & 7;
        return file_paths && GVM_CLASS(o) == 2 && ((mode == GVM_M_FREAD && k == GVM_FK_LF) || (mode == GVM_M_FWRITE && (k == GVM_FK_SF || k == GVM_FK_PAIR)));
    }
    static bool madlike(uint32_t f) { return f == GVM_F_MAD || f == GVM_F_MSB || f == GVM_F_NMSB; }
    static bool float_op(uint32_t op) { uint32_t o = GVM_OPC(op); return GVM_CLASS(o) == 3 && !(GVM_IFORM(o) && madlike(o & 31)); }
    static bool flite(uint32_t op) { return float_op(op) && fn(op) <= GVM_F_MAX; }
    // x18: path 1's op 0 is an integer op 0-23 (int_op), sel, lpc or a float op
    static bool sel_lpc(uint32_t op) { return GVM_OPC(op) == GVM_OP_I(GVM_I_SEL) || GVM_OPC(op) == GVM_X_LPC; }
    // x18: the path of a cc 0 bundle (jr, callr, j, call, the add: step12's, paths 1 and 2), or 0
    static uint32_t path12_of(uint32_t ctl, uint32_t w0, uint32_t w1) {
        uint32_t sub = GVM_SUB(ctl), n = GVM_N(ctl);
        bool ok = sub == GVM_SUB_J || sub == GVM_SUB_CALL || sub == GVM_SUB_JR || sub == GVM_SUB_CALLR || (sub == GVM_SUB_ADD && GVM_CRS(ctl) != 0);
        if (!ok || n == 3) return GVM_PATH_GENERAL;
        GvmAsm a(0);
        if (n > 0) a.cur.push_back(w0);
        if (n > 1) a.cur.push_back(w1);
        uint32_t p = a.path();
        if (p == GVM_PATH_INT) return GVM_PATH_ALU;
        if (p == GVM_PATH_LOAD || p == GVM_PATH_STORE) return GVM_PATH_MEMF;
        return p == GVM_PATH_ALU || p == GVM_PATH_MEMF ? p : GVM_PATH_GENERAL;
    }
    uint32_t path() const {
        if (cur.empty()) return GVM_PATH_INT;
        uint32_t a = cur[0];
        bool one = cur.size() == 1;
        uint32_t b = one ? 0 : cur[1];
        if (mid(a) && (one || lite(b))) return GVM_PATH_INT;
        if ((int_op(a) || float_op(a) || sel_lpc(a)) && (one || lite(b) || flite(b))) return GVM_PATH_ALU;
        if (mem_op(a) && (one || lite(b))) return store_op(a) ? GVM_PATH_STORE : GVM_PATH_LOAD;
        if (file_fast(a) || file_fast(b)) return GVM_PATH_GENERAL;   // (a file op: paths 3 and 5 only)
        if (mem_op(a) && mem_op(b)) return store_op(a) ? GVM_PATH_MEM2 : store_op(b) ? GVM_PATH_GENERAL : GVM_PATH_LOAD2;
        if (mem_op(a)) return flite(b) ? GVM_PATH_MEMF : GVM_PATH_GENERAL;
        return GVM_PATH_GENERAL;
    }
    void close(uint32_t ctl = GVM_CTRL_NONE) {
        if (cur.empty() && ctl == GVM_CTRL_NONE && bump_reg < 0) return;
        if (bump_reg >= 0) {   // (a control op flushes a pending add first: see ctl_op)
            ctl = GVM_CTRL_SPECIAL(GVM_SUB_ADD) | (uint32_t)bump_reg << 14 | (uint32_t)(bump_imm & 0xfff) << 20;
            bump_reg = -1;
        }
        uint32_t n = (uint32_t)cur.size();
        uint32_t L = long_of < 0 ? 0u : long_of == 1 ? 0x40u : 0x20u;
        // cc 0 (jr, callr, call, a far j, the add): step12's paths 1 and 2 when its ops fit them, else the general step's;
        // eret is the general step's (isa.h, x18). A j's path is the ops' until finish() finds it far.
        uint32_t p = GVM_CC(ctl) != GVM_CC_SPECIAL ? path() : path12_of(ctl | n << 3, n > 0 ? cur[0] : 0, n > 1 ? cur[1] : 0);
        w.push_back(n > 0 ? cur[0] : 0);
        w.push_back(n > 1 ? cur[1] : 0);
        w.push_back(n > 2 ? cur[2] : long_of < 0 ? 0 : long_imm);
        w.push_back(ctl | n << 3 | L | p);
        cur.clear();
        long_of = -1;
    }
    // op 1 takes only path 1's cheap ops: anything else opens a bundle
    bool fits1(uint32_t op) const {   // op 1's place: what some path has after op 0
        if (max_ops < 2) return false;
        // a memory pair reads nothing op 0 writes, and a store's is op 0 (isa.h's paths 5 and 7)
        if (file_fast(op) || file_fast(cur[0])) return lite(op) && !file_fast(op);
        if (mem_op(op) && mem_op(cur[0])) return !reads_rd0(op) && (store_op(cur[0]) || !store_op(op));
        return lite(op) || (flite(op) && (float_op(cur[0]) || mem_op(cur[0])));
    }
    bool reads_rd0(uint32_t op) const {   // op names op 0's result in a field it reads (a store's rd is its value)
        uint32_t d = GVM_RD(cur[0]);
        return !store_op(cur[0]) && d != 0 && (GVM_RA(op) == d || GVM_RB(op) == d || (store_op(op) && GVM_RD(op) == d));
    }
    // an op that names a register in any field (conservatively: rd, ra, rb, rc)
    static bool touches(uint32_t op, uint32_t r) { return GVM_RD(op) == r || GVM_RA(op) == r || GVM_RB(op) == r || GVM_RC(op) == r; }
    static bool store_op(uint32_t op) { uint32_t o = GVM_OPC(op); return GVM_CLASS(o) == 2 && (((o >> 3) & 7) == GVM_M_STORE || ((o >> 3) & 7) == GVM_M_STOREX || ((o >> 3) & 7) == GVM_M_FWRITE); }
    static uint32_t span(uint32_t op) {   // the registers a memory op writes from rd: d two, q four (modes 0-3), else one
        uint32_t o = GVM_OPC(op);
        if (GVM_CLASS(o) != 2 || ((o >> 3) & 7) > 3) return 1;
        return (o & 7) == GVM_W_Q ? 4 : (o & 7) == GVM_W_D ? 2 : 1;
    }
    bool br_old(uint32_t r) const { return !late_br && writes(r); }   // a branch on r must open a new bundle (it reads r as the bundle found it)
    bool writes(uint32_t r) const {   // an op of the open bundle that writes r (a store's rd is its value: read); d and q by rd's group
        for (uint32_t op : cur) if (!store_op(op) && (r - GVM_RD(op)) < span(op)) return true;
        return false;
    }
    void put(uint32_t op) {
        if (bump_reg >= 0 && touches(op, (uint32_t)bump_reg)) close();   // it comes after the add in program order
        if (cur.size() == 2 ? !fits2(op) : cur.size() >= 3 || (!cur.empty() && !fits1(op))) close();
        cur.push_back(op);
    }
    // max_ops 3 (gvm_machine.wide3): a third op, lite, in word 2, after a lite op 1 in a bundle with no long immediate
    bool fits2(uint32_t op) const {
        if (max_ops < 3 || long_of >= 0 || !lite(op) || !lite(cur[1])) return false;
        uint32_t p = path();
        return p == GVM_PATH_INT || p == GVM_PATH_LOAD || p == GVM_PATH_STORE;   // (the fast loop's: step12 makes two ops)
    }
    void put2(uint32_t op, uint32_t imm) {
        if (bump_reg >= 0 && touches(op, (uint32_t)bump_reg)) close();
        if (cur.size() >= 2 || (!cur.empty() && !fits1(op)) || long_of >= 0) close();
        long_of = (int)cur.size();
        long_imm = imm;
        cur.push_back(op);
    }

    static bool fits12(int32_t v) { return v >= -2048 && v <= 2047; }
    void R(uint32_t opc, uint32_t rd, uint32_t ra, uint32_t rb, uint32_t rc = 0) { put(GVM_WORD_R(opc, rd, ra, rb, rc)); }
    void I(uint32_t opc, uint32_t rd, uint32_t ra, int32_t imm, bool force_long = false) {
        uint32_t base_w = GVM_WORD_I(opc, rd, ra, 0u);
        if (fits12(imm) && !force_long) put(base_w | (uint32_t)(imm & 0xfff) << 20);
        else put2(base_w, (uint32_t)imm);
    }

    // integer
    void op(uint32_t f, uint32_t rd, uint32_t ra, uint32_t rb) { R(GVM_OP_I(f), rd, ra, rb); }
    void opi(uint32_t f, uint32_t rd, uint32_t ra, int32_t imm) { I(GVM_OP_II(f), rd, ra, imm); }
    void add(uint32_t rd, uint32_t ra, uint32_t rb) { op(GVM_I_ADD, rd, ra, rb); }
    void sub(uint32_t rd, uint32_t ra, uint32_t rb) { op(GVM_I_SUB, rd, ra, rb); }
    void addi(uint32_t rd, uint32_t ra, int32_t imm) { opi(GVM_I_ADD, rd, ra, imm); }
    void li(uint32_t rd, int32_t imm) { opi(GVM_I_ADD, rd, 0, imm); }
    void mv(uint32_t rd, uint32_t ra) { opi(GVM_I_ADD, rd, ra, 0); }
    void shadd(uint32_t rd, uint32_t ra, uint32_t rb, uint32_t sh) { R(GVM_OP_I(GVM_I_SHADD), rd, ra, rb, sh); }
    void sel(uint32_t rd, uint32_t ra, uint32_t rb, uint32_t rc) { R(GVM_OP_I(GVM_I_SEL), rd, ra, rb, rc); }
    void bfe(uint32_t rd, uint32_t ra, uint32_t pos, uint32_t len, bool sgn) {
        I(GVM_OP_II(GVM_I_BFE), rd, ra, (int32_t)(pos | (len - 1) << 5 | (sgn ? 0x400u : 0)));
    }
    // memory
    void ld(uint32_t wd, uint32_t rd, uint32_t ra, int32_t off) { I(GVM_OP_M(GVM_M_LOAD, wd), rd, ra, off); }
    void st(uint32_t wd, uint32_t rs, uint32_t ra, int32_t off) { I(GVM_OP_M(GVM_M_STORE, wd), rs, ra, off); }
    void ldx(uint32_t wd, uint32_t rd, uint32_t ra, uint32_t rb, uint32_t sh) { R(GVM_OP_M(GVM_M_LOADX, wd), rd, ra, rb, sh); }
    void stx(uint32_t wd, uint32_t rs, uint32_t ra, uint32_t rb, uint32_t sh) { R(GVM_OP_M(GVM_M_STOREX, wd), rs, ra, rb, sh); }
    void lw(uint32_t rd, uint32_t ra, int32_t off) { ld(GVM_W_W, rd, ra, off); }
    void sw(uint32_t rs, uint32_t ra, int32_t off) { st(GVM_W_W, rs, ra, off); }
    void lw_nc(uint32_t rd, uint32_t ra, int32_t off) { ld(nc_loads ? GVM_W_NC : GVM_W_W, rd, ra, off); }   // read-only data
    void amo(uint32_t f, uint32_t rd, uint32_t ra, uint32_t rb) { R(GVM_OP_M(GVM_M_AMO, f), rd, ra, rb); }
    // the core's file (isa.h, GVM_M_FREAD; phase 2): slot i = ra + imm (words, mod the file's size)
    void lf(uint32_t rd, uint32_t ra, int32_t i) { I(GVM_OP_M(GVM_M_FREAD, GVM_FK_LF), rd, ra, i); }
    void sf(uint32_t rs, uint32_t ra, int32_t i) { I(GVM_OP_M(GVM_M_FWRITE, GVM_FK_SF), rs, ra, i); }
    void fbind(uint32_t raddr, uint32_t ra, int32_t i) { I(GVM_OP_M(GVM_M_FREAD, GVM_FK_BIND), raddr, ra, i); }   // slot i <- the word at raddr's address
    void funbind(uint32_t ra, int32_t i) { I(GVM_OP_M(GVM_M_FWRITE, GVM_FK_UNBIND), 0, ra, i); }
    void fpair(uint32_t rs, uint32_t ra, int32_t off) { I(GVM_OP_M(GVM_M_FWRITE, GVM_FK_PAIR), rs, ra, off); }   // the word rs at ra + off, at the pass's end
    // float: the I form's short immediate is a float's top 12 bits
    void fop(uint32_t f, uint32_t rd, uint32_t ra, uint32_t rb, uint32_t rc = 0) { R(GVM_OP_F(f), rd, ra, rb, rc); }
    void fopi(uint32_t f, uint32_t rd, uint32_t ra, float v) {
        uint32_t bits; memcpy(&bits, &v, 4);
        uint32_t base_w = GVM_WORD_I(GVM_OP_FI(f), rd, ra, 0u);
        if ((bits & 0xfffff) == 0) put(base_w | (bits >> 20) << 20);
        else put2(base_w, bits);
    }
    // extended
    void x(uint32_t opc, uint32_t rd = 0, uint32_t ra = 0, uint32_t rb = 0) { R(opc, rd, ra, rb); }
    void xi(uint32_t opc, uint32_t rd, uint32_t ra, int32_t imm) { I(opc, rd, ra, imm); }
    void mfsr(uint32_t rd, uint32_t n) { I(GVM_X_MFSR, rd, 0, (int32_t)n); }
    void mtsr(uint32_t n, uint32_t ra) { I(GVM_X_MTSR, 0, ra, (int32_t)n); }
    void lpc(uint32_t rd, int l) {   // rd = the label's address (a long immediate, fixed up)
        put2(GVM_WORD_I(GVM_X_LPC, rd, 0u, 0u), 0);
        fixes.push_back({w.size() / 4, l, 2, 2});   // (after put2, which may close a bundle: the open one, its immediate word 2)
    }
    // rd = an absolute address by lpc (a PIC program's global: the bundle's address + a long immediate)
    void lpca(uint32_t rd, uint32_t addr) {
        put2(GVM_WORD_I(GVM_X_LPC, rd, 0u, 0u), 0);
        long_imm = addr - (base + (uint32_t)(w.size() / 4) * 16);   // (the open bundle's address)
    }
    void halt() { close(); x(GVM_X_BREAK); close(); }   // break in a bundle of its own

    // fused (gpu_vm/isa.h): rd += imm12 in the open bundle's control word, after its ops (a post-increment: ops already in
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
    void beq(uint32_t a, uint32_t b, int l) { br(GVM_CC_BEQ, a, b, l); }
    void bne(uint32_t a, uint32_t b, int l) { br(GVM_CC_BNE, a, b, l); }
    void blt(uint32_t a, uint32_t b, int l) { br(GVM_CC_BLT, a, b, l); }
    void bnei(uint32_t a, int32_t i, int l) { br(GVM_CC_BNEI, a, (uint32_t)i & 63, l); }
    // fused: ra += imm6 (-32..31) after the bundle's ops, and to l when the sum is not 0 (`addi ra, ra, i; bnez ra`)
    void addbnz(uint32_t a, int32_t i, int l) {
        if (i < -32 || i > 31 || (a & 63) == 0) { fprintf(stderr, "gvm_asm: addbnz r%u by %d\n", a & 63, i); exit(1); }
        if (br_old(a & 63)) close();
        br(GVM_CC_ADDBNZ, a, (uint32_t)i & 63, l);
    }
    // j: beq r0, r0 (+-4 KB, the ops' path) when finish() finds the label near, else cc 0's far j (the general step)
    void j(int l) { ctl_op(); fixes.push_back({w.size() / 4, l, 3, 0}); close(GVM_CTRL_J); }
    void call(int l) { ctl_op(); fixes.push_back({w.size() / 4, l, 1, 0}); close(GVM_CTRL_SPECIAL(GVM_SUB_CALL)); }
    void jr(uint32_t rs) { ctl_op(); close(GVM_CTRL_SPECIAL(GVM_SUB_JR) | (rs & 63) << 14); }
    void callr(uint32_t rs) { ctl_op(); close(GVM_CTRL_SPECIAL(GVM_SUB_CALLR) | (rs & 63) << 14); }
    void eret() { ctl_op(); close(GVM_CTRL_SPECIAL(GVM_SUB_ERET)); }
    void ret() { jr(1); }

    std::vector<uint32_t> finish() {
        close();
        if (thin8) return finish_thin();
        for (const Fix& f : fixes) {
            if (labels[f.label] < 0) { fprintf(stderr, "gvm_asm: label %d not bound\n", f.label); exit(1); }
            int64_t off = labels[f.label] - (int64_t)f.bundle;
            uint32_t& ctl = w[f.bundle * 4 + 3];
            if (f.kind == 3 && (off < -256 || off > 255)) {   // a far j: cc 0, the general step's (x18: step12's when its ops fit)
                ctl = (ctl & 0x78u) | GVM_CTRL_SPECIAL(GVM_SUB_J) | (uint32_t)(off << 14);
                ctl |= path12_of(ctl, w[f.bundle * 4], w[f.bundle * 4 + 1]);
            }
            else if (f.kind == 0 || f.kind == 3) {
                if (off < -256 || off > 255) { fprintf(stderr, "gvm_asm: a branch of %lld bundles\n", (long long)off); exit(1); }
                ctl = (ctl & ~(0x1ffu << 17)) | ((uint32_t)off & 0x1ffu) << 17;
            } else if (f.kind == 1) ctl = (ctl & 0x3fffu) | (uint32_t)(off << 14);
            else w[f.bundle * 4 + f.word] = (uint32_t)(off * 16);   // lpc: its immediate's word
        }
        return w;
    }
    // thin8: bundle i (n <= 1) becomes {op, control} at byte at[i], or {op, control, imm, 0} at a texel's start (a pad
    // {0, none} before it when needed); branches count 8 bytes; lpc's immediates move with their instruction
    std::vector<uint32_t> finish_thin() {
        size_t nb = w.size() / 4;
        std::vector<uint32_t> at(nb + 1);
        uint32_t o = 0;
        for (size_t i = 0; i < nb; i++) {
            uint32_t ctl = w[i * 4 + 3];
            if (GVM_N(ctl) > 1) { fprintf(stderr, "gvm_asm: thin8 with a bundle of %u ops\n", GVM_N(ctl)); exit(1); }
            if (GVM_LONG0(ctl) && (o & 8)) o += 8;
            at[i] = o;
            o += GVM_LONG0(ctl) ? 16 : 8;
        }
        at[nb] = o;
        std::vector<uint32_t> out(o / 4, 0);
        for (uint32_t k = 0; k < o / 4; k += 2) out[k + 1] = GVM_CTRL_NONE | GVM_PATH_INT;   // (pads: no op, no control)
        auto off_of = [&](const Fix& f) { return ((int64_t)at[labels[f.label]] - (int64_t)at[f.bundle]) / 8; };
        for (size_t i = 0; i < nb; i++) {
            uint32_t ctl = w[i * 4 + 3], k = at[i] / 4, imm = w[i * 4 + 2];
            if (GVM_N(ctl) == 1 && GVM_OPC(w[i * 4]) == GVM_X_LPC && GVM_LONG0(ctl)) imm += (uint32_t)(i * 16) - at[i];   // (lpca: pc-relative)
            out[k] = w[i * 4]; out[k + 1] = ctl;
            if (GVM_LONG0(ctl)) out[k + 2] = imm;
        }
        for (const Fix& f : fixes) {
            if (labels[f.label] < 0) { fprintf(stderr, "gvm_asm: label %d not bound\n", f.label); exit(1); }
            int64_t off = off_of(f);
            uint32_t& ctl = out[at[f.bundle] / 4 + 1];
            if (f.kind == 3 && (off < -256 || off > 255)) {
                ctl = (ctl & 0x78u) | GVM_CTRL_SPECIAL(GVM_SUB_J) | (uint32_t)(off << 14);
                ctl |= path12_of(ctl, w[f.bundle * 4], 0);
            } else if (f.kind == 0 || f.kind == 3) {
                if (off < -256 || off > 255) { fprintf(stderr, "gvm_asm: a branch of %lld instructions\n", (long long)off); exit(1); }
                ctl = (ctl & ~(0x1ffu << 17)) | ((uint32_t)off & 0x1ffu) << 17;
            } else if (f.kind == 1) ctl = (ctl & 0x3fffu) | (uint32_t)(off << 14);
            else out[at[f.bundle] / 4 + 2] = (uint32_t)(off * 8);   // lpc: its immediate
        }
        return out;
    }
};
