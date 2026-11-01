// ---------------------------------------------------------------- threaded code: a routine a register shape
// tjt[tp] is word tp's code, its pc tlo + 4 tp, ttim[tp] its immediate; tjt, ttim, tlo, tlen are set on entry.

static void tpc(Asm& o, i64 add, const string& dest) {     // dest = the pc of word tp, plus add
    o.ex(SHL, {"tp", o.c(2), "t"});
    if (add) { o.ex(ADD, {"t", "tlo", "t"}); o.ex(ADD, {"t", o.c(add), dest}); } else o.ex(ADD, {"t", "tlo", dest});
}

static void prof_word(Asm& o, const string& at) {         // L2U_PROF: runs of each word (prof5, tbase + tp)
    if (!PROF) return;
    o.ex(AGET, {"prof5", at, "pt"}); o.ex(ADD, {"pt", o.c(1), "pt"}); o.ex(ASET, {"prof5", at, "pt"});
}

static void thread_set(Asm& o, const Seg* seg) {           // the segment whose words tp counts
    if (PROF) o.copy(o.c(seg->pbase), "tbase");
    o.ex(SUB, {"tp", o.c(1), "tst"}); o.copy(seg->jt, "tjt"); o.copy(seg->tim, "ttim"); o.copy(o.c((seg->hi - seg->lo) / 4 + 1), "tlen");
    if (seg->user) o.ex(ADD, {"ud", o.c(seg->lo - 4), "tlo"}); else o.copy(o.c(seg->lo - 4), "tlo");
}

static void charge(Asm& o) { o.ex(SUB, {"tp", "tst", "t"}); o.ex(SUB, {"gleft", "t", "gleft"}); }   // the words run after tst

static void thread_common(Asm& o) {
    o.L("tn"); o.ex(ADD, {"tp", o.c(1), "tp"});
    o.L("td");
    if (PROF) { o.ex(ADD, {"tbase", "tp", "t"}); prof_word(o, "t"); }
    o.ex(UGET, {"tjt", "tp", "tgt"}); o.jind("tgt");
    o.L("thalt"); o.copy(o.u("tresume"), "resume"); tpc(o, 0, "rpc"); o.jump("exit");
    o.L("tresume"); o.ex(SUB, {"rpc", "tlo", "u"}); o.ex(SHR, {"u", o.c(2), "tp"}); o.ex(SUB, {"tp", o.c(1), "tst"}); o.jump("td");
    o.L("ti"); tpc(o, 0, "ipc"); o.jump("to_interp");
    o.L("tjalr_halt"); o.copy(o.u("tresume"), "resume"); o.ex(ADD, {"u", "tlo", "t"}); o.ex(AND, {"t", o.c(~1LL), "rpc"}); o.jump("exit");
    o.L("tdent"); o.ex(SUB, {"tp", o.c(1), "tst"}); o.jump("td");
    o.L("thalt_rpc"); o.copy(o.u("tresume"), "resume"); o.jump("exit");
    o.L("tbv");                                             // taken: tp moves by ttim[tp], paying for the run
    charge(o); o.ex(AGET, {"ttim", "tp", "t"}); o.ex(ADD, {"tp", "t", "tp"}); o.ex(SUB, {"tp", o.c(1), "tst"});
    o.ex(GT, {"gleft", "x0", "cnd"}); o.jif("cnd", "thalt"); o.jump("td");
}

static vector<uint8_t> readfile(const string& p);

static void load_profile(const char* dir) {
    vector<uint8_t> b = readfile(string(dir) + "/prof5_counts.i32");
    FILE* f = fopen((string(dir) + "/prof5.layout").c_str(), "rb");
    if (!f) { fprintf(stderr, "no %s/prof5.layout\n", dir); exit(1); }
    char nm[512]; long long base, n;
    while (fscanf(f, "%511s %lld %lld", nm, &base, &n) == 3) {
        vector<int32_t>& v = PROFILE[nm]; v.resize(n);
        memcpy(v.data(), b.data() + 4 * (base + 1), 4 * n);
    }
    fclose(f);
}

// Words made inline: in loops of L2U_LOOPW words, in innermost loops of a program's of L2U_INNER (the kernel's
// L2U_INNERK), or run L2U_HOTN times in the profile of L2U_HOT.
static vector<char> hot_words(const vector<D>& ds) {
    const char* e = getenv("L2U_LOOPW"); i64 lw = e ? atoll(e) : -1;
    const char* hn = getenv("L2U_HOTN"); i64 th = hn ? atoll(hn) : 100;
    const char* ie = getenv("L2U_INNER"); i64 iw = ie ? atoll(ie) : 64;
    if (!S->user) { const char* ik = getenv("L2U_INNERK"); iw = ik ? atoll(ik) : 0; }
    auto back = [&](size_t j) { const D& d = ds[j]; return (d.k == K_BRANCH || (d.k == K_JAL && d.rd == 0)) && d.imm <= 0; };
    vector<int> nback(ds.size() + 1, 0);
    for (size_t j = 0; j < ds.size(); j++) nback[j + 1] = nback[j] + back(j);
    vector<int> mark(ds.size() + 1, 0);
    for (size_t j = 0; j < ds.size(); j++) {
        if (!back(j)) continue;
        i64 len = -ds[j].imm / 4, t = (i64)j - len;
        if (t < 0) continue;
        bool inner = nback[j] - nback[t] == 0;
        if (len <= lw || (inner && len <= iw)) { mark[t]++; mark[j + 1]--; }
    }
    vector<char> hot(ds.size()); int run = 0;
    for (size_t i = 0; i < ds.size(); i++) { run += mark[i]; hot[i] = run > 0; }
    auto pf = PROFILE.find(S->name.empty() ? "kernel" : S->name);
    if (pf != PROFILE.end() && pf->second.size() == ds.size())
        for (size_t i = 0; i < ds.size(); i++) if (pf->second[i] >= th) hot[i] = 1;
    return hot;
}

static long kind_n[16], kind_b[16];

// The routine of word d and the immediate it reads from ttim (set in imm).
static string troutine(Asm& o, const D& d, i64 pc, i64& imm) {
    int rd = d.rd, rs1 = d.rs1, rs2 = d.rs2, f3 = d.f3; K k = d.k;
    imm = 0;
    if (k == K_INTERP) return "ti";
    string key;
    switch (k) {
    case K_LUI: if (!rd) return "tn"; imm = d.imm; key = "T" + fmt("lu%d", rd); break;
    case K_AUIPC: if (!rd) return "tn"; imm = (pc + d.imm) & MASK; key = "T" + fmt("au%d", rd) + (S->user ? "u" : ""); break;
    case K_JAL: imm = d.imm / 4; if (!rd) return "tbv"; key = "T" + fmt("jl%d", rd); break;
    case K_BRANCH: imm = d.imm / 4; key = "T" + fmt("br%d_%d_%d", f3, rs1, rs2); break;
    case K_OPIMM:
        if (!rd) return "tn";
        if (f3 == 0 && d.imm == 0) key = "T" + fmt("mv%d_%d", rd, rs1);
        else if (f3 == 0 && rs1 == 0) { imm = d.imm; key = "T" + fmt("li%d", rd); }
        else if (f3 == 0 || f3 == 4 || f3 == 6 || f3 == 7) { imm = d.imm; key = "T" + fmt("oi%d_%d_%d", f3, rd, rs1); }
        else key = "T" + fmt("w%08x", d.w);
        break;
    case K_OP: if (!rd) return "tn"; key = "T" + fmt("w%08x", d.w); break;
    case K_LOAD: case K_STORE:
        if (k == K_LOAD && !rd) return "tn";
        if (rs1 == 0) return "ti";
        if (isfree(rs1)) { imm = d.imm + MIN; key = "T" + fmt("f%d_%d_%d_%d", (int)k, f3, k == K_LOAD ? rd : rs2, rs1); }
        else { imm = d.imm; key = "T" + fmt("m%d_%d_%d_%d", (int)k, f3, k == K_LOAD ? rd : rs2, rs1) + (S->user ? "u" : ""); }
        break;
    case K_FENCE: if (d.w != PAUSE) return "tn"; key = "Tpause"; break;
    case K_FLOAD: case K_FSTORE:
        if (rs1 == 0) return "ti";
        imm = d.imm; key = "T" + fmt("fm%d_%d_%d", (int)k, k == K_FLOAD ? rd : rs2, rs1) + (S->user ? "u" : ""); break;
    case K_JALR: imm = d.imm; key = "T" + fmt("jr%d_%d_%d", rd, rs1, (int)(rd == 0 && rs1 == 1 && d.imm == 0)); break;
    default: key = "T" + fmt("w%08x", d.w);
    }
    if (o.labels.count(key)) return key;
    o.L(key); size_t b0 = o.code.size();
    MState st; ST = &st;
    Item it; it.d = d; it.pc = 0; it.form = k;
    it.hasKA = rs1 == 0; it.hasKB = rs2 == 0; it.A = x(rs1); it.B = x(rs2);
    switch (k) {
    case K_LUI: o.ex(AGET, {"ttim", "tp", x(rd)}); o.jump("tn"); break;
    case K_AUIPC: o.ex(AGET, {"ttim", "tp", x(rd)}); if (S->user) o.ex(ADD, {x(rd), "ud", x(rd)}); o.jump("tn"); break;
    case K_JAL: tpc(o, 4, x(rd)); o.jump("tbv"); break;
    case K_BRANCH: t_branch(o, it, "tbv"); o.jump("tn"); break;
    case K_OPIMM:
        if (key[1] == 'm') o.copy(x(rs1), x(rd));
        else if (key[1] == 'l') o.ex(AGET, {"ttim", "tp", x(rd)});
        else if (key[1] == 'o') { o.ex(AGET, {"ttim", "tp", "t"}); o.ex(f3 == 0 ? ADD : f3 == 4 ? XOR : f3 == 6 ? OR : AND, {x(rs1), "t", x(rd)}); }
        else t_opimm(o, it);
        o.jump("tn"); break;
    case K_OP: if (d.f7 == 1) emit_mul(o, f3, it.A, it.B, x(rd)); else t_op(o, it); o.jump("tn"); break;
    case K_FENCE: o.copy(o.c(3), "status"); o.ex(ADD, {"tp", o.c(1), "tp"}); o.jump("thalt"); break;
    case K_AMO: t_amo(o, it); o.jump("tn"); break;
    case K_FLOAD: case K_FSTORE: {                         // x_rs1 + ttim[tp]; a float load from the ROM too
        string rl = k == K_FLOAD ? fmt("tromf%d", rd) : "ti";
        o.ex(AGET, {"ttim", "tp", "t"}); o.ex(ADD, {x(rs1), "t", "t"});
        if (!isfree(rs1)) { o.ex(LT, {"t", S->user ? o.c(ULIM) : "x0", "cnd"}); o.jif("cnd", rl); }
        o.ex(ADD, {"t", o.c(MIN), "t2"}); o.ex(SHR, {"t2", o.c(2), "idx"});
        if (k == K_FLOAD) { o.ex(AGET, {"mem", "idx", fx(rd)}); fwrote(o); } else { if (DIRTY) mark_word(o, "idx"); o.ex(ASET, {"mem", "idx", fx(rs2)}); }
        o.jump("tn");
        if (k == K_FLOAD && !o.labels.count(rl)) { o.L(rl); rom_word(o, "ti"); o.copy("w", fx(rd)); fwrote(o); o.jump("tn"); }
        break;
    }
    case K_FP: case K_FMA: t_float(o, d, []() { return string("ti"); }); o.jump("tn"); break;
    case K_CSR: t_csr(o, d, [&o](const string& v) { tpc(o, 4, v); }); o.jump("tn"); break;
    case K_LOAD: case K_STORE:
        if (key[1] == 'm') {                                // x_rs1 + ttim[tp] checked to be RAM
            o.ex(AGET, {"ttim", "tp", "t"}); o.ex(ADD, {x(rs1), "t", "t"});
            string rl = k == K_LOAD ? fmt("trom%d_%d", f3, rd) : "ti";
            o.ex(LT, {"t", S->user ? o.c(ULIM) : "x0", "cnd"}); o.jif("cnd", rl);
            o.ex(ADD, {"t", o.c(MIN), "toff"}); ST->off[rs1] = {"toff", d.imm};
            it.mode = M_FREE; it.hasKA = false; t_mem(o, it); o.jump("tn");
            if (k == K_LOAD && !o.labels.count(rl)) {       // a load from the ROM, as mapped files are read
                o.L(rl); rom_word(o, "ti");
                if (f3 == 2) o.copy("w", x(rd)); else emit_extract(o, f3, x(rd));
                o.jump("tn");
            }
            break;
        } else if (key[1] == 'f') { o.ex(AGET, {"ttim", "tp", "t"}); o.ex(ADD, {x(rs1), "t", "toff"}); ST->off[rs1] = {"toff", d.imm}; }
        it.mode = M_FREE; it.hasKA = false; t_mem(o, it); o.jump("tn"); break;
    case K_JALR: {
        bool ret = rd == 0 && rs1 == 1 && d.imm == 0;
        if (rd) tpc(o, 4, "t3");
        charge(o);
        o.ex(AGET, {"ttim", "tp", "t"}); o.ex(ADD, {x(rs1), "t", "u"}); o.ex(SUB, {"u", "tlo", "u"}); o.ex(SHR, {"u", o.c(2), "tp"});
        o.ex(CLAMP, {"tp", "x0", "tlen", "tp"}); o.ex(SUB, {"tp", o.c(1), "tst"});
        if (rd) o.copy("t3", x(rd));
        if (PROF) { o.ex(ADD, {"tbase", "tp", "t"}); prof_word(o, "t"); }
        o.ex(UGET, {"tjt", "tp", "tgt"});
        if (!ret) { o.ex(GT, {"gleft", "x0", "cnd2"}); o.jif("cnd2", "tjalr_halt"); }
        o.jind("tgt"); break;
    }
    default: o.jump("ti");
    }
    ST = nullptr; kind_n[k]++; kind_b[k] += o.code.size() - b0;
    if (MIX) ROPS[key] = scan_ops(o.code, b0, o.code.size());
    return key;
}

// A word's own routine, entered from inline code whose check of a pointer failed (a ROM pointer, say).
static bool THR = true;                  // L2U_THR=0: a failed check goes to the interpreter
static string thr_at(Asm& o, i64 pc) {
    if (!TIER || !THR) return to_interp_at(o, pc);
    if (!INL.count(pc)) return target(o, pc);
    D d0 = decode(S->words[(pc - S->lo) / 4]);
    if (d0.k != K_LOAD && d0.k != K_FLOAD) return to_interp_at(o, pc);
    string lab = S->lab("tw", pc);
    if (!o.istubs.count(lab)) {
        o.istubs.insert(lab); Seg* seg = S;
        o.later.push_back([&o, lab, pc, seg]() {
            i64 i = (pc - seg->lo) / 4; D d = decode(seg->words[i]); i64 imm = 0, t = (pc + d.imm) & MASK;
            o.L(lab);
            if (((d.k == K_BRANCH || d.k == K_JAL) && !in_text(t)) || i + 1 == (i64)seg->words.size()) { pcv(o, seg, pc, "ipc"); o.jump("to_interp"); return; }
            o.copy(o.c(i + 1), "tp"); o.ex(SUB, {"tp", o.c(1), "tst"});
            size_t c0 = o.code.size();
            string key = troutine(o, d, pc, imm);
            seg->timv[i + 1] = (int32_t)sx(imm, 32);
            if (o.code.size() == c0) o.jump(key);
        });
    }
    return lab;
}

static vector<string> thread_words(Asm& o, const vector<D>& ds, Stats& stats) {
    vector<string> lab(ds.size());
    S->timv.assign(ds.size() + 2, 0);
    size_t before = o.labels.size();
    for (size_t i = 0; i < ds.size(); i++) {
        i64 pc = S->lo + 4 * (i64)i; const D& d = ds[i];
        if (INL.count(pc)) continue;
        bool rel = d.k == K_BRANCH || d.k == K_JAL;
        i64 t = (pc + d.imm) & MASK, imm = 0;
        if ((rel && !in_text(t)) || (i + 1 == ds.size() && !ends_block(d))) { lab[i] = to_interp_at(o, pc); continue; }
        lab[i] = troutine(o, d, pc, imm);
        if (MIX && !MIXI[i]) {
            Ops p = ROPS.count(lab[i]) ? ROPS[lab[i]] : Ops();
            p.push += 6; p.ext += 2; p.jind++;
            mix_add(string("thr:") + mix_class(d), mix_count(pc), p, pc);
        }
        S->timv[i + 1] = (int32_t)sx(imm, 32);
    }
    stats.routines += o.labels.size() - before;
    return lab;
}
