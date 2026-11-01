// ---------------------------------------------------------------- the dynamic tier: threaded code made at run time
// A program nobody translated runs through a window of DN words (djt, djt2, dtim, dtim2) filled a word at a time
// on first use: a routine that stages the word's source registers into da/db, then one for its operation and rd.
static const i64 DN = 1 << 20;
enum { DC_OPIMM = 0, DC_OP = 16, DC_LD = 48, DC_LI = 56, DC_JR = 57, DC_JRR = 58, DC_N = 59 };

static void dyn_vars(Asm& o) {
    for (const char* v : {"djt", "djt2"}) o.var(v, T_UA);
    for (const char* v : {"dtim", "dtim2"}) o.var(v, T_IA);
    for (const char* v : {"dlo", "dinit", "da", "db"}) o.var(v, T_I);
    for (const char* v : {"ds1", "ds2", "dop", "dfjt", "dst", "dbr", "djl"}) o.var(v, T_UA);
    o.var("tgt2", T_U); o.var("u0", T_U);
    for (const char* v : {"dsh_mv", "dsh_oi", "dsh_ld", "dsh_st", "dsh_br", "dsh_jr"}) o.var(v, T_UA);
    if (FLT) { for (const char* v : {"fda", "fdb"}) o.var(v, T_I); for (const char* v : {"dsf1", "dsf2", "dfop", "dflw", "dfsw"}) o.var(v, T_UA); o.var("dfpk", T_IA); }
    if (PROF) o.var("dbase", T_I);
}

static void dyn_set(Asm& o, const string& stage, const string& op, const string& imm) {   // fill word tp, run it
    const string USET = "SystemUInt32Array.__Set__SystemInt32_SystemUInt32__SystemVoid";
    o.ex(USET, {"djt", "tp", stage});
    if (!op.empty()) o.ex(USET, {"djt2", "tp", op});
    if (!imm.empty()) o.ex(ASET, {"dtim", "tp", imm});
    o.jind(stage);
}

static void dyn_clear(Asm& o) {
    const string USET = "SystemUInt32Array.__Set__SystemInt32_SystemUInt32__SystemVoid";
    o.ex(ACLEAR, {"djt", "x0", o.c(DN + 2)});
    o.ex(USET, {"djt", "x0", o.u("dpad")}); o.ex(USET, {"djt", o.c(DN + 1), o.u("dpad")});
}

static void dyn_float(Asm& o, Tables& T);

static void dyn_routines(Asm& o, Tables& T) {
    auto c = [&](i64 v) { return o.c(v); };
    const string USET = "SystemUInt32Array.__Set__SystemInt32_SystemUInt32__SystemVoid";
    // entry from the interpreter's check: ipc in an untranslated program
    o.L("dent");
    o.ex(EQ, {"dinit", "x0", "cnd"}); o.jif("cnd", "dent2");
    o.ex("SystemUInt32Array.__ctor__SystemInt32__SystemUInt32Array", {c(DN + 2), "djt"});
    o.ex("SystemUInt32Array.__ctor__SystemInt32__SystemUInt32Array", {c(DN + 2), "djt2"});
    o.ex("SystemInt32Array.__ctor__SystemInt32__SystemInt32Array", {c(DN + 2), "dtim"});
    o.ex("SystemInt32Array.__ctor__SystemInt32__SystemInt32Array", {c(DN + 2), "dtim2"});
    o.copy(c(1), "dinit"); o.copy(c(MIN), "dlo");
    o.L("dent2");
    o.ex(SUB, {"ipc", "dlo", "u"}); o.ex(SHR, {"u", c(2), "tp"}); o.ex(SUB, {"tp", c(1), "t"});
    o.ex(GE, {"t", "x0", "cnd"}); o.jif("cnd", "dreset"); o.ex(LT, {"t", c(DN - 1), "cnd"}); o.jif("cnd", "dreset");
    if (PROF) o.copy("dbase", "tbase");
    o.copy("djt", "tjt"); o.copy("dtim", "ttim"); o.copy("dlo", "tlo"); o.copy(c(DN + 1), "tlen"); o.ex(SUB, {"tp", c(1), "tst"});
    o.ex(UGET, {"tjt", "tp", "tgt"}); o.jump("to_trans");
    o.L("dreset");                                         // the window moves to ipc: a quarter below it
    o.ex(AND, {"ipc", c(~0xFFFFLL), "t"}); o.ex(SUB, {"t", c(DN + 4), "dlo"});
    dyn_clear(o); o.jump("dent2");
    o.L("dpad"); o.ex(ADD, {"u", "tlo", "t"}); o.ex(AND, {"t", c(~1LL), "ipc"}); o.jump("to_interp");
    o.L("dinval"); dyn_clear(o); o.jump("tn");             // fence.i: every word is filled again
    o.L("dpause"); o.copy(c(3), "status"); o.ex(ADD, {"tp", c(1), "tp"}); o.jump("thalt");
    o.L("dti"); o.ex(USET, {"djt", "tp", o.u("ti")}); o.jump("ti");

    // fill: decode word tp and dispatch by opcode
    o.L("dfill");
    o.ex(EQ, {"tp", c(DN), "cnd"}); o.jif("cnd", "df_ok"); o.jump("dti");   // the last word goes on outside: interpreted
    o.L("df_ok");
    tpc(o, 0, "t"); o.ex(ADD, {"t", c(MIN), "t2"});
    o.ex(GE, {"t2", "x0", "cnd"}); o.jif("cnd", "ti"); o.ex(LT, {"t2", c(RAMSIZE), "cnd"}); o.jif("cnd", "ti");
    o.ex(SHR, {"t2", c(2), "t2"}); o.ex(AGET, {"mem", "t2", "iw"});
    o.ex(AND, {"iw", c(127), "iop"});
    o.ex(SHR, {"iw", c(7), "t"}); o.ex(AND, {"t", c(31), "ird"});
    o.ex(SHR, {"iw", c(12), "t"}); o.ex(AND, {"t", c(7), "if3"});
    o.ex(SHR, {"iw", c(15), "t"}); o.ex(AND, {"t", c(31), "irs1"});
    o.ex(SHR, {"iw", c(20), "t"}); o.ex(AND, {"t", c(31), "irs2"});
    o.ex(UGET, {"dfjt", "iop", "tgt"}); o.jind("tgt");
    T.add("dfjt", 128, "dti");
    auto fj = [&](int op, const string& l) { T.lab["dfjt"][op] = l; o.L(l); };
    auto opl = [&](const string& idx) { o.ex(UGET, {"dop", idx, "tgt2"}); };   // op label of class*32+rd
    auto s1 = [&]() { o.ex(UGET, {"ds1", "irs1", "tgt"}); };
    auto s2 = [&]() { o.ex(SHL, {"irs1", c(5), "t"}); o.ex(ADD, {"t", "irs2", "t"}); o.ex(UGET, {"ds2", "t", "tgt"}); };
    auto rdz = [&](const string& to) { o.ex(EQ, {"ird", "x0", "cnd"}); string l = o.fresh(); o.jif("cnd", l); o.ex(USET, {"djt", "tp", o.u(to)}); o.jump(to); o.L(l); };
    auto tgt_ok = [&](const string& words) {               // tp + words inside the window, or the interpreter
        o.ex(ADD, {"tp", words, "t"}); o.ex(SUB, {"t", c(1), "t"});
        o.ex(GE, {"t", "x0", "cnd"}); o.jif("cnd", "dti"); o.ex(LT, {"t", c(DN), "cnd"}); o.jif("cnd", "dti");
    };
    const string UEQ = "SystemUInt32.__op_Equality__SystemUInt32_SystemUInt32__SystemBoolean";
    auto shape = [&](const string& table, const string& idx, const string& direct) {   // a threaded routine of the word's shape
        o.ex(UGET, {table, idx, "tgt"}); o.ex(UEQ, {"tgt", "u0", "cnd"}); o.jif("cnd", direct);
    };
    auto idx3 = [&](const string& a, const string& b, const string& cc) {   // t = a * 1024 + b * 32 + cc
        o.ex(SHL, {a, c(10), "t"}); o.ex(SHL, {b, c(5), "t2"}); o.ex(ADD, {"t", "t2", "t"}); o.ex(ADD, {"t", cc, "t"});
    };
    // opimm: class f3 | 8 * bit 30
    fj(0x13, "df_oi");
    rdz("tn");
    o.ex(SHR, {"iw", c(20), "iimm"});
    o.ex(EQ, {"irs1", "x0", "cnd"}); o.jif("cnd", "df_oi2");
    o.ex(EQ, {"if3", "x0", "cnd"}); o.jif("cnd", "df_oi2");
    o.ex(ADD, {c(DC_LI * 32), "ird", "t"}); opl("t"); dyn_set(o, "tgt2", "", "iimm");      // li
    o.L("df_oi2");
    {
        string notmv = o.fresh(), stage = o.fresh(), direct = o.fresh();
        o.ex(EQ, {"if3", "x0", "cnd"}); o.jif("cnd", notmv); o.ex(EQ, {"iimm", "x0", "cnd"}); o.jif("cnd", notmv);
        o.ex(SHL, {"ird", c(5), "t"}); o.ex(ADD, {"t", "irs1", "t"}); shape("dsh_mv", "t", direct); o.jump(stage);
        o.L(notmv); idx3("if3", "ird", "irs1"); shape("dsh_oi", "t", direct); o.jump(stage);
        o.L(direct); dyn_set(o, "tgt", "", "iimm");
        o.L(stage);
    }
    o.copy("if3", "t3");
    o.ex(EQ, {"if3", c(5), "cnd"}); string n5 = o.fresh(); o.jif("cnd", n5);
    o.ex(SHR, {"iw", c(27), "t"}); o.ex(AND, {"t", c(8), "t"}); o.ex(OR, {"t", "if3", "t3"}); o.L(n5);
    o.ex(EQ, {"t3", c(3), "cnd"}); string nsu = o.fresh(); o.jif("cnd", nsu); o.ex(XOR, {"iimm", c(MIN), "iimm"}); o.L(nsu);
    o.ex(AND, {"if3", c(3), "t"}); o.ex(EQ, {"t", c(1), "cnd"}); string nsh = o.fresh(); o.jif("cnd", nsh);
    o.ex(AND, {"iimm", c(31), "iimm"});                    // a shift: its amount, and srli's mask
    o.ex(SHR, {c(MIN), "iimm", "t"}); o.ex(SHL, {"t", c(1), "t"}); o.ex(XOR, {"t", c(MASK), "t"}); o.ex(ASET, {"dtim2", "tp", "t"});
    o.L(nsh);
    o.ex(SHL, {"t3", c(5), "t"}); o.ex(ADD, {"t", "ird", "t"}); opl("t"); s1(); dyn_set(o, "tgt", "tgt2", "iimm");
    // lui, auipc: li of the value
    fj(0x37, "df_lui"); rdz("tn"); o.ex(AND, {"iw", c(0xFFFFF000LL), "iimm"});
    o.ex(ADD, {c(DC_LI * 32), "ird", "t"}); opl("t"); dyn_set(o, "tgt2", "", "iimm");
    fj(0x17, "df_auipc"); rdz("tn"); o.ex(AND, {"iw", c(0xFFFFF000LL), "iimm"}); tpc(o, 0, "t"); o.ex(ADD, {"iimm", "t", "iimm"});
    o.ex(ADD, {c(DC_LI * 32), "ird", "t"}); opl("t"); dyn_set(o, "tgt2", "", "iimm");
    // op: the interpreter's rjt index
    fj(0x33, "df_op"); rdz("tn");
    o.ex(SHR, {"iw", c(22), "t"}); o.ex(AND, {"t", c(8), "t"});
    o.ex(SHR, {"iw", c(26), "t2"}); o.ex(AND, {"t2", c(16), "t2"}); o.ex(OR, {"t", "t2", "t"}); o.ex(OR, {"t", "if3", "t"});
    o.ex(ADD, {"t", c(DC_OP), "t"}); o.ex(SHL, {"t", c(5), "t"}); o.ex(ADD, {"t", "ird", "t"}); opl("t");
    s2(); dyn_set(o, "tgt", "tgt2", "");
    // loads, stores
    fj(0x03, "df_ld"); rdz("dti");
    o.ex(SHR, {"iw", c(20), "iimm"});
    { string d = o.fresh(), s = o.fresh(); idx3("if3", "ird", "irs1"); shape("dsh_ld", "t", d); o.jump(s); o.L(d); dyn_set(o, "tgt", "", "iimm"); o.L(s); }
    o.ex(ADD, {"if3", c(DC_LD), "t"}); o.ex(SHL, {"t", c(5), "t"}); o.ex(ADD, {"t", "ird", "t"}); opl("t"); s1(); dyn_set(o, "tgt", "tgt2", "iimm");
    T.add("dst", 8, "dti");
    for (int f3 = 0; f3 < 3; f3++) T.lab["dst"][f3] = "Dst" + to_string(f3);
    fj(0x23, "df_st");
    o.ex(SHR, {"iw", c(20), "t"}); o.ex(AND, {"t", c(~31LL), "t"}); o.ex(OR, {"t", "ird", "iimm"});
    { string d = o.fresh(), s = o.fresh(); idx3("if3", "irs2", "irs1"); shape("dsh_st", "t", d); o.jump(s); o.L(d); dyn_set(o, "tgt", "", "iimm"); o.L(s); }
    o.ex(UGET, {"dst", "if3", "tgt2"}); s2(); dyn_set(o, "tgt", "tgt2", "iimm");
    // branches, jal, jalr
    T.add("dbr", 8, "dti");
    for (int f3 : {0, 1, 4, 5, 6, 7}) T.lab["dbr"][f3] = "Dbr" + to_string(f3);
    fj(0x63, "df_br");
    o.ex(SHR, {"iw", c(19), "t"}); o.ex(AND, {"t", c(0xFFFFF000LL), "t"});
    o.ex(SHL, {"iw", c(4), "t2"}); o.ex(AND, {"t2", c(0x800), "t2"}); o.ex(OR, {"t", "t2", "t"});
    o.ex(SHR, {"iw", c(20), "t2"}); o.ex(AND, {"t2", c(0x7E0), "t2"}); o.ex(OR, {"t", "t2", "t"});
    o.ex(SHR, {"iw", c(7), "t2"}); o.ex(AND, {"t2", c(0x1E), "t2"}); o.ex(OR, {"t", "t2", "t"});
    o.ex(SHR, {"t", c(2), "iimm"}); tgt_ok("iimm");
    { string d = o.fresh(), s = o.fresh(); idx3("if3", "irs1", "irs2"); shape("dsh_br", "t", d); o.jump(s); o.L(d); dyn_set(o, "tgt", "", "iimm"); o.L(s); }
    o.ex(UGET, {"dbr", "if3", "tgt2"}); s2(); dyn_set(o, "tgt", "tgt2", "iimm");
    T.add("djl", 32, "tbv");
    for (int r = 1; r < 32; r++) T.lab["djl"][r] = "Djl" + to_string(r);
    fj(0x6F, "df_jal");
    o.ex(SHR, {"iw", c(11), "t"}); o.ex(AND, {"t", c(0xFFF00000LL), "t"});
    o.ex(AND, {"iw", c(0xFF000), "t2"}); o.ex(OR, {"t", "t2", "t"});
    o.ex(SHR, {"iw", c(9), "t2"}); o.ex(AND, {"t2", c(0x800), "t2"}); o.ex(OR, {"t", "t2", "t"});
    o.ex(SHR, {"iw", c(20), "t2"}); o.ex(AND, {"t2", c(0x7FE), "t2"}); o.ex(OR, {"t", "t2", "t"});
    o.ex(SHR, {"t", c(2), "iimm"}); tgt_ok("iimm");
    o.ex(UGET, {"djl", "ird", "tgt"}); dyn_set(o, "tgt", "", "iimm");
    fj(0x67, "df_jalr");
    o.ex(EQ, {"if3", "x0", "cnd"}); o.jif("cnd", "dti");
    o.ex(SHR, {"iw", c(20), "iimm"}); o.ex(ADD, {c(DC_JR * 32), "ird", "t"});
    {
        string d = o.fresh(), s = o.fresh(); o.copy("x0", "t3");
        o.ex(EQ, {"iw", c(0x00008067), "cnd"}); string n1 = o.fresh(); o.jif("cnd", n1); o.copy(c(1), "t3"); o.L(n1);
        idx3("t3", "ird", "irs1"); shape("dsh_jr", "t", d); o.jump(s); o.L(d); dyn_set(o, "tgt", "", "iimm"); o.L(s);
        o.ex(ADD, {c(DC_JR * 32), "ird", "t"});
    }
    o.ex(EQ, {"iw", c(0x00008067), "cnd"}); string nr = o.fresh(); o.jif("cnd", nr); o.copy(c(DC_JRR * 32), "t"); o.L(nr);
    opl("t"); s1(); dyn_set(o, "tgt", "tgt2", "iimm");
    fj(0x0F, "df_fence");
    o.ex(EQ, {"iw", c(PAUSE), "cnd"}); string np = o.fresh(); o.jif("cnd", np); o.ex(USET, {"djt", "tp", o.u("dpause")}); o.jump("dpause"); o.L(np);
    o.ex(EQ, {"if3", c(1), "cnd"}); string nf = o.fresh(); o.jif("cnd", nf); o.jump("dinval"); o.L(nf);
    o.ex(USET, {"djt", "tp", o.u("tn")}); o.jump("tn");

    // staging routines
    T.add("ds1", 32, "dti"); T.add("ds2", 1024, "dti");
    for (int a = 0; a < 32; a++) {
        string l = "DS1_" + to_string(a); T.lab["ds1"][a] = l;
        o.L(l); o.copy(x(a), "da"); o.ex(UGET, {"djt2", "tp", "tgt"}); o.jind("tgt");
        for (int b = 0; b < 32; b++) {
            string l2 = fmt("DS2_%d_%d", a, b); T.lab["ds2"][a * 32 + b] = l2;
            o.L(l2); o.copy(x(a), "da"); o.copy(x(b), "db"); o.ex(UGET, {"djt2", "tp", "tgt"}); o.jind("tgt");
        }
    }
    // operation routines by class and rd
    T.add("dop", DC_N * 32, "dti");
    auto& dop = T.lab["dop"];
    MState st; ST = &st;
    for (int r = 0; r < 32; r++) {
        string rd = x(r);
        auto beg = [&](int cls) { string l = fmt("Dc%d_%d", cls, r); dop[cls * 32 + r] = l; o.L(l); };
        for (int cls : {0, 1, 2, 3, 4, 5, 6, 7, 13}) {
            if (!r) { dop[DC_OPIMM * 32 + cls * 32 + r] = "tn"; continue; }
            beg(DC_OPIMM + cls); o.ex(AGET, {"dtim", "tp", "t"});
            int f3 = cls & 7;
            if (f3 == 0) o.ex(ADD, {"da", "t", rd});
            else if (f3 == 1) o.ex(SHL, {"da", "t", rd});
            else if (f3 == 2) { o.ex(LT, {"da", "t", "cnd"}); o.ex(B2I, {"cnd", rd}); }
            else if (f3 == 3) { o.ex(XOR, {"da", c(MIN), "t2"}); o.ex(LT, {"t2", "t", "cnd"}); o.ex(B2I, {"cnd", rd}); }
            else if (f3 == 5 && cls == 5) { o.ex(SHR, {"da", "t", "t2"}); o.ex(AGET, {"dtim2", "tp", "t"}); o.ex(AND, {"t2", "t", rd}); }
            else if (f3 == 5) o.ex(SHR, {"da", "t", rd});
            else o.ex(f3 == 4 ? XOR : f3 == 6 ? OR : AND, {"da", "t", rd});
            o.jump("tn");
        }
        for (int idx : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 21}) {
            if (!r) { dop[(DC_OP + idx) * 32] = "tn"; continue; }
            beg(DC_OP + idx);
            Item it; it.d = D{}; it.d.f3 = idx & 7; it.d.f7 = idx >= 16 ? 0x20 : idx >= 8 ? 1 : 0; it.d.rd = r; it.d.rs1 = 5; it.d.rs2 = 6;
            it.A = "da"; it.B = "db";
            if (it.d.f7 == 1) emit_mul(o, it.d.f3, "da", "db", rd); else t_op(o, it);
            o.jump("tn");
        }
        if (r) {
            beg(DC_LI); o.ex(AGET, {"dtim", "tp", rd}); o.jump("tn");
            for (int f3 : {0, 1, 2, 4, 5}) {
                beg(DC_LD + f3);
                o.ex(AGET, {"dtim", "tp", "t"}); o.ex(ADD, {"da", "t", "t"});
                o.ex(LT, {"t", o.c(ULIM), "cnd"}); o.jif("cnd", "ti");
                o.ex(ADD, {"t", o.c(MIN), "toff"});
                st = MState(); ST->off[5] = {"toff", 0};
                Item it; it.d = D{}; it.d.k = K_LOAD; it.d.f3 = f3; it.d.rd = r; it.d.rs1 = 5; it.d.imm = 0; it.mode = M_FREE;
                t_mem(o, it); o.jump("tn");
            }
            o.L("Djl" + to_string(r)); tpc(o, 4, rd); o.jump("tbv");
        }
        for (int ret = 0; ret < 2; ret++) {                // jalr: da + dtim
            if (ret && r) continue;
            string l = fmt("Djr%d_%d", r, ret); dop[(ret ? DC_JRR : DC_JR) * 32 + r] = l; o.L(l);
            if (r) tpc(o, 4, "t3");
            charge(o);
            o.ex(AGET, {"ttim", "tp", "t"}); o.ex(ADD, {"da", "t", "u"}); o.ex(SUB, {"u", "tlo", "u"}); o.ex(SHR, {"u", c(2), "tp"});
            o.ex(CLAMP, {"tp", "x0", "tlen", "tp"}); o.ex(SUB, {"tp", c(1), "tst"});
            if (r) o.copy("t3", rd);
            if (PROF) { o.ex(ADD, {"tbase", "tp", "t"}); prof_word(o, "t"); }
            o.ex(UGET, {"tjt", "tp", "tgt"});
            if (!ret) { o.ex(GT, {"gleft", "x0", "cnd2"}); o.jif("cnd2", "tjalr_halt"); }
            o.jind("tgt");
        }
    }
    for (int f3 = 0; f3 < 3; f3++) {
        o.L("Dst" + to_string(f3));
        o.ex(AGET, {"dtim", "tp", "t"}); o.ex(ADD, {"da", "t", "t"});
        o.ex(LT, {"t", o.c(ULIM), "cnd"}); o.jif("cnd", "ti");
        o.ex(ADD, {"t", o.c(MIN), "toff"});
        st = MState(); ST->off[5] = {"toff", 0};
        Item it; it.d = D{}; it.d.k = K_STORE; it.d.f3 = f3; it.d.rs1 = 5; it.d.rs2 = 6; it.d.imm = 0; it.mode = M_FREE; it.B = "db";
        t_mem(o, it); o.jump("tn");
    }
    for (int f3 : {0, 1, 4, 5, 6, 7}) {
        o.L("Dbr" + to_string(f3));
        st = MState();
        Item it; it.d = D{}; it.d.k = K_BRANCH; it.d.f3 = f3; it.d.rs1 = 5; it.d.rs2 = 6; it.A = "da"; it.B = "db";
        t_branch(o, it, "tbv"); o.jump("tn");
    }
    ST = nullptr;
    if (FLT) dyn_float(o, T);
}

// Floats: OP-FP by class (dfpk[f5 * 16 + f3 * 2 + (rs2 & 1)] = class | stage << 8 | rs2 at most 1 << 12), flw and fsw.
static void dyn_float(Asm& o, Tables& T) {
    auto c = [&](i64 v) { return o.c(v); };
    const string USET = "SystemUInt32Array.__Set__SystemInt32_SystemUInt32__SystemVoid";
    struct FC { int f5, f3, rs2, stage; bool xout; };     // stage 0: two f sources, 1: one f source, 2: one x source
    vector<FC> cls = {{0, -1, -1, 0, 0}, {1, -1, -1, 0, 0}, {2, -1, -1, 0, 0}, {3, -1, -1, 0, 0}, {11, -1, 0, 1, 0},
                      {4, 0, -1, 0, 0}, {4, 1, -1, 0, 0}, {4, 2, -1, 0, 0}, {5, 0, -1, 0, 0}, {5, 1, -1, 0, 0},
                      {20, 0, -1, 0, 1}, {20, 1, -1, 0, 1}, {20, 2, -1, 0, 1}, {24, -1, 0, 1, 1}, {24, -1, 1, 1, 1},
                      {26, -1, 0, 2, 0}, {26, -1, 1, 2, 0}, {28, 0, 0, 1, 1}, {28, 1, 0, 1, 1}, {30, 0, 0, 2, 0}};
    vector<int32_t> pk(512, 0);
    for (size_t k = 0; k < cls.size(); k++) {
        const FC& f = cls[k];
        for (int f3 = 0; f3 < 8; f3++) for (int b = 0; b < 2; b++) {
            if (f.f3 >= 0 && f3 != f.f3) continue;
            if (f.f5 == 24 && !(f3 <= 4 || f3 == 7)) continue;
            if (f.rs2 >= 0 && b != f.rs2) continue;
            pk[f.f5 * 16 + f3 * 2 + b] = (int32_t)(k + 1) | f.stage << 8 | (f.rs2 >= 0) << 12;
        }
    }
    T.ints["dfpk"] = pk;
    T.add("dfop", 32 * ((int)cls.size() + 1), "dti");
    T.add("dsf1", 32, "dti"); T.add("dsf2", 1024, "dti"); T.add("dflw", 32, "dti"); T.add("dfsw", 32, "dti");
    o.L("df_fp");
    T.lab["dfjt"][0x53] = "df_fp";
    o.ex(SHR, {"iw", c(25), "t"}); o.ex(AND, {"t", c(3), "t"}); o.ex(EQ, {"t", "x0", "cnd"}); o.jif("cnd", "dti");
    o.ex(SHR, {"iw", c(27), "t"}); o.ex(AND, {"t", c(31), "t"}); o.ex(SHL, {"t", c(4), "t"}); o.ex(SHL, {"if3", c(1), "t2"}); o.ex(OR, {"t", "t2", "t"});
    o.ex(AND, {"irs2", c(1), "t2"}); o.ex(OR, {"t", "t2", "t"}); o.ex(AGET, {"dfpk", "t", "t3"});
    o.ex(EQ, {"t3", "x0", "cnd"}); string ok = o.fresh(); o.jif("cnd", ok); o.jump("dti"); o.L(ok);
    o.ex(AND, {"t3", c(0x1000), "t"}); o.ex(EQ, {"t", "x0", "cnd"}); string small = o.fresh(), go = o.fresh(); o.jif("cnd", small); o.jump(go);
    o.L(small); o.ex(GT, {"irs2", c(1), "cnd"}); o.jif("cnd", go); o.jump("dti");
    o.L(go);
    o.ex(AND, {"t3", c(255), "t"}); o.ex(SHL, {"t", c(5), "t"}); o.ex(ADD, {"t", "ird", "t"}); o.ex(UGET, {"dfop", "t", "tgt2"});
    o.ex(SHR, {"t3", c(8), "t"}); o.ex(AND, {"t", c(15), "t"});
    string st1 = o.fresh(), st2 = o.fresh(), set = o.fresh();
    o.ex(EQ, {"t", "x0", "cnd"}); o.jif("cnd", st1);
    o.ex(SHL, {"irs1", c(5), "t"}); o.ex(ADD, {"t", "irs2", "t"}); o.ex(UGET, {"dsf2", "t", "tgt"}); o.jump(set);
    o.L(st1); o.ex(EQ, {"t", c(1), "cnd"}); o.jif("cnd", st2); o.ex(UGET, {"dsf1", "irs1", "tgt"}); o.jump(set);
    o.L(st2); o.ex(UGET, {"ds1", "irs1", "tgt"});
    o.L(set); dyn_set(o, "tgt", "tgt2", "if3");
    o.L("df_flw"); T.lab["dfjt"][0x07] = "df_flw";
    o.ex(EQ, {"if3", c(2), "cnd"}); o.jif("cnd", "dti");
    o.ex(SHR, {"iw", c(20), "iimm"}); o.ex(UGET, {"dflw", "ird", "tgt2"}); o.ex(UGET, {"ds1", "irs1", "tgt"}); dyn_set(o, "tgt", "tgt2", "iimm");
    o.L("df_fsw"); T.lab["dfjt"][0x27] = "df_fsw";
    o.ex(EQ, {"if3", c(2), "cnd"}); o.jif("cnd", "dti");
    o.ex(SHR, {"iw", c(20), "t"}); o.ex(AND, {"t", c(~31LL), "t"}); o.ex(OR, {"t", "ird", "iimm"});
    o.ex(UGET, {"dfsw", "irs2", "tgt2"}); o.ex(UGET, {"ds1", "irs1", "tgt"}); dyn_set(o, "tgt", "tgt2", "iimm");
    for (int a = 0; a < 32; a++) {
        string l = "DF1_" + to_string(a); T.lab["dsf1"][a] = l;
        o.L(l); o.copy(fx(a), "fda"); o.ex(UGET, {"djt2", "tp", "tgt"}); o.jind("tgt");
        for (int b = 0; b < 32; b++) {
            string l2 = fmt("DF2_%d_%d", a, b); T.lab["dsf2"][a * 32 + b] = l2;
            o.L(l2); o.copy(fx(a), "fda"); o.copy(fx(b), "fdb"); o.ex(UGET, {"djt2", "tp", "tgt"}); o.jind("tgt");
        }
    }
    MState st; ST = &st;
    for (size_t k = 0; k < cls.size(); k++) {
        const FC& f = cls[k];
        for (int r = 0; r < 32; r++) {
            if (f.xout && !r) { T.lab["dfop"][(k + 1) * 32] = "tn"; continue; }
            string l = fmt("Dfp%d_%d", (int)k + 1, r); T.lab["dfop"][(k + 1) * 32 + r] = l; o.L(l);
            D d{}; d.k = K_FP; d.rd = r; d.f3 = f.f3 < 0 ? 0 : f.f3; d.rs1 = 32; d.rs2 = f.stage == 0 ? 33 : max(f.rs2, 0);
            d.w = (u32)f.f5 << 27;
            t_float(o, d, []() { return string("ti"); }); o.jump("tn");
        }
    }
    for (int r = 0; r < 32; r++) {
        for (int store = 0; store < 2; store++) {
            string l = fmt("Df%s%d", store ? "sw" : "lw", r); T.lab[store ? "dfsw" : "dflw"][r] = l; o.L(l);
            o.ex(AGET, {"dtim", "tp", "t"}); o.ex(ADD, {"da", "t", "t"});
            o.ex(LT, {"t", c(ULIM), "cnd"}); o.jif("cnd", "ti");
            o.ex(ADD, {"t", c(MIN), "t"}); o.ex(SHR, {"t", c(2), "idx"});
            if (store) { if (DIRTY) mark_word(o, "idx"); o.ex(ASET, {"mem", "idx", fx(r)}); } else { o.ex(AGET, {"mem", "idx", fx(r)}); fwrote(o); }
            o.jump("tn");
        }
    }
    ST = nullptr;
}

// The shape tables: threaded routines the translated programs made, by the word's registers ("dz", 0: none).
static void dyn_tables(Asm& o, Tables& T) {
    auto put = [&](const char* tab, int n, const function<string(int, int, int)>& key) {
        auto& v = T.add(tab, n, "dz");
        for (int i = 0; i < n; i++) { string k = key(i >> 10, (i >> 5) & 31, i & 31); if (!k.empty() && o.labels.count(k)) v[i] = k; }
    };
    put("dsh_mv", 1024, [](int, int rd, int rs) { return fmt("Tmv%d_%d", rd, rs); });
    put("dsh_oi", 8192, [](int f3, int rd, int rs) { return (f3 == 0 || f3 >= 4) && f3 != 5 && rs ? fmt("Toi%d_%d_%d", f3, rd, rs) : string(); });
    put("dsh_ld", 8192, [](int f3, int rd, int rs) { return rd && rs && !isfree(rs) ? fmt("Tm%d_%d_%d_%du", (int)K_LOAD, f3, rd, rs) : string(); });
    put("dsh_st", 8192, [](int f3, int r2, int rs) { return rs && !isfree(rs) ? fmt("Tm%d_%d_%d_%du", (int)K_STORE, f3, r2, rs) : string(); });
    put("dsh_br", 8192, [](int f3, int a, int b) { return fmt("Tbr%d_%d_%d", f3, a, b); });
    put("dsh_jr", 2048, [](int ret, int rd, int rs) { return fmt("Tjr%d_%d_%d", rd, rs, ret); });
}
