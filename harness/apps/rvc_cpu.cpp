// rvc_cpu: the machine on the processor, for work that only needs its instructions counted.
//
// The shader machine runs three million instructions a second; a program's cost on it is a
// number of instructions, and that number does not depend on what runs them. This is the same
// machine as an ordinary interpreter (docs/cpu-harness.md): RV32IMAF with a supervisor and
// Sv32, the firmware's calls answered in place (SBI_HLE), the ROM, the parallel copy, the
// control words (clock, keys and pointer, the GPU's submit word) and the worker cores. It
// boots the same image, takes the same --expect/--send/--until script and prints the same
// console, some fifty times faster. It draws nothing: a submitted list is taken as drawn, and
// a picture a program asked to have copied back is not there. Pictures, sound and timings in
// seconds are the shader machine's (rvc_harness) to check.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

typedef uint32_t u32;
typedef int32_t i32;
typedef uint64_t u64;
typedef int64_t i64;

static const u32 RAM_BASE = 0x80000000u, RAM_SIZE = 0x08000000u;   // to 0x88000000: Linux's RAM, then the GPU's
static const u32 ROM_BASE = 0x40000000u;
static const u32 CTRL = 0x87000000u;   // the control words (docs/gpu.md, docs/input.md)

static std::vector<uint8_t> ram, rom;
static u32* ramw;   // the same bytes as words

static inline u32& word(u32 phys) { return ramw[(phys - RAM_BASE) >> 2]; }

// For a host that shows the machine (rvc_harness --cpu): the 4 MB bands of RAM written since
// it last looked, one bit each; whether the GPU's and the input's control words are its own
// to keep (the shader's control pass, not control_pass() here); and where the console goes.
static u32 dirty_bands = 0;
static bool external_devices = false;
static void (*console_hook)(char) = nullptr;
static inline void dirty(u32 phys) { dirty_bands |= 1u << (((phys - RAM_BASE) >> 22) & 31); }

// ---- options and the console ----

struct Options {
    std::string image = "build/images/linux", uartLog, until;
    std::vector<std::pair<std::string, std::string>> script;   // expect, send
    double seconds = 0;      // of guest time; 0: no limit
    double wall = 0;         // of the host's time
    u64 maxInstr = 0;
    u32 ticks = 16384;       // instructions a pass
    double ips = 3700000;    // what the shader machine runs while it runs
    double passMs = 0.25;    // and what a pass costs it besides
    int cores = 1;
    long long sweep = -1;
    bool desktop = false, tabs = false, quiet = false;
};
static Options opt;

static std::string console_text;   // everything the guest said, for --expect and --until
static size_t script_at = 0, expect_from = 0;
static std::deque<uint8_t> console_in;
static FILE* uart_log = nullptr;
static bool until_hit = false;

static void console_put(u32 c) {
    char ch = (char)c;
    if (console_hook) { console_hook(ch); return; }
    console_text.push_back(ch);
    if (!opt.quiet) fputc(ch, stdout);
    if (uart_log) fputc(ch, uart_log);
    if (ch == '\n' || ch == ' ' || ch == '#') {
        if (!opt.quiet) fflush(stdout);
    }
    if (script_at < opt.script.size()) {
        const std::string& want = opt.script[script_at].first;
        size_t at = console_text.find(want, expect_from);
        if (at != std::string::npos) {
            for (char s : opt.script[script_at].second) console_in.push_back((uint8_t)s);
            expect_from = at + want.size();
            script_at++;
        }
    } else if (!opt.until.empty() && !until_hit && console_text.size() >= opt.until.size()) {
        // (only once the script has been typed: its echo may hold the text)
        if (console_text.find(opt.until, expect_from) != std::string::npos) until_hit = true;
    }
}

// ---- a core ----

enum { PRIV_U = 0, PRIV_S = 1 };
enum { FETCH = 0, READ = 1, WRITE = 2 };
static const u32 TLB_N = 1024;
struct TlbEntry { u32 vpn; u32 ppage; };   // vpn + 1 (0: empty); the page's physical address

struct Core {
    u32 x[32] = {0}, f[32] = {0}, pc = 0, priv = PRIV_S;
    u32 mstatus = 0, mie = 0, mip = 0, stvec = 0, sscratch = 0, sepc = 0, scause = 0, stval = 0, satp = 0;
    u32 scounteren = 0, fcsr = 0, sedeleg = 0, sideleg = 0;
    u32 memop_src = 0, memop_dst = 0, memop_n = 0;
    u64 clock = 0;            // instructions run
    u64 timecmp = 0;
    bool reserved = false;
    u32 reservation = 0;
    bool waiting = false;     // wfi: until an interrupt
    bool end_pass = false;
    bool recheck = false;     // interrupts may have been let in: look before the next instruction
    TlbEntry tlb[2][3][TLB_N];
    int hart = 0;
    // a worker core (docs/multicore.md): bit 0 running, see mc_* below
    u32 mc_word = 0;
    bool started = false;
    Core() { memset(tlb, 0, sizeof tlb); }
    void flush() { memset(tlb, 0, sizeof tlb); }
};

static std::vector<Core> cores;
static u64 mtime_now = 0;      // 5,000 a second, as the shader machine's
static double guest_seconds = 0;
static u32 rng_state = 0x2545f491;

struct Trap { bool on = false; u32 cause = 0, value = 0; };

static u32 rom_word(u32 offset) {
    u32 v = 0;
    if (offset + 4 <= rom.size()) memcpy(&v, &rom[offset], 4);
    return v;
}

// ---- memory: Sv32 ----

static bool walk(Core& c, u32 va, int access, u32& phys, Trap& t) {
    u32 fault = access == FETCH ? 12 : access == READ ? 13 : 15;
    if (!(c.satp >> 31)) { phys = va; return true; }
    bool sum = (c.mstatus >> 18) & 1, mxr = (c.mstatus >> 19) & 1;
    u32 table = (c.satp & 0x3fffff) << 12;
    for (int level = 1; level >= 0; level--) {
        u32 at = table + (((va >> (12 + 10 * level)) & 0x3ff) << 2);
        if (at < RAM_BASE || at >= RAM_BASE + RAM_SIZE) break;
        u32 pte = word(at);
        if (!(pte & 1) || ((pte & 6) == 4)) break;
        if (pte & 0xa) {   // a leaf
            if (level == 1 && (pte & 0xffc00)) break;   // a misaligned megapage
            bool user = (pte >> 4) & 1;
            if (c.priv == PRIV_U ? !user : (user && (access == FETCH || !sum))) break;
            if (access == FETCH ? !(pte & 8) : access == READ ? !((pte & 2) || (mxr && (pte & 8))) : !(pte & 4)) break;
            u32 want = 0x40 | (access == WRITE ? 0x80 : 0);
            if ((pte & want) != want) { word(at) = pte | want; dirty(at); }
            u32 ppn = pte >> 10;
            phys = level == 1 ? ((ppn << 12) | (va & 0x3fffff)) : ((ppn << 12) | (va & 0xfff));
            return true;
        }
        table = (pte >> 10) << 12;
    }
    t.on = true, t.cause = fault, t.value = va;
    return false;
}

// The page a virtual address is on, through the core's own table of the last ones.
static inline bool page_of(Core& c, u32 va, int access, u32& phys, Trap& t) {
    u32 vpn = va >> 12;
    TlbEntry& e = c.tlb[c.priv][access][vpn & (TLB_N - 1)];
    if (e.vpn == vpn + 1) { phys = e.ppage | (va & 0xfff); return true; }
    if (!walk(c, va, access, phys, t)) return false;
    // (kept only for RAM, and for a write only once the page is marked written: walk did that)
    if (phys >= RAM_BASE && phys < RAM_BASE + RAM_SIZE) { e.vpn = vpn + 1; e.ppage = phys & ~0xfffu; }
    return true;
}

static u32 rtc0 = 0, rtc1 = 0;

static u32 device_read(Core& c, u32 phys) {
    if (phys >= ROM_BASE && phys < ROM_BASE + 0x20000000u) {
        // (any four bytes of it, at any address: the kernel's own copies are not aligned)
        u32 v = 0, at = phys - ROM_BASE;
        if (at < rom.size()) memcpy(&v, &rom[at], rom.size() - at < 4 ? rom.size() - at : 4);
        return v;
    }
    switch (phys & ~3u) {
    case 0x02004000: return (u32)c.timecmp;
    case 0x02004004: return (u32)(c.timecmp >> 32);
    case 0x0200bff8: return (u32)mtime_now;
    case 0x0200bffc: return (u32)(mtime_now >> 32);
    case 0x10000000: return 0;
    case 0x10000004: return ((0x60u << 8) >> ((phys & 3) * 8));   // line status: the transmitter is free
    case 0x030007f8: return rtc0 >> ((phys & 3) * 8);
    case 0x030007fc: return rtc1 >> ((phys & 3) * 8);
    }
    return 0;
}

static void device_write(Core& c, u32 phys, u32 value, int size) {
    (void)size;
    switch (phys) {
    case 0x02004000: c.timecmp = (c.timecmp & 0xffffffff00000000ull) | value; c.mip &= ~0x80u; break;
    case 0x02004004: c.timecmp = (c.timecmp & 0xffffffffull) | ((u64)value << 32); break;
    case 0x10000000: console_put(value & 0xff); break;
    }
}

static inline bool load(Core& c, u32 va, int size, u32& value, Trap& t) {
    u32 phys;
    if (((va & 0xfff) + size) > 0x1000) {   // across two pages: a byte at a time
        value = 0;
        for (int i = 0; i < size; i++) {
            u32 b;
            if (!load(c, va + i, 1, b, t)) return false;
            value |= b << (8 * i);
        }
        return true;
    }
    if (!page_of(c, va, READ, phys, t)) return false;
    if (phys - RAM_BASE < RAM_SIZE) {
        const uint8_t* p = &ram[phys - RAM_BASE];
        if (size == 4) memcpy(&value, p, 4);
        else if (size == 2) { uint16_t h; memcpy(&h, p, 2); value = h; }
        else value = *p;
        return true;
    }
    value = device_read(c, phys);
    if (size < 4) value &= (1u << (8 * size)) - 1;
    return true;
}

static inline bool store(Core& c, u32 va, int size, u32 value, Trap& t) {
    u32 phys;
    if (((va & 0xfff) + size) > 0x1000) {
        for (int i = 0; i < size; i++)
            if (!store(c, va + i, 1, value >> (8 * i), t)) return false;
        return true;
    }
    if (!page_of(c, va, WRITE, phys, t)) return false;
    if (phys - RAM_BASE < RAM_SIZE) {
        uint8_t* p = &ram[phys - RAM_BASE];
        dirty(phys);
        if (size == 4) memcpy(p, &value, 4);
        else if (size == 2) { uint16_t h = (uint16_t)value; memcpy(p, &h, 2); }
        else *p = (uint8_t)value;
        return true;
    }
    device_write(c, phys, value, size);
    return true;
}

// ---- the firmware's calls (experiments/rvc_opt/src/emu.h, sbi_call) ----

static u64 sbi_counts[16];
static void sbi_call(Core& c) {
    u32 ext = c.x[17], a0 = c.x[10], error = 0, value = 0;
    sbi_counts[ext == 0x54494D45 ? 0 : ext < 16 ? ext : 15]++;
    if (ext == 0x54494D45 || ext == 0) {
        c.timecmp = (u64)a0 | ((u64)c.x[11] << 32);
        c.mip &= ~0x20u;
    } else if (ext == 1) {
        console_put(a0 & 0xff);
    } else if (ext == 2) {
        if (!console_in.empty()) { value = console_in.front(); console_in.pop_front(); }
        else value = 0xffffffff;
    } else if (ext == 0x10) {
        u32 fn = c.x[16];
        value = fn == 0 ? 2 : fn == 1 ? 0x454d55 : fn == 2 ? 1 : (fn == 3 && (a0 == 0x54494D45 || a0 <= 2)) ? 1 : 0;
    } else {
        error = 0xfffffffe;
    }
    if (ext < 0x10) c.x[10] = value;
    else c.x[10] = error, c.x[11] = value;
}

// ---- control and status registers ----

static const u32 SSTATUS_MASK = 0x000de162;

static bool csr_read(Core& c, u32 n, u32& v) {
    switch (n) {
    case 0x001: v = c.fcsr & 0x1f; return true;
    case 0x002: v = (c.fcsr >> 5) & 7; return true;
    case 0x003: v = c.fcsr & 0xff; return true;
    case 0x100: v = c.mstatus & SSTATUS_MASK; if (v & 0x6000) v |= 0x80006000; return true;
    case 0x102: v = c.sedeleg; return true;
    case 0x103: v = c.sideleg; return true;
    case 0x104: v = c.mie & 0x222; return true;
    case 0x105: v = c.stvec; return true;
    case 0x106: v = c.scounteren; return true;
    case 0x140: v = c.sscratch; return true;
    case 0x141: v = c.sepc; return true;
    case 0x142: v = c.scause; return true;
    case 0x143: v = c.stval; return true;
    case 0x144: v = c.mip & 0x222; return true;
    case 0x180: v = c.satp; return true;
    case 0xc00: case 0xc02: v = (u32)c.clock; return true;
    case 0xc80: case 0xc82: v = (u32)(c.clock >> 32); return true;
    case 0xc01: v = (u32)mtime_now; return true;
    case 0xc81: v = (u32)(mtime_now >> 32); return true;
    case 0x0b0: v = 0; return true;
    case 0x0b1: v = c.memop_src; return true;
    case 0x0b2: v = c.memop_dst; return true;
    case 0x0b3: v = c.memop_n; return true;
    case 0x0bf: rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; v = rng_state; return true;
    case 0x0be: v = 0; return true;
    case 0x0c0: case 0x0c1: case 0x0c2: case 0x0c3: v = 0; return true;   // the network card: none
    }
    return false;
}

static void memop(Core& c, u32 what, Trap& t);

static bool csr_write(Core& c, u32 n, u32 v, Trap& t) {
    switch (n) {
    case 0x001: c.fcsr = (c.fcsr & ~0x1fu) | (v & 0x1f); return true;
    case 0x002: c.fcsr = (c.fcsr & ~0xe0u) | ((v & 7) << 5); return true;
    case 0x003: c.fcsr = v & 0xff; return true;
    case 0x100: {
        u32 old = c.mstatus;
        c.mstatus = (c.mstatus & ~SSTATUS_MASK) | (v & SSTATUS_MASK);
        c.recheck = true;
        if ((old ^ c.mstatus) & 0xc0000) c.flush();   // SUM and MXR change what a page allows
        return true;
    }
    case 0x102: c.sedeleg = v; return true;
    case 0x103: c.sideleg = v; return true;
    case 0x104: c.mie = (c.mie & ~0x222u) | (v & 0x222); c.recheck = true; return true;
    case 0x105: c.stvec = v; return true;
    case 0x106: c.scounteren = v; return true;
    case 0x140: c.sscratch = v; return true;
    case 0x141: c.sepc = v; return true;
    case 0x142: c.scause = v; return true;
    case 0x143: c.stval = v; return true;
    case 0x144: c.mip = (c.mip & ~0x222u) | (v & 0x222); c.recheck = true; return true;
    case 0x180: c.satp = v & 0x803fffff; c.flush(); return true;
    case 0x0b1: c.memop_src = v; return true;
    case 0x0b2: c.memop_dst = v; return true;
    case 0x0b3: c.memop_n = v; return true;
    case 0x0b0: memop(c, v, t); return true;
    case 0x0c0: case 0x0c1: case 0x0c2: case 0x0c3: return true;
    }
    return false;
}

// The parallel copy (CSRs 0x0b0 to 0x0b3): whole words from one place to another, or one word
// over a range, made between two passes. The source may be the ROM.
static void memop(Core& c, u32 what, Trap& t) {
    u32 dst = 0, src = 0;
    if (what != 2 && !walk(c, c.memop_src, READ, src, t)) return;
    if (!walk(c, c.memop_dst, WRITE, dst, t)) return;
    // whole words, as the machine's commit makes them: a word is written when its address is in the range
    u32 words = (c.memop_n + 3) / 4, fill = c.memop_src;
    dst &= ~3u;
    src &= ~3u;
    for (u32 k = 0; k < words; k++) {
        u32 to = dst + 4 * k, from = src + 4 * k, value = fill;
        if (to - RAM_BASE >= RAM_SIZE) break;
        if (what != 2) {
            if (from - RAM_BASE < RAM_SIZE) memcpy(&value, &ram[from - RAM_BASE], 4);
            else value = rom_word(from - ROM_BASE);
        }
        memcpy(&ram[to - RAM_BASE], &value, 4);
        dirty(to);
    }
    c.end_pass = true;
}

// ---- traps ----

static u64 trap_counts[2][16];   // exceptions and interrupts, by cause

static void take_trap(Core& c, u32 cause, u32 value, u32 pc) {
    trap_counts[cause >> 31][cause & 15]++;
    c.scause = cause;
    c.stval = value;
    c.sepc = pc;
    u32 sie = (c.mstatus >> 1) & 1;
    c.mstatus = (c.mstatus & ~0x122u) | (sie << 5) | ((c.priv & 1) << 8);
    c.priv = PRIV_S;
    c.pc = (c.stvec & 1) && (cause >> 31) ? (c.stvec & ~3u) + 4 * (cause & 0xffff) : (c.stvec & ~3u);
    c.reserved = false;
}

// An interrupt that is pending, enabled and allowed now; its cause, or 0.
static inline u32 pending_interrupt(Core& c) {
    if (c.timecmp && mtime_now >= c.timecmp) c.mip |= 0x20;
    u32 ready = c.mip & c.mie & 0x222;
    if (!ready) return 0;
    if (c.priv == PRIV_S && !((c.mstatus >> 1) & 1)) return 0;
    return ready & 0x200 ? 0x80000009u : ready & 0x002 ? 0x80000001u : 0x80000005u;
}

// ---- floats (docs/fpu.md): the host's own, which are IEEE single precision too ----

static inline float fl(u32 bits) { float v; memcpy(&v, &bits, 4); return v; }
static inline u32 bits(float v) { u32 b; memcpy(&b, &v, 4); return b; }
static inline bool is_nan(u32 b) { return (b & 0x7f800000) == 0x7f800000 && (b & 0x7fffff); }
static inline u32 canon(float v) { u32 b = bits(v); return is_nan(b) ? 0x7fc00000u : b; }

static u32 to_int(u32 b, bool is_unsigned) {
    float v = fl(b);
    if (is_nan(b)) return is_unsigned ? 0xffffffffu : 0x7fffffffu;
    if (is_unsigned) {
        if (v <= -1.0f) return 0;
        if (v >= 4294967296.0f) return 0xffffffffu;
        return (u32)(i64)v;
    }
    if (v >= 2147483648.0f) return 0x7fffffffu;
    if (v < -2147483648.0f) return 0x80000000u;
    return (u32)(i32)v;
}

static float round_mode(float v, u32 rm) {
    switch (rm) {
    case 0: return nearbyintf(v);   // to nearest, ties to even (the host's mode)
    case 1: return truncf(v);
    case 2: return floorf(v);
    case 3: return ceilf(v);
    case 4: return roundf(v);
    }
    return nearbyintf(v);
}

static u32 fclass(u32 b) {
    u32 sign = b >> 31, e = (b >> 23) & 0xff, m = b & 0x7fffff;
    if (e == 0xff) return m ? (m & 0x400000 ? 0x200 : 0x100) : (sign ? 0x001 : 0x080);
    if (e == 0) return m ? (sign ? 0x004 : 0x020) : (sign ? 0x008 : 0x010);
    return sign ? 0x002 : 0x040;
}

bool mc_fault(Core& c, const Trap& t);
static u32 last_pc[32], last_ins[32];   // core 0's last instructions, for the summary of a run that stopped oddly

// ---- one pass of a core: up to `budget` instructions ----

static u64 run(Core& c, u64 budget) {
    u64 done = 0;
    Trap t;
    c.end_pass = false;
    while (done < budget && !c.end_pass) {
        u32 irq = pending_interrupt(c);
        if (irq) {
            c.waiting = false;
            c.mip &= irq == 0x80000005u ? ~0u : ~(1u << (irq & 31));   // (the timer's stays until a new deadline)
            take_trap(c, irq, 0, c.pc);
        }
        // wfi ends when an interrupt is pending and enabled, taken or not (Linux waits with
        // interrupts off and switches them on afterwards)
        if (c.waiting && (c.mip & c.mie & 0x222)) c.waiting = false;
        if (c.waiting) break;
        // a run with no interrupt check: until the timer could fire, at most
        u64 chunk = budget - done;
        if (chunk > 256) chunk = 256;
        for (u64 k = 0; k < chunk && !c.end_pass; k++) {
            u32 pc = c.pc, phys, ins;
            t.on = false;
            if (!page_of(c, pc, FETCH, phys, t)) { take_trap(c, t.cause, t.value, pc); done++; c.clock++; break; }
            if (phys - RAM_BASE >= RAM_SIZE) { take_trap(c, 1, pc, pc); done++; c.clock++; break; }
            memcpy(&ins, &ram[phys - RAM_BASE], 4);
            done++;
            c.clock++;
            if (c.hart == 0) { last_pc[c.clock & 31] = pc; last_ins[c.clock & 31] = ins; }
            u32 op = ins & 0x7f, rd = (ins >> 7) & 31, f3 = (ins >> 12) & 7, rs1 = (ins >> 15) & 31, rs2 = (ins >> 20) & 31, f7 = ins >> 25;
            u32 a = c.x[rs1], b = c.x[rs2], next = pc + 4, result = 0;
            bool write = true, illegal = false;
            switch (op) {
            case 0x37: result = ins & 0xfffff000; break;
            case 0x17: result = pc + (ins & 0xfffff000); break;
            case 0x6f: {
                u32 imm = ((i32)ins >> 31 << 20) | (ins & 0xff000) | ((ins >> 9) & 0x800) | ((ins >> 20) & 0x7fe);
                result = pc + 4, next = pc + imm;
                break;
            }
            case 0x67: result = pc + 4, next = (a + ((i32)ins >> 20)) & ~1u; break;
            case 0x63: {
                u32 imm = ((i32)ins >> 31 << 12) | ((ins << 4) & 0x800) | ((ins >> 20) & 0x7e0) | ((ins >> 7) & 0x1e);
                bool take = false;
                switch (f3) {
                case 0: take = a == b; break;
                case 1: take = a != b; break;
                case 4: take = (i32)a < (i32)b; break;
                case 5: take = (i32)a >= (i32)b; break;
                case 6: take = a < b; break;
                case 7: take = a >= b; break;
                default: illegal = true;
                }
                if (take) next = pc + imm;
                write = false;
                break;
            }
            case 0x03: {
                u32 va = a + ((i32)ins >> 20), v;
                int size = f3 & 3;
                if (size == 3 || f3 == 7 || f3 == 6) { illegal = true; break; }
                if (!load(c, va, 1 << size, v, t)) break;
                result = f3 == 0 ? (u32)(i32)(int8_t)v : f3 == 1 ? (u32)(i32)(int16_t)v : v;
                break;
            }
            case 0x23: {
                u32 va = a + (((i32)ins >> 25 << 5) | rd);
                if (f3 > 2) { illegal = true; break; }
                store(c, va, 1 << f3, b, t);
                write = false;
                break;
            }
            case 0x13: {
                u32 imm = (i32)ins >> 20, sh = rs2;
                switch (f3) {
                case 0: result = a + imm; break;
                case 1: result = a << sh; break;
                case 2: result = (i32)a < (i32)imm; break;
                case 3: result = a < imm; break;
                case 4: result = a ^ imm; break;
                case 5: result = f7 & 0x20 ? (u32)((i32)a >> sh) : a >> sh; break;
                case 6: result = a | imm; break;
                case 7: result = a & imm; break;
                }
                break;
            }
            case 0x33:
                if (f7 == 1) {
                    switch (f3) {
                    case 0: result = a * b; break;
                    case 1: result = (u32)(((i64)(i32)a * (i64)(i32)b) >> 32); break;
                    case 2: result = (u32)(((i64)(i32)a * (i64)(u64)b) >> 32); break;
                    case 3: result = (u32)(((u64)a * (u64)b) >> 32); break;
                    case 4: result = b == 0 ? 0xffffffffu : (a == 0x80000000u && b == 0xffffffffu) ? a : (u32)((i32)a / (i32)b); break;
                    case 5: result = b == 0 ? 0xffffffffu : a / b; break;
                    case 6: result = b == 0 ? a : (a == 0x80000000u && b == 0xffffffffu) ? 0 : (u32)((i32)a % (i32)b); break;
                    case 7: result = b == 0 ? a : a % b; break;
                    }
                } else {
                    switch (f3) {
                    case 0: result = f7 & 0x20 ? a - b : a + b; break;
                    case 1: result = a << (b & 31); break;
                    case 2: result = (i32)a < (i32)b; break;
                    case 3: result = a < b; break;
                    case 4: result = a ^ b; break;
                    case 5: result = f7 & 0x20 ? (u32)((i32)a >> (b & 31)) : a >> (b & 31); break;
                    case 6: result = a | b; break;
                    case 7: result = a & b; break;
                    }
                }
                break;
            case 0x0f:
                write = false;
                if (ins == 0x0100000f) c.end_pass = true;   // pause: the pass ends (docs/gpu.md)
                break;
            case 0x2f: {   // atomics, on words
                u32 kind = f7 >> 2, v;
                if (f3 != 2) { illegal = true; break; }
                if (kind == 2) {   // lr
                    if (!load(c, a, 4, v, t)) break;
                    c.reserved = true, c.reservation = a;
                    result = v;
                } else if (kind == 3) {   // sc
                    if (c.reserved && c.reservation == a) {
                        if (!store(c, a, 4, b, t)) break;
                        result = 0;
                    } else result = 1;
                    c.reserved = false;
                } else {
                    if (!load(c, a, 4, v, t)) break;
                    u32 n;
                    switch (kind) {
                    case 1: n = b; break;
                    case 0: n = v + b; break;
                    case 4: n = v ^ b; break;
                    case 12: n = v & b; break;
                    case 8: n = v | b; break;
                    case 16: n = (i32)v < (i32)b ? v : b; break;
                    case 20: n = (i32)v > (i32)b ? v : b; break;
                    case 24: n = v < b ? v : b; break;
                    case 28: n = v > b ? v : b; break;
                    default: n = v; illegal = true;
                    }
                    if (illegal || !store(c, a, 4, n, t)) break;
                    result = v;
                }
                break;
            }
            case 0x73: {
                if (f3 == 0) {
                    write = false;
                    if (ins == 0x00000073) {   // ecall
                        if (c.priv == PRIV_S) sbi_call(c);
                        else t.on = true, t.cause = 8, t.value = 0;
                    } else if (ins == 0x00100073) {
                        t.on = true, t.cause = 3, t.value = pc;
                    } else if (ins == 0x10200073 && c.priv == PRIV_S) {   // sret
                        u32 spie = (c.mstatus >> 5) & 1, spp = (c.mstatus >> 8) & 1;
                        c.mstatus = (c.mstatus & ~0x122u) | (spie << 1) | 0x20;
                        c.priv = spp;
                        next = c.sepc;
                        c.reserved = false;
                        c.recheck = true;
                    } else if (ins == 0x10500073 && c.priv == PRIV_S) {   // wfi
                        c.waiting = true;
                        c.end_pass = true;
                    } else if (f7 == 0x09 && c.priv == PRIV_S) {   // sfence.vma
                        c.flush();
                    } else illegal = true;
                    break;
                }
                u32 n = ins >> 20, old = 0, src = f3 & 4 ? rs1 : a;
                if (((n >> 8) & 3) > c.priv || !csr_read(c, n, old)) {
                    // the counters and the machine's own registers may be read by anyone
                    if (!(n >= 0xc00 && n <= 0xc82 && csr_read(c, n, old)) && !((n & 0xff0) == 0x0b0 && csr_read(c, n, old))) { illegal = true; break; }
                }
                bool writes = (f3 & 3) == 1 || rs1 != 0;
                if (writes) {
                    u32 v = (f3 & 3) == 1 ? src : (f3 & 3) == 2 ? old | src : old & ~src;
                    if ((n >> 10) == 3 || !csr_write(c, n, v, t)) { illegal = true; break; }
                }
                result = old;
                break;
            }
            // ---- floats ----
            case 0x07:   // flw
            case 0x27: { // fsw
                u32 va, v;
                if (f3 != 2 || !(c.mstatus & 0x6000)) { illegal = true; break; }
                write = false;
                if (op == 0x07) {
                    va = a + ((i32)ins >> 20);
                    if (load(c, va, 4, v, t)) c.f[rd] = v;
                } else {
                    va = a + (((i32)ins >> 25 << 5) | rd);
                    store(c, va, 4, c.f[rs2], t);
                }
                break;
            }
            case 0x43: case 0x47: case 0x4b: case 0x4f: {   // fmadd, fmsub, fnmsub, fnmadd
                if (!(c.mstatus & 0x6000) || (f7 & 3)) { illegal = true; break; }
                float x = fl(c.f[rs1]), y = fl(c.f[rs2]), z = fl(c.f[ins >> 27]);
                float r = op == 0x43 ? fmaf(x, y, z) : op == 0x47 ? fmaf(x, y, -z) : op == 0x4b ? fmaf(-x, y, z) : fmaf(-x, y, -z);
                c.f[rd] = canon(r);
                write = false;
                break;
            }
            case 0x53: {
                if (!(c.mstatus & 0x6000)) { illegal = true; break; }
                u32 fa = c.f[rs1], fb = c.f[rs2], rm = f3 == 7 ? (c.fcsr >> 5) & 7 : f3;
                float x = fl(fa), y = fl(fb);
                write = false;
                switch (f7) {
                case 0x00: c.f[rd] = canon(x + y); break;
                case 0x04: c.f[rd] = canon(x - y); break;
                case 0x08: c.f[rd] = canon(x * y); break;
                case 0x0c: c.f[rd] = canon(x / y); break;
                case 0x2c: c.f[rd] = canon(sqrtf(x)); break;
                case 0x10: c.f[rd] = f3 == 0 ? (fa & 0x7fffffff) | (fb & 0x80000000) : f3 == 1 ? (fa & 0x7fffffff) | (~fb & 0x80000000) : fa ^ (fb & 0x80000000); break;
                case 0x14: {
                    bool an = is_nan(fa), bn = is_nan(fb);
                    if (an && bn) c.f[rd] = 0x7fc00000;
                    else if (an) c.f[rd] = fb;
                    else if (bn) c.f[rd] = fa;
                    else if (x == y) c.f[rd] = f3 == 0 ? (fa | fb) : (fa & fb);   // -0 is under +0
                    else c.f[rd] = (f3 == 0) == (x < y) ? fa : fb;
                    break;
                }
                case 0x60: result = to_int(bits(round_mode(x, rm)), rs2 == 1); if (is_nan(fa)) result = rs2 == 1 ? 0xffffffffu : 0x7fffffffu; write = true; break;
                case 0x68: c.f[rd] = bits(rs2 == 1 ? (float)a : (float)(i32)a); break;
                case 0x70: result = f3 == 0 ? fa : fclass(fa); write = true; break;
                case 0x78: c.f[rd] = a; break;
                case 0x50: result = f3 == 2 ? x == y : f3 == 1 ? x < y : x <= y; write = true; break;
                default: illegal = true;
                }
                break;
            }
            default: illegal = true;
            }
            if (illegal) t.on = true, t.cause = 2, t.value = ins;
            if (t.on) {
                // a worker core in user mode does not trap: it stops where it is (mc_fault)
                if (c.hart == 0 || !mc_fault(c, t)) take_trap(c, t.cause, t.value, pc);
                break;
            }
            if (write && rd) c.x[rd] = result;
            c.pc = next;
            if (c.recheck) {
                // (the kernel's idle loop lets interrupts in for three instructions at a time)
                c.recheck = false;
                break;
            }
        }
    }
    return done;
}

// ---- the worker cores (experiments/rvc_opt/src/mc.h, docs/multicore.md) ----
//
// A worker is parked until core 0 writes a start texel in the mailbox page: "MCSU", a pc, a
// stack and a page table. It then runs in user mode on that page table; `pause` puts it to
// sleep until its job word changes; a fault or a system call stops it and leaves a record
// for core 0, which answers through the resume texel.
static const u32 MC_MBOX = 0x86C00000u, MC_START = 0x5553434Du, MC_STOP = 0x5453434Du;
static inline u32 mc_start_at(int hart) { return MC_MBOX + 16 * hart; }
static inline u32 mc_job_at(int hart) { return MC_MBOX + 0x400 + 64 * hart; }
static inline u32 mc_fault_at(int hart) { return MC_MBOX + 0x400 + 64 * hart + 32; }
static inline u32 mc_resume_at(int hart) { return MC_MBOX + 0x400 + 64 * hart + 48; }
static inline u32 mc_call_at(int hart) { return MC_MBOX + 0x800 + 32 * hart; }
enum { MC_PARKED, MC_RUNNING, MC_ASLEEP, MC_FAULTED, MC_CALLING };
struct Worker { int state = MC_PARKED; u32 seen = 0, start_seen = 0, job_seen = 0; };
static std::vector<Worker> workers;

bool mc_fault(Core& c, const Trap& t) {
    Worker& w = workers[c.hart];
    u32 seen = word(mc_resume_at(c.hart));
    bool call = t.cause == 8;
    if (call) {
        word(mc_call_at(c.hart)) = c.x[17];
        for (int i = 0; i < 6; i++) word(mc_call_at(c.hart) + 4 + 4 * i) = c.x[10 + i];
    }
    word(mc_fault_at(c.hart) + 4) = t.cause;
    word(mc_fault_at(c.hart) + 8) = t.value;
    word(mc_fault_at(c.hart) + 12) = c.pc;
    word(mc_fault_at(c.hart)) = seen + 1;
    dirty(MC_MBOX);
    w.state = call ? MC_CALLING : MC_FAULTED;
    w.seen = seen;
    c.end_pass = true;
    return true;
}

static u64 run_worker(Core& c, u64 budget) {
    Worker& w = workers[c.hart];
    u32 start = word(mc_start_at(c.hart));
    if (start == MC_STOP) { w.state = MC_PARKED; c.started = false; return 0; }
    if (w.state == MC_PARKED) {
        if (start != MC_START) return 0;
        // a start: its texel is pc, stack, page table; a0 is the core's number
        u32 pc = word(mc_start_at(c.hart) + 4);
        if (pc == w.start_seen && c.started) return 0;
        memset(c.x, 0, sizeof c.x);
        c.pc = pc;
        c.x[2] = word(mc_start_at(c.hart) + 8);
        c.satp = 0x80000000u | word(mc_start_at(c.hart) + 12);
        c.x[10] = (u32)c.hart;
        c.priv = PRIV_U;
        c.mstatus = 0x6000;   // floats on
        c.flush();
        c.started = true;
        w.start_seen = pc;
        w.state = MC_RUNNING;
    }
    if (w.state == MC_ASLEEP) {
        if (word(mc_job_at(c.hart)) == w.job_seen) return 0;
        w.state = MC_RUNNING;
    }
    if (w.state == MC_FAULTED || w.state == MC_CALLING) {
        if ((word(mc_resume_at(c.hart)) & 0xffffff) == (w.seen & 0xffffff)) return 0;
        if (w.state == MC_CALLING) {
            c.x[10] = word(mc_resume_at(c.hart) + 4);
            c.pc += 4;
        }
        c.flush();   // core 0 has put a page there
        w.state = MC_RUNNING;
    }
    u64 before = c.clock;
    u64 done = 0;
    while (done < budget && w.state == MC_RUNNING) {
        u32 pc = c.pc, phys, ins = 0;
        Trap t;
        if (page_of(c, pc, FETCH, phys, t) && phys - RAM_BASE < RAM_SIZE) memcpy(&ins, &ram[phys - RAM_BASE], 4);
        if (ins == 0x0100000f) {   // pause: asleep until the job word changes
            w.job_seen = word(mc_job_at(c.hart));
            w.state = MC_ASLEEP;
            c.pc += 4;
            break;
        }
        if (ins == 0x00100073) {   // ebreak parks it
            w.state = MC_PARKED;
            break;
        }
        done += run(c, 1);
    }
    (void)before;
    return done;
}

// ---- the host's part of a pass: the control words ----

static u32 key_seq = 0;
static u64 frame_no = 0;

static void control_pass() {
    dirty(CTRL);
    if (external_devices) {
        // the shader's control pass keeps these words; between two of its runs only the clock moves
        word(CTRL + 0x34) = (u32)(guest_seconds * 1000.0);
        return;
    }
    // a submitted list has been drawn: the GPU is free, and the copy it asked for is counted
    u32 submit = word(CTRL + 0x10);
    if (submit) {
        word(CTRL + 0x10) = 0;
        word(CTRL + 0x1c)++;
        if ((submit & 1) && (submit & 6)) word(CTRL + 0x38)++;
    }
    word(CTRL + 0x34) = (u32)(guest_seconds * 1000.0);
    word(CTRL + 0x3c) = (opt.desktop ? 1u : 0u) | (opt.tabs ? 2u : 0u) | (128u << 8) | (128u << 16);
    u32 width = word(CTRL + 4), height = word(CTRL + 8);
    if (opt.sweep >= 0 && (long long)frame_no >= opt.sweep && width && height) {
        // (as rvc_harness --pointer-sweep: round the middle of the display)
        double turn = guest_seconds * 2.0;
        word(CTRL + 0x20) = (u32)(width / 2 + width / 10 * cos(turn));
        word(CTRL + 0x24) = (u32)(height / 2 + height / 10 * sin(turn));
    }
    word(CTRL + 0x2c) = key_seq;
    // what each core ran, and how many there are (docs/multicore.md)
    for (size_t i = 0; i < cores.size() && i < 16; i++) word(CTRL + 0x380 + 4 * (u32)i) = (u32)cores[i].clock;
    if (cores.size() > 1) {
        u32 running = 1, asleep = 0;
        for (size_t i = 1; i < cores.size(); i++) {
            running |= (workers[i].state != MC_PARKED ? 1u : 0u) << i;
            asleep |= (workers[i].state == MC_ASLEEP ? 1u : 0u) << i;
        }
        word(CTRL + 0x3c0) = (u32)cores.size();
        word(CTRL + 0x3c4) = running;
        word(CTRL + 0x3c8) = asleep;
    }
}

static bool read_file(const std::string& path, std::vector<uint8_t>& to) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    to.resize((size_t)n);
    size_t got = fread(to.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

// The machine at power-on: the image's RAM and ROM, core 0 at the kernel.
static bool machine_boot(const std::string& image, int count) {
    std::vector<uint8_t> payload;
    if (!read_file(image + "/linux_payload.bin", payload) || !read_file(image + "/rootfs.bin", rom)) return false;
    ram.assign(RAM_SIZE, 0);
    ramw = (u32*)ram.data();
    memcpy(ram.data(), payload.data(), payload.size() < RAM_SIZE ? payload.size() : RAM_SIZE);
    dirty_bands = 0xffffffffu;
    count = count < 1 ? 1 : count > 16 ? 16 : count;
    cores.clear();
    workers.clear();
    cores.resize((size_t)count);
    workers.resize((size_t)count);
    for (int i = 0; i < count; i++) cores[(size_t)i].hart = i;
    cores[0].pc = 0x80400000;      // the kernel, in supervisor mode, with the device tree in a1
    cores[0].x[11] = 0x82200000;
    cores[0].priv = PRIV_S;
    return true;
}

// One pass of every core, after the host's part of it; the most instructions a core ran.
static u64 worker_instr = 0;
static u64 machine_pass(u32 ticks) {
    control_pass();
    mtime_now = (u64)(guest_seconds * 5000.0);
    u64 most = run(cores[0], ticks);
    for (size_t i = 1; i < cores.size(); i++) {
        u64 n = run_worker(cores[i], ticks);
        worker_instr += n;
        if (n > most) most = n;
    }
    frame_no++;
    return most;
}

#ifndef RVC_CPU_LIBRARY
static std::string unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[++i];
            out.push_back(n == 'n' ? '\n' : n == 'r' ? '\r' : n == 't' ? '\t' : n);
        } else out.push_back(s[i]);
    }
    return out;
}

int main(int argc, char** argv) {
    std::string pending;
    bool have_pending = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", name); exit(2); }
            return argv[++i];
        };
        if (a == "--image") { std::string v = next("--image"); if (v != "linux-net") opt.image = v; }
        else if (a == "--expect") { pending = next("--expect"); have_pending = true; }
        else if (a == "--send") { std::string s = unescape(next("--send")); opt.script.push_back({have_pending ? pending : "", s}); have_pending = false; }
        else if (a == "--until") opt.until = next("--until");
        else if (a == "--seconds") opt.seconds = atof(next("--seconds").c_str());
        else if (a == "--wall") opt.wall = atof(next("--wall").c_str());
        else if (a == "--instructions") opt.maxInstr = strtoull(next("--instructions").c_str(), nullptr, 10);
        else if (a == "--uart-log") opt.uartLog = next("--uart-log");
        else if (a == "--ticks") opt.ticks = (u32)atoi(next("--ticks").c_str());
        else if (a == "--ips") opt.ips = atof(next("--ips").c_str());
        else if (a == "--pass-ms") opt.passMs = atof(next("--pass-ms").c_str());
        else if (a == "--cores") opt.cores = atoi(next("--cores").c_str());
        else if (a == "--pointer-sweep") opt.sweep = atoll(next("--pointer-sweep").c_str());
        else if (a == "--desktop") opt.desktop = true;
        else if (a == "--tabs") opt.tabs = true;
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--no-stdin" || a == "--d3d11" || a == "--dxc") {}
        else if (a == "--rvc" || a == "--fixed-dt") next(a.c_str());   // (the shader machine's: taken and ignored)
        else if (a == "--help" || a == "-h") {
            printf("rvc_cpu: the machine as an interpreter, for counting instructions (docs/cpu-harness.md)\n"
                   "  --image DIR|linux-net  the image's folder (default build/images/linux)\n"
                   "  --expect T --send T    type the second text when the console has said the first\n"
                   "  --until T              stop when the console says this (after the script)\n"
                   "  --seconds S            stop after S seconds of the guest's time\n"
                   "  --wall S               or S of this computer's\n"
                   "  --instructions N       or N instructions of core 0\n"
                   "  --uart-log FILE        the console, added to the file\n"
                   "  --cores N              core 0 and N - 1 worker cores\n"
                   "  --ticks N              instructions a pass (16384; the world: 8192)\n"
                   "  --ips N --pass-ms M    the guest's clock: a pass takes M ms and its instructions 1/N s each\n"
                   "                         (3700000 and 0.25: the harness; the world is about 3100000 and 0.2)\n"
                   "  --pointer-sweep N      the pointer goes round from pass N on\n"
                   "  --desktop --tabs       the host flags of those names\n"
                   "  --quiet                do not print the console\n");
            return 0;
        } else { fprintf(stderr, "rvc_cpu: unknown option %s\n", a.c_str()); return 2; }
    }
    if (!machine_boot(opt.image, opt.cores)) {
        fprintf(stderr, "rvc_cpu: no image in %s (python tools\\make_linux_image.py)\n", opt.image.c_str());
        return 2;
    }
    if (!opt.uartLog.empty()) {
        uart_log = fopen(opt.uartLog.c_str(), "ab");
        if (uart_log) fprintf(uart_log, "\n=== rvc_cpu ===\n");
    }
    Core& c0 = cores[0];

    auto t0 = std::chrono::steady_clock::now();
    u64 idle_passes = 0;
    int exit_code = 0;
    for (;;) {
        u64 most = machine_pass(opt.ticks);
        guest_seconds += opt.passMs / 1000.0 + (double)most / opt.ips;
        if (most == 0 && c0.waiting && console_in.empty()) {
            // nothing to run: on to the timer's deadline (an idle machine's passes are all alike)
            idle_passes++;
            if (c0.timecmp > mtime_now) {
                double until = (double)c0.timecmp / 5000.0;
                if (until > guest_seconds && until < guest_seconds + 1.0) guest_seconds = until;
            }
        }
        if (until_hit) break;
        if (opt.seconds > 0 && guest_seconds >= opt.seconds) { exit_code = opt.until.empty() ? 0 : 3; break; }
        if (opt.maxInstr && c0.clock >= opt.maxInstr) { exit_code = opt.until.empty() ? 0 : 3; break; }
        if (opt.wall > 0 && (frame_no & 255) == 0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= opt.wall) { exit_code = opt.until.empty() ? 0 : 3; break; }
    }
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!opt.quiet) fputc('\n', stdout);
    for (size_t i = 0; i < cores.size(); i++)
        fprintf(stderr, "CORE %zu: pc %08x, %llu instructions\n", i, cores[i].pc, (unsigned long long)cores[i].clock);
    fprintf(stderr, "[cpu] %llu passes in %.1fs, %llu guest instructions (%llu on worker cores), %.1f s of guest time, %.1fM instructions/s here, --until %s\n",
            (unsigned long long)frame_no, wall, (unsigned long long)c0.clock, (unsigned long long)worker_instr, guest_seconds,
            (double)(c0.clock + worker_instr) / wall / 1e6, opt.until.empty() ? "not given" : until_hit ? "matched" : "NOT matched");
    fprintf(stderr, "[cpu] firmware calls: timer %llu, putchar %llu, getchar %llu, others %llu\n", (unsigned long long)sbi_counts[0],
            (unsigned long long)sbi_counts[1], (unsigned long long)sbi_counts[2], (unsigned long long)sbi_counts[15]);
    fprintf(stderr, "[cpu] traps:");
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 16; i++)
            if (trap_counts[k][i]) fprintf(stderr, " %s%d x %llu", k ? "interrupt " : "", i, (unsigned long long)trap_counts[k][i]);
    fprintf(stderr, "; sstatus %08x sie %x sip %x, timer at %llu of %llu\n", c0.mstatus, c0.mie, c0.mip, (unsigned long long)c0.timecmp, (unsigned long long)mtime_now);
    if (getenv("RVC_CPU_LAST"))
        for (u64 k = 1; k <= 32; k++)
            fprintf(stderr, "  %08x: %08x\n", last_pc[(c0.clock + k) & 31], last_ins[(c0.clock + k) & 31]);
    if (uart_log) fclose(uart_log);
    return exit_code;
}
#endif
