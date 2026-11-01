// ---------------------------------------------------------------- the F extension translated (fpu()'s semantics)
// f0..f31 are Int32 heap variables holding the bits (a Single quietens a signalling NaN);
// fdirty says they are newer than fr[], ifd that fr[] is newer than them.

static string fx(int r) { return r == 32 ? "fda" : r == 33 ? "fdb" : "f" + to_string(r); }
static void fwrote(Asm& o) { o.copy(o.c(1), "fdirty"); }

static bool fp_ok(u32 w) {                                // an OP-FP word of single precision that fpu() runs
    if (((w >> 25) & 3) != 0) return false;
    int f5 = w >> 27, f3 = (w >> 12) & 7, rs2 = (w >> 20) & 31;
    switch (f5) {
    case 0: case 1: case 2: case 3: return true;
    case 11: return rs2 == 0;
    case 4: case 20: return f3 <= 2;
    case 5: return f3 <= 1;
    case 24: case 26: return rs2 <= 1;
    case 28: return rs2 == 0 && f3 <= 1;
    case 30: return rs2 == 0 && f3 == 0;
    }
    return false;
}

static void fbits(Asm& o, int rs1, int rs2) { o.copy(fx(rs1), "fia"); o.copy(fx(rs2), "fib"); }
static void fnum(Asm& o, int r, const string& f) { o.ex(BITS2F, {fx(r), f}); }

static void fcvt_w(Asm& o, int rm, bool uns, int rs1, const string& rd) {   // fcvt.w[u].s into rd
    string t = "t", done = o.fresh();
    fnum(o, rs1, "fa"); o.copy(fx(rs1), "fia");
    if (rm == 8) {                                       // 8: the mode is dtim[tp], 7 there meaning fcsr's
        string own = o.fresh(); o.ex(AGET, {"dtim", "tp", "t"}); o.ex(EQ, {"t", o.c(7), "cnd"}); o.jif("cnd", own);
        o.ex(SHR, {"fcsr", o.c(5), "t"}); o.ex(AND, {"t", o.c(7), "t"}); o.L(own);
    }
    if (rm == 7) { o.ex(SHR, {"fcsr", o.c(5), "t"}); o.ex(AND, {"t", o.c(7), "t"}); }
    string nan = o.fresh();
    o.ex(AND, {"fia", o.c(0x7FFFFFFF), "t2"}); o.ex(GT, {"t2", o.c(0x7F800000), "cnd"}); o.jif("cnd", nan);
    o.copy(o.c(uns ? MASK : 0x7FFFFFFF), rd); o.jump(done + "x");
    o.L(nan);
    auto rnd = [&](int m, const string& out) {
        if (m == 1) { string pos = o.fresh(); o.ex(FCMP("LessThan"), {"fa", "k_f0", "cnd"}); o.jif("cnd", pos);
                      o.ex(FMATH("Ceil"), {"fa", "fv"}); o.jump(out); o.L(pos); o.ex(FMATH("Floor"), {"fa", "fv"}); o.jump(out); }
        else if (m == 2) { o.ex(FMATH("Floor"), {"fa", "fv"}); o.jump(out); }
        else if (m == 3) { o.ex(FMATH("Ceil"), {"fa", "fv"}); o.jump(out); }
        else { o.ex(FMATH("Round"), {"fa", "fv"}); o.jump(out); }
    };
    if (rm >= 7) {                                       // the rounding mode of fcsr, as fpu() reads it
        string m1 = o.fresh(), m2 = o.fresh(), m3 = o.fresh(), m4 = o.fresh();
        o.ex(EQ, {"t", o.c(1), "cnd"}); o.jif("cnd", m2); rnd(1, done);
        o.L(m2); o.ex(EQ, {"t", o.c(2), "cnd"}); o.jif("cnd", m3); rnd(2, done);
        o.L(m3); o.ex(EQ, {"t", o.c(3), "cnd"}); o.jif("cnd", m4); rnd(3, done);
        o.L(m4); rnd(0, done);
        (void)m1;
    } else rnd(rm == 1 || rm == 2 || rm == 3 ? rm : 0, done);
    o.L(done);
    string fin = done + "x";
    if (!uns) {
        string rn = o.fresh(), big = o.fresh();
        o.copy(o.c(0x7FFFFFFF), rd); o.ex(FCMP("GreaterThanOrEqual"), {"fv", "k_f2p31", "cnd"}); o.jif("cnd", rn); o.jump(fin);
        o.L(rn); o.copy(o.c(MIN), rd); o.ex(FCMP("LessThan"), {"fv", "k_fm2p31", "cnd"}); o.jif("cnd", big); o.jump(fin);
        o.L(big); o.ex(F2I, {"fv", rd});
    } else {
        string u1 = o.fresh(), u2 = o.fresh(), u3 = o.fresh();
        o.copy(o.c(MASK), rd); o.ex(FCMP("GreaterThanOrEqual"), {"fv", "k_f2p32", "cnd"}); o.jif("cnd", u1); o.jump(fin);
        o.L(u1); o.copy("x0", rd); o.ex(FCMP("LessThan"), {"fv", "k_f0", "cnd"}); o.jif("cnd", u2); o.jump(fin);
        o.L(u2); o.ex(FCMP("GreaterThanOrEqual"), {"fv", "k_f2p31", "cnd"}); o.jif("cnd", u3);
        o.ex(FOP("Subtraction"), {"fv", "k_f2p32", "fv"});
        o.L(u3); o.ex(F2I, {"fv", rd});
    }
    o.L(fin);
}

static void fclass(Asm& o, const string& rd) {             // fpu()'s fclass of fia
    o.ex(SHR, {"fia", o.c(23), "t"}); o.ex(AND, {"t", o.c(255), "t"}); o.ex(AND, {"fia", o.c(0x7FFFFF), "t2"});
    o.ex(LT, {"fia", "x0", "cnd"}); o.ex(B2I, {"cnd", "t3"});
    string other = o.fresh(), nanl = o.fresh(), qn = o.fresh(), normal = o.fresh(), subl = o.fresh(), put = o.fresh();
    o.ex(EQ, {"t", o.c(255), "cnd"}); o.jif("cnd", other);
    o.ex(EQ, {"t2", "x0", "cnd"}); o.jif("cnd", nanl);
    o.ex(MUL, {"t3", o.c(-7), "t"}); o.ex(ADD, {"t", o.c(7), "t"}); o.jump(put);
    o.L(nanl); o.ex(AND, {"t2", o.c(0x400000), "t2"}); o.copy(o.c(8), "t");
    o.ex(EQ, {"t2", "x0", "cnd"}); o.jif("cnd", qn); o.jump(put);
    o.L(qn); o.copy(o.c(9), "t"); o.jump(put);
    o.L(other); o.ex(EQ, {"t", "x0", "cnd"}); o.jif("cnd", normal);
    o.ex(EQ, {"t2", "x0", "cnd"}); o.jif("cnd", subl); o.ex(SUB, {o.c(4), "t3", "t"}); o.jump(put);
    o.L(subl); o.ex(MUL, {"t3", o.c(-3), "t"}); o.ex(ADD, {"t", o.c(5), "t"}); o.jump(put);
    o.L(normal); o.ex(MUL, {"t3", o.c(-5), "t"}); o.ex(ADD, {"t", o.c(6), "t"});
    o.L(put); o.ex(SHL, {o.c(1), "t", rd});
}

// One float instruction; fail() is where a load or store whose address is not RAM goes.
static void t_float(Asm& o, const D& d, const function<string()>& fail) {
    int rd = d.rd, rs1 = d.rs1, rs2 = d.rs2, f3 = d.f3;
    if (d.k == K_FLOAD || d.k == K_FSTORE) {
        if (rs1 == 0) { o.jump(fail()); return; }
        if (!isfree(rs1)) mem_check(o, rs1, d.imm, fail());
        string idx = mem_idx(o, rs1, d.imm);
        if (d.k == K_FLOAD) { o.ex(AGET, {"mem", idx, fx(rd)}); fwrote(o); }
        else { mark_store(o, d, d.imm, idx, true); o.ex(ASET, {"mem", idx, fx(rs2)}); }
        return;
    }
    if (d.k == K_FMA) {
        int rs3 = d.w >> 27, kind = (d.op - 0x43) / 4;
        fnum(o, rs1, "fa"); fnum(o, rs2, "fb"); fnum(o, rs3, "fc");
        o.ex(FOP("Multiplication"), {"fa", "fb", "fa"});
        if (kind == 0) o.ex(FOP("Addition"), {"fa", "fc", "fv"});
        else if (kind == 1) o.ex(FOP("Subtraction"), {"fa", "fc", "fv"});
        else if (kind == 2) o.ex(FOP("Subtraction"), {"fc", "fa", "fv"});
        else { o.ex(FOP("Addition"), {"fa", "fc", "fv"}); o.ex(FNEG, {"fv", "fv"}); }
        o.ex(F2BITS, {"fv", fx(rd)}); fwrote(o); return;
    }
    int f5 = d.w >> 27;
    const char* ops[] = {"Addition", "Subtraction", "Multiplication", "Division"};
    string xd = rd ? x(rd) : "t3";                         // an integer result for x0 goes nowhere
    switch (f5) {
    case 0: case 1: case 2: case 3:
        fnum(o, rs1, "fa"); fnum(o, rs2, "fb"); o.ex(FOP(ops[f5]), {"fa", "fb", "fv"}); o.ex(F2BITS, {"fv", fx(rd)}); fwrote(o); break;
    case 11: fnum(o, rs1, "fa"); o.ex(FMATH("Sqrt"), {"fa", "fv"}); o.ex(F2BITS, {"fv", fx(rd)}); fwrote(o); break;
    case 4:
        if (rs1 == rs2 && f3 == 0) { if (rd != rs1) o.copy(fx(rs1), fx(rd)); fwrote(o); break; }
        fbits(o, rs1, rs2); o.ex(AND, {"fib", o.c(MIN), "t2"});
        if (f3 == 1) o.ex(XOR, {"t2", o.c(MIN), "t2"});
        if (f3 == 2) o.ex(XOR, {"fia", "t2", "iv"});
        else { o.ex(AND, {"fia", o.c(0x7FFFFFFF), "t"}); o.ex(OR, {"t", "t2", "iv"}); }
        o.copy("iv", fx(rd)); fwrote(o); break;
    case 5: {
        fbits(o, rs1, rs2);
        string an = o.fresh(), bn = o.fresh(), ne = o.fresh(), pb = o.fresh(), put = o.fresh();
        o.ex(AND, {"fia", o.c(0x7FFFFFFF), "t"}); o.ex(GT, {"t", o.c(0x7F800000), "cnd"}); o.jif("cnd", an);
        o.copy("fib", "iv"); o.jump(put);
        o.L(an); o.ex(AND, {"fib", o.c(0x7FFFFFFF), "t"}); o.ex(GT, {"t", o.c(0x7F800000), "cnd"}); o.jif("cnd", bn);
        o.copy("fia", "iv"); o.jump(put);
        o.L(bn); fnum(o, rs1, "fa"); fnum(o, rs2, "fb"); o.ex(FCMP("Equality"), {"fa", "fb", "cnd"}); o.jif("cnd", ne);
        o.ex(f3 == 0 ? OR : AND, {"fia", "fib", "iv"}); o.jump(put);
        o.L(ne); o.ex(FCMP("LessThan"), {"fa", "fb", "cnd"});
        if (f3 == 0) o.jif("cnd", pb); else { string pa = o.fresh(); o.jif("cnd", pa); o.jump(pb); o.L(pa); }
        o.copy("fia", "iv"); o.jump(put);
        o.L(pb); o.copy("fib", "iv");
        o.L(put); o.copy("iv", fx(rd)); fwrote(o); break;
    }
    case 20:
        fnum(o, rs1, "fa"); fnum(o, rs2, "fb");
        o.ex(FCMP(f3 == 2 ? "Equality" : f3 == 1 ? "LessThan" : "LessThanOrEqual"), {"fa", "fb", "cnd"});
        if (rd) o.ex(B2I, {"cnd", x(rd)});
        break;
    case 24: if (rd) fcvt_w(o, rs1 == 32 ? 8 : f3, rs2 == 1, rs1, x(rd)); break;
    case 26:
        if (rs2 == 0) o.ex(I2F, {x(rs1), "fv"});
        else { o.ex(I2L, {x(rs1), "l"}); o.ex(I64("LogicalAnd"), {"l", "k_u32", "l"}); o.ex(L2F, {"l", "fv"}); }
        o.ex(F2BITS, {"fv", fx(rd)}); fwrote(o); break;
    case 28:
        if (!rd) break;
        if (f3 == 0) o.copy(fx(rs1), x(rd));
        else { o.copy(fx(rs1), "fia"); fclass(o, x(rd)); }
        break;
    case 30: o.copy(x(rs1), fx(rd)); fwrote(o); break;
    }
    (void)xd;
}

static void fsync_out(Asm& o) {                            // f0..f31 into fr[] when the translation wrote them
    string go = o.fresh(), skip = o.fresh();
    o.ex(EQ, {"fdirty", "x0", "cnd"}); o.jif("cnd", go); o.jump(skip);
    o.L(go);
    for (int r = 0; r < 32; r++) o.ex(ASET, {"fr", o.c(r), fx(r)});
    o.copy("x0", "fdirty");
    o.L(skip);
}

static void fsync_in(Asm& o) {                             // fr[] into f0..f31 when the interpreter wrote it
    string go = o.fresh(), skip = o.fresh();
    o.ex(EQ, {"ifd", "x0", "cnd"}); o.jif("cnd", go); o.jump(skip);
    o.L(go);
    for (int r = 0; r < 32; r++) o.ex(AGET, {"fr", o.c(r), fx(r)});
    o.copy("x0", "ifd");
    o.L(skip);
}

// ---------------------------------------------------------------- CSR instructions translated (the interpreter's csrop)
static void emit_cycles(Asm& o);
static void irq_pending(Asm& o, const string& none);
static i64 csr_mask(int n);

static bool csr_ok(u32 w) {
    int f3 = (w >> 12) & 7, n = w >> 20, rs1 = (w >> 15) & 31;
    if (f3 == 0 || f3 == 4) return false;
    if ((n & ~0x47) == 0x100 || (n >= 1 && n <= 3)) return true;
    bool counter = n == 0xC00 || n == 0xC01 || n == 0xC02 || n == 0xC80 || n == 0xC81 || n == 0xC82;
    return counter && (f3 & 3) != 1 && rs1 == 0;              // a read that writes nothing
}

// One CSR instruction; next(v) puts pc + 4 in v for an interrupt the write lets in.
static void t_csr(Asm& o, const D& d, const function<void(const string&)>& next) {
    int n = d.w >> 20, f3 = d.f3, rd = d.rd, rs1 = d.rs1;
    string src = (f3 & 4) ? o.c(rs1) : x(rs1);
    bool write = (f3 & 3) == 1 || rs1 != 0;
    if (n >= 0xC00) {
        if (n == 0xC00 || n == 0xC02) emit_cycles(o); else if (n == 0xC01) o.copy("now", "iv"); else o.copy("x0", "iv");
        if (rd) o.copy("iv", x(rd));
        return;
    }
    auto newval = [&]() {                                      // inv from the value read and the source
        if ((f3 & 3) == 1) o.copy(src, "inv");
        else if ((f3 & 3) == 2) o.ex(OR, {"iv", src, "inv"});
        else { o.ex(XOR, {src, o.c(MASK), "t2"}); o.ex(AND, {"iv", "t2", "inv"}); }
    };
    if (n <= 3) {
        if (n == 1) o.ex(AND, {"fcsr", o.c(31), "iv"}); else if (n == 2) o.ex(SHR, {"fcsr", o.c(5), "iv"}); else o.copy("fcsr", "iv");
        if (write) {
            newval();
            if (n == 1) { o.ex(AND, {"fcsr", o.c(0xE0), "t"}); o.ex(AND, {"inv", o.c(0x1F), "t2"}); o.ex(OR, {"t", "t2", "fcsr"}); }
            else if (n == 2) { o.ex(AND, {"fcsr", o.c(0x1F), "t"}); o.ex(AND, {"inv", o.c(7), "t2"}); o.ex(SHL, {"t2", o.c(5), "t2"}); o.ex(OR, {"t", "t2", "fcsr"}); }
            else o.ex(AND, {"inv", o.c(0xFF), "fcsr"});
        }
        if (rd) o.copy("iv", x(rd));
        return;
    }
    int s = (n >> 3 & 8) | (n & 7); i64 mask = csr_mask(n);
    o.ex(AGET, {"csr", o.c(s), "t"}); o.ex(AND, {"t", o.c(mask), "iv"});
    if (s == 0) {
        string no = o.fresh();
        o.ex(AND, {"iv", o.c(0x6000), "t"}); o.ex(EQ, {"t", "x0", "cnd"}); o.jif("cnd", no + "y"); o.jump(no);
        o.L(no + "y"); o.ex(OR, {"iv", o.c(0x80006000LL), "iv"});
        o.L(no);
    }
    if (write) {
        newval();
        o.ex(AGET, {"csr", o.c(s), "t"}); o.ex(AND, {"t", o.c(~mask), "t"});
        o.ex(AND, {"inv", o.c(mask), "t2"}); o.ex(OR, {"t", "t2", "t"}); o.ex(ASET, {"csr", o.c(s), "t"});
    }
    if (rd) o.copy("iv", x(rd));
    if (write && (s == 0 || s == 4 || s == 12) && (f3 & 3) != 3) {   // what it enabled may be taken now
        string none = o.fresh();
        irq_pending(o, none);
        next("ipc"); o.jump("to_irq");
        o.L(none);
    }
}
