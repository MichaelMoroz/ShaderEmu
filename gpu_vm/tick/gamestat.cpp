// gamestat: a game bench case (gamebench.h) on the reference machine, in passes as the card runs them, and what it is
// made of: core 0's and the workers' ops by kind, control words, branches taken, bundles a pass and why passes end
// (core 0 with a write cache model: --wc NB,WAYS[,RELOC], x23's is 32,4), distinct texels core 0 stores in 4,096
// bundles, workers that run a pass, units (frames) a pass. The numbers to hold against the games' (GAMEBENCH.md).
//
//   gamestat CASE [--passes N] [--ticks N] [--wc NB,WAYS[,RELOC]] [--check]
//   (g++ -O2 -std=c++17 -o build/gamestat gpu_vm/tick/gamestat.cpp)
#define GVM_REF_IMPL
#include "../ref/gvm_ref.h"
#include "../ref/gvm_asm.h"
#include "cases.h"
#include <algorithm>
#include <map>
#include <set>

static gvm_machine g_m;
struct Mix { uint64_t bundles = 0, ops = 0, k[16] = {}, ctl[10] = {}, cond = 0, taken = 0; };
static Mix g_mix[2];
static uint32_t g_prev_pc[256], g_prev_cond[256];
static std::set<uint32_t> g_win;   // core 0's stored texels in the window
static uint64_t g_win_b = 0, g_wins = 0, g_win_tex = 0;
static std::set<uint32_t> g_fw, g_fws;   // core 0's footprint in 8,192 bundles: texels stored, those of them on the stack
static uint64_t g_fw_b = 0, g_fw_n = 0, g_fw_tex = 0, g_fw_stack = 0;
static uint64_t g_st_stack = 0, g_st_all = 0;
static const char* KN[16] = {"ilite", "iother", "imul", "load", "store", "loadQ", "storeQ", "amo", "flite", "fother", "fspecial", "idiv", "lpc", "sync", "xother", "-"};
static const char* CN[10] = {"none", "cond", "j", "call", "callr", "ret", "jr", "eret", "add", "addbnz"};
static int kind(uint32_t w) {
    uint32_t opc = (w >> 12) & 255, c = opc >> 6;
    if (c == 1) { uint32_t f = opc & 31; return f == 19 ? 2 : (f <= 7 || (f >= 16 && f <= 18)) ? 0 : 1; }
    if (c == 2) { uint32_t m = (opc - 0x80) / 8, wd = opc & 7; if (m == 0 || m == 2) return wd == 4 ? 5 : 3; if (m == 1 || m == 3) return wd == 4 ? 6 : 4; return 7; }
    if (c == 3) { uint32_t f = opc & 31; return f < 8 ? 8 : f < 24 ? 9 : 10; }
    if (opc >= 1 && opc <= 3) return 2;
    if (opc >= 4 && opc <= 7) return 11;
    if (opc == 0x0e) return 12;
    if (opc == 0x12 || opc == 0x13) return 13;
    return 14;
}
static int step_hook(gvm_core* c) {
    uint32_t id = c->id, pc = c->pc & ~15u;
    Mix& M = g_mix[id ? 1 : 0];
    if (g_prev_cond[id]) { M.cond++; if (pc != g_prev_pc[id] + 16) M.taken++; }
    g_prev_cond[id] = 0;
    if ((c->pc & 15) != 0) return 0;   // (a bundle resumed: counted when it began)
    const uint32_t* t = &g_m.ram[pc >> 2];
    uint32_t ctl = t[3], n = GVM_N(ctl), cc = GVM_CC(ctl);
    M.bundles++;
    for (uint32_t k = 0; k < n && k < 2; k++) { M.k[kind(t[k])]++; M.ops++; }
    int ck = 0;
    if (cc == GVM_CC_SPECIAL) {
        uint32_t sub = GVM_SUB(ctl);
        ck = sub == GVM_SUB_J ? 2 : sub == GVM_SUB_CALL ? 3 : sub == GVM_SUB_CALLR ? 4 : sub == GVM_SUB_JR ? (GVM_CRS(ctl) == 1 ? 5 : 6) : sub == GVM_SUB_ERET ? 7 : sub == GVM_SUB_ADD ? 8 : 0;
    } else if (cc == GVM_CC_BNE && GVM_CRA(ctl) == 0 && GVM_CRB(ctl) == 0) ck = 0;
    else if (cc == GVM_CC_BEQ && GVM_CRA(ctl) == 0 && GVM_CRB(ctl) == 0) ck = 2;
    else { ck = cc == GVM_CC_ADDBNZ ? 9 : 1; g_prev_cond[id] = 1; g_prev_pc[id] = pc; }
    M.ctl[ck]++;
    if (id == 0 && ++g_win_b == 4096) { g_win_tex += g_win.size(); g_wins++; g_win.clear(); g_win_b = 0; }
    if (id == 0 && ++g_fw_b == 8192) { g_fw_tex += g_fw.size(); g_fw_stack += g_fws.size(); g_fw_n++; g_fw.clear(); g_fws.clear(); g_fw_b = 0; }
    return 0;
}
static void access_hook(gvm_core* c, uint32_t a, uint32_t bytes, int store) {
    if (!store || c->id != 0) return;
    g_win.insert(a >> 4);
    g_st_all++;
    g_fw.insert(a >> 4);
    if (a >= c->x[2] - 64 && a < c->x[2] + 0x8000) { g_st_stack++; g_fws.insert(a >> 4); }
    (void)bytes;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("gamestat CASE [--passes N] [--ticks N] [--wc NB,WAYS[,RELOC]] [--check]\n"); return 1; }
    std::string name = argv[1];
    uint32_t passes = 2000, ticks = 8192, nb = 0, ways = 4, reloc = 0;
    bool check = false;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--passes" && i + 1 < argc) passes = atoi(argv[++i]);
        else if (a == "--ticks" && i + 1 < argc) ticks = atoi(argv[++i]);
        else if (a == "--wc" && i + 1 < argc) sscanf(argv[++i], "%u,%u,%u", &nb, &ways, &reloc);
        else if (a == "--check") check = true;
    }
    g_loop_n = check ? 3000 : 3000000;
    std::vector<Case> all = cases();
    const Case* cs = nullptr;
    for (auto& c : all) if (name == c.name) cs = &c;
    if (!cs) { printf("no case %s\n", name.c_str()); return 1; }
    RAM_END = cs->mc || cs->big ? RAM_MC : 2048 * 2048 * 16;
    Prepared pr = prepare(*cs, 1);
    std::vector<uint32_t> ram = pr.ramImg;
    std::vector<uint8_t> rom(ROM_SIZE, 0);
    g_m.ram = ram.data(); g_m.ram_end = RAM_END; g_m.rom = rom.data(); g_m.rom_size = ROM_SIZE; g_m.flags = GVM_REF_BREAK_HALTS;
    std::vector<gvm_core> rc(cs->cores);
    for (uint32_t c = 0; c < cs->cores; c++) {
        gvm_core_reset(&rc[c], c, pr.bases[c]); rc[c].sr[GVM_SR_TVEC] = HALT_STUB;
        for (int k = 1; k < 64; k++) rc[c].x[k] = pr.state[c][k];
        rc[c].host_step = step_hook; rc[c].host_access = access_hook;
        rc[c].xr = cs->xr_need ? 256 : 0; rc[c].npair = 64;   // (a file for the _f cases)
    }
    if (nb) { rc[0].wl.nb = nb; rc[0].wl.ways = ways; rc[0].wl.reloc = reloc; }
    g_m.cores = rc.data(); g_m.ncores = cs->cores;
    uint64_t ends[8] = {}, ran = 0, whole = 0, idle = 0, b0 = 0, units0 = rc[0].x[60], p = 0;
    std::vector<uint64_t> wb(cs->cores, 0);
    for (p = 0; p < (check ? 1000000u : passes); p++) {
        g_m.time = 1000 + p;
        bool any = false;
        for (uint32_t c = 0; c < cs->cores; c++) {
            if (rc[c].halted) continue;
            any = true;
            uint64_t before = rc[c].bundles_run;
            int e = gvm_run(&g_m, &rc[c], ticks);
            uint64_t d = rc[c].bundles_run - before;
            if (c == 0) { ends[e & 7]++; b0 += d; }
            else { if (d > 40) ran++; else if (d > 0) idle++; if (d >= ticks) whole++; }
        }
        gvm_commit(&g_m);
        if (!any) break;
    }
    for (uint32_t c = 0; c < cs->cores; c++)
        if (rc[c].pc == HALT_STUB || (rc[c].halted && rc[c].sr[GVM_SR_CAUSE])) {
            printf("core %u: trapped, cause %x at %08x tval %08x;", c, rc[c].sr[GVM_SR_CAUSE], rc[c].sr[GVM_SR_EPC], rc[c].sr[GVM_SR_TVAL]);
            for (int k = 1; k < 32; k++) printf(" r%d=%x", k, rc[c].x[k]);
            printf("\n");
        }
    printf("%s: %llu passes of %u bundles%s, %u cores%s\n", cs->name, (unsigned long long)p, ticks, nb ? " (core 0 with a write cache model)" : "", cs->cores, check ? " (a check's run, to the halt)" : "");
    double frames = (rc[0].x[60] - units0) / cs->unit_div;
    printf("  frames %.2f: %.1f passes a frame; core 0 %.0f bundles a pass, %.0f a frame\n", frames, frames > 0 ? p / frames : 0, (double)b0 / p, frames > 0 ? b0 / frames : 0);
    printf("  core 0's passes ended:");
    const char* EN[8] = {"budget", "sync", "wait", "full", "parked", "halt", "6", "7"};
    for (int e = 0; e < 8; e++) if (ends[e]) printf(" %s %.0f%%", EN[e], 100.0 * ends[e] / p);
    printf("\n  workers a pass: %.1f ran (> 40 bundles), %.1f only looked, %.2f ran the whole pass\n", (double)ran / p, (double)idle / p, (double)whole / p);
    printf("  core 0: distinct texels stored in 4,096 bundles %.0f; stores to the stack %.0f%%\n", g_wins ? (double)g_win_tex / g_wins : 0, g_st_all ? 100.0 * g_st_stack / g_st_all : 0);
    printf("  core 0's footprint in 8,192 bundles: %.0f texels stored, %.0f of them the stack's\n", g_fw_n ? (double)g_fw_tex / g_fw_n : 0, g_fw_n ? (double)g_fw_stack / g_fw_n : 0);
    for (int w = 0; w < 2; w++) {
        Mix& M = g_mix[w];
        if (!M.bundles) continue;
        printf("  %s: %.2f ops a bundle; ops:", w ? "workers" : "core 0", (double)M.ops / M.bundles);
        std::vector<std::pair<uint64_t, int>> o;
        for (int k = 0; k < 16; k++) if (M.k[k]) o.push_back({M.k[k], k});
        std::sort(o.rbegin(), o.rend());
        for (auto& x : o) printf(" %s %.1f%%", KN[x.second], 100.0 * x.first / M.ops);
        printf("\n    control a bundle:");
        for (int k = 0; k < 10; k++) if (M.ctl[k]) printf(" %s %.1f%%", CN[k], 100.0 * M.ctl[k] / M.bundles);
        printf("; conditional taken %.0f%%\n", M.cond ? 100.0 * M.taken / M.cond : 0);
    }
    return 0;
}
