static vector<uint8_t> readfile(const string& p);
#include "sec_mix.inc"
static const u32 PAUSE = 0x0100000F;
static unordered_map<i64, int> NAT;        // the segment's library routines run natively: entry pc -> kind
static void emit_native(Asm& o, int kind, i64 pc);
static void native_scan(const vector<u32>& w);
static bool pure(int f) { return f == K_LUI || f == K_AUIPC || f == K_OPIMM || f == K_OP; }
static const i64 GK = 23;                       // instructions a unit of the host's budget is worth in translated code
static const i64 DMAX = 1000;                   // how far addi may move a pointer checked to be RAM
static const i64 ULIM = MIN + RAMSIZE - 4096;   // a program's pointer is RAM below this (signed)

static bool ends_block(const D& d) {            // control does not go on to the next instruction
    return d.k == K_JAL || d.k == K_JALR || d.k == K_INTERP || (d.k == K_FENCE && d.w == PAUSE);
}

static unordered_set<i64> leaders_of(const vector<D>& ds, const unordered_set<i64>& data) {
    i64 TX = S->lo, TE = S->hi;
    unordered_set<i64> lead{TX};
    for (size_t i = 0; i < ds.size(); i++) {
        const D& d = ds[i]; i64 pc = TX + 4 * (i64)i; K k = d.k;
        if (k == K_BRANCH || k == K_JAL) { i64 t = (pc + d.imm) & MASK; if (in_text(t) && t % 4 == 0) lead.insert(t); }
        if (k == K_JAL || k == K_JALR || k == K_INTERP || (k == K_FENCE && d.w == PAUSE)) lead.insert(pc + 4);
        if (k == K_INTERP) lead.insert(pc);
        if (k == K_JALR && i > 0 && !S->user && (ds[i - 1].k == K_AUIPC || ds[i - 1].k == K_LUI) && ds[i - 1].rd == d.rs1) {
            i64 t = ((ds[i - 1].k == K_AUIPC ? pc - 4 : 0) + ds[i - 1].imm + d.imm) & MASK & ~1LL;   // a far call
            if (in_text(t) && t % 4 == 0) lead.insert(t);
        }
        if (k == K_JALR && d.rd == 0 && d.rs1 != 1)
            for (size_t j = i + 1; j < min(i + 257, ds.size()); j++) {
                lead.insert(TX + 4 * (i64)j);
                if (ds[j].k == K_BRANCH || ds[j].k == K_JAL || ds[j].k == K_JALR) break;
            }
    }
    for (i64 a : data) if (TX <= a && a < TE && a % 4 == 0) lead.insert(a);
    const char* entries = getenv("L2U_ENTRIES");
    if (entries && !S->user) {
        FILE* f = fopen(entries, "rb"); vector<int32_t> seen;
        if (f) { int32_t v; while (fread(&v, 4, 1, f) == 1) seen.push_back(v); fclose(f); }
        for (size_t i = 0; i < seen.size() && i < ds.size(); i++) if (seen[i]) lead.insert(TX + 4 * (i64)i);
    }
    return lead;
}

typedef function<bool(int, i64&)> Get;     // the known value of a register

static bool fold(const D& d, i64 pc, const Get& get, i64& out) {
    K k = d.k; int f3 = d.f3;
    if (k == K_LUI) { out = d.imm & MASK; return true; }
    if (k == K_AUIPC) { if (S->user) return false; out = (pc + d.imm) & MASK; return true; }
    i64 a, b; bool alt;
    if (!get(d.rs1, a)) return false;
    if (k == K_OPIMM) { b = d.imm & MASK; alt = f3 == 5 && ((d.w >> 30) & 1); }
    else {
        if (d.f7 == 1) return false;
        if (!get(d.rs2, b)) return false;
        alt = d.f7 == 0x20;
    }
    i64 sa = sx(a, 32), sb = sx(b, 32); int s = b & 31; i64 r = 0;
    switch (f3) {
    case 0: r = (k == K_OP && alt) ? a - b : a + b; break;
    case 1: r = a << s; break;
    case 2: r = sa < sb; break;
    case 3: r = a < b; break;
    case 4: r = a ^ b; break;
    case 5: r = alt ? (sa >> s) : (a >> s); break;
    case 6: r = a | b; break;
    case 7: r = a & b; break;
    }
    out = r & MASK; return true;
}

static vector<int> reads_of(const D& d) {
    K k = d.k;
    if (k == K_OPIMM || k == K_LOAD || k == K_JALR) return {d.rs1};
    if (k == K_OP || k == K_BRANCH || k == K_STORE || k == K_AMO) return {d.rs1, d.rs2};
    if (k == K_FLOAD || k == K_FSTORE) return {d.rs1};
    if (k == K_CSR) { if (d.f3 & 4) return {}; return {d.rs1}; }
    if (k == K_FP && ((d.w >> 27) == 26 || (d.w >> 27) == 30)) return {d.rs1};
    return {};
}
static int writes_of(const D& d) {
    K k = d.k;
    if (k == K_FP) { int f5 = d.w >> 27; return (f5 == 20 || f5 == 24 || f5 == 28) ? d.rd : 0; }
    if (k == K_CSR) return d.rd;
    return (k == K_LUI || k == K_AUIPC || k == K_JAL || k == K_JALR || k == K_OPIMM || k == K_OP || k == K_LOAD || k == K_AMO) ? d.rd : 0;
}

enum Mode { M_NONE_SET, M_NONE, M_CONST, M_CHECK, M_FREE, M_UD };   // M_UD: the program image, ud + addr
static bool UDREL = true;
static bool HWCOPY = true;                  // L2U_HW=0: halves by shifts and masks of whole words                   // L2U_UDREL=0: no M_UD
enum { F_CONST = 100 };
struct Item {
    D d; i64 pc; int form; uint32_t reads = 0; i64 value = 0;
    string A, B; bool hasKA = false, hasKB = false; i64 KA = 0, KB = 0;
    Mode mode = M_NONE_SET; i64 addr = 0; bool dead = false;
    vector<pair<int, i64>> hoist;       // checks made before this item: register, first offset
};

static bool XFACTS = true, XCALLS = false;   // L2U_XCALLS=1: calls are edges too                  // L2U_XFACTS=0: none
struct MarkPlan { bool on = false, skip = false; i64 lo = 0, hi = 0; };   // a block's stores marked once a group
static MarkPlan MP;
static int MCOUNT = 0;                      // L2U_MCOUNT=1: count inline stores (scnt) and marks run (mcnt); 2: each mark site (profm)
static vector<string> MSITES;
static i64 CURPC = 0;                       // the word being emitted
static i64 MARKHOT = 20000;                 // L2U_MARKHOT=N: stores run fewer times than N mark through markchk
static int store_width(const D& d) { return d.k == K_FSTORE || d.f3 == 2 ? 4 : d.f3 == 1 ? 2 : 1; }
// pages known marked: key (a register, -1 the image, -2 - page a constant page) -> its bytes lo..hi (span < 4096);
// src: the block entry's key the fact comes from unchanged, 0 when marked since
struct MF { i64 lo, hi, src; bool operator==(const MF& b) const { return lo == b.lo && hi == b.hi; } };
typedef map<i64, MF> MFact;
static unordered_map<i64, MFact> MINF;      // what is marked on entry to an inline block: edges that lack it mark it first
static bool MXF = true;                      // L2U_MXF=0: no marks known across blocks
static const MFact* MCUR = nullptr;          // what is marked at the edge being emitted
static i64 mkey(const Item& a, i64& X) {
    if (a.d.k == K_FSTORE) { X = a.d.imm; return a.d.rs1 ? a.d.rs1 : -100; }
    if (a.d.k != K_STORE) return -100;
    if (a.mode == M_FREE || a.mode == M_CHECK) { X = a.d.imm; return a.d.rs1; }
    if (a.mode == M_UD) { X = a.addr; return -1; }
    if (a.mode == M_CONST) { X = a.addr; return -2 - ((a.addr - RAM) >> 12); }
    return -100;
}
static bool mcovers(const MFact& f, const MFact& need) {
    for (auto& kv : need) { auto g = f.find(kv.first); if (g == f.end() || g->second.lo > kv.second.lo || g->second.hi < kv.second.hi) return false; }
    return true;
}
static void mmeet(MFact& a, const MFact& b) {
    for (auto it = a.begin(); it != a.end();) {
        auto f = b.find(it->first);
        if (f != b.end()) { it->second.lo = max(it->second.lo, f->second.lo); it->second.hi = min(it->second.hi, f->second.hi); }
        if (f == b.end() || it->second.lo > it->second.hi) it = a.erase(it); else ++it;
    }
}
struct MOut { vector<pair<i64, MFact>> edges; set<i64> used; };
// a block's marks from what is marked on entry (F, left as at the end): a store not covered marks the span of the
// stores through its base until that is written, and is known marked after
static void mark_plan(const vector<Item>& items, MFact& F, vector<MarkPlan>& plan, MOut* out) {
    plan.assign(items.size(), MarkPlan());
    for (size_t j = 0; j < items.size(); j++) {
        const Item& a = items[j]; i64 X, kj = mkey(a, X);
        if (kj != -100) {
            i64 h0 = X + store_width(a.d) - 1; auto f = F.find(kj);
            if (f != F.end() && f->second.lo <= X && h0 <= f->second.hi) { plan[j].on = plan[j].skip = true; if (out && f->second.src) out->used.insert(f->second.src); }
            else {
                i64 lo = X, hi = h0;
                for (size_t k = j + 1; k < items.size(); k++) {
                    if (kj > 0 && writes_of(items[k - 1].d) == kj) break;
                    i64 Y; if (mkey(items[k], Y) != kj) continue;
                    i64 l2 = min(lo, Y), h2 = max(hi, Y + store_width(items[k].d) - 1);
                    if (h2 - l2 < 4096) { lo = l2; hi = h2; }
                }
                plan[j].on = true; plan[j].lo = lo; plan[j].hi = hi; F[kj] = {lo, hi, 0};
            }
        }
        const D& d = a.d; int rd = writes_of(d);
        if (rd) {
            auto f = F.find(d.rs1);
            if (a.form == K_OPIMM && d.f3 == 0 && d.rs1 && !a.dead && !a.hasKA && f != F.end()) { MF m = f->second; m.lo -= d.imm; m.hi -= d.imm; F[rd] = m; }
            else F.erase(rd);
        }
        if (out && (d.k == K_BRANCH || (d.k == K_JAL && (!d.rd || XCALLS)))) out->edges.push_back({(a.pc + d.imm) & MASK, F});
    }
}
// marks the pages of facts, clamped to mem: a register here need not point into RAM
static void mark_facts(Asm& o, const MFact& f) {
    for (auto& kv : f) for (i64 X : {kv.second.lo, kv.second.hi}) {
        i64 k = kv.first;
        if (X == kv.second.hi && (X == kv.second.lo || k <= -2)) continue;
        if (k <= -2) o.copy(o.c((X - RAM) >> 12), "pg");
        else {
            o.ex(ADD, {k == -1 ? string("udo") : x((int)k), o.c(k == -1 ? X : X + MIN), "mt"});
            o.ex(SHR, {"mt", o.c(12), "pg"}); o.ex(CLAMP, {"pg", "x0", o.c(NPAGES - 1), "pg"});
        }
        string back = o.fresh(); o.copy(o.u(back), "dret"); o.jump("markchk"); o.L(back);
    }
}
struct Stats { long translated = 0, interp = 0, unchecked = 0, checked_mem = 0, const_mem = 0, folded = 0, dead = 0, blocks = 0,
               unreachable = 0, hoisted = 0, jalr_shared = 0, cold = 0, routines = 0; };

// What the translated code knows of registers as pointers into RAM: off[r] = (v, k), v holding x_r + k + 2^31;
// idx[r] = (v, k), v holding that >> 2; neg: x_r >= RAM.
struct MState {
    unordered_map<int, pair<string, i64>> off, idx; uint32_t neg = 0;
    void drop(int r) { off.erase(r); idx.erase(r); neg &= ~(1u << r); }
    void kill_var(const string& v) {
        for (auto it = off.begin(); it != off.end();) if (it->second.first == v) it = off.erase(it); else ++it;
        for (auto it = idx.begin(); it != idx.end();) if (it->second.first == v) it = idx.erase(it); else ++it;
    }
};
static MState* ST = nullptr;

static void t_insn(Asm& o, Item& it);
static void thread_common(Asm& o);
static vector<char> hot_words(const vector<D>& ds);
static void prof_word(Asm& o, const string& at);
static string halt_label(Asm& o, i64 t);
static void mem_check(Asm& o, int r, i64 imm, const string& fail);

static bool isfree(int r) { return r == 2 || r == 3 || r == 4; }

static void track_write(Item& it) {
    D& d = it.d; int rd = d.rd, rs = d.rs1;
    if (it.form == K_OPIMM && d.f3 == 0 && rs && !it.hasKA && !it.dead) {
        auto off = ST->off.find(rs); auto idx = ST->idx.find(rs);
        bool hasoff = off != ST->off.end(), hasidx = idx != ST->idx.end();
        pair<string, i64> o2 = hasoff ? off->second : pair<string, i64>(), i2 = hasidx ? idx->second : pair<string, i64>();
        bool neg = d.imm == 0 && ((ST->neg >> rs & 1) || isfree(rs));
        ST->drop(rd);
        if (hasoff) ST->off[rd] = {o2.first, o2.second - d.imm};
        if (hasidx) ST->idx[rd] = {i2.first, i2.second - d.imm};
        if (neg) ST->neg |= 1u << rd;
        return;
    }
    ST->drop(rd);
}

static unordered_map<i64, uint32_t> INF;   // registers known RAM pointers on every direct edge into an inline block
typedef vector<pair<i64, uint32_t>> Edges;
static uint32_t ram_mask(const unordered_map<int, i64>& ram) { uint32_t m = 0; for (auto& kv : ram) if (kv.first > 0 && !isfree(kv.first)) m |= 1u << kv.first; return m; }
static unordered_map<i64, int> CH;         // what a block's entry charges: its length and its prepaid successors' most
static unordered_set<i64> PRE;              // blocks charged by every direct predecessor: no header behind their q-label
static bool PREPAY = true;                  // L2U_PREPAY=0: a header in every block
static map<string, vector<int32_t>> PROFILE;               // L2U_HOT=DIR: runs of each word by segment, from a PROF build
static unordered_map<i64, vector<i64>> RETS;   // a return's likeliest return sites, by the profile
static int RETP = 0;                            // L2U_RETP=N: compare that many before the table (no gain measured)
static bool has_q(i64 t) { auto f = INF.find(t); auto m = MINF.find(t); return (f != INF.end() && f->second) || PRE.count(t) || (m != MINF.end() && !m->second.empty()); }
static string qlab(i64 t) { return has_q(t) ? S->lab("q", t) : S->lab("p", t); }
static string target(Asm& o, i64 t);
static string tq(Asm& o, i64 t) {
    if (!has_q(t)) return target(o, t);
    auto m = MINF.find(t); string q = S->lab("q", t);
    if (m == MINF.end() || m->second.empty() || (MCUR && mcovers(*MCUR, m->second))) return q;
    MFact miss = m->second;                  // an edge that lacks what the block counts on marks it first
    if (MCUR) for (auto it = miss.begin(); it != miss.end();) { MFact one{*it}; if (mcovers(*MCUR, one)) it = miss.erase(it); else ++it; }
    string lab = o.fresh();
    o.later.push_back([&o, lab, miss, q]() { o.L(lab); mark_facts(o, miss); o.jump(q); });
    return lab;
}

static void scan_block(Asm& o, const vector<D>& ds, int i0, int iend, int i1, vector<Item>& items, Stats& stats, Edges* edges) {
    unordered_map<int, i64> known; unordered_map<int, i64> ram;   // ram: checked, moved by at most that since
    { auto f = INF.find(S->lo + 4 * (i64)i0); if (f != INF.end()) for (int r = 1; r < 32; r++) if (f->second >> r & 1) ram[r] = 0; }
    unordered_map<int, i64> rel;                                  // x_r = ud + rel[r], from auipc
    Get get = [&](int r, i64& v) { if (r == 0) { v = 0; return true; } auto it = known.find(r); if (it == known.end()) return false; v = it->second; return true; };
    bool seg_start = true;
    for (int i = i0; i < iend; i++) {
        Item it; it.d = ds[i]; it.pc = S->lo + 4 * (i64)i; it.form = it.d.k;
        if (seg_start) {               // checks of registers the segment uses as they come in, made at its start
            seg_start = false;
            uint32_t written = 0, seen = 0;
            for (int j = i; j < iend; j++) {
                const D& e = ds[j];
                if ((e.k == K_LOAD && e.rd) || e.k == K_STORE) {
                    int r = e.rs1; i64 v;
                    if (!(written >> r & 1) && !(seen >> r & 1) && r && !isfree(r) && !ram.count(r) && !rel.count(r) && !get(r, v)) {
                        it.hoist.push_back({r, e.imm}); seen |= 1u << r;
                    }
                }
                int w = writes_of(e); if (w) written |= 1u << w;
                if (e.k == K_BRANCH) break;
            }
            for (auto& h : it.hoist) ram[h.first] = 0;
            stats.hoisted += it.hoist.size();
        }
        D& d = it.d; K k = d.k; int rd = writes_of(ds[i]);
        i64 v;
        for (int r : reads_of(d)) if (r && !get(r, v)) it.reads |= 1u << r;
        it.hasKA = get(d.rs1, it.KA); it.hasKB = get(d.rs2, it.KB);
        it.A = it.hasKA ? o.c(it.KA) : x(d.rs1);
        it.B = it.hasKB ? o.c(it.KB) : x(d.rs2);
        if (k == K_AMO || k >= K_FLOAD) { it.reads = 0; for (int r : reads_of(d)) if (r) it.reads |= 1u << r; }
        if (pure(k) && fold(d, it.pc, get, v)) { it.form = F_CONST; it.value = v; it.reads = 0; stats.folded++; }
        if (k == K_LOAD || k == K_STORE) {
            i64 base; bool hb = get(d.rs1, base);
            i64 a = hb ? (base + d.imm) & MASK : 0;
            auto rl = rel.find(d.rs1); i64 ua = rl != rel.end() ? rl->second + d.imm : -1;
            int width = d.f3 == 2 ? 4 : (d.f3 & 3) == 1 ? 2 : 1;
            if (k == K_LOAD && !rd) it.mode = M_NONE;
            else if (!hb && UDREL && ua >= 0x40 && ua + 4 <= S->bss && ua % width == 0) { it.mode = M_UD; it.addr = ua; it.reads &= ~(1u << d.rs1); stats.const_mem++; }
            else if (hb && RAM <= a && a < RAM + RAMSIZE - 4) { it.mode = M_CONST; it.addr = a; it.reads &= ~(1u << d.rs1); stats.const_mem++; }
            else {
                int r = d.rs1;
                bool free_ = !hb && (ram.count(r) || isfree(r));
                it.mode = free_ ? M_FREE : M_CHECK;
                if (free_) stats.unchecked++; else stats.checked_mem++;
                if (!hb && !ram.count(r)) ram[r] = 0;
            }
        }
        if (rd) {
            if (it.form == F_CONST) known[rd] = it.value; else known.erase(rd);
            int src = d.rs1;
            auto rs = ram.find(src);
            i64 dr = isfree(src) ? 0 : rs != ram.end() ? rs->second : -1;
            if (it.form == K_OPIMM && d.f3 == 0 && src && dr >= 0 && dr + llabs(d.imm) <= DMAX) ram[rd] = dr + llabs(d.imm);
            else ram.erase(rd);
        }
        if (rd) {
            auto rl = rel.find(d.rs1); bool from = rl != rel.end() && it.form != F_CONST;
            if (k == K_AUIPC && S->user) rel[rd] = it.pc + d.imm;
            else if (k == K_OPIMM && d.f3 == 0 && from) rel[rd] = rl->second + d.imm;
            else rel.erase(rd);
        }
        if (k == K_BRANCH) seg_start = true;
        if (edges && (k == K_BRANCH || (k == K_JAL && (!d.rd || XCALLS)))) edges->push_back({(it.pc + d.imm) & MASK, ram_mask(ram)});
        items.push_back(it);
    }
    if (edges && iend == i1 && !ends_block(ds[iend - 1])) edges->push_back({S->lo + 4 * (i64)i1, ram_mask(ram)});
}

static void emit_block(Asm& o, const vector<D>& ds, int i0, int i1, const unordered_set<i64>& checked, Stats& stats, vector<int>& tk,
                       bool next_follows = true) {
    i64 pc0 = S->lo + 4 * (i64)i0;
    if (has_q(pc0)) {                          // a dynamic entry pays and checks what direct edges bring
        uint32_t m = INF.count(pc0) ? INF[pc0] : 0; Seg* seg = S; string pl = S->lab("p", pc0), ql = qlab(pc0);
        int pay = PRE.count(pc0) ? CH[pc0] : 0;
        MFact mf = MINF.count(pc0) ? MINF[pc0] : MFact();
        o.later.push_back([&o, m, seg, pl, ql, pc0, pay, mf]() {
            Seg* s0 = S; S = seg; MState st; MState* st0 = ST; ST = &st;
            o.L(pl); if (pay) o.ex(SUB, {"gleft", o.c(pay), "gleft"});
            string fail = m ? thr_at(o, pc0) : "";
            for (int r = 1; r < 32; r++) if (m >> r & 1) mem_check(o, r, 0, fail);
            mark_facts(o, mf);
            o.jump(ql); ST = st0; S = s0;
        });
    }
    o.L(qlab(pc0));
    if (PROF && TIER) prof_word(o, o.c(S->pbase + i0 + 1));
    { auto nk = NAT.find(pc0); if (nk != NAT.end()) emit_native(o, nk->second, pc0); }
    o.pcpos[i0] = (u32)o.code.size();
    if (ds[i0].k == K_INTERP) {
        stats.interp += i1 - i0;
        pcv(o, S, pc0, "ipc"); o.jump("to_interp");
        return;
    }
    tk[i0] = 1;
    int iend = i0;                       // the block runs to its first unconditional transfer
    while (iend < i1 && !ends_block(ds[iend])) iend++;
    if (iend < i1) iend++;
    stats.unreachable += i1 - iend;
    vector<Item> items; items.reserve(iend - i0);
    scan_block(o, ds, i0, iend, i1, items, stats, nullptr);
    uint32_t live = 0xFFFFFFFEu;
    for (int j = (int)items.size() - 1; j >= 0; j--) {
        Item& it = items[j];
        int rd = writes_of(it.d);
        if (it.d.k == K_BRANCH) live = 0xFFFFFFFEu;          // the taken side hands over every register
        if ((pure(it.form) || it.form == F_CONST) && rd && !(live >> rd & 1)) { it.dead = true; stats.dead++; continue; }
        if (rd) live &= ~(1u << rd);
        live |= it.reads;
        if (it.mode == M_CHECK || it.d.k == K_FLOAD || it.d.k == K_FSTORE || it.d.k == K_CSR) live = 0xFFFFFFFEu;
    }
    size_t h0 = o.code.size();
    if (!PRE.count(pc0)) o.ex(SUB, {"gleft", o.c(CH.count(pc0) ? CH[pc0] : iend - i0), "gleft"});
    if (checked.count(pc0)) { o.ex(GT, {"gleft", "x0", "cnd"}); o.jif("cnd", halt_label(o, pc0)); }
    if (MIX) mix_add("inl:hdr", mix_count(pc0), scan_ops(o.code, h0, o.code.size()), pc0);
    MState st; ST = &st;
    vector<MarkPlan> plan(items.size());
    MFact F; MOut mo;
    if (DIRTY) { auto mi = MINF.find(pc0); if (mi != MINF.end()) F = mi->second; mark_plan(items, F, plan, &mo); }
    size_t ei = 0;
    size_t ji = 0;
    auto pf = PROFILE.find(S->name.empty() ? "kernel" : S->name);
    for (auto& it : items) {
        MP = plan[ji++];
        { i64 w = (it.pc - S->lo) / 4; MFAST = pf == PROFILE.end() || w >= (i64)pf->second.size() || (u32)pf->second[w] >= (u32)MARKHOT;
          CURPC = it.pc; }
        int ix = (int)((it.pc - S->lo) / 4);
        if (it.pc != pc0) o.pcpos[ix] = max(o.pcpos[ix], (u32)o.code.size());
        stats.translated++;
        size_t q0 = o.code.size();
        if (!it.hoist.empty()) {
            string fail = thr_at(o, it.pc);
            for (auto& h : it.hoist) mem_check(o, h.first, h.second, fail);
        }
        size_t q1 = o.code.size();
        bool edge = DIRTY && (it.d.k == K_BRANCH || (it.d.k == K_JAL && (!it.d.rd || XCALLS)));
        if (edge) MCUR = &mo.edges[ei++].second;
        if (!it.dead) {
            if (it.form == F_CONST) { if (it.d.rd) o.copy(o.c(it.value), x(it.d.rd)); }
            else t_insn(o, it);
        }
        MCUR = nullptr;
        if (MIX) {
            double n = mix_count(it.pc);
            mix_add("inl:check", n, scan_ops(o.code, q0, q1));
            Ops p = scan_ops(o.code, q1, o.code.size());
            if (it.d.k == K_JALR && !(it.hasKA && !S->user)) { p.ext += S->user ? 6 : 5; p.push += S->user ? 20 : 17; p.jind++; }
            mix_add(string("inl:") + mix_class(it.d), n, p);
            mix_word(it.pc, it.d, n, scan_ops(o.code, q0, q1), p, it.pc == pc0 ? scan_ops(o.code, h0, q0).ext : -1);
        }
        if (writes_of(it.d)) track_write(it);
    }
    MP = MarkPlan(); MFAST = true;
    ST = nullptr;
    if (iend == i1 && !ends_block(ds[iend - 1])) {
        i64 nx = S->lo + 4 * (i64)i1; MCUR = &F;
        if (!next_follows) o.jump(tq(o, nx));
        else { auto m = MINF.find(nx); if (m != MINF.end() && !mcovers(F, m->second)) o.jump(tq(o, nx)); }
        MCUR = nullptr;
    }
}

static void emit_text(Asm& o, const vector<D>& ds, const unordered_set<i64>& checked, Stats& stats, vector<int>& tk) {
    set<int> st;
    for (i64 pc : o.leaders) { i64 i = (pc - S->lo) / 4; if ((pc - S->lo) >= 0 && i < (i64)ds.size()) st.insert((int)i); }
    vector<int> starts(st.begin(), st.end());
    tk.assign(ds.size(), 0);
    INL.clear();
    native_scan(S->words);
    if (MIX) MIXI.assign(ds.size(), 0);
    if (TIER) {
        vector<char> hot = TIER == 2 ? hot_words(ds) : vector<char>(ds.size(), 1);
        for (size_t bi = 0; bi < starts.size(); bi++) {
            int i = starts[bi], i1 = bi + 1 < starts.size() ? starts[bi + 1] : (int)ds.size();
            bool h = false;
            for (int j = i; j < i1 && !h; j++) h = hot[j];
            if (NAT.count(S->lo + 4 * (i64)i)) h = true;
            if (ds[i].k != K_INTERP && h) { INL.insert(S->lo + 4 * (i64)i); if (MIX) for (int j = i; j < i1; j++) MIXI[j] = 1; }
        }
    }
    auto inl = [&](size_t bi) { return !TIER || INL.count(S->lo + 4 * (i64)starts[bi]) > 0; };
    RETS.clear();
    if (RETP > 0) {
        auto pf = PROFILE.find(S->name.empty() ? "kernel" : S->name);
        map<i64, vector<i64>> callers;
        for (size_t i = 0; i < ds.size(); i++) if (ds[i].k == K_JAL && ds[i].rd == 1) callers[(S->lo + 4 * (i64)i + ds[i].imm) & MASK].push_back(S->lo + 4 * (i64)i + 4);
        for (size_t i = 0; i < ds.size(); i++) {
            const D& d = ds[i];
            if (!(d.k == K_JALR && d.rd == 0 && d.rs1 == 1 && d.imm == 0)) continue;
            i64 pc = S->lo + 4 * (i64)i; auto f = callers.upper_bound(pc);
            if (f == callers.begin()) continue;
            --f; vector<pair<i64, i64>> c;
            for (i64 t : f->second) {
                i64 j = (t - S->lo) / 4;
                if ((!TIER || INL.count(t)) && pf != PROFILE.end() && j < (i64)pf->second.size() && pf->second[j] > 0) c.push_back({-(i64)pf->second[j], t});
            }
            sort(c.begin(), c.end());
            for (int k = 0; k < (int)c.size() && k < RETP; k++) RETS[pc].push_back(c[k].second);
        }
    }
    INF.clear(); CH.clear(); PRE.clear(); MINF.clear();
    if (XFACTS || PREPAY) {                // must-dataflow of RAM pointers over direct edges, by a worklist from a start down
        unordered_map<i64, int> at; Stats dummy;
        for (size_t bi = 0; bi < starts.size(); bi++) if (inl(bi) && ds[starts[bi]].k != K_INTERP) at[S->lo + 4 * (i64)starts[bi]] = (int)bi;
        auto bounds = [&](i64 pc, int& i0, int& iend, int& i1) {
            int bi = at[pc]; i0 = starts[bi]; i1 = bi + 1 < (int)starts.size() ? starts[bi + 1] : (int)ds.size();
            iend = i0; while (iend < i1 && !ends_block(ds[iend])) iend++; if (iend < i1) iend++;
        };
        unordered_map<i64, uint32_t> use, def; unordered_map<i64, vector<i64>> succ;   // the edges' shape, seeds apart
        for (auto& kv : at) {
            int i0, iend, i1; bounds(kv.first, i0, iend, i1);
            uint32_t u = 0, w = 0;
            for (int i = i0; i < iend; i++) {
                const D& e = ds[i];
                if ((e.k == K_LOAD || e.k == K_STORE) && e.rs1 && !(w >> e.rs1 & 1)) u |= 1u << e.rs1;
                int wr = writes_of(e); if (wr) w |= 1u << wr;
                if (e.k == K_BRANCH || (e.k == K_JAL && (!e.rd || XCALLS))) succ[kv.first].push_back((S->lo + 4 * (i64)i + e.imm) & MASK);
            }
            if (iend == i1 && !ends_block(ds[iend - 1])) succ[kv.first].push_back(S->lo + 4 * (i64)i1);
            use[kv.first] = u; def[kv.first] = w;
        }
        auto solve = [&](const unordered_map<i64, uint32_t>& start) {
            INF = start;
            unordered_map<i64, unordered_map<i64, uint32_t>> contrib;
            deque<i64> wl; unordered_set<i64> inq;
            for (auto& kv : at) { wl.push_back(kv.first); inq.insert(kv.first); }
            while (!wl.empty()) {
                i64 b = wl.front(); wl.pop_front(); inq.erase(b);
                int i0, iend, i1; bounds(b, i0, iend, i1);
                vector<Item> items; Edges ed; scan_block(o, ds, i0, iend, i1, items, dummy, &ed);
                unordered_map<i64, uint32_t> m;
                for (auto& e : ed) if (INF.count(e.first)) { auto f = m.find(e.first); if (f == m.end()) m[e.first] = e.second; else f->second &= e.second; }
                for (auto& tm : m) {
                    contrib[tm.first][b] = tm.second;
                    uint32_t n = start.at(tm.first); for (auto& c : contrib[tm.first]) n &= c.second;
                    if (n != INF[tm.first]) { INF[tm.first] = n; if (!inq.count(tm.first)) { inq.insert(tm.first); wl.push_back(tm.first); } }
                }
            }
        };
        unordered_set<i64> rets;            // return sites: entered by returns, through the table
        for (size_t i = 0; i < ds.size(); i++) if ((ds[i].k == K_JAL || ds[i].k == K_JALR) && ds[i].rd) rets.insert(S->lo + 4 * (i64)i + 4);
        unordered_map<i64, uint32_t> start;
        for (auto& kv : succ) for (i64 t : kv.second) if (at.count(t) && !NAT.count(t) && !rets.count(t)) start[t] = 0xFFFFFFFEu & ~(7u << 2);
        if (PREPAY) {                      // edges into blocks that are no loop heads go forward: charge the longest way on
            for (auto& kv : start) if (!checked.count(kv.first)) PRE.insert(kv.first);
            vector<i64> order; for (auto& kv : at) order.push_back(kv.first);
            sort(order.rbegin(), order.rend());
            for (i64 b : order) {
                int i0, iend, i1; bounds(b, i0, iend, i1); int m = 0;
                for (i64 t : succ[b]) if (PRE.count(t)) { if (t <= b) { m = -1; break; } m = max(m, CH[t]); }
                if (m < 0) { PRE.clear(); CH.clear(); break; }
                CH[b] = iend - i0 + m;
            }
        }
        if (XFACTS) solve(start);
        unordered_map<i64, uint32_t> need;  // registers a block reached by direct edges uses as a base before writing them
        for (bool ch = true; ch;) {
            ch = false;
            for (auto& kv : at) {
                uint32_t n = use[kv.first];
                for (i64 t : succ[kv.first]) { auto f = need.find(t); if (f != need.end()) n |= f->second & ~def[kv.first]; }
                uint32_t& cur = need[kv.first]; if ((cur | n) != cur) { cur |= n; ch = true; }
            }
        }
        for (auto& kv : start) kv.second = INF[kv.first] & need[kv.first];
        if (XFACTS) solve(start);
        if (DIRTY && MXF) {                // what is marked on entry: the meet of direct edges, a loop head's of its back edges
            vector<i64> order; for (auto& kv : at) order.push_back(kv.first);
            sort(order.begin(), order.end());
            unordered_map<i64, map<i64, MFact>> ein; unordered_map<i64, MOut> outs;
            for (int round = 0; round < 8; round++) {
                bool ch = false;
                for (i64 b : order) {
                    if (start.count(b)) {
                        MFact m; bool first = true;
                        for (auto& pe : ein[b]) {
                            if (checked.count(b) && pe.first < b) continue;
                            if (first) { m = pe.second; first = false; } else mmeet(m, pe.second);
                        }
                        for (auto& kv : m) kv.second.src = kv.first;
                        if (!(m == MINF[b])) { MINF[b] = m; ch = true; }
                    }
                    int i0, iend, i1; bounds(b, i0, iend, i1);
                    vector<Item> items; scan_block(o, ds, i0, iend, i1, items, dummy, nullptr);
                    MFact F = MINF[b]; vector<MarkPlan> plan; MOut out; mark_plan(items, F, plan, &out);
                    if (iend == i1 && !ends_block(ds[iend - 1])) out.edges.push_back({S->lo + 4 * (i64)i1, F});
                    map<i64, MFact> mine;
                    for (auto& e : out.edges) { if (!start.count(e.first)) continue; auto f = mine.find(e.first); if (f == mine.end()) mine[e.first] = e.second; else mmeet(f->second, e.second); }
                    for (auto& kv : mine) ein[kv.first][b] = kv.second;
                    outs[b] = move(out);
                }
                if (!ch) break;
            }
            unordered_map<i64, set<i64>> useful;   // facts kept: those covering a store here or one a successor counts on
            for (i64 b : order) useful[b] = outs[b].used;
            for (bool ch = true; ch;) {
                ch = false;
                for (i64 b : order) for (auto& e : outs[b].edges) {
                    auto u = useful.find(e.first); if (u == useful.end()) continue;
                    for (auto& kv : e.second) if (kv.second.src && u->second.count(kv.first) && !useful[b].count(kv.second.src)) { useful[b].insert(kv.second.src); ch = true; }
                }
            }
            for (auto it = MINF.begin(); it != MINF.end();) {
                auto& u = useful[it->first];
                for (auto f = it->second.begin(); f != it->second.end();) if (u.count(f->first)) ++f; else f = it->second.erase(f);
                if (it->second.empty()) it = MINF.erase(it); else ++it;
            }
        }
    }
    for (size_t bi = 0; bi < starts.size(); bi++) {
        int i1 = bi + 1 < starts.size() ? starts[bi + 1] : (int)ds.size();
        if (!inl(bi)) { stats.cold++; continue; }
        stats.blocks++;
        emit_block(o, ds, starts[bi], i1, checked, stats, tk, bi + 1 == starts.size() || inl(bi + 1));
    }
    o.L(S->pre + "text_end");
    pcv(o, S, S->hi, "ipc"); o.jump("to_interp");
}

static void jt_index(Asm& o, const string& base, i64 imm, const Seg* seg = nullptr) {
    if (!seg) seg = S;
    if (seg->user) { o.ex(SUB, {base, "ud", "u"}); o.ex(ADD, {"u", o.c(imm - seg->lo + 4), "u"}); }
    else o.ex(ADD, {base, o.c(imm - seg->lo + 4), "u"});
    o.ex(SHR, {"u", o.c(2), "tp"});
    o.ex(CLAMP, {"tp", "x0", o.c((seg->hi - seg->lo) / 4 + 1), "tp"});
}

static void t_branch(Asm& o, Item& it, const string& tgt) {
    D& d = it.d; int f3 = d.f3;
    if (it.hasKA && it.hasKB) {
        i64 a = it.KA, b = it.KB;
        if (f3 < 6) { a = sx(a, 32); b = sx(b, 32); }
        bool tk = f3 == 0 ? a == b : f3 == 1 ? a != b : (f3 == 4 || f3 == 6) ? a < b : a >= b;
        if (tk) o.jump(tgt);
        return;
    }
    string a = it.A, b = it.B;
    bool negf = true;
    for (int r : {d.rs1, d.rs2}) if (!((ST->neg >> r & 1) || isfree(r))) negf = false;
    if ((f3 == 6 || f3 == 7) && negf) f3 -= 2;
    if (f3 == 6 || f3 == 7) {
        if (it.hasKB && it.KB == 0) { if (f3 == 7) o.jump(tgt); return; }
        if (it.hasKA && it.KA == 0) { o.ex(f3 == 6 ? EQ : NE, {b, "x0", "cnd"}); o.jif("cnd", tgt); return; }
        if (it.hasKA) a = o.c(it.KA ^ MIN); else { o.ex(XOR, {a, o.c(MIN), "t"}); a = "t"; }
        if (it.hasKB) b = o.c(it.KB ^ MIN); else { o.ex(XOR, {b, o.c(MIN), "t2"}); b = "t2"; }
    }
    const string& inv = f3 == 0 ? NE : f3 == 1 ? EQ : f3 == 4 ? GE : f3 == 5 ? LT : f3 == 6 ? GE : LT;
    o.ex(inv, {a, b, "cnd"}); o.jif("cnd", tgt);
}

static string halt_label(Asm& o, i64 t) {
    string lab = S->lab("h", t);
    if (!o.hstubs.count(lab)) {
        o.hstubs.insert(lab);
        Seg* seg = S; bool inl = !TIER || INL.count(t);
        o.later.push_back([&o, lab, t, seg, inl]() {
            o.L(lab);
            pcv(o, seg, t, "rpc");
            if (TIER) o.jump("thalt_rpc"); else { o.copy(o.u(seg->lab("p", t)), "resume"); o.jump("exit"); }
        });
    }
    return lab;
}

static void mem_check(Asm& o, int r, i64 imm, const string& fail) {
    o.ex(LT, {x(r), S->user ? o.c(ULIM - max<i64>(imm, 0)) : "x0", "cnd"}); o.jif("cnd", fail);
    ST->neg |= 1u << r;
}

static string mem_off(Asm& o, int r, i64 imm) {       // a variable holding x_r + imm + 2^31
    auto f = ST->off.find(r);
    if (f != ST->off.end()) {
        string v = f->second.first; i64 k = f->second.second;
        if (imm == k) return v;
        o.ex(ADD, {v, o.c(imm - k), "t"}); return "t";
    }
    string v = "o" + to_string(r);
    ST->kill_var(v); o.ex(ADD, {x(r), o.c(imm + MIN), v}); ST->off[r] = {v, imm};
    return v;
}

static string mem_idx(Asm& o, int r, i64 imm) {       // a variable holding (x_r + imm + 2^31) >> 2
    auto f = ST->idx.find(r);
    if (f != ST->idx.end() && ((imm - f->second.second) & 3) == 0) {
        string v = f->second.first; i64 k = f->second.second;
        if (imm == k) return v;
        o.ex(ADD, {v, o.c((imm - k) >> 2), "idx"}); return "idx";
    }
    string src = mem_off(o, r, imm);
    string v = "b" + to_string(r);
    ST->kill_var(v); o.ex(SHR, {src, o.c(2), v}); ST->idx[r] = {v, imm};
    return v;
}

// marks the pages the store (or its group, MP) writes; a: its word index (word) or mem byte offset, for offset ref
static void mark_store(Asm& o, const D& d, i64 ref, const string& a, bool word) {
    if (DIRTY && MCOUNT && MP.on) o.ex(ADD, {"scnt", o.c(1), "scnt"});
    if (!DIRTY || (MP.on && MP.skip)) return;
    i64 lo = MP.on ? MP.lo : ref, hi = MP.on ? MP.hi : ref + store_width(d) - 1;
    bool one = word ? ((lo - ref) >> 2) == ((hi - ref) >> 2) : hi - lo < store_width(d), pair = !one && !DIRTYB && (PAIR == 1 || (PAIR == 2 && d.rs1 == 2));
    auto at = [&](i64 X) {
        i64 dl = word ? (X - ref) >> 2 : X - ref;
        string v = a; if (dl) { o.ex(ADD, {a, o.c(dl), "mt"}); v = "mt"; }
        if (MCOUNT) o.ex(ADD, {"mcnt", o.c(1), "mcnt"});
        if (MCOUNT == 2) {
            i64 id = (i64)MSITES.size();
            MSITES.push_back(fmt("%s %llx r%d %lld %lld %d %d", S->name.empty() ? "kernel" : S->name.c_str(), (unsigned long long)CURPC, d.rs1, (long long)lo, (long long)hi, (int)pair, (int)(X == hi)));
            o.ex(AGET, {"profm", o.c(id), "scnt"}); o.ex(ADD, {"scnt", o.c(1), "scnt"}); o.ex(ASET, {"profm", o.c(id), "scnt"});
        }
        if (word) mark_word(o, v, true, pair); else mark_byte(o, v, true, pair);
    };
    at(lo);
    if (!one && !pair) at(hi);
}

static void t_mem(Asm& o, Item& it) {
    D& d = it.d; Mode mode = it.mode;
    K k = d.k; int f3 = d.f3, rd = d.rd;
    if (mode == M_NONE) return;
    string t, idx;
    if ((f3 == 1 || f3 == 5) && HWCOPY && !it.hasKA) {   // halves through 2-byte copies to and from hw (bytes 2-3 of hw[0] stay 0)
        if (mode == M_CONST) t = o.c(it.addr - RAM);
        else if (mode == M_UD) { o.ex(ADD, {"udo", o.c(it.addr), "t"}); t = "t"; }
        else { if (mode == M_CHECK) mem_check(o, d.rs1, d.imm, thr_at(o, it.pc)); t = mem_off(o, d.rs1, d.imm); }
        if (k == K_STORE) {
            mark_store(o, d, mode == M_UD || mode == M_CONST ? it.addr : d.imm, t, false);
            o.ex(ASET, {"hw", o.c(1), it.B}); o.ex(BLOCKCOPY, {"hw", o.c(4), "mem", t, o.c(2)}); return;
        }
        string r = x(rd);
        if (f3 == 5) { o.ex(BLOCKCOPY, {"mem", t, "hw", o.c(0), o.c(2)}); o.ex(AGET, {"hw", o.c(0), r}); }
        else { o.ex(BLOCKCOPY, {"mem", t, "hw", o.c(10), o.c(2)}); o.ex(AGET, {"hw", o.c(2), r}); o.ex(SHR, {r, o.c(16), r}); }
        return;
    }
    if (mode == M_CONST) {
        i64 off = it.addr - RAM;
        t = o.c(off); idx = o.c(off >> 2);
        if (f3 == 1 || f3 == 5 || (k == K_STORE && f3 == 1)) o.copy(t, "t");
    } else if (mode == M_UD) {
        if (f3 == 0 || f3 == 4) { o.ex(ADD, {"udo", o.c(it.addr), "t"}); t = "t"; }
        else { o.ex(ADD, {"udi", o.c(it.addr >> 2), "idx"}); idx = "idx"; if (f3 != 2) { o.copy(o.c(it.addr), "t"); t = "t"; } }
    } else if (it.hasKA) {                         // a known base outside RAM: the interpreter's
        o.jump(to_interp_at(o, it.pc));
        return;
    } else {
        int r = d.rs1; i64 imm = d.imm;
        if (mode == M_CHECK) mem_check(o, r, imm, thr_at(o, it.pc));
        if (f3 == 2) idx = mem_idx(o, r, imm);
        else {
            t = mem_off(o, r, imm); idx = "idx";
            if (f3 == 1 || f3 == 5) {
                if (t != "t") { o.copy(t, "t"); t = "t"; }
                o.ex(SHR, {"t", o.c(2), "idx"});
            }
        }
    }
    if (k == K_LOAD) {
        string r = x(rd);
        if (f3 == 2) o.ex(AGET, {"mem", idx, r});
        else if (f3 == 0 || f3 == 4) {
            o.ex(GETB, {"mem", t, "bt"}); o.ex(BTOI, {"bt", r});
            if (f3 == 0) { o.ex(SHL, {r, o.c(24), r}); o.ex(SHR, {r, o.c(24), r}); }
        } else { o.ex(AGET, {"mem", idx, "w"}); emit_extract(o, f3, r); }
        return;
    }
    string v = it.B;
    mark_store(o, d, mode == M_UD || mode == M_CONST ? it.addr : d.imm, f3 == 2 ? idx : t, f3 == 2);
    if (f3 == 2) o.ex(ASET, {"mem", idx, v});
    else if (f3 == 0) { o.ex(AND, {v, o.c(255), "t2"}); o.ex(TOB, {"t2", "bt"}); o.ex(SETB, {"mem", t, "bt"}); }
    else { if (idx != "idx") o.copy(idx, "idx"); o.ex(AGET, {"mem", "idx", "w"}); emit_insert(o, 16, v); }
}

