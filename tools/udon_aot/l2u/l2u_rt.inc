// ---------------------------------------------------------------- the machine's routines and the interpreter

static void irq_pending(Asm& o, const string& none) {
    o.ex(AGET, {"csr", o.c(12), "t"}); o.ex(AGET, {"csr", o.c(4), "t2"}); o.ex(AND, {"t", "t2", "t"});
    o.ex(AND, {"t", o.c(0x222), "ip"}); o.ex(EQ, {"ip", "x0", "cnd"});
    string some = o.fresh(); o.jif("cnd", some); o.jump(none);
    o.L(some);
    o.ex(EQ, {"priv", o.c(1), "cnd"}); string take = o.fresh(); o.jif("cnd", take);
    o.ex(AGET, {"csr", o.c(0), "t"}); o.ex(AND, {"t", o.c(2), "t"}); o.ex(EQ, {"t", "x0", "cnd"}); o.jif("cnd", take);
    o.jump(none);
    o.L(take);
}

static void routines(Asm& o) {
    auto c = [&](i64 v) { return o.c(v); };
    o.L("exit"); emit_exit(o);
    o.L("jalr_halt");
    o.copy(o.u("resume_rpc"), "resume"); o.ex(ADD, {"u", c(TEXT - 4), "t"}); o.ex(AND, {"t", c(~1LL), "rpc"}); o.jump("exit");
    o.L("resume_rpc");
    jt_index(o, "rpc", 0);
    if (TIER) thread_set(o, S); o.ex(UGET, {"jt", "tp", "tgt"}); o.jind("tgt");
    o.L("nonleader");
    o.ex(ADD, {"u", c(TEXT - 4), "t"}); o.ex(AND, {"t", c(~1LL), "ipc"}); o.jump("to_interp");

    o.L("to_interp");
    sync_to_xr(o); o.copy(c(0), "mode"); o.ex(ADD, {"nint", c(1), "nint"}); o.jump("iloop");
    o.L("to_irq");                       // translated code that let an interrupt in: take it at ipc
    sync_to_xr(o); o.copy(c(0), "mode"); o.ex(ADD, {"nint", c(1), "nint"}); o.jump("irq_take");
    o.L("to_trans");
    sync_from_xr(o); o.copy(c(1), "mode"); o.jind("tgt");

    o.L("mload");
    o.ex(ADD, {"t", c(ROM), "t2"}); o.ex(GE, {"t2", "x0", "cnd"}); o.jif("cnd", "ml_low");
    o.ex(SHR, {"t2", c(2), "t2"}); o.ex(LT, {"t2", "romlen", "cnd"}); o.jif("cnd", "ml_zero");
    o.ex(AGET, {"rom", "t2", "w"}); o.jind("mret");
    o.L("ml_low");
    o.ex(AND, {"t", c(~3LL), "t2"});
    o.ex(EQ, {"t2", c(0x030007F8 + MIN), "cnd"}); o.jif("cnd", "ml_r1"); o.copy("rtc0", "w"); o.jind("mret");
    o.L("ml_r1");
    o.ex(EQ, {"t2", c(0x030007FC + MIN), "cnd"}); o.jif("cnd", "ml_zero"); o.copy("rtc1", "w"); o.jind("mret");
    o.L("ml_zero"); o.copy("x0", "w"); o.jind("mret");

    o.L("gread");
    o.ex(ADD, {"ga", c(MIN), "t"}); o.ex(GE, {"t", "x0", "cnd"}); o.jif("cnd", "mload");
    o.ex(LT, {"t", c(RAMSIZE), "cnd"}); o.jif("cnd", "ml_zero");
    o.ex(SHR, {"t", c(2), "idx"}); o.ex(AGET, {"mem", "idx", "w"}); o.jind("mret");

    o.L("entry");
    emit_entry_extra(o);
    o.ex(EQ, {"th", "x0", "cnd"}); o.jif("cnd", "e_irq");
    o.ex(GT, {"tl", "x0", "cnd"}); o.jif("cnd", "e_irq");
    o.ex(LE, {"tl", "now", "cnd"}); o.jif("cnd", "e_irq");
    o.ex(AGET, {"csr", c(12), "t"}); o.ex(OR, {"t", c(0x20), "t"}); o.ex(ASET, {"csr", c(12), "t"});
    o.L("e_irq");
    irq_pending(o, "e_none");
    o.ex(EQ, {"mode", c(1), "cnd"}); o.jif("cnd", "irq_take");
    sync_to_xr(o); o.copy("rpc", "ipc"); o.copy(c(0), "mode"); o.jump("irq_take");
    o.L("e_none"); o.jind("resume");

    o.L("irq_look");
    irq_pending(o, "ichk");
    o.L("irq_take");
    o.ex(AND, {"ip", c(0x200), "t"}); o.ex(EQ, {"t", "x0", "cnd"}); o.jif("cnd", "irq_9");
    o.ex(AND, {"ip", c(2), "t"}); o.ex(EQ, {"t", "x0", "cnd"}); o.jif("cnd", "irq_1");
    o.copy(c(0x80000005LL), "tcause"); o.jump("irq_go");
    for (int bit : {9, 1}) {
        o.L("irq_" + to_string(bit)); o.copy(c(0x80000000LL | bit), "tcause");
        o.ex(AGET, {"csr", c(12), "t"}); o.ex(AND, {"t", c(~(1LL << bit)), "t"}); o.ex(ASET, {"csr", c(12), "t"});
        o.jump("irq_go");
    }
    o.L("irq_go"); o.copy("x0", "tval"); o.ex(ADD, {"nirq", c(1), "nirq"});

    o.L("take_trap");
    o.ex(ASET, {"csr", c(9), "ipc"}); o.ex(ASET, {"csr", c(10), "tcause"}); o.ex(ASET, {"csr", c(11), "tval"});
    o.ex(AGET, {"csr", c(5), "t"}); o.ex(AND, {"t", c(~3LL), "t2"}); o.ex(AND, {"t", c(3), "t3"});
    o.ex(EQ, {"t3", "x0", "cnd"}); o.jif("cnd", "tt_v"); o.copy("t2", "ipc"); o.jump("tt_st");
    o.L("tt_v");
    o.ex(LT, {"tcause", "x0", "cnd"}); o.jif("cnd", "tt_d");
    o.ex(AND, {"tcause", c(31), "t3"}); o.ex(SHL, {"t3", c(2), "t3"}); o.ex(ADD, {"t2", "t3", "ipc"}); o.jump("tt_st");
    o.L("tt_d"); o.copy("t2", "ipc");
    o.L("tt_st");
    o.ex(AGET, {"csr", c(0), "t"}); o.ex(AND, {"t", c(~0x122LL), "t2"});
    o.ex(AND, {"t", c(2), "t3"}); o.ex(SHL, {"t3", c(4), "t3"}); o.ex(OR, {"t2", "t3", "t2"});
    o.ex(AND, {"priv", c(1), "t3"}); o.ex(SHL, {"t3", c(8), "t3"}); o.ex(OR, {"t2", "t3", "t2"});
    o.ex(ASET, {"csr", c(0), "t2"}); o.copy(c(1), "priv"); o.ex(ADD, {"ntrap", c(1), "ntrap"});
    o.jump("ichk");
}

static void seg_routines(Asm& o, Seg* seg) {
    auto c = [&](i64 v) { return o.c(v); };
    const string& pre = seg->pre;
    o.L(pre + "jalr_halt");
    o.copy(o.u(pre + "resume_rpc"), "resume");
    o.ex(ADD, {"u", c(seg->lo - 4), "t"}); o.ex(ADD, {"t", "ud", "t"}); o.ex(AND, {"t", c(~1LL), "rpc"}); o.jump("exit");
    o.L(pre + "resume_rpc");
    jt_index(o, "rpc", 0, seg);
    if (TIER) thread_set(o, seg); o.ex(UGET, {seg->jt, "tp", "tgt"}); o.jind("tgt");
    o.L(pre + "nonleader");
    o.ex(ADD, {"u", c(seg->lo - 4), "t"}); o.ex(ADD, {"t", "ud", "t"}); o.ex(AND, {"t", c(~1LL), "ipc"}); o.jump("to_interp");
    o.L(pre + "ichk");
    jt_index(o, "ipc", 0, seg);
    o.ex(AGET, {seg->tk, "tp", "t"}); o.ex(GT, {"t", "x0", "cnd"}); o.jif("cnd", "iloop");
    if (TIER) thread_set(o, seg);
    o.ex(UGET, {seg->jt, "tp", "tgt"}); o.jump("to_trans");
}

static void ub_word(Asm& o, const string& fail) {
    o.ex(ADD, {"t", o.c(MIN), "t"}); o.ex(GE, {"t", "x0", "cnd"}); o.jif("cnd", fail);
    o.ex(LT, {"t", o.c(RAMSIZE - 4), "cnd"}); o.jif("cnd", fail);
    o.ex(SHR, {"t", o.c(2), "t"}); o.ex(AGET, {"mem", "t", "w"});
}

static void ubind(Asm& o, vector<Seg*>& segs) {
    auto c = [&](i64 v) { return o.c(v); };
    o.L("ubind");
    o.copy(c(0), "bi");
    o.L("ub_loop");
    o.ex(LT, {"bi", c(16), "cnd"}); o.jif("cnd", "ub_disc");
    o.ex(AGET, {"bs", "bi", "bseg"}); o.ex(ADD, {"bi", c(1), "bi"});
    o.ex(LT, {"bseg", "x0", "cnd"}); o.jif("cnd", "ub_has"); o.jump("ub_loop");
    o.L("ub_has");
    o.ex(SUB, {"bi", c(1), "t"}); o.ex(AGET, {"bb", "t", "bbase"});
    o.ex(SUB, {"inext", "bbase", "brel"});
    o.ex(AGET, {"sglo", "bseg", "t"}); o.ex(LT, {"brel", "t", "cnd"}); o.jif("cnd", "ub_lo"); o.jump("ub_loop");
    o.L("ub_lo");
    o.ex(AGET, {"sghi", "bseg", "t"}); o.ex(LT, {"brel", "t", "cnd"}); o.jif("cnd", "ub_loop");
    o.ex(SHL, {"bseg", c(2), "bk"});
    for (int k = 0; k < 4; k++) {
        o.ex(AGET, {"sgoff", "bk", "t"}); o.ex(ADD, {"bbase", "t", "t"}); ub_word(o, "ub_loop");
        o.ex(AGET, {"sgw", "bk", "t"}); o.ex(EQ, {"w", "t", "cnd"}); o.jif("cnd", "ub_loop");
        if (k < 3) o.ex(ADD, {"bk", c(1), "bk"});
    }
    o.copy("bseg", "useg"); o.copy("bbase", "ud"); o.ex(ADD, {"ud", c(MIN), "udo"}); o.ex(SHR, {"udo", c(2), "udi"}); o.jump("ilook");
    o.L("ub_disc");
    for (Seg* sg : segs) {
        string nxt = o.fresh();
        o.ex(SUB, {"inext", c(sg->entry), "bbase"});
        for (auto& ow : sg->sig) {
            o.ex(ADD, {"bbase", c(ow.first), "t"}); ub_word(o, nxt);
            o.ex(EQ, {"w", c(ow.second), "cnd"}); o.jif("cnd", nxt);
        }
        o.ex(ASET, {"bb", "rr", "bbase"}); o.ex(ASET, {"bs", "rr", c(sg->sid)});
        o.ex(ADD, {"rr", c(1), "rr"}); o.ex(AND, {"rr", c(15), "rr"});
        o.copy(c(sg->sid), "useg"); o.copy("bbase", "ud"); o.ex(ADD, {"ud", c(MIN), "udo"}); o.ex(SHR, {"udo", c(2), "udi"}); o.jump("ilook");
        o.L(nxt);
    }
    o.copy(c(MASK), "useg"); o.jump("ilook");
}

struct Tables {
    map<string, vector<string>> lab;        // tables of labels (.u32)
    map<string, vector<int32_t>> ints;      // tables of ints (.i32)
    vector<string>& add(const string& name, int n, const string& def) { lab[name] = vector<string>(n, def); return lab[name]; }
};

static void fpu(Asm& o, Tables& T);
static void systemr(Asm& o, Tables& T);

static void interpreter(Asm& o, Tables& T) {
    auto c = [&](i64 v) { return o.c(v); };
    o.L("iloop");
    o.ex(SUB, {"budget", c(1), "budget"}); o.ex(GT, {"budget", "x0", "cnd"}); o.jif("cnd", "ihalt");
    o.ex(ADD, {"icount", c(1), "icount"});
    if (PROF) {                          // interpreted instructions by kernel text word (last entry: elsewhere)
        o.ex(SUB, {"ipc", c(TEXT), "t"}); o.ex(SHR, {"t", c(2), "t"}); o.ex(CLAMP, {"t", c(0), c((TEND - TEXT) / 4), "t"});
        o.ex(AGET, {"prof2", "t", "pt"}); o.ex(ADD, {"pt", c(1), "pt"}); o.ex(ASET, {"prof2", "t", "pt"});
        string sk = o.fresh(); o.ex(EQ, {"priv", c(0), "cnd"}); o.jif("cnd", sk);
        o.ex(SUB, {"ipc", "ud", "t"}); o.ex(SHR, {"t", c(2), "t"}); o.ex(CLAMP, {"t", c(0), c((1 << 20) - 1), "t"});
        o.ex(AND, {"useg", c(3), "pt"}); o.ex(SHL, {"pt", c(20), "pt"}); o.ex(ADD, {"t", "pt", "t"});
        o.ex(AGET, {"prof3", "t", "pt"}); o.ex(ADD, {"pt", c(1), "pt"}); o.ex(ASET, {"prof3", "t", "pt"});
        o.L(sk);
    }
    o.copy("ipc", "ga"); o.copy(o.u("ifetched"), "mret"); o.jump("gread");
    o.L("ifetched");
    o.copy("w", "iw");
    o.ex(AND, {"iw", c(127), "iop"});
    o.ex(SHR, {"iw", c(7), "t"}); o.ex(AND, {"t", c(31), "ird"});
    o.ex(SHR, {"iw", c(12), "t"}); o.ex(AND, {"t", c(7), "if3"});
    o.ex(SHR, {"iw", c(15), "t"}); o.ex(AND, {"t", c(31), "irs1"});
    o.ex(SHR, {"iw", c(20), "t"}); o.ex(AND, {"t", c(31), "irs2"});
    o.ex(AGET, {"xr", "irs1", "ia"}); o.ex(AGET, {"xr", "irs2", "ib"});
    o.ex(ADD, {"ipc", c(4), "inext"});
    if (ISTAT) {
        o.ex(AGET, {"st_op", "iop", "t"}); o.ex(ADD, {"t", c(1), "t"}); o.ex(ASET, {"st_op", "iop", "t"});
        o.ex(MUL, {"ipv", c(128), "t"}); o.ex(ADD, {"t", "iop", "t"}); o.ex(AGET, {"st_pair", "t", "t2"}); o.ex(ADD, {"t2", c(1), "t2"});
        o.ex(ASET, {"st_pair", "t", "t2"}); o.copy("iop", "ipv");
        o.ex(SUB, {"priv", c(1), "t"}); o.ex(EQ, {"t", "x0", "cnd"}); o.ex(B2I, {"cnd", "t"});   // 1 in the kernel
        o.ex(LT, {"useg", "x0", "cnd"}); o.ex(B2I, {"cnd", "t2"}); o.ex(SHL, {"t2", c(1), "t2"}); o.ex(OR, {"t", "t2", "t"});
        o.ex(AGET, {"st_seg", "t", "t2"}); o.ex(ADD, {"t2", c(1), "t2"}); o.ex(ASET, {"st_seg", "t", "t2"});
        o.ex(SHR, {"ipc", c(2), "t"}); o.ex(AND, {"t", c(65535), "t"});       // by pc: a count and the last pc a bucket
        o.ex(AGET, {"st_pc", "t", "t2"}); o.ex(ADD, {"t2", c(1), "t2"}); o.ex(ASET, {"st_pc", "t", "t2"}); o.ex(ASET, {"st_pcv", "t", "ipc"});
    }
    o.ex(UGET, {"ijt", "iop", "tgt"}); o.jind("tgt");
    T.add("ijt", 128, "illegal");
    auto ijt = [&](int i, const string& l) { T.lab["ijt"][i] = l; };

    o.L("ihalt"); o.copy(o.u("iloop"), "resume"); o.copy("ipc", "rpc"); o.jump("exit");

    o.L("iwb"); o.ex(ASET, {"xr", "ird", "iv"}); o.ex(ASET, {"xr", "x0", "x0"});
    o.L("inoreg"); o.copy("inext", "ipc");
    o.L("ichk");
    o.ex(EQ, {"priv", c(1), "cnd"}); o.jif("cnd", "ichk_u");
    jt_index(o, "ipc", 0);
    o.ex(AGET, {"tk", "tp", "t"}); o.ex(GT, {"t", "x0", "cnd"}); o.jif("cnd", "iloop");
    if (TIER) thread_set(o, S);
    o.ex(UGET, {"jt", "tp", "tgt"}); o.jump("to_trans");

    o.L("ichk_u");
    o.ex(LT, {"useg", "x0", "cnd"}); o.jif("cnd", "ichk_u2"); o.jump(DYN ? "dent" : "iloop");
    o.L("ichk_u2"); o.ex(UGET, {"usjt", "useg", "tgt"}); o.jind("tgt");

    o.L("ilook"); o.copy("inext", "ipc"); o.jump("irq_look");

    o.L("illegal"); o.copy(c(2), "tcause"); o.copy("iw", "tval"); o.jump("take_trap");

    o.L("lui"); o.ex(AND, {"iw", c(0xFFFFF000LL), "iv"}); o.jump("iwb");
    o.L("auipc"); o.ex(AND, {"iw", c(0xFFFFF000LL), "t"}); o.ex(ADD, {"t", "ipc", "iv"}); o.jump("iwb");
    ijt(0x37, "lui"); ijt(0x17, "auipc");

    o.L("jal");
    o.copy("inext", "iv");
    o.ex(SHR, {"iw", c(11), "t"}); o.ex(AND, {"t", c(0xFFF00000LL), "t"});
    o.ex(AND, {"iw", c(0xFF000), "t2"}); o.ex(OR, {"t", "t2", "t"});
    o.ex(SHR, {"iw", c(9), "t2"}); o.ex(AND, {"t2", c(0x800), "t2"}); o.ex(OR, {"t", "t2", "t"});
    o.ex(SHR, {"iw", c(20), "t2"}); o.ex(AND, {"t2", c(0x7FE), "t2"}); o.ex(OR, {"t", "t2", "t"});
    o.ex(ADD, {"ipc", "t", "inext"}); o.jump("iwb");
    o.L("jalr");
    o.copy("inext", "iv"); o.ex(SHR, {"iw", c(20), "t"}); o.ex(ADD, {"ia", "t", "t"}); o.ex(AND, {"t", c(~1LL), "inext"});
    o.jump("iwb");
    ijt(0x6F, "jal"); ijt(0x67, "jalr");

    o.L("branch"); o.ex(UGET, {"bjt", "if3", "tgt"}); o.jind("tgt");
    ijt(0x63, "branch");
    T.add("bjt", 8, "illegal");
    struct BC { int f3; const string* cmp; int uns; };
    for (BC b : {BC{0, &EQ, 0}, BC{1, &NE, 0}, BC{4, &LT, 0}, BC{5, &GE, 0}, BC{6, &LT, 1}, BC{7, &GE, 1}}) {
        string lb = "b" + to_string(b.f3);
        o.L(lb); T.lab["bjt"][b.f3] = lb;
        string a = "ia", bb = "ib";
        if (b.uns) { o.ex(XOR, {"ia", c(MIN), "t"}); o.ex(XOR, {"ib", c(MIN), "t2"}); a = "t"; bb = "t2"; }
        o.ex(*b.cmp, {a, bb, "cnd"}); o.jif("cnd", "inoreg"); o.jump("btaken");
    }
    o.L("btaken");
    o.ex(SHR, {"iw", c(19), "t"}); o.ex(AND, {"t", c(0xFFFFF000LL), "t"});
    o.ex(SHL, {"iw", c(4), "t2"}); o.ex(AND, {"t2", c(0x800), "t2"}); o.ex(OR, {"t", "t2", "t"});
    o.ex(SHR, {"iw", c(20), "t2"}); o.ex(AND, {"t2", c(0x7E0), "t2"}); o.ex(OR, {"t", "t2", "t"});
    o.ex(SHR, {"iw", c(7), "t2"}); o.ex(AND, {"t2", c(0x1E), "t2"}); o.ex(OR, {"t", "t2", "t"});
    o.ex(ADD, {"ipc", "t", "inext"}); o.jump("inoreg");

    o.L("load"); o.ex(SHR, {"iw", c(20), "t3"}); o.ex(ADD, {"ia", "t3", "ga"});
    o.copy(o.u("load2"), "mret"); o.jump("gread");
    o.L("load2"); o.ex(UGET, {"ljt", "if3", "tgt"}); o.jind("tgt");
    ijt(0x03, "load");
    T.add("ljt", 8, "illegal");
    for (int f3 : {0, 1, 4, 5}) { string l = "l" + to_string(f3); o.L(l); T.lab["ljt"][f3] = l; emit_extract(o, f3, "iv"); o.jump("iwb"); }
    o.L("l2"); T.lab["ljt"][2] = "l2"; o.copy("w", "iv"); o.jump("iwb");
    o.L("flw"); o.ex(SHR, {"iw", c(20), "t3"}); o.ex(ADD, {"ia", "t3", "ga"});
    o.copy(o.u("flw2"), "mret"); o.jump("gread");
    o.L("flw2"); o.ex(ASET, {"fr", "ird", "w"}); if (FLT) o.copy(c(1), "ifd"); o.jump("inoreg");
    ijt(0x07, "flw");

    auto store_addr = [&]() {
        o.ex(SHR, {"iw", c(20), "t3"}); o.ex(AND, {"t3", c(~31LL), "t3"}); o.ex(OR, {"t3", "ird", "t3"});
        o.ex(ADD, {"ia", "t3", "ga"}); o.ex(ADD, {"ga", c(MIN), "t"});
        o.ex(GE, {"t", "x0", "cnd"}); o.jif("cnd", "inoreg");
        o.ex(LT, {"t", c(RAMSIZE), "cnd"}); o.jif("cnd", "inoreg");
        o.ex(SHR, {"t", c(2), "idx"});
    };
    o.L("store"); store_addr(); o.ex(UGET, {"sjt", "if3", "tgt"}); o.jind("tgt");
    ijt(0x23, "store");
    T.add("sjt", 8, "illegal");
    o.L("s2"); T.lab["sjt"][2] = "s2"; if (DIRTY) mark_word(o, "idx", false); o.ex(ASET, {"mem", "idx", "ib"}); o.jump("inoreg");
    for (int f3 : {0, 1}) {
        string l = "s" + to_string(f3); o.L(l); T.lab["sjt"][f3] = l;
        if (DIRTY) mark_word(o, "idx", false);
        o.ex(AGET, {"mem", "idx", "w"}); emit_insert(o, f3 == 0 ? 8 : 16, "ib"); o.jump("inoreg");
    }
    o.L("fsw"); store_addr(); o.ex(AGET, {"fr", "irs2", "iv"}); if (DIRTY) mark_word(o, "idx", false); o.ex(ASET, {"mem", "idx", "iv"}); o.jump("inoreg");
    ijt(0x27, "fsw");
    fpu(o, T);

    o.L("opimm"); o.ex(SHR, {"iw", c(20), "iimm"}); o.ex(UGET, {"oijt", "if3", "tgt"}); o.jind("tgt");
    ijt(0x13, "opimm");
    T.add("oijt", 8, "illegal");
    struct OPN { int f3; const string* op; };
    for (OPN p : {OPN{0, &ADD}, OPN{1, &SHL}, OPN{4, &XOR}, OPN{6, &OR}, OPN{7, &AND}}) {
        string l = "oi" + to_string(p.f3); o.L(l); T.lab["oijt"][p.f3] = l; o.ex(*p.op, {"ia", "iimm", "iv"}); o.jump("iwb");
    }
    o.L("oi2"); T.lab["oijt"][2] = "oi2"; o.ex(LT, {"ia", "iimm", "cnd"}); o.ex(B2I, {"cnd", "iv"}); o.jump("iwb");
    o.L("oi3"); T.lab["oijt"][3] = "oi3";
    o.ex(XOR, {"ia", c(MIN), "t"}); o.ex(XOR, {"iimm", c(MIN), "t2"}); o.ex(LT, {"t", "t2", "cnd"}); o.ex(B2I, {"cnd", "iv"});
    o.jump("iwb");
    o.L("oi5"); T.lab["oijt"][5] = "oi5";
    o.ex(AND, {"iw", c(0x40000000), "t"}); o.ex(EQ, {"t", "x0", "cnd"}); o.jif("cnd", "oi5a");
    emit_srl(o, "ia", "iimm", "iv"); o.jump("iwb");
    o.L("oi5a"); o.ex(SHR, {"ia", "iimm", "iv"}); o.jump("iwb");

    o.L("op");
    o.ex(SHR, {"iw", c(22), "t"}); o.ex(AND, {"t", c(8), "t"});
    o.ex(SHR, {"iw", c(26), "t2"}); o.ex(AND, {"t2", c(16), "t2"}); o.ex(OR, {"t", "t2", "t"}); o.ex(OR, {"t", "if3", "t"});
    o.ex(UGET, {"rjt", "t", "tgt"}); o.jind("tgt");
    ijt(0x33, "op");
    T.add("rjt", 32, "illegal");
    for (OPN p : {OPN{0, &ADD}, OPN{1, &SHL}, OPN{4, &XOR}, OPN{6, &OR}, OPN{7, &AND}}) {
        string l = "r" + to_string(p.f3); o.L(l); T.lab["rjt"][p.f3] = l; o.ex(*p.op, {"ia", "ib", "iv"}); o.jump("iwb");
    }
    o.L("r2"); T.lab["rjt"][2] = "r2"; o.ex(LT, {"ia", "ib", "cnd"}); o.ex(B2I, {"cnd", "iv"}); o.jump("iwb");
    o.L("r3"); T.lab["rjt"][3] = "r3";
    o.ex(XOR, {"ia", c(MIN), "t"}); o.ex(XOR, {"ib", c(MIN), "t2"}); o.ex(LT, {"t", "t2", "cnd"}); o.ex(B2I, {"cnd", "iv"});
    o.jump("iwb");
    o.L("r5"); T.lab["rjt"][5] = "r5"; emit_srl(o, "ia", "ib", "iv"); o.jump("iwb");
    o.L("r16"); T.lab["rjt"][16] = "r16"; o.ex(SUB, {"ia", "ib", "iv"}); o.jump("iwb");
    o.L("r21"); T.lab["rjt"][21] = "r21"; o.ex(SHR, {"ia", "ib", "iv"}); o.jump("iwb");
    for (int f3 = 0; f3 < 8; f3++) {
        string l = "m" + to_string(f3); o.L(l); T.lab["rjt"][8 + f3] = l; emit_mul(o, f3, "ia", "ib", "iv"); o.jump("iwb");
    }

    o.L("amo");
    o.ex(ADD, {"ia", c(MIN), "t"}); o.ex(SHR, {"t", c(2), "idx"}); o.ex(AGET, {"mem", "idx", "w"});
    o.ex(SHR, {"iw", c(27), "t2"}); o.ex(AND, {"t2", c(31), "t2"}); o.ex(UGET, {"ajt", "t2", "tgt"}); o.jind("tgt");
    ijt(0x2F, "amo");
    T.add("ajt", 32, "illegal");
    o.L("a2"); T.lab["ajt"][2] = "a2"; o.copy("w", "iv"); o.copy("ia", "res"); o.jump("iwb");
    o.L("a3"); T.lab["ajt"][3] = "a3";
    o.ex(EQ, {"res", "ia", "cnd"}); o.copy(c(MASK), "res"); o.jif("cnd", "a3f");
    if (DIRTY) mark_word(o, "idx", false);
    o.ex(ASET, {"mem", "idx", "ib"}); o.copy("x0", "iv"); o.jump("iwb");
    o.L("a3f"); o.copy(c(1), "iv"); o.jump("iwb");
    for (int f5 : {0, 1, 4, 8, 12, 16, 20, 24, 28}) {
        string l = "a" + to_string(f5); o.L(l); T.lab["ajt"][f5] = l;
        amo_value(o, f5, "ib"); if (DIRTY) mark_word(o, "idx", false); o.ex(ASET, {"mem", "idx", "t3"}); o.copy("w", "iv"); o.jump("iwb");
    }

    o.L("fence"); o.ex(EQ, {"iw", c(0x0100000F), "cnd"}); o.jif("cnd", "inoreg");
    o.copy("inext", "ipc"); o.copy(c(3), "status"); o.copy(o.u("iloop"), "resume"); o.copy("ipc", "rpc"); o.jump("exit");
    ijt(0x0F, "fence");

    systemr(o, T);
    ijt(0x73, "system");
}

static void fpu(Asm& o, Tables& T) {
    auto c = [&](i64 v) { return o.c(v); };
    o.L("fp");
    o.ex(AGET, {"fr", "irs1", "fia"}); o.ex(AGET, {"fr", "irs2", "fib"});
    o.ex(BITS2F, {"fia", "fa"}); o.ex(BITS2F, {"fib", "fb"});
    o.ex(SHR, {"iw", c(27), "t"}); o.ex(AND, {"t", c(31), "t"}); o.ex(UGET, {"fjt", "t", "tgt"}); o.jind("tgt");
    T.lab["ijt"][0x53] = "fp";
    T.add("fjt", 32, "illegal");
    auto fjt = [&](int i, const string& l) { T.lab["fjt"][i] = l; };
    const char* ops[] = {"Addition", "Subtraction", "Multiplication", "Division"};
    for (int f5 = 0; f5 < 4; f5++) { string l = "f" + to_string(f5); o.L(l); fjt(f5, l); o.ex(FOP(ops[f5]), {"fa", "fb", "fv"}); o.jump("fwbv"); }
    o.L("f11"); fjt(11, "f11"); o.ex(FMATH("Sqrt"), {"fa", "fv"});
    o.L("fwbv"); o.ex(F2BITS, {"fv", "iv"});
    o.L("fwb"); o.ex(ASET, {"fr", "ird", "iv"}); if (FLT) o.copy(c(1), "ifd"); o.jump("inoreg");

    o.L("f4"); fjt(4, "f4");
    o.ex(AND, {"fib", c(MIN), "t2"});
    string l1 = o.fresh(), lj = o.fresh();
    o.ex(EQ, {"if3", c(1), "cnd"}); o.jif("cnd", l1); o.ex(XOR, {"t2", c(MIN), "t2"}); o.L(l1);
    o.ex(EQ, {"if3", c(2), "cnd"}); o.jif("cnd", lj); o.ex(XOR, {"fia", "t2", "iv"}); o.jump("fwb");
    o.L(lj); o.ex(AND, {"fia", c(0x7FFFFFFF), "t"}); o.ex(OR, {"t", "t2", "iv"}); o.jump("fwb");

    o.L("f5"); fjt(5, "f5");
    string an = o.fresh(), bn = o.fresh(), ne = o.fresh(), pb = o.fresh();
    o.ex(AND, {"fia", c(0x7FFFFFFF), "t"}); o.ex(GT, {"t", c(0x7F800000), "cnd"}); o.jif("cnd", an);
    o.copy("fib", "iv"); o.jump("fwb");
    o.L(an); o.ex(AND, {"fib", c(0x7FFFFFFF), "t"}); o.ex(GT, {"t", c(0x7F800000), "cnd"}); o.jif("cnd", bn);
    o.copy("fia", "iv"); o.jump("fwb");
    o.L(bn); o.ex(FCMP("Equality"), {"fa", "fb", "cnd"}); o.jif("cnd", ne);
    string mx = o.fresh();
    o.ex(EQ, {"if3", "x0", "cnd"}); o.jif("cnd", mx); o.ex(OR, {"fia", "fib", "iv"}); o.jump("fwb");
    o.L(mx); o.ex(AND, {"fia", "fib", "iv"}); o.jump("fwb");
    o.L(ne);
    o.ex(FCMP("LessThan"), {"fa", "fb", "cnd"}); o.ex(B2I, {"cnd", "t"});
    o.ex(EQ, {"if3", "x0", "cnd"}); o.ex(B2I, {"cnd", "t2"});
    o.ex(EQ, {"t", "t2", "cnd"}); o.jif("cnd", pb); o.copy("fia", "iv"); o.jump("fwb");
    o.L(pb); o.copy("fib", "iv"); o.jump("fwb");

    o.L("f20"); fjt(20, "f20");
    l1 = o.fresh(); string l2 = o.fresh();
    o.ex(EQ, {"if3", c(2), "cnd"}); o.jif("cnd", l1); o.ex(FCMP("Equality"), {"fa", "fb", "cnd"}); o.ex(B2I, {"cnd", "iv"}); o.jump("iwb");
    o.L(l1); o.ex(EQ, {"if3", c(1), "cnd"}); o.jif("cnd", l2); o.ex(FCMP("LessThan"), {"fa", "fb", "cnd"}); o.ex(B2I, {"cnd", "iv"}); o.jump("iwb");
    o.L(l2); o.ex(FCMP("LessThanOrEqual"), {"fa", "fb", "cnd"}); o.ex(B2I, {"cnd", "iv"}); o.jump("iwb");

    o.L("f24"); fjt(24, "f24");
    o.copy("if3", "t");
    l1 = o.fresh();
    o.ex(EQ, {"t", c(7), "cnd"}); o.jif("cnd", l1); o.ex(SHR, {"fcsr", c(5), "t"}); o.ex(AND, {"t", c(7), "t"}); o.L(l1);
    string nan = o.fresh(), done = o.fresh();
    o.ex(AND, {"fia", c(0x7FFFFFFF), "t2"}); o.ex(GT, {"t2", c(0x7F800000), "cnd"}); o.jif("cnd", nan);
    o.copy(c(MASK), "iv"); o.ex(EQ, {"irs2", "x0", "cnd"}); o.jif("cnd", "iwb"); o.copy(c(0x7FFFFFFF), "iv"); o.jump("iwb");
    o.L(nan);
    string rz = o.fresh(), rdn = o.fresh(), ru = o.fresh(), rn = o.fresh();
    o.ex(EQ, {"t", c(1), "cnd"}); o.jif("cnd", rz);
    string neg = o.fresh();
    o.ex(FCMP("LessThan"), {"fa", "k_f0", "cnd"}); o.jif("cnd", neg); o.ex(FMATH("Ceil"), {"fa", "fv"}); o.jump(done);
    o.L(neg); o.ex(FMATH("Floor"), {"fa", "fv"}); o.jump(done);
    o.L(rz); o.ex(EQ, {"t", c(2), "cnd"}); o.jif("cnd", rdn); o.ex(FMATH("Floor"), {"fa", "fv"}); o.jump(done);
    o.L(rdn); o.ex(EQ, {"t", c(3), "cnd"}); o.jif("cnd", ru); o.ex(FMATH("Ceil"), {"fa", "fv"}); o.jump(done);
    o.L(ru); o.ex(FMATH("Round"), {"fa", "fv"});
    o.L(done);
    string uns = o.fresh(), big = o.fresh();
    o.ex(EQ, {"irs2", "x0", "cnd"}); o.jif("cnd", uns);
    o.copy(c(0x7FFFFFFF), "iv"); o.ex(FCMP("GreaterThanOrEqual"), {"fv", "k_f2p31", "cnd"}); o.jif("cnd", rn); o.jump("iwb");
    o.L(rn); o.copy(c(MIN), "iv"); o.ex(FCMP("LessThan"), {"fv", "k_fm2p31", "cnd"}); o.jif("cnd", big); o.jump("iwb");
    o.L(big); o.ex(F2I, {"fv", "iv"}); o.jump("iwb");
    o.L(uns);
    string u1 = o.fresh(), u2 = o.fresh(), u3 = o.fresh();
    o.copy(c(MASK), "iv"); o.ex(FCMP("GreaterThanOrEqual"), {"fv", "k_f2p32", "cnd"}); o.jif("cnd", u1); o.jump("iwb");
    o.L(u1); o.copy("x0", "iv"); o.ex(FCMP("LessThan"), {"fv", "k_f0", "cnd"}); o.jif("cnd", u2); o.jump("iwb");
    o.L(u2); o.ex(FCMP("GreaterThanOrEqual"), {"fv", "k_f2p31", "cnd"}); o.jif("cnd", u3);
    o.ex(FOP("Subtraction"), {"fv", "k_f2p32", "fv"});
    o.L(u3); o.ex(F2I, {"fv", "iv"}); o.jump("iwb");

    o.L("f26"); fjt(26, "f26");
    l1 = o.fresh();
    o.ex(EQ, {"irs2", "x0", "cnd"}); o.jif("cnd", l1); o.ex(I2F, {"ia", "fv"}); o.jump("fwbv");
    o.L(l1); o.ex(I2L, {"ia", "l"}); o.ex(I64("LogicalAnd"), {"l", "k_u32", "l"}); o.ex(L2F, {"l", "fv"}); o.jump("fwbv");

    o.L("f28"); fjt(28, "f28");
    string cl = o.fresh();
    o.ex(EQ, {"if3", "x0", "cnd"}); o.jif("cnd", cl); o.copy("fia", "iv"); o.jump("iwb");
    o.L(cl);
    o.ex(SHR, {"fia", c(23), "t"}); o.ex(AND, {"t", c(255), "t"}); o.ex(AND, {"fia", c(0x7FFFFF), "t2"});
    o.ex(LT, {"fia", "x0", "cnd"}); o.ex(B2I, {"cnd", "t3"});
    string other = o.fresh(), nanl = o.fresh(), qn = o.fresh(), normal = o.fresh(), subl = o.fresh(), put = o.fresh();
    o.ex(EQ, {"t", c(255), "cnd"}); o.jif("cnd", other);
    o.ex(EQ, {"t2", "x0", "cnd"}); o.jif("cnd", nanl);
    o.ex(MUL, {"t3", c(-7), "t"}); o.ex(ADD, {"t", c(7), "t"}); o.jump(put);
    o.L(nanl); o.ex(AND, {"t2", c(0x400000), "t2"}); o.copy(c(8), "t");
    o.ex(EQ, {"t2", "x0", "cnd"}); o.jif("cnd", qn); o.jump(put);
    o.L(qn); o.copy(c(9), "t"); o.jump(put);
    o.L(other); o.ex(EQ, {"t", "x0", "cnd"}); o.jif("cnd", normal);
    o.ex(EQ, {"t2", "x0", "cnd"}); o.jif("cnd", subl); o.ex(SUB, {c(4), "t3", "t"}); o.jump(put);
    o.L(subl); o.ex(MUL, {"t3", c(-3), "t"}); o.ex(ADD, {"t", c(5), "t"}); o.jump(put);
    o.L(normal); o.ex(MUL, {"t3", c(-5), "t"}); o.ex(ADD, {"t", c(6), "t"});
    o.L(put); o.ex(SHL, {c(1), "t", "iv"}); o.jump("iwb");
    o.L("f30"); fjt(30, "f30"); o.copy("ia", "iv"); o.jump("fwb");

    int fops[4] = {0x43, 0x47, 0x4B, 0x4F};
    for (int kind = 0; kind < 4; kind++) {
        string lab = "fma" + to_string(kind);
        o.L(lab); T.lab["ijt"][fops[kind]] = lab;
        o.ex(AGET, {"fr", "irs1", "t"}); o.ex(BITS2F, {"t", "fa"});
        o.ex(AGET, {"fr", "irs2", "t"}); o.ex(BITS2F, {"t", "fb"});
        o.ex(SHR, {"iw", c(27), "t"}); o.ex(AND, {"t", c(31), "t"}); o.ex(AGET, {"fr", "t", "t"}); o.ex(BITS2F, {"t", "fc"});
        o.ex(FOP("Multiplication"), {"fa", "fb", "fa"});
        if (kind == 0) o.ex(FOP("Addition"), {"fa", "fc", "fv"});
        else if (kind == 1) o.ex(FOP("Subtraction"), {"fa", "fc", "fv"});
        else if (kind == 2) o.ex(FOP("Subtraction"), {"fc", "fa", "fv"});
        else { o.ex(FOP("Addition"), {"fa", "fc", "fv"}); o.ex(FNEG, {"fv", "fv"}); }
        o.jump("fwbv");
    }
}

static i64 csr_mask(int n) {
    int s = (n >> 3 & 8) | (n & 7);
    if ((n & ~0x47) == 0x100)
        return s == 0 ? 0xDE162 : (s & 7) == 4 ? 0x222 : ((0xF60 >> s) & 1) ? MASK : 0;
    return (n == 0x300 || n == 0x304 || n == 0x344) ? MASK : 0;
}

static void memop(Asm& o) {
    auto c = [&](i64 v) { return o.c(v); };
    o.L("memop");
    o.ex(ADD, {"nmemop", c(1), "nmemop"});
    o.ex(ADD, {"mn", c(3), "t"}); o.ex(SHR, {"t", c(2), "icnt"}); o.ex(GT, {"icnt", "x0", "cnd"}); o.jif("cnd", "csr_done");
    o.ex(SHL, {"icnt", c(2), "ibytes"});
    o.ex(ADD, {"mdst", c(MIN), "dt"}); o.ex(GE, {"dt", "x0", "cnd"}); o.jif("cnd", "mo_slow");
    o.ex(ADD, {"dt", "ibytes", "t2"}); o.ex(LE, {"t2", c(RAMSIZE), "cnd"}); o.jif("cnd", "mo_slow");
    o.ex(SHR, {"dt", c(2), "didx"});
    if (DIRTY) { o.ex(SUB, {"t2", c(1), "mhi"}); mark_range(o, "dt", "mhi"); }
    o.ex(EQ, {"inv", c(2), "cnd"}); o.jif("cnd", "mo_copy");
    o.ex(EQ, {"msrc", "x0", "cnd"}); o.jif("cnd", "mo_slow");
    o.ex(ACLEAR, {"mem", "didx", "icnt"}); o.jump("csr_done");
    o.L("mo_copy");
    o.ex(ADD, {"msrc", c(MIN), "st"}); o.ex(GE, {"st", "x0", "cnd"}); o.jif("cnd", "mo_rom");
    o.ex(ADD, {"st", "ibytes", "t2"}); o.ex(LE, {"t2", c(RAMSIZE), "cnd"}); o.jif("cnd", "mo_slow");
    o.ex(SHR, {"st", c(2), "sidx"}); o.ex(ACOPY, {"mem", "sidx", "mem", "didx", "icnt"}); o.jump("csr_done");
    o.L("mo_rom");
    o.ex(SUB, {"msrc", c(ROM), "st"}); o.ex(GE, {"st", "x0", "cnd"}); o.jif("cnd", "mo_slow");
    o.ex(SHR, {"st", c(2), "sidx"}); o.ex(ADD, {"sidx", "icnt", "t2"}); o.ex(LE, {"t2", "romlen", "cnd"}); o.jif("cnd", "mo_slow");
    o.ex(ACOPY, {"rom", "sidx", "mem", "didx", "icnt"}); o.jump("csr_done");
    o.L("mo_slow");
    o.ex(ADD, {"nmoslow", c(1), "nmoslow"});
    o.copy("x0", "ii");
    o.L("mo_loop");
    o.ex(LT, {"ii", "ibytes", "cnd"}); o.jif("cnd", "csr_done");
    o.copy("msrc", "w"); o.ex(EQ, {"inv", c(2), "cnd"}); o.jif("cnd", "mo_rd"); o.jump("mo_wr");
    o.L("mo_rd"); o.ex(ADD, {"msrc", "ii", "ga"}); o.copy(o.u("mo_wr"), "mret"); o.jump("gread");
    o.L("mo_wr");
    o.ex(ADD, {"mdst", "ii", "t"}); o.ex(ADD, {"t", c(MIN), "t"});
    o.ex(GE, {"t", "x0", "cnd"}); o.jif("cnd", "mo_next"); o.ex(LT, {"t", c(RAMSIZE), "cnd"}); o.jif("cnd", "mo_next");
    o.ex(SHR, {"t", c(2), "idx"}); if (DIRTY) mark_word(o, "idx", false); o.ex(ASET, {"mem", "idx", "w"});
    o.L("mo_next"); o.ex(ADD, {"ii", c(4), "ii"}); o.jump("mo_loop");
}

static void systemr(Asm& o, Tables& T) {
    auto c = [&](i64 v) { return o.c(v); };
    o.L("system");
    o.ex(EQ, {"if3", "x0", "cnd"}); o.jif("cnd", "csrop");
    o.ex(ADD, {"nsys", c(1), "nsys"});
    pair<u32, const char*> sys[] = {{0x00000073, "ecall"}, {0x00100073, "ebreak"}, {0x10200073, "sret"}, {0x10500073, "wfi"}};
    for (auto& p : sys) { o.ex(EQ, {"iw", c(p.first), "cnd"}); string nxt = o.fresh(); o.jif("cnd", nxt); o.jump(p.second); o.L(nxt); }
    o.ex(SHR, {"iw", c(25), "t"}); o.ex(EQ, {"t", c(9), "cnd"}); o.jif("cnd", "illegal"); o.jump("ilook");

    o.L("ecall"); o.ex(EQ, {"priv", c(1), "cnd"}); o.jif("cnd", "ecall_u"); o.jump("sbi");
    o.L("ecall_u"); o.copy(c(8), "tcause"); o.copy("x0", "tval"); o.jump("take_trap");
    o.L("ebreak"); o.copy(c(3), "tcause"); o.copy("ipc", "tval"); o.jump("take_trap");
    o.L("sret");
    o.ex(AGET, {"csr", c(9), "inext"});
    o.ex(AGET, {"csr", c(0), "t"}); o.ex(SHR, {"t", c(8), "t2"}); o.ex(AND, {"t2", c(1), "priv"});
    o.ex(AND, {"t", c(~0x122LL), "t2"}); o.ex(SHR, {"t", c(4), "t3"}); o.ex(AND, {"t3", c(2), "t3"}); o.ex(OR, {"t2", "t3", "t2"});
    o.ex(OR, {"t2", c(0x20), "t2"}); o.ex(ASET, {"csr", c(0), "t2"});
    o.ex(EQ, {"priv", "x0", "cnd"}); o.jif("cnd", "ilook"); o.jump("ubind");
    o.L("wfi"); o.copy("inext", "ipc"); o.copy(c(2), "status"); o.copy(o.u("iloop"), "resume"); o.copy("ipc", "rpc");
    o.jump("exit");

    o.L("sbi");
    o.ex(AGET, {"xr", c(17), "sext"}); o.ex(AGET, {"xr", c(16), "sfn"}); o.ex(AGET, {"xr", c(10), "sa0"});
    o.ex(EQ, {"sext", c(0x54494D45), "cnd"}); o.jif("cnd", "sbi_t0"); o.jump("sbi_time");
    o.L("sbi_t0"); o.ex(EQ, {"sext", "x0", "cnd"}); o.jif("cnd", "sbi_val");
    o.L("sbi_time");
    o.copy("sa0", "tl"); o.ex(AGET, {"xr", c(11), "th"});
    o.ex(AGET, {"csr", c(12), "t"}); o.ex(AND, {"t", c(~0x20LL), "t"});
    o.ex(EQ, {"th", "x0", "cnd"}); o.jif("cnd", "sbi_ts");
    o.ex(GT, {"tl", "x0", "cnd"}); o.jif("cnd", "sbi_ts");
    o.ex(LE, {"tl", "now", "cnd"}); o.jif("cnd", "sbi_ts");
    o.ex(OR, {"t", c(0x20), "t"});
    o.L("sbi_ts"); o.ex(ASET, {"csr", c(12), "t"});
    o.L("sbi_val");
    o.copy("x0", "sval");
    o.ex(EQ, {"sext", c(0x10), "cnd"}); o.jif("cnd", "sbi_ret");
    pair<int, int> fv[] = {{0, 2}, {1, 0x454D55}, {2, 1}};
    for (auto& p : fv) { o.ex(EQ, {"sfn", c(p.first), "cnd"}); string nxt = o.fresh(); o.jif("cnd", nxt); o.copy(c(p.second), "sval"); o.jump("sbi_ret"); o.L(nxt); }
    o.ex(EQ, {"sfn", c(3), "cnd"}); o.jif("cnd", "sbi_ret");
    o.ex(EQ, {"sa0", c(0x54494D45), "cnd"}); o.jif("cnd", "sbi_p2"); o.copy(c(1), "sval"); o.jump("sbi_ret");
    o.L("sbi_p2");
    o.ex(XOR, {"sa0", c(MIN), "t"}); o.ex(LE, {"t", c(2 ^ MIN), "cnd"}); o.jif("cnd", "sbi_ret"); o.copy(c(1), "sval");
    o.L("sbi_ret");
    o.ex(XOR, {"sext", c(MIN), "t"}); o.ex(LT, {"t", c(0x10 ^ MIN), "cnd"}); o.jif("cnd", "sbi_new");
    o.ex(ASET, {"xr", c(10), "sval"}); o.jump("ilook");
    o.L("sbi_new");
    o.copy(c(0xFFFFFFFELL), "t2");
    { o.ex(EQ, {"sext", c(0x10), "cnd"}); string nxt = o.fresh(); o.jif("cnd", nxt); o.copy("x0", "t2"); o.L(nxt); }
    { o.ex(EQ, {"sext", c(0x54494D45), "cnd"}); string nxt = o.fresh(); o.jif("cnd", nxt); o.copy("x0", "t2"); o.L(nxt); }
    o.ex(ASET, {"xr", c(10), "t2"}); o.ex(ASET, {"xr", c(11), "sval"}); o.jump("ilook");

    o.L("csrop");
    o.ex(ADD, {"ncsr", c(1), "ncsr"});
    o.ex(SHR, {"iw", c(20), "t"}); o.ex(AND, {"t", c(0xFFF), "icsr"});
    o.ex(AGET, {"cslot", "icsr", "islot"}); o.ex(AGET, {"cmask", "icsr", "imask"});
    o.ex(AGET, {"csr", "islot", "t"}); o.ex(AND, {"t", "imask", "iv"});
    o.ex(AND, {"if3", c(4), "t"}); o.ex(EQ, {"t", "x0", "cnd"}); o.jif("cnd", "csr_imm");
    o.copy("ia", "isrc"); o.jump("csr_src");
    o.L("csr_imm"); o.copy("irs1", "isrc");
    o.L("csr_src"); o.ex(UGET, {"csrr", "icsr", "tgt"}); o.jind("tgt");
    o.L("csr_rd");
    o.ex(AND, {"if3", c(3), "t"}); o.ex(EQ, {"t", c(1), "cnd"}); o.jif("cnd", "csr_sc");
    o.copy("isrc", "inv"); o.jump("csr_write");
    o.L("csr_sc");
    o.ex(EQ, {"irs1", "x0", "cnd"}); o.jif("cnd", "csr_sc2"); o.jump("csr_done");
    o.L("csr_sc2");
    o.ex(EQ, {"t", c(2), "cnd"}); o.jif("cnd", "csr_clr"); o.ex(OR, {"iv", "isrc", "inv"}); o.jump("csr_write");
    o.L("csr_clr"); o.ex(XOR, {"isrc", c(MASK), "t2"}); o.ex(AND, {"iv", "t2", "inv"});
    o.L("csr_write");
    o.ex(AGET, {"csr", "islot", "t"}); o.ex(XOR, {"imask", c(MASK), "t2"}); o.ex(AND, {"t", "t2", "t"});
    o.ex(AND, {"inv", "imask", "t2"}); o.ex(OR, {"t", "t2", "t"}); o.ex(ASET, {"csr", "islot", "t"});
    o.ex(UGET, {"csrw", "icsr", "tgt"}); o.jind("tgt");
    o.L("csr_done"); emit_csr_done(o);

    vector<string> csrr(4096, "csr_rd"), csrw(4096, "csr_done");
    vector<int32_t> cslot(4096), cmask(4096);
    for (int n = 0; n < 4096; n++) {
        int s = (n >> 3 & 8) | (n & 7);
        cslot[n] = s; cmask[n] = (int32_t)sx(csr_mask(n), 32);
        if (s == 0) csrr[n] = "r_sd";
    }
    o.L("r_sd");
    o.ex(AND, {"iv", c(0x6000), "t"}); o.ex(EQ, {"t", "x0", "cnd"}); o.jif("cnd", "r_sd1"); o.jump("csr_rd");
    o.L("r_sd1"); o.ex(OR, {"iv", c(0x80006000LL), "iv"}); o.jump("csr_rd");
    vector<pair<int, function<void()>>> reads = {
        {1, [&] { o.ex(AND, {"fcsr", c(31), "iv"}); }}, {2, [&] { o.ex(SHR, {"fcsr", c(5), "iv"}); }},
        {3, [&] { o.copy("fcsr", "iv"); }}, {0x301, [&] { o.copy(c(0x40141101), "iv"); }},
        {0x0BF, [&] { o.ex(MUL, {"ipc", c(2654435761LL), "t"}); o.ex(XOR, {"t", c(0x5EED1234), "iv"}); }},
        {0xC01, [&] { o.copy("now", "iv"); }}, {0xC81, [&] { o.copy("x0", "iv"); }},
        {0xC00, [&] { emit_cycles(o); }}, {0xC02, [&] { emit_cycles(o); }},
        {0xC80, [&] { o.copy("x0", "iv"); }}, {0xC82, [&] { o.copy("x0", "iv"); }}};
    for (auto& r : reads) { string l = fmt("r_%x", r.first); o.L(l); csrr[r.first] = l; r.second(); o.jump("csr_rd"); }
    vector<pair<int, function<void()>>> writes = {
        {1, [&] { o.ex(AND, {"fcsr", c(0xE0), "t"}); o.ex(AND, {"inv", c(0x1F), "t2"}); o.ex(OR, {"t", "t2", "fcsr"}); }},
        {2, [&] { o.ex(AND, {"fcsr", c(0x1F), "t"}); o.ex(AND, {"inv", c(7), "t2"}); o.ex(SHL, {"t2", c(5), "t2"}); o.ex(OR, {"t", "t2", "fcsr"}); }},
        {3, [&] { o.ex(AND, {"inv", c(0xFF), "fcsr"}); }},
        {0x0B1, [&] { o.copy("inv", "msrc"); }}, {0x0B2, [&] { o.copy("inv", "mdst"); }}, {0x0B3, [&] { o.copy("inv", "mn"); }}};
    for (auto& w : writes) { string l = fmt("w_%x", w.first); o.L(l); csrw[w.first] = l; w.second(); o.jump("csr_done"); }
    csrw[0x0B0] = "memop";
    T.lab["csrr"] = csrr; T.lab["csrw"] = csrw;
    T.ints["cslot"] = cslot; T.ints["cmask"] = cmask;
    memop(o);
}
