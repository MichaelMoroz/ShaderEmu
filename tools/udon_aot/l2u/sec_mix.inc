// ---------------------------------------------------------------- L2U_MIX=DIR:TAG, the code's cost weighed by a run
// DIR/prof5.layout and DIR/prof5_TAG.out are a threaded-only PROF build's word counts; a table by class is printed.
struct Ops { i64 push = 0, ext = 0, copy = 0, jump = 0, jif = 0, jind = 0; };
static Ops scan_ops(const vector<uint8_t>& c, size_t a, size_t b) {
    Ops r;
    for (size_t p = a; p + 4 <= b;) {
        u32 op = (u32)c[p] << 24 | (u32)c[p + 1] << 16 | (u32)c[p + 2] << 8 | c[p + 3];
        switch (op) {
        case 1: r.push++; p += 8; break; case 6: r.ext++; p += 8; break; case 9: r.copy++; p += 4; break;
        case 5: r.jump++; p += 8; break; case 4: r.jif++; p += 8; break; case 8: r.jind++; p += 8; break;
        default: p += 4;
        }
    }
    return r;
}
static bool MIX = false;
static FILE* MIXF = nullptr;             // L2U_MIXF=FILE: every run word's segment, pc, runs, modelled ns and class
static map<string, vector<int32_t>> MIXC;
struct MixRow { double n = 0, push = 0, ext = 0, copy = 0, jump = 0, jif = 0, jind = 0; };
static map<string, MixRow> MIXT;
static unordered_map<string, Ops> ROPS;
static vector<char> MIXI;                // words of the segment in inline blocks
static double op_ns(const MixRow& r) { return 12 * r.push + 42 * r.ext + 4 * r.copy + 9 * r.jump + 5 * r.jif + 19 * r.jind; }

static void mix_load(const char* spec) {
    string s = spec; size_t c = s.rfind(':'); string dir = s.substr(0, c), tag = s.substr(c + 1);
    vector<uint8_t> b = readfile(dir + "/prof5_" + tag + ".out");
    FILE* f = fopen((dir + "/prof5.layout").c_str(), "rb");
    char nm[512]; long long base, n;
    while (fscanf(f, "%511s %lld %lld", nm, &base, &n) == 3) {
        vector<int32_t>& v = MIXC[nm]; v.resize(n);
        memcpy(v.data(), b.data() + 4 * (base + 1), 4 * n);
    }
    fclose(f); MIX = true;
    if (const char* mf = getenv("L2U_MIXF")) MIXF = fopen(mf, "w");
}
static double mix_count(i64 pc) {
    auto it = MIXC.find(S->name.empty() ? "kernel" : S->name);
    if (it == MIXC.end()) return 0;
    i64 i = (pc - S->lo) / 4;
    return i >= 0 && i < (i64)it->second.size() ? (double)(uint32_t)it->second[i] : 0;
}
static void mix_add(const string& cls, double n, const Ops& p, i64 pc = -1) {
    if (!MIX || n == 0) return;
    if (MIXF && pc >= 0) {
        MixRow r; r.push = p.push; r.ext = p.ext; r.copy = p.copy; r.jump = p.jump; r.jif = p.jif; r.jind = p.jind;
        fprintf(MIXF, "%s %llx %.0f %.1f %s\n", S->name.empty() ? "kernel" : S->name.c_str(), (unsigned long long)pc, n, op_ns(r), cls.c_str());
    }
    MixRow& r = MIXT[cls]; r.n += n;
    r.push += n * p.push; r.ext += n * p.ext; r.copy += n * p.copy; r.jump += n * p.jump; r.jif += n * p.jif; r.jind += n * p.jind;
}
static string mix_class(const D& d) {
    static const char* ld[] = {"lb", "lh", "lw", "?", "lbu", "lhu"}, *st[] = {"sb", "sh", "sw"};
    static const char* oi[] = {"addi", "slli", "slti", "sltiu", "xori", "sr*i", "ori", "andi"};
    static const char* op[] = {"add/sub", "sll", "slt", "sltu", "xor", "srl/sra", "or", "and"};
    switch (d.k) {
    case K_LUI: return "lui"; case K_AUIPC: return "auipc"; case K_JAL: return d.rd ? "jal(call)" : "j";
    case K_JALR: return (d.rd == 0 && d.rs1 == 1 && d.imm == 0) ? "ret" : d.rd ? "jalr(call)" : "jr";
    case K_BRANCH: return d.f3 >= 6 ? "bltu/bgeu" : "beq..bge";
    case K_LOAD: return ld[d.f3]; case K_STORE: return st[d.f3];
    case K_OPIMM: return (d.f3 == 0 && (d.rs1 == 0 || d.imm == 0)) ? "li/mv" : oi[d.f3];
    case K_OP: return d.f7 == 1 ? (d.f3 == 0 ? "mul" : d.f3 < 4 ? "mulh" : "div/rem") : op[d.f3];
    case K_AMO: return "amo"; case K_FLOAD: case K_FSTORE: return "flw/fsw"; case K_FP: case K_FMA: return "fp";
    case K_CSR: return "csr"; case K_FENCE: return "fence"; default: return "interp";
    }
}
static string MIXW_SEG; static i64 MIXW_LO = 0, MIXW_HI = 0;   // L2U_MIXW=SEG:LO:HI prints those words' code
static void mix_word(i64 pc, const D& d, double n, const Ops& chk, const Ops& p, i64 hdr) {
    string seg = S->name.empty() ? "kernel" : S->name;
    if (MIXF && n > 0) {
        MixRow r; r.push = p.push + chk.push; r.ext = p.ext + chk.ext; r.copy = p.copy + chk.copy; r.jump = p.jump + chk.jump; r.jif = p.jif + chk.jif; r.jind = p.jind + chk.jind;
        fprintf(MIXF, "%s %llx %.0f %.1f %s\n", seg.c_str(), (unsigned long long)pc, n, op_ns(r), ("inl:" + mix_class(d)).c_str());
    }
    if (seg != MIXW_SEG || pc < MIXW_LO || pc >= MIXW_HI) return;
    printf("%8llx %08x %-10s %10.0f ext %lld push %lld copy %lld chk %lld%s\n", (unsigned long long)pc, d.w, mix_class(d).c_str(), n,
           (long long)p.ext, (long long)p.push, (long long)p.copy, (long long)chk.ext, hdr >= 0 ? fmt(" [block, hdr %lld]", (long long)hdr).c_str() : "");
}
static void mix_print() {
    if (!MIX) return;
    vector<pair<double, string>> rows; double tot = 0, totn = 0;
    for (auto& kv : MIXT) { double t = op_ns(kv.second) / 1e6; rows.push_back({t, kv.first}); tot += t; if (kv.first.find("hdr") == string::npos && kv.first.find("check") == string::npos && kv.first.find("disp") == string::npos) totn += kv.second.n; }
    sort(rows.rbegin(), rows.rend());
    printf("mix: %.1f M guest instructions, model %.0f ms (%.0f ns each)\n", totn / 1e6, tot, tot * 1e6 / max(totn, 1.0));
    printf("%-22s %9s %6s %6s %6s %6s %6s %8s %6s\n", "class", "M runs", "ext", "push", "copy", "jmp", "jind", "ms", "%");
    for (auto& r : rows) {
        MixRow& m = MIXT[r.second];
        printf("%-22s %9.2f %6.2f %6.2f %6.2f %6.2f %6.2f %8.1f %6.1f\n", r.second.c_str(), m.n / 1e6, m.ext / m.n, m.push / m.n, m.copy / m.n,
               (m.jump + m.jif) / m.n, m.jind / m.n, r.first, 100 * r.first / tot);
    }
}
