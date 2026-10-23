// RVC_PROF=DIR: a profile of every core of a run, for tools/prof.py (docs/machine/profiling.md).
// Included by rvc_cpu.cpp after its memory and before run(); each hook there is one line behind
// prof::on. What a pc ran, loaded and stored, the calls between functions, every pass's
// instructions on each core, distinct texels stored a pass, and (RVC_PROF_DEPS) what the
// iterations of chosen loops and calls read and write of each other's.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace prof {

static bool on = false, active = false, mem_on = false;
static bool pc_cores = false;   // RVC_PROF_PCS=1: pcs.txt's pages kept apart for each core, its number an eighth column
static std::string dir;
static u32 ticks = 8192;
static u64 pass_no = 0;

// ---- what each pc ran: a page of 1,024 pcs, by class (0 core 0, 1 the workers), table and page
struct PcCell { u64 exec; u32 loads, stores, word, pad; };
struct PcPage { PcCell cell[1024]; };
static std::unordered_map<u64, PcPage*> pc_pages;
static u64 cache_key[256];
static PcPage* cache_page[256];
static u32 pend_loads[256], pend_stores[256];   // the instruction now running: counted at its step

static inline PcCell& cell(int hart, u32 satp, u32 pc) {
    u64 key = ((u64)(hart != 0) << 62) | (pc_cores ? (u64)(hart & 255) << 54 : 0) | ((u64)(satp & 0x3fffff) << 20) | (pc >> 12);
    if (cache_key[hart] != key || !cache_page[hart]) {
        PcPage*& p = pc_pages[key];
        if (!p) p = (PcPage*)calloc(1, sizeof(PcPage));
        cache_key[hart] = key, cache_page[hart] = p;
    }
    return cache_page[hart]->cell[(pc >> 2) & 1023];
}

// ---- memory: each texel's loads and stores by class, and how many distinct texels a core stored a pass
struct MemPage { u32 loads[2][256], stores[2][256], stamp[256]; };
static std::vector<MemPage*> mem_pages;   // a RAM page each
static u32 distinct[256];                 // this pass, each core's
static u64 distinct_hist[2][257];         // passes by distinct texels stored (in 16s; the last: more)

// ---- calls: a stack for each core and kind (kernel or program), edges by caller and callee
struct Frame { u32 target, ret; u64 at, serial; };
struct Stack { std::vector<Frame> f; u64 clock = 0; };
static std::unordered_map<u64, Stack> stacks;
struct Edge { u64 calls = 0, inclusive = 0; };
static std::unordered_map<u64, Edge> edges[2];   // (caller << 32) | callee; caller 0: none seen
static u64 serial_next = 1;

// ---- RVC_PROF_DEPS: loops (l:LO-HI, the head and the branch back) and calls (f:ADDR), core 0's
struct TexAcc { u32 w_inst, r_inst, w_pc, r_pc; uint16_t w_mask; };
struct Watch {
    u32 lo = 0, hi = 0;
    bool loop = false, active = false, in_inst = false, truncated = false;
    size_t depth = 0;
    u32 sp0 = 0, inst = 0, out = 0;   // out: instructions of a loop's run outside its range since it left
    u64 parent = 0, inst_at = 0, stack_key = 0;
    bool inst_raw = false;
    u64 groups = 0, instances = 0, instr = 0, with_raw = 0, count[8] = {0};
    std::unordered_map<u64, u64> pairs;   // pair_key: the kind, this access's pc, the other's
    std::unordered_map<u32, TexAcc> tex;
};
static std::vector<Watch> watches;
enum { RAW, WAW, WAW_TEXEL, WAR };   // +4: in the frame above the stack pointer at the start

static FILE* timeline = nullptr;
static FILE* per_core = nullptr;
static std::vector<u64> clock_at;

static void init(const char* d, size_t ncores, bool from_start) {
    on = true;
    active = from_start;
    dir = d;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    mem_on = !getenv("RVC_PROF_MEM") || atoi(getenv("RVC_PROF_MEM")) != 0;
    pc_cores = getenv("RVC_PROF_PCS") && atoi(getenv("RVC_PROF_PCS")) != 0;
    if (mem_on) mem_pages.assign(RAM_SIZE >> 12, nullptr);
    timeline = fopen((dir + "/timeline.bin").c_str(), "wb");
    if (getenv("RVC_PROF_CORES") && atoi(getenv("RVC_PROF_CORES")))
        per_core = fopen((dir + "/cores.bin").c_str(), "wb");
    clock_at.assign(ncores, 0);
    if (const char* w = getenv("RVC_PROF_DEPS")) {
        std::string s = w;
        for (size_t at = 0; at < s.size();) {
            size_t end = s.find(',', at);
            std::string one = s.substr(at, end == std::string::npos ? std::string::npos : end - at);
            at = end == std::string::npos ? s.size() : end + 1;
            Watch x;
            if (one.size() > 2 && one[1] == ':') {
                x.loop = one[0] == 'l';
                x.lo = (u32)strtoul(one.c_str() + 2, nullptr, 16);
                size_t dash = one.find('-');
                x.hi = dash != std::string::npos ? (u32)strtoul(one.c_str() + dash + 1, nullptr, 16) : x.lo;
                watches.push_back(x);
            }
        }
    }
}

static inline u64 pair_key(int kind, u32 a, u32 b) { return ((u64)kind << 61) | ((u64)(a >> 2) << 30) | (b >> 2); }

static void inst_end(Watch& w, u64 clock) {
    if (!w.in_inst) return;
    w.instr += clock - w.inst_at;
    if (w.inst_raw) w.with_raw++;
    w.in_inst = false;
}

static void group_end(Watch& w, u64 clock) {
    inst_end(w, clock);
    w.active = false;
    w.tex.clear();
}

static void group_begin(Watch& w, u64 parent, size_t depth, u32 sp, u64 key, u64 clock) {
    if (w.active) group_end(w, clock);
    w.active = true, w.parent = parent, w.depth = depth, w.sp0 = sp, w.inst = 0, w.out = 0, w.stack_key = key;
    w.groups++;
}

static void inst_begin(Watch& w, u64 clock) {
    inst_end(w, clock);
    w.inst++, w.instances++, w.inst_at = clock, w.inst_raw = false, w.in_inst = true;
}

static void deps_access(Core& c, u32 va, int size, bool store) {
    for (Watch& w : watches) {
        if (!w.in_inst) continue;
        if (va < w.sp0 && va >= w.sp0 - (1u << 20)) continue;   // (the stack below its start: locals of its own)
        int frame = va >= w.sp0 && va < w.sp0 + 4096 ? 4 : 0;
        u32 texel = va >> 4;
        uint16_t mask = (uint16_t)(((1u << size) - 1) << (va & 15));
        if ((va & 15) + (u32)size > 16) mask = (uint16_t)(0xffffu << (va & 15));   // (the rest: the next texel's, not counted)
        auto it = w.tex.find(texel);
        if (it == w.tex.end()) {
            if (w.tex.size() >= (4u << 20)) { w.truncated = true; continue; }
            it = w.tex.emplace(texel, TexAcc{0, 0, 0, 0, 0}).first;
        }
        TexAcc& a = it->second;
        if (!store) {
            if (a.w_inst && a.w_inst != w.inst && (a.w_mask & mask)) {
                w.count[RAW + frame]++, w.pairs[pair_key(RAW + frame, c.pc, a.w_pc)]++;
                w.inst_raw = true;
            }
            a.r_inst = w.inst, a.r_pc = c.pc;
        } else {
            if (a.w_inst && a.w_inst != w.inst) {
                int k = (a.w_mask & mask) ? WAW : WAW_TEXEL;
                w.count[k + frame]++, w.pairs[pair_key(k + frame, c.pc, a.w_pc)]++;
            }
            if (a.r_inst && a.r_inst != w.inst) w.count[WAR + frame]++, w.pairs[pair_key(WAR + frame, c.pc, a.r_pc)]++;
            if (a.w_inst != w.inst) a.w_mask = 0;
            a.w_inst = w.inst, a.w_mask |= mask, a.w_pc = c.pc;
        }
    }
}

// A load or store to RAM at physical `phys` (virtual `va`) by the instruction at c.pc.
static inline void mem(Core& c, u32 va, u32 phys, int size, bool store) {
    if (!active) return;
    int k = c.hart != 0;
    MemPage*& p = mem_pages[(phys - RAM_BASE) >> 12];
    if (!p) p = (MemPage*)calloc(1, sizeof(MemPage));
    u32 t = (phys >> 4) & 255;
    if (store) {
        p->stores[k][t]++, pend_stores[c.hart]++;
        u32 stamp = (u32)(pass_no << 8) | (u32)c.hart;
        if (p->stamp[t] != stamp) p->stamp[t] = stamp, distinct[c.hart]++;
    } else p->loads[k][t]++, pend_loads[c.hart]++;
    if (!k && !watches.empty()) deps_access(c, va, size, store);
}

// After an instruction at `pc` ran on core c; `next` is where it goes.
static inline void step(Core& c, u32 op, u32 rd, u32 rs1, u32 ins, u32 pc, u32 next) {
    if (!active) return;
    PcCell& x = cell(c.hart, c.satp, pc);
    x.exec++, x.word = ins;
    x.loads += pend_loads[c.hart], x.stores += pend_stores[c.hart];
    pend_loads[c.hart] = pend_stores[c.hart] = 0;
    // (a stack for each thread: tp is the C library's thread pointer in a program, the task in the kernel)
    u64 key = ((u64)c.hart << 56) | ((u64)(c.priv == PRIV_S) << 55) | ((u64)(c.satp & 0x3fffff) << 32) | c.x[4];
    Stack& s = stacks[key];
    s.clock++;
    int k = c.hart != 0;
    bool call = (op == 0x6f || op == 0x67) && (rd == 1 || rd == 5);
    bool ret = op == 0x67 && rd == 0 && (rs1 == 1 || rs1 == 5) && (ins >> 20) == 0;
    if (call) {
        u32 caller = s.f.empty() ? 0 : s.f.back().target;
        u64 parent = s.f.empty() ? 0 : s.f.back().serial;
        if (s.f.size() < 4096) s.f.push_back(Frame{next, pc + 4, s.clock, serial_next++});
        edges[k][((u64)caller << 32) | next].calls++;
        if (!k)
            for (Watch& w : watches)
                if (!w.loop && next == w.lo && !(w.in_inst && w.stack_key == key && s.f.size() > w.depth)) {   // (not a recursion)
                    if (!w.active || w.parent != parent || w.stack_key != key) group_begin(w, parent, s.f.size() - 1, c.x[2], key, s.clock);
                    w.depth = s.f.size();
                    w.sp0 = c.x[2];
                    inst_begin(w, s.clock);
                }
    } else if (ret) {
        size_t depth = s.f.size(), j = depth;
        while (j > 0 && depth - j < 64 && s.f[j - 1].ret != next) j--;
        if (j > 0 && s.f[j - 1].ret == next) {
            while (s.f.size() >= j) {
                Frame f = s.f.back();
                s.f.pop_back();
                u32 caller = s.f.empty() ? 0 : s.f.back().target;
                edges[k][((u64)caller << 32) | f.target].inclusive += s.clock - f.at;
                if (!k)
                    for (Watch& w : watches) {
                        if (w.stack_key != key || !w.active) continue;
                        if (!w.loop && s.f.size() < w.depth) inst_end(w, s.clock);
                        if (f.serial == w.parent || s.f.size() < w.depth - (w.loop ? 0 : 1)) group_end(w, s.clock);
                    }
            }
        }
    }
    if (!k)
        for (Watch& w : watches) {
            if (!w.loop) continue;
            if (!w.active) {
                if (next == w.lo) group_begin(w, 0, s.f.size(), c.x[2], key, s.clock), inst_begin(w, s.clock);
            } else if (w.stack_key == key && s.f.size() == w.depth) {
                // (a compiler lays parts of a loop's body past its branch back: a run ends once it stays out)
                if (next == w.lo) inst_begin(w, s.clock), w.out = 0;
                else if (next >= w.lo && next <= w.hi) w.out = 0;
                else if (++w.out > 256) w.out = 0, group_end(w, s.clock);
            }
        }
}

static void pass_begin() {
    for (size_t i = 0; i < cores.size(); i++) clock_at[i] = cores[i].clock;
}

// The pass's record: core 0's instructions, where it ended and why; the workers' sums.
static void pass_end() {
    if (!active) { pass_no++; return; }
    Core& c0 = cores[0];
    u32 rec[8] = {(u32)(c0.clock - clock_at[0]), c0.pc, 0, 0, 0, 0, 0, 0};
    rec[2] = (c0.waiting ? 1 : 0) | (c0.end_pass ? 2 : 0) | (c0.priv == PRIV_S ? 4 : 0);
    u32 ran = 0, full = 0;
    u64 sum = 0;
    for (size_t i = 1; i < cores.size(); i++) {
        u64 n = cores[i].clock - clock_at[i];
        if (n) ran++, sum += n;
        if (n >= ticks) full++;
    }
    rec[3] = ran, rec[4] = full, rec[5] = (u32)sum, rec[6] = distinct[0];
    u32 most = 0;
    for (size_t i = 0; i < cores.size(); i++) {
        if (distinct[i]) distinct_hist[i != 0][distinct[i] / 16 < 256 ? distinct[i] / 16 : 256]++;
        if (i && distinct[i] > most) most = distinct[i];
        distinct[i] = 0;
    }
    rec[7] = most;
    if (timeline) fwrite(rec, 4, 8, timeline);
    if (per_core) {
        std::vector<uint16_t> n(cores.size());
        for (size_t i = 0; i < cores.size(); i++) n[i] = (uint16_t)std::min<u64>(cores[i].clock - clock_at[i], 65535);
        fwrite(n.data(), 2, n.size(), per_core);
    }
    pass_no++;
}

static void report() {
    if (timeline) fclose(timeline);
    if (per_core) fclose(per_core);
    FILE* f = fopen((dir + "/meta.txt").c_str(), "w");
    if (f) { fprintf(f, "ticks %u\ncores %zu\npasses %llu\nmem %d\n", ticks, cores.size(), (unsigned long long)pass_no, mem_on ? 1 : 0); fclose(f); }
    f = fopen((dir + "/pcs.txt").c_str(), "w");
    size_t pcs = 0;
    if (f) {
        fprintf(f, "# class table pc word executed loads stores\n");
        for (auto& e : pc_pages) {
            u32 cls = (u32)(e.first >> 62), table = (u32)(e.first >> 20) & 0x3fffff, page = (u32)(e.first & 0xfffff), hart = (u32)(e.first >> 54) & 255;
            for (u32 i = 0; i < 1024; i++) {
                const PcCell& x = e.second->cell[i];
                if (!x.exec) continue;
                fprintf(f, "%u %x %08x %08x %llu %u %u", cls, table, (page << 12) | (i << 2), x.word, (unsigned long long)x.exec, x.loads, x.stores);
                fprintf(f, pc_cores ? " %u\n" : "\n", hart);
                pcs++;
            }
        }
        fclose(f);
    }
    f = fopen((dir + "/edges.txt").c_str(), "w");
    if (f) {
        fprintf(f, "# class caller callee calls inclusive\n");
        for (int k = 0; k < 2; k++)
            for (auto& e : edges[k])
                fprintf(f, "%d %08x %08x %llu %llu\n", k, (u32)(e.first >> 32), (u32)e.first, (unsigned long long)e.second.calls,
                        (unsigned long long)e.second.inclusive);
        fclose(f);
    }
    if (mem_on) {
        f = fopen((dir + "/mem.bin").c_str(), "wb");
        if (f) {
            for (size_t i = 0; i < mem_pages.size(); i++) {
                if (!mem_pages[i]) continue;
                u32 at = RAM_BASE + (u32)(i << 12);
                fwrite(&at, 4, 1, f);
                fwrite(mem_pages[i], 4, 4 * 256, f);   // loads[2][256], stores[2][256]
            }
            fclose(f);
        }
        f = fopen((dir + "/wcache.txt").c_str(), "w");
        if (f) {
            fprintf(f, "# class texels/16 passes\n");
            for (int k = 0; k < 2; k++)
                for (int b = 0; b < 257; b++)
                    if (distinct_hist[k][b]) fprintf(f, "%d %d %llu\n", k, b, (unsigned long long)distinct_hist[k][b]);
            fclose(f);
        }
    }
    if (!watches.empty() && (f = fopen((dir + "/deps.txt").c_str(), "w"))) {
        fprintf(f, "# watch kind lo hi groups instances instructions with_raw raw waw waw_texel war fraw fwaw fwaw_texel fwar truncated\n");
        for (Watch& w : watches) {
            inst_end(w, w.inst_at);
            fprintf(f, "watch %c %08x %08x %llu %llu %llu %llu", w.loop ? 'l' : 'f', w.lo, w.hi, (unsigned long long)w.groups,
                    (unsigned long long)w.instances, (unsigned long long)w.instr, (unsigned long long)w.with_raw);
            for (int k = 0; k < 8; k++) fprintf(f, " %llu", (unsigned long long)w.count[k]);
            fprintf(f, " %d\n", w.truncated ? 1 : 0);
            for (auto& p : w.pairs)
                fprintf(f, "pair %d %08x %08x %llu\n", (int)(p.first >> 61), (u32)((p.first >> 30) & 0x3fffffff) << 2,
                        (u32)(p.first & 0x3fffffff) << 2, (unsigned long long)p.second);
        }
        fclose(f);
    }
    fprintf(stderr, "[prof] %zu pcs, %zu call edges, %llu passes, in %s (python tools/prof.py report %s)\n", pcs,
            edges[0].size() + edges[1].size(), (unsigned long long)pass_no, dir.c_str(), dir.c_str());
}

}  // namespace prof
