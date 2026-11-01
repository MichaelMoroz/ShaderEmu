// ---------------------------------------------------------------- build

static vector<uint8_t> readfile(const string& p) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) { fprintf(stderr, "cannot read %s\n", p.c_str()); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    vector<uint8_t> b(n); if (n && fread(b.data(), 1, n, f) != (size_t)n) exit(1); fclose(f); return b;
}
static u32 be32(const vector<uint8_t>& b, size_t o) { return (u32)b[o] << 24 | (u32)b[o + 1] << 16 | (u32)b[o + 2] << 8 | b[o + 3]; }
static u32 le32(const uint8_t* b) { return (u32)b[0] | (u32)b[1] << 8 | (u32)b[2] << 16 | (u32)b[3] << 24; }

static void romfs_walk(const vector<uint8_t>& rom, size_t off, const string& path, map<string, pair<size_t, size_t>>& out) {
    auto name_at = [&](size_t o, size_t& next) { size_t e = o; while (rom[e]) e++; next = (e + 16) & ~(size_t)15; return string((const char*)&rom[o], e - o); };
    while (off) {
        u32 nxt = be32(rom, off), spec = be32(rom, off + 4), size = be32(rom, off + 8);
        size_t data; string name = name_at(off + 16, data);
        if ((nxt & 7) == 1 && name != "." && name != "..") romfs_walk(rom, spec, path + "/" + name, out);
        else if ((nxt & 7) == 2) out[path + "/" + name] = {data, size};
        off = nxt & ~15u;
    }
}

static Seg* flat_segment(const uint8_t* b, size_t len, int sid) {
    vector<uint8_t> v(b, b + len);
    u32 entry = be32(v, 8), ds = be32(v, 12), reloc = be32(v, 32), nrel = be32(v, 36);
    Seg* sg = new Seg(0x40, ds, true, sid);
    sg->entry = entry; sg->bss = be32(v, 20);
    size_t n = (ds - 0x40) / 4;
    for (size_t i = 0; i < n; i++) sg->words.push_back(le32(b + 0x40 + 4 * i));
    for (u32 i = 0; i < nrel; i++) {
        u32 r = be32(v, reloc + 4 * i);
        if (0x40 + (size_t)r + 4 <= len) sg->data.insert((i64)be32(v, 0x40 + r) + 0x40);
    }
    for (int k = 1; k <= 4; k++) { size_t j = n * k / 5; sg->sig.push_back({0x40 + 4 * (i64)j, sg->words[j]}); }
    return sg;
}

static void wfile(const string& p, const void* d, size_t n) { FILE* f = fopen(p.c_str(), "wb"); fwrite(d, 1, n, f); fclose(f); }
static void wtext(const string& p, const string& s) {          // as Python's text mode on Windows writes it
    string o; o.reserve(s.size() + s.size() / 16);
    for (char ch : s) { if (ch == '\n') o += '\r'; o += ch; }
    wfile(p, o.data(), o.size());
}
template <class T> static void wvec(const string& p, const vector<T>& v) { wfile(p, v.data(), v.size() * sizeof(T)); }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "l2u linux_payload.bin OUT_DIR [rootfs.bin PROGRAM...]\n"); return 2; }
    const char* ce = getenv("L2U_COUNT"); COUNT = !ce || string(ce) == "1";
    string payload = argv[1], out_dir = argv[2];
    auto t0 = chrono::steady_clock::now();
    vector<uint8_t> img = readfile(payload);
    size_t n = (TEND - TEXT) / 4;
    vector<u32> words(n);
    for (size_t i = 0; i < n; i++) words[i] = le32(&img[TEXT - RAM + 4 * i]);

    Asm o;
    o.var("x0", T_I);
    for (int r = 1; r < 32; r++) o.var(x(r), T_I);
    for (int r = 0; r < 32; r++) { o.var("o" + to_string(r), T_I); o.var("b" + to_string(r), T_I); }
    for (const char* v : {"t", "t2", "t3", "u", "idx", "tp", "w", "sh", "budget", "status", "rpc", "ipc", "mode", "priv", "res",
              "now", "th", "tl", "fcsr", "msrc", "mdst", "mn", "romlen", "ga", "ip", "tcause", "tval",
              "iw", "iop", "ird", "irs1", "irs2", "if3", "ia", "ib", "iv", "inext", "iimm",
              "icsr", "islot", "imask", "isrc", "inv", "icnt", "ibytes", "dt", "st", "didx", "sidx", "ii",
              "sext", "sfn", "sa0", "sval", "icount", "gcount", "nint", "nirq", "ntrap", "nsys", "ncsr", "nmemop", "nmoslow"})
        o.var(v, T_I);
    extra_vars(o);
    { const char* p = getenv("L2U_PROF"); PROF = p && string(p) == "1"; }
    { const char* p = getenv("L2U_ISTAT"); ISTAT = p && string(p) == "1"; }
    if (ISTAT) { o.var("st_op", T_IA); o.var("st_pair", T_IA); o.var("st_seg", T_IA); o.var("ipv", T_I); o.var("st_pc", T_IA); o.var("st_pcv", T_IA); }
    { const char* p = getenv("L2U_TIER"); TIER = p ? atoi(p) : 2; }
    { const char* p = getenv("L2U_DYN"); DYN = TIER && (!p || string(p) != "0"); }
    { const char* p = getenv("L2U_NATIVE"); NATIVE = !p || string(p) != "0"; }
    { const char* p = getenv("L2U_NATMIN"); if (p) NATMIN = atoll(p); }
    { const char* p = getenv("L2U_THR"); THR = !p || string(p) != "0"; }
    { const char* p = getenv("L2U_UDREL"); UDREL = !p || string(p) != "0"; }
    { const char* p = getenv("L2U_XFACTS"); XFACTS = !p || string(p) != "0"; }
    { const char* p = getenv("L2U_XCALLS"); XCALLS = p && string(p) == "1"; }
    { const char* p = getenv("L2U_PREPAY"); PREPAY = !p || string(p) != "0"; }
    { const char* p = getenv("L2U_RETP"); if (p) RETP = atoi(p); }
    { const char* p = getenv("L2U_DIRTY"); DIRTY = !p || string(p) != "0"; DIRTYB = p && string(p) == "2"; }
    { const char* p = getenv("L2U_MARKHOT"); if (p) MARKHOT = atoll(p); }
    { const char* p = getenv("L2U_MXF"); if (p) MXF = atoi(p) != 0; }
    { const char* p = getenv("L2U_MCOUNT"); MCOUNT = p ? atoi(p) : 0; }
    { const char* p = getenv("L2U_PAIR"); if (p) PAIR = atoi(p); }
    { const char* p = getenv("L2U_HW"); HWCOPY = !p || string(p) != "0"; }
    { const char* p = getenv("L2U_FLOAT"); FLT = !p || string(p) != "0"; }
    { const char* p = getenv("L2U_CSR"); CSRT = !p || string(p) != "0"; }
    if (const char* p = getenv("L2U_HOT")) if (*p) load_profile(p);
    if (const char* p = getenv("L2U_MIX")) if (*p) mix_load(p);
    if (const char* p = getenv("L2U_MIXW")) { string s = p; size_t a = s.find(':'), b = s.find(':', a + 1);
        MIXW_SEG = s.substr(0, a); MIXW_LO = strtoll(s.substr(a + 1, b - a - 1).c_str(), 0, 16); MIXW_HI = strtoll(s.substr(b + 1).c_str(), 0, 16); }
    if (PROF) { o.var("prof", T_IA); o.var("pt", T_I); o.var("prof2", T_IA); o.var("prof3", T_IA); o.var("prof5", T_IA); o.var("tbase", T_I); }
    for (const char* v : {"ud", "udi", "udo", "bi", "bseg", "bbase", "brel", "bk", "rr"}) o.var(v, T_I);
    if (DYN) dyn_vars(o);
    o.var("useg", T_I, -1);
    for (const char* v : {"cnd", "cnd2"}) o.var(v, T_B);
    o.var("bt", T_BY);
    for (const char* v : {"tgt", "resume", "mret", "resume0"}) o.var(v, T_U);
    for (const char* v : {"l", "l2", "l3", "k_u32", "k_l0"}) o.var(v, T_L);
    for (const char* v : {"fia", "fib"}) o.var(v, T_I);
    o.var("rtc0", T_I, RTC0); o.var("rtc1", T_I, RTC1);
    for (const char* v : {"fa", "fb", "fc", "fv"}) o.var(v, T_F);
    if (FLT) { for (int r = 0; r < 32; r++) o.var(fx(r), T_I); o.var("fdirty", T_I); o.var("ifd", T_I); }
    o.var("k_f0", T_F, 0); o.var("k_f2p31", T_F, 0x4F000000); o.var("k_fm2p31", T_F, sx(0xCF000000LL, 32)); o.var("k_f2p32", T_F, 0x4F800000);
    for (const char* v : {"mem", "rom", "xr", "fr", "csr", "tk", "cslot", "cmask", "hw"}) o.var(v, T_IA);
    if (DIRTY) {
        o.var("pdirty", DIRTYB ? T_BA : T_IA); o.var("plist", T_IA); o.var("k_true", T_B, 1);
        for (const char* v : {"pn", "pd", "pg", "mt", "mlo", "mhi", "mp", "mq"}) o.var(v, T_I);
        if (MCOUNT) { o.var("mcnt", T_I); o.var("scnt", T_I); }
        if (MCOUNT == 2) o.var("profm", T_IA);
        for (const char* v : {"dret", "dret2"}) o.var(v, T_U);
    }
    for (const char* v : {"jt", "ijt", "bjt", "ljt", "sjt", "oijt", "rjt", "ajt", "csrr", "csrw", "usjt", "fjt"}) o.var(v, T_UA);
    for (const char* v : {"bb", "bs", "sglo", "sghi", "sgoff", "sgw"}) o.var(v, T_IA);

    Seg kseg(TEXT, TEND);
    kseg.words = words;
    for (size_t off = TEND - RAM; off + 4 <= img.size(); off += 4) kseg.data.insert(le32(&img[off]));
    vector<Seg*> segs{&kseg}, users;
    vector<uint8_t> rom;
    if (argc > 4) {
        rom = readfile(argv[3]);
        if (memcmp(rom.data(), "-rom1fs-", 8)) { fprintf(stderr, "not a romfs\n"); return 1; }
        map<string, pair<size_t, size_t>> files;
        size_t first; { size_t e = 16; while (rom[e]) e++; first = (e + 16) & ~(size_t)15; }
        romfs_walk(rom, first, "", files);
        for (int i = 4; i < argc; i++) {
            auto it = files.find(argv[i]);
            if (it == files.end()) { fprintf(stderr, "no %s in the ROM\n", argv[i]); return 1; }
            Seg* sg = flat_segment(&rom[it->second.first], it->second.second, i - 4);
            sg->name = argv[i]; segs.push_back(sg); users.push_back(sg);
        }
    }
    for (Seg* v : users) { o.var(v->jt, T_UA); o.var(v->tk, T_IA); if (TIER) o.var(v->tim, T_IA); }
    if (TIER) { o.var("tim", T_IA); o.var("ttim", T_IA); o.var("toff", T_I); }
    o.var("tjt", T_UA); o.var("tlo", T_I); o.var("tst", T_I); o.var("tlen", T_I);
    if (const char* dp = getenv("L2U_DUMP")) {        // sid:hexpc:n prints a segment's words (sid -1 = kernel)
        int sid; unsigned long long pc; int nn; sscanf(dp, "%d:%llx:%d", &sid, &pc, &nn); Seg* sg = segs[sid + 1];
        for (int k = 0; k < nn; k++) { i64 a2 = (i64)pc + 4 * k - sg->lo; if (a2 >= 0 && a2 / 4 < (i64)sg->words.size()) printf("%llx %08x\n", (unsigned long long)(pc + 4 * k), sg->words[a2 / 4]); }
        return 0;
    }

    i64 ptotal = 0;
    for (Seg* sg : segs) { sg->pbase = ptotal; ptotal += (i64)sg->words.size() + 2; }
    Tables T;
    S = &kseg;
    if (DYN) { o.L("dz"); o.jump("dfill"); }           // label 0: a word of the dynamic window not filled yet
    routines(o);
    interpreter(o, T);
    if (DIRTY) dirty_routines(o);
    ubind(o, users);
    if (TIER) thread_common(o);
    if (DYN) dyn_routines(o, T);
    Stats stats; long checks = 0;
    size_t b_rt = o.code.size(), b_inl = 0, b_thr = 0, b_later = 0;
    map<string, size_t> stub_b, stub_n;
    for (Seg* sg : segs) {
        S = sg;
        if (sg->user) seg_routines(o, sg);
        vector<D> ds(sg->words.size());
        for (size_t i = 0; i < ds.size(); i++) ds[i] = decode(sg->words[i]);
        o.leaders = leaders_of(ds, sg->data);
        unordered_set<i64> checked;
        for (size_t i = 0; i < ds.size(); i++) {
            i64 pc = sg->lo + 4 * (i64)i; const D& d = ds[i];
            if (d.k == K_BRANCH || (d.k == K_JAL && d.rd == 0)) { i64 t = (pc + d.imm) & MASK; if (t <= pc && in_text(t)) checked.insert(t); }
        }
        checks += checked.size();
        o.pcpos.assign(ds.size(), 0);
        size_t b0 = o.code.size();
        emit_text(o, ds, checked, stats, sg->tkv);
        b_inl += o.code.size() - b0; b0 = o.code.size();
        sg->lead = o.leaders;
        if (TIER) {
            sg->lead = INL; sg->tlab = thread_words(o, ds, stats); b_thr += o.code.size() - b0;
            for (size_t i = 0; i < ds.size(); i++) sg->tkv[i] = ds[i].k != K_INTERP;
        }
        sg->pcpos = o.pcpos;
        size_t b1 = o.code.size();
        while (!o.later.empty()) {
            auto later = move(o.later); o.later.clear();
            for (auto& f : later) {
                size_t c0 = o.code.size(); f();
                string k = o.last; size_t p = k.find('_'); if (k.size() > 1 && k[0] == 's' && p != string::npos) k = k.substr(p + 1);
                { string m = "?"; for (const char* q : {"ext", "te", "tb", "jr", "h", "i"}) if (k.rfind(q, 0) == 0 && m == "?") m = q; k = m; }
                stub_b[k] += o.code.size() - c0; stub_n[k]++;
            }
        }
        b_later += o.code.size() - b1;
    }
    if (DYN) dyn_tables(o, T);
    auto t1 = chrono::steady_clock::now();
    o.finish();
    if (PROF && DYN) o.heap[o.slots["dbase"]].second = ptotal;
    o.heap[o.slots["resume0"]].second = o.labels.at(TIER ? string("resume_rpc") : fmt("p%llx", (unsigned long long)TEXT));
    for (Seg* sg : segs) {
        u32 pad = o.labels.at(sg->pre + "nonleader");
        size_t nw = sg->words.size();
        sg->jtv.assign(1, pad);
        for (size_t i = 0; i < nw; i++) { i64 pc = sg->lo + 4 * (i64)i; 
            sg->jtv.push_back(sg->lead.count(pc) ? o.labels.at(sg->lab("p", pc)) : TIER ? o.labels.at(sg->tlab[i]) : pad); }
        sg->jtv.push_back(pad);
        vector<int> tk{0}; tk.insert(tk.end(), sg->tkv.begin(), sg->tkv.end()); tk.push_back(0); sg->tkv = tk;
    }
    vector<string> usjt; for (Seg* sg : users) usjt.push_back(sg->pre + "ichk");
    if (usjt.empty()) usjt.push_back("iloop");
    T.lab["usjt"] = usjt;

    filesystem::create_directories(out_dir);
    auto path = [&](const string& nm) { return out_dir + "/" + nm; };
    wvec(path("prog.bc"), o.code);
    vector<uint8_t> hb(4); u32 nh = (u32)o.heap.size(); memcpy(hb.data(), &nh, 4);
    for (auto& h : o.heap) { hb.push_back((uint8_t)h.first); i64 v = h.second; uint8_t b8[8]; memcpy(b8, &v, 8); hb.insert(hb.end(), b8, b8 + 8); }
    wvec(path("prog.heap"), hb);
    string ex; for (auto& e : o.externs) ex += e + "\n";
    wtext(path("prog.externs"), ex);
    string sy;
    for (auto& name : o.slotOrder) if (name[0] != '#') sy += name + " " + to_string(o.slots[name]) + "\n";
    sy += "@entry " + to_string(o.labels.at("entry")) + "\n@resume0 " + to_string(o.labels.at(TIER ? string("resume_rpc") : fmt("p%llx", (unsigned long long)TEXT))) + "\n";
    u32 tsum = 0; for (size_t i = 0; i < n; i += 61) tsum = tsum * 31 + words[i];
    sy += "@textwords " + to_string(n) + "\n@textsum " + to_string((int32_t)tsum) + "\n";
    wtext(path("prog.syms"), sy);
    if (getenv("L2U_LABELS")) {                       // every label by address, for reading a VM exception's pc
        vector<pair<u32, string>> v; for (auto& kv : o.labels) v.push_back({kv.second, kv.first}); sort(v.begin(), v.end());
        string t; for (auto& p : v) t += fmt("%08x ", p.first) + p.second + "\n"; wtext(path("labels.txt"), t);
    }
    wvec(path("jt.u32"), kseg.jtv);
    wvec(path("pcpos.bin"), kseg.pcpos);
    wvec(path("tk.i32"), kseg.tkv);
    for (Seg* sg : users) { wvec(path(sg->jt + ".u32"), sg->jtv); wvec(path(sg->tk + ".i32"), sg->tkv); }
    if (TIER) for (Seg* sg : segs) wvec(path(sg->tim + ".i32"), sg->timv);
    map<string, vector<int32_t>> ints;
    ints["bb"] = vector<int32_t>(16, 0); ints["hw"] = vector<int32_t>(3, 0);
    if (DIRTY) { if (!DIRTYB) ints["pdirty"] = vector<int32_t>(NPAGES, 0); ints["plist"] = vector<int32_t>(NPAGES, 0); } ints["bs"] = vector<int32_t>(16, -1);
    for (Seg* sg : users) { ints["sglo"].push_back((int32_t)sg->lo); ints["sghi"].push_back((int32_t)sg->hi);
        for (auto& ow : sg->sig) { ints["sgoff"].push_back((int32_t)ow.first); ints["sgw"].push_back((int32_t)sx(ow.second, 32)); } }
    for (const char* k : {"sglo", "sghi", "sgoff", "sgw"}) if (ints[k].empty()) ints[k].push_back(0);
    for (auto& kv : ints) wvec(path(kv.first + ".i32"), kv.second);
    for (auto& kv : T.ints) wvec(path(kv.first + ".i32"), kv.second);
    if (ISTAT) { wvec(path("st_op.i32"), vector<int32_t>(128, 0)); wvec(path("st_pair.i32"), vector<int32_t>(128 * 128, 0)); wvec(path("st_seg.i32"), vector<int32_t>(8, 0));
                 wvec(path("st_pc.i32"), vector<int32_t>(65536, 0)); wvec(path("st_pcv.i32"), vector<int32_t>(65536, 0)); }
    for (auto& kv : T.lab) { vector<u32> v; for (auto& l : kv.second) v.push_back(o.labels.at(l)); wvec(path(kv.first + ".u32"), v); }
    if (MCOUNT == 2) {
        string t; for (auto& s : MSITES) t += s + "\n";
        wtext(path("profm_sites.txt"), t); wvec(path("profm.i32"), vector<int32_t>(MSITES.size() + 1, 0));
    }
    if (PROF) {
        wvec(path("prof5.i32"), vector<int32_t>(ptotal + (DYN ? DN + 2 : 0), 0));
        string lay; for (Seg* sg : segs) lay += (sg->name.empty() ? string("kernel") : sg->name) + " " + to_string(sg->pbase) + " " + to_string(sg->words.size()) + "\n";
        wtext(path("prof5.layout"), lay);
        string t; for (auto& p : prof_sites) t += to_string(p.first) + " " + fmt("%llx", (unsigned long long)p.second) + "\n";
        wtext(path("prof_sites.txt"), t); wvec(path("prof.i32"), vector<int32_t>(prof_sites.size() + 1, 0));
        wvec(path("prof2.i32"), vector<int32_t>((TEND - TEXT) / 4 + 1, 0)); wvec(path("prof3.i32"), vector<int32_t>(4 << 20, 0));
    }
    for (Seg* sg : users) printf("%s: %zu words at file offsets %llx to %llx, entry %llx\n", sg->name.c_str(), sg->words.size(),
                                 (unsigned long long)sg->lo, (unsigned long long)sg->hi, (unsigned long long)sg->entry);
    size_t tw = 0; for (Seg* sg : segs) tw += sg->words.size();
    printf("%zu words of text in %ld blocks: %ld translated, %ld to the interpreter; %ld loop checks\n", tw, stats.blocks, stats.translated, stats.interp, checks);
    printf("loads and stores: %ld checked, %ld after a check or by sp/gp/tp, %ld at known addresses; %ld folded to constants, %ld dead writes\n",
           stats.checked_mem, stats.unchecked, stats.const_mem, stats.folded, stats.dead);
    printf("bytes: runtime %zu, inline %zu, routines %zu, stubs %zu\n", b_rt, b_inl, b_thr, b_later);
    if (getenv("L2U_SIZES")) {                       // the stubs and routines by kind: count/bytes
        for (auto& kv : stub_b) printf(" %s %zu/%zu", kv.first.c_str(), stub_n[kv.first], kv.second);
        printf(" (stubs)\n");
        for (int k = 0; k <= K_INTERP; k++) printf(" k%d %ld/%ld", k, kind_n[k], kind_b[k]);
        printf(" (routines)\n");
    }
    if (!nat_found.empty()) { printf("native:"); for (auto& kv : nat_found) printf(" %s %d", kv.first.c_str(), kv.second); printf("\n"); }
    if (TIER) printf("tier %d: %ld blocks threaded, %ld word routines\n", TIER, stats.cold, stats.routines);
    double dt1 = chrono::duration<double>(t1 - t0).count(), dt2 = chrono::duration<double>(chrono::steady_clock::now() - t1).count();
    printf("%zu bytes of Udon code, %zu heap slots, %zu externs; translated in %.1f s, written in %.1f s\n", o.code.size(), o.heap.size(), o.externs.size(), dt1, dt2);
    printf("%.1f bytes and %.3f heap slots a text word\n", (double)o.code.size() / tw, (double)o.heap.size() / tw);
    mix_print();
    return 0;
}
