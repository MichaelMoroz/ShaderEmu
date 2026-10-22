// memsim: what a real program's bundles ask of the tick's memory, on the reference machine (gpu_vm/tick/steps/README.md,
// "x19"). Runs a case of the bench (cases.h: coremark, gzip, gunzip...) and, bundle by bundle before the reference makes
// it, models the tick's write cache (two ways of HALF sets and two overflow slots: where a pass ends full) and other
// organisations, a cache of RAM texels in registers, and the bundles' paths. In WSL or with MSVC:
//   g++ -O2 -std=c++17 -o build/tick_real/memsim gpu_vm/tick/real/memsim.cpp && build/tick_real/memsim coremark [bundles]
#define GVM_REF_IMPL
#include "../cases.h"
#include <cstdio>
#include <map>
#include <algorithm>

static uint64_t g_bundles = 0, g_limit = 20000000;
static gvm_machine g_m;

// ---- write cache models: a pass ends when a store's word finds no entry
struct Wc {
    const char* name;
    uint32_t sets, ways, over;   // sets of `ways` entries, then `over` overflow slots
    int hash;                    // 0: x18's (w ^ w >> 6), 1: w alone, 2: skewed ways
    std::vector<uint32_t> tag;
    uint32_t used_over = 0, n = 0;
    uint64_t passes = 0, bundles_in = 0, full_passes = 0, fill_sum = 0;
    void reset() { tag.assign(sets * ways, 0xffffffffu); used_over = 0; n = 0; }
    uint32_t idx(uint32_t w, uint32_t way) const {
        if (hash == 1) return w & (sets - 1);
        if (hash >= 3) return (w ^ (w >> hash)) & (sets - 1);
        if (hash == 2) return way == 0 ? ((w ^ (w >> 6)) & (sets - 1)) : ((w ^ (w >> 8) ^ (w >> 13)) & (sets - 1));
        return (w ^ (w >> 6)) & (sets - 1);
    }
    std::vector<uint32_t> overw;
    bool store(uint32_t w) {   // false: no room
        for (uint32_t k = 0; k < ways; k++) {
            uint32_t& t = tag[idx(w, k) * ways + k];
            if (t == w) return true;
        }
        for (uint32_t x : overw) if (x == w) return true;
        for (uint32_t k = 0; k < ways; k++) {
            uint32_t& t = tag[idx(w, k) * ways + k];
            if (t == 0xffffffffu) { t = w; n++; return true; }
        }
        if (overw.size() < over) { overw.push_back(w); n++; return true; }
        return false;
    }
};
static std::vector<Wc> g_wc;
static uint64_t g_pass_bundles = 0;
static const uint32_t TICKS = 8192;

// ---- texel caches: a load's (and an old word's) texel from registers
struct Tc {
    const char* name;
    uint32_t n;   // entries
    int dm;       // 1: direct mapped by the texel's low bits
    std::vector<uint32_t> t;
    uint64_t look = 0, hit = 0;
    void reset() { t.assign(n, 0xffffffffu); }
    void use(uint32_t key) {
        look++;
        if (dm) { uint32_t i = key & (n - 1); if (t[i] == key) hit++; else t[i] = key; return; }
        for (uint32_t i = 0; i < n; i++)
            if (t[i] == key) { hit++; for (; i > 0; i--) t[i] = t[i - 1]; t[0] = key; return; }
        for (uint32_t i = n - 1; i > 0; i--) t[i] = t[i - 1];
        t[0] = key;
    }
};
static std::vector<Tc> g_tc;
static uint64_t g_long[2], g_ccs[16];
static uint64_t g_path[8], g_cc0, g_mem_ops, g_loads, g_stores, g_need_texel, g_in_wc, g_nops[3];
static uint64_t g_dep_prev = 0, g_memb = 0, g_samebase = 0;   // a memory bundle whose address registers the one before wrote
static uint32_t g_prev_writes[4], g_prev_nw = 0;
static std::map<uint32_t, uint64_t> g_pcs;
static uint64_t g_dep01[8], g_two[8];   // two-op bundles by path, and those whose op 1 reads op 0's result   // memory bundles by pc (the hot ones)
static uint32_t g_last_base_reg = 64, g_last_base_val = 0;

static int step(gvm_core* c) {
    if (++g_bundles >= g_limit) return 1;
    if (c->pc & 15) return 0;
    const uint32_t* t = &g_m.ram[c->pc >> 2];
    uint32_t ctl = t[3], n = GVM_N(ctl), path = GVM_PATH(ctl);
    g_path[path]++;
    g_nops[n > 2 ? 2 : n]++;
    if (GVM_CC(ctl) == 0) g_cc0++;
    g_long[0] += GVM_LONG0(ctl); g_long[1] += GVM_LONG1(ctl); g_ccs[GVM_CC(ctl)]++;
    bool full[16] = {};
    if (n == 2) {
        uint32_t w0 = t[0], w1 = t[1], o0 = GVM_OPC(w0), d0 = GVM_RD(w0);
        bool st0 = GVM_CLASS(o0) == 2 && (((o0 >> 3) & 7) == GVM_M_STORE || ((o0 >> 3) & 7) == GVM_M_STOREX);
        uint32_t o1 = GVM_OPC(w1);
        bool st1 = GVM_CLASS(o1) == 2 && (((o1 >> 3) & 7) == GVM_M_STORE || ((o1 >> 3) & 7) == GVM_M_STOREX);
        bool rdr = !st0 && d0 != 0 && (GVM_RA(w1) == d0 || GVM_RB(w1) == d0 || GVM_RC(w1) == d0 || (st1 && GVM_RD(w1) == d0));
        g_two[path]++;
        if (rdr) g_dep01[path]++;
    }
    bool memb = false, dep = false;
    uint32_t writes[4], nw = 0;
    for (uint32_t s = 0; s < n && s < 2; s++) {
        uint32_t w = t[s], opc = GVM_OPC(w);
        if (GVM_CLASS(opc) != 2) { if (GVM_RD(w)) writes[nw++] = GVM_RD(w); continue; }
        uint32_t mode = (opc >> 3) & 7, wd = opc & 7;
        if (mode > 3) continue;
        bool L = ((ctl >> (5 + s)) & 1) != 0;
        uint32_t imm = L ? t[2] : (uint32_t)GVM_IMM12(w);
        uint32_t ad = c->x[GVM_RA(w)] + (mode <= 1 ? imm : c->x[GVM_RB(w)] << (GVM_RC(w) & 31));
        bool st = mode & 1;
        memb = true;
        for (uint32_t k = 0; k < g_prev_nw; k++) if (g_prev_writes[k] == GVM_RA(w) || (mode >= 2 && g_prev_writes[k] == GVM_RB(w))) dep = true;
        if (!st && GVM_RD(w)) writes[nw++] = GVM_RD(w);
        g_mem_ops++;
        if (st) g_stores++; else g_loads++;
        uint32_t by = wd == GVM_W_W ? 4 : wd == GVM_W_D ? 8 : wd == GVM_W_Q ? 16 : (wd & 1) ? 1 : 2;
        bool need = !st || by < 4 || (ad & 3) != 0;
        if (need) {
            g_need_texel++;
            uint32_t key = (ad >> 4) | (ad & GVM_ROM);
            for (auto& x : g_tc) x.use(key);
        }
        if (!st) {   // in the write cache already?
            for (uint32_t k = 0; k < g_wc[0].ways * g_wc[0].sets; k++) if (g_wc[0].tag[k] == ad >> 2) { g_in_wc++; break; }
        }
        if (st && ad < GVM_ROM) {
            uint32_t words = by <= 4 ? 1 : by / 4;
            for (size_t i = 0; i < g_wc.size(); i++)
                for (uint32_t k = 0; k < words; k++) if (!g_wc[i].store((ad >> 2) + k)) full[i] = true;
        }
    }
    if (memb) { g_memb++; if (dep) g_dep_prev++; g_pcs[c->pc]++; }
    memcpy(g_prev_writes, writes, sizeof writes); g_prev_nw = nw;
    if (GVM_CC(ctl) == GVM_CC_ADDBNZ) g_prev_writes[g_prev_nw++ & 3] = GVM_CRA(ctl);
    g_pass_bundles++;
    // a pass ends at TICKS bundles, or where model 0 (the tick's) is full; the others are counted on their own passes
    bool anyfull = false;
    for (size_t i = 0; i < g_wc.size(); i++) if (full[i]) {
        g_wc[i].full_passes++; anyfull = true;
    }
    if (anyfull || g_pass_bundles >= TICKS) {
        // each model ends its own pass: model i's bundles a pass is measured by restarting only it
        for (size_t i = 0; i < g_wc.size(); i++) if (full[i] || g_pass_bundles >= TICKS) {
            g_wc[i].passes++; g_wc[i].fill_sum += g_wc[i].n;
            g_wc[i].reset(); g_wc[i].overw.clear();
        }
        if (full[0] || g_pass_bundles >= TICKS) {
            static int shown = 0;
            if (getenv("SHOW") && shown++ < 60) printf(" %llu", (unsigned long long)g_pass_bundles);
            for (auto& x : g_tc) x.reset();
            g_pass_bundles = 0;
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* name = argc > 1 ? argv[1] : "coremark";
    if (argc > 2) g_limit = strtoull(argv[2], nullptr, 0);
    g_texw = 8192; g_wcn = 514; g_twc = 40; g_loop_n = 3000000;
    std::vector<Case> all = cases();
    const Case* cs = nullptr;
    for (auto& c : all) if (!strcmp(c.name, name)) cs = &c;
    if (!cs) { printf("no case %s\n", name); return 1; }
    Prepared pr = prepare(*cs, 1);
    std::vector<uint8_t> rom(ROM_SIZE, 0);
    g_m.ram = pr.ramImg.data(); g_m.ram_end = RAM_END; g_m.rom = rom.data(); g_m.rom_size = ROM_SIZE;
    g_m.flags = GVM_REF_BREAK_HALTS;
    gvm_core core;
    gvm_core_reset(&core, 0, pr.bases[0]);
    core.sr[GVM_SR_TVEC] = HALT_STUB;
    for (int k = 1; k < 64; k++) core.x[k] = pr.state[0][k];
    core.host_step = step;
    g_m.cores = &core; g_m.ncores = 1;
    g_wc = {{"x18: 2 ways x 256", 256, 2, 0, 0}, {"512 sets >>6", 512, 2, 0, 0}, {"512 >>9", 512, 2, 0, 9}, {"512 >>4", 512, 2, 0, 4}, {"512 >>11", 512, 2, 0, 11}, {"512 >>3", 512, 2, 0, 3}, {"4 ways x 128, 2 over", 128, 4, 2, 0}, {"8 ways x 64", 64, 8, 2, 0},
            {"2 ways skewed", 256, 2, 2, 2}, {"1 way x 512, 2 over", 512, 1, 2, 0}, {"2 ways x 512 (1026)", 512, 2, 2, 0},
            {"16 ways x 32", 32, 16, 2, 0}, {"2 ways x 256, 16 over", 256, 2, 16, 0}, {"2 ways x 256, w alone", 256, 2, 2, 1}};
    for (auto& w : g_wc) w.reset();
    g_tc = {{"1", 1, 0}, {"2 LRU", 2, 0}, {"4 LRU", 4, 0}, {"2 dm", 2, 1}, {"4 dm", 4, 1}, {"8 LRU", 8, 0}};
    for (auto& t : g_tc) t.reset();
    for (int p = 0; p < 1000000 && !core.halted && g_bundles < g_limit; p++) {
        gvm_run(&g_m, &core, 8192);
        gvm_commit(&g_m);
    }
    double B = (double)g_bundles;
    printf("%s: %llu bundles, %llu memory ops (%.1f%% loads), cc 0 %.1f%%; ops 0/1/2: %.1f/%.1f/%.1f%%\n", name, (unsigned long long)g_bundles,
           (unsigned long long)g_mem_ops, 100.0 * g_loads / g_mem_ops, 100 * g_cc0 / B, 100 * g_nops[0] / B, 100 * g_nops[1] / B, 100 * g_nops[2] / B);
    printf("paths:"); for (int i = 0; i < 8; i++) printf(" %d %.1f%%", i, 100 * g_path[i] / B); printf("\n");
    printf("long immediates: op 0's %.2f%%, op 1's %.2f%%; cc:", 100 * g_long[0] / B, 100 * g_long[1] / B);
    for (int i = 0; i < 16; i++) printf(" %d %.1f%%", i, 100 * g_ccs[i] / B);
    printf("\n");
    printf("two-op bundles whose op 1 reads op 0's result, by path (%% of all bundles):");
    for (int i = 0; i < 8; i++) if (g_two[i]) printf(" %d: %.2f%% of %.1f%%", i, 100 * g_dep01[i] / B, 100 * g_two[i] / B);
    printf("\n");
    printf("memory bundles %.1f%%; their address registers written by the bundle before: %.1f%%\n", 100.0 * g_memb / B, 100.0 * g_dep_prev / g_memb);
    printf("texel reads %.1f%% of memory ops; loads that find their word in the write cache: %.1f%%\n", 100.0 * g_need_texel / g_mem_ops, 100.0 * g_in_wc / g_loads);
    for (auto& x : g_tc) printf("  texel cache %-6s: %.1f%% hits\n", x.name, 100.0 * x.hit / x.look);
    for (auto& w : g_wc) printf("  write cache %-28s: %llu passes, %.0f bundles a pass, %.0f%% end full, %.0f entries at the end\n", w.name,
                                (unsigned long long)w.passes, B / w.passes, 100.0 * w.full_passes / w.passes, (double)w.fill_sum / w.passes);
    std::vector<std::pair<uint64_t, uint32_t>> hot;
    for (auto& kv : g_pcs) hot.push_back({kv.second, kv.first});
    std::sort(hot.rbegin(), hot.rend());
    printf("hot memory bundles:");
    for (size_t i = 0; i < hot.size() && i < 12; i++) printf(" %08x %.1f%%", hot[i].second, 100.0 * hot[i].first / B);
    printf("\n");
    return 0;
}
