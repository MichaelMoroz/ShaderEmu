#include "sec_float.inc"

static void t_jalr(Asm& o, Item& it) {
    D& d = it.d; i64 pc = it.pc; int rd = d.rd, rs1 = d.rs1; i64 imm = d.imm;
    if (it.hasKA && !S->user) {                       // a known target: a direct jump
        i64 t = (it.KA + imm) & MASK & ~1LL;
        if (rd) o.copy(o.c(pc + 4), x(rd));
        bool lead = TIER ? in_text(t) : o.leaders.count(t) > 0;
        if (t <= pc && lead) { o.ex(GT, {"gleft", "x0", "cnd"}); o.jif("cnd", halt_label(o, t)); }
        o.jump(lead ? target(o, t) : to_interp_at(o, t));
        return;
    }
    bool ret = rd == 0 && rs1 == 1 && imm == 0;       // a return cannot make a loop by itself
    Seg* seg = S;
    auto body = [&o, rs1, imm, ret, seg]() {
        jt_index(o, x(rs1), imm, seg); o.ex(UGET, {seg->jt, "tp", "tgt"});
        if (!ret) { o.ex(GT, {"gleft", "x0", "cnd2"}); o.jif("cnd2", seg->pre + "jalr_halt"); }
        if (TIER) o.ex(SUB, {"tp", o.c(1), "tst"});
        o.jind("tgt");
    };
    if (rd && rd == rs1) {                            // the link overwrites the base: in place
        jt_index(o, x(rs1), imm); pcv(o, S, pc + 4, x(rd)); o.ex(UGET, {S->jt, "tp", "tgt"});
        if (!ret) { o.ex(GT, {"gleft", "x0", "cnd2"}); o.jif("cnd2", S->pre + "jalr_halt"); }
        if (TIER) o.ex(SUB, {"tp", o.c(1), "tst"});
        o.jind("tgt");
        return;
    }
    if (rd) pcv(o, S, pc + 4, x(rd));
    auto rp = ret ? RETS.find(pc) : RETS.end();
    if (rp != RETS.end()) {                           // the likeliest return sites first
        string a = x(1);
        if (S->user) { o.ex(SUB, {x(1), "ud", "u"}); a = "u"; }
        for (i64 t : rp->second) { o.ex(NE, {a, o.c(t), "cnd"}); o.jif("cnd", target(o, t)); }
    }
    string lab = S->pre + fmt("jr%d_%llx_%d", rs1, (unsigned long long)(imm & MASK), (int)!ret);
    if (!o.istubs.count(lab)) { o.istubs.insert(lab); o.later.push_back([&o, lab, body]() { o.L(lab); body(); }); }
    o.jump(lab);
}

static void t_insn(Asm& o, Item& it) {
    D& d = it.d; i64 pc = it.pc;
    K k = d.k; int rd = d.rd, f3 = d.f3, f7 = d.f7;
    i64 imm = d.imm;
    if (k == K_JAL) {
        if (rd) pcv(o, S, pc + 4, x(rd));
        o.jump(rd && !XCALLS ? target(o, (pc + imm) & MASK) : tq(o, (pc + imm) & MASK));
    } else if (k == K_JALR) t_jalr(o, it);
    else if (k == K_BRANCH) t_branch(o, it, tq(o, (pc + imm) & MASK));
    else if (k == K_OPIMM) { if (rd) t_opimm(o, it); }
    else if (k == K_OP) {
        if (!rd) return;
        if (f7 == 1) emit_mul(o, f3, it.A, it.B, x(rd)); else t_op(o, it);
    } else if (k == K_AUIPC) o.ex(ADD, {"ud", o.c(pc + imm), x(rd)});
    else if (k == K_LUI) {}
    else if (k == K_LOAD || k == K_STORE) t_mem(o, it);
    else if (k == K_FENCE) { if (d.w == PAUSE) { o.copy(o.c(3), "status"); t_halt(o, pc + 4); } }
    else if (k == K_AMO) t_amo(o, it);
    else if (k >= K_FLOAD && k <= K_FMA) t_float(o, d, [&o, pc]() { return thr_at(o, pc); });
    else if (k == K_CSR) t_csr(o, d, [&o, pc](const string& v) { pcv(o, S, pc + 4, v); });
}

#include "sec_thread.inc"
#include "sec_native.inc"

// hooks the routines call
static void emit_exit(Asm& o) {
    if (COUNT) { o.ex(SUB, {"gstart", "gleft", "t"}); o.ex(ADD, {"gcount", "t", "gcount"}); }
    o.exit_();
}
static void emit_entry_extra(Asm& o) { o.ex(MUL, {"budget", o.c(GK), "gleft"}); o.copy("gleft", "gstart"); }
static void emit_csr_done(Asm& o) { o.ex(ASET, {"xr", "ird", "iv"}); o.ex(ASET, {"xr", "x0", "x0"}); o.jump("ilook"); }
static void emit_cycles(Asm& o) { o.ex(SUB, {"gstart", "gleft", "t"}); o.ex(ADD, {"gcount", "t", "t"}); o.ex(ADD, {"t", "icount", "iv"}); }
static void extra_vars(Asm& o) { o.var("gleft", T_I); o.var("gstart", T_I); }

#include "l2u_rt.inc"
#include "sec_dyn.inc"
#include "l2u_main.inc"
