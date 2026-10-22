// gvm_cpu: programs for gpu_vm (gpu_vm/GPU_VM.md, gpu_vm/isa.h) on the host, around the reference machine
// (gpu_vm/ref/gvm_ref.h): passes of gvm_run and gvm_commit as the card makes them, an ELF or raw image loaded, and
// bare-metal semihosting (a syscall while TVEC is 0 is made by the host). The compiler's tests run here, and later
// Linux. gpu_vm/cpu/README.md has the options and the conventions.
//
//   gvm_cpu [options] PROGRAM.elf [ARGS...]
//   gvm_cpu [options] --bin FILE --at ADDR [--entry ADDR] [ARGS...]
#define _CRT_SECURE_NO_WARNINGS
#define GVM_REF_IMPL
#include "../ref/gvm_ref.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define host_read_fd _read
#else
#include <unistd.h>
#define host_read_fd read
#endif

#define CPU_MAX_CORES 64

// exit codes of gvm_cpu itself (a program's exit is its own code)
#define EXIT_USAGE 1      // bad options, a file that cannot be loaded
#define EXIT_TRAP 2       // an unhandled trap (break too), or every core waiting with nothing to wake it
#define EXIT_LIMIT 3      // --max-bundles reached

// errno values a semihosted call returns (negated), as Linux's
#define E_BADF 9
#define E_NOMEM 12
#define E_FAULT 14
#define E_INVAL 22
#define E_NOTTY 25
#define E_NOSYS 38

static struct {
    gvm_machine m;
    gvm_core *cores;
    uint32_t ncores, budget, pass_time;
    uint64_t max_bundles, bundles, passes;
    int trace, strace, stats;
    int stop, exit_code;
    uint32_t heap_start, brk, mmap_top, stack_limit;
    // the trace: what the bundle being traced found, to print what it wrote
    int tr_open;
    uint32_t tr_core, tr_x[64];
} H;

static void out_flush(void) { fflush(stdout); fflush(stderr); }

// ---- Guest memory as a core sees it (its write log first); the host's stores go to RAM and to the core's log entry
// for the word if it has one, so the core sees them at once and the commit does not undo them.
static int guest_read(gvm_core *c, uint32_t a, void *dst, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v;
        if (!gvm__load(&H.m, c, a + i, 1, &v)) return 0;
        d[i] = (uint8_t)v;
    }
    return 1;
}
static int guest_write(gvm_core *c, uint32_t a, const void *src, uint32_t n) {
    const uint8_t *s = (const uint8_t *)src;
    if (n && (!gvm__writable(&H.m, a) || !gvm__writable(&H.m, a + n - 1) || a + n - 1 < a)) return 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t b = a + i, word = b >> 2, sh = (b & 3) * 8, mask = ~(0xffu << sh);
        H.m.ram[word] = (H.m.ram[word] & mask) | (uint32_t)s[i] << sh;
        uint32_t *e = c ? gvm__wl_find(&c->wl, word, 0) : NULL;
        if (e) *e = (*e & mask) | (uint32_t)s[i] << sh;
    }
    return 1;
}
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

// ---- A disassembler for --trace and the trap report
static const char *int_name[31] = {"add", "and", "or", "xor", "sub", "andn", "orn", "xnor", "slt", "sltu", "min", "minu",
    "sge", "sgeu", "max", "maxu", "sll", "srl", "sra", "mul", "ror", "rol", "seq", "sne", "shadd", "sel", "addcy", "carry",
    "subby", "borrow", "bfe"};
static const char *flt_name[32] = {"fadd", "fmul", "fmad", "fmsb", "fsub", "fnmsb", "fmin", "fmax", "feq", "flt", "fle",
    "funord", "fsgnj", "fsgnjn", "fsgnjx", "ffrac", "itof", "utof", "ftoi", "ftou", "ffloor", "fceil", "ftrunc", "fround",
    "fdiv", "fsqrt", "frsqrt", "frcp", "fsin", "fcos", "fexp2", "flog2"};
static const char *width_name[8] = {"w", "d", "h", "b", "q", "?", "hu", "bu"};
static const char *amo_name[8] = {"swap", "add", "and", "or", "xor", "min", "max", "cas"};
static const char *ext_name(uint32_t opc) {
    static const char *n[0x1d] = {"ill", "mulh", "mulhu", "mulhsu", "div", "divu", "rem", "remu", "clz", "ctz", "popc",
        "bswap", "brev", "bfi", "lpc", 0, "syscall", "break", "wait", "sync", "fence", 0, 0, 0, "mfsr", "mtsr", "xsr",
        "srset", "srclr"};
    return opc < 0x1d && n[opc] ? n[opc] : 0;
}

static void dis_op(char *o, size_t sz, uint32_t w, int is_long, uint32_t limm) {
    uint32_t opc = GVM_OPC(w), rd = GVM_RD(w), ra = GVM_RA(w), rb = GVM_RB(w), rc = GVM_RC(w), f = opc & 31;
    int32_t imm = is_long ? (int32_t)limm : GVM_IMM12(w);
    char is[24];
    if (is_long) snprintf(is, sizeof is, "0x%08x", limm); else snprintf(is, sizeof is, "%d", imm);
    switch (GVM_CLASS(opc)) {
    case 1:
        if (f > 30) break;
        if (!GVM_IFORM(opc)) {
            if (f == GVM_I_SHADD) { snprintf(o, sz, "shadd r%u, r%u, r%u << %u", rd, ra, rb, rc & 31); return; }
            if (f >= GVM_I_SEL && f <= GVM_I_BORROW) { snprintf(o, sz, "%s r%u, r%u, r%u, r%u", int_name[f], rd, ra, rb, rc); return; }
            if (f > GVM_I_COMMON_LAST) break;
            snprintf(o, sz, "%s r%u, r%u, r%u", int_name[f], rd, ra, rb);
            return;
        }
        if (f == GVM_I_BFE) { snprintf(o, sz, "bfe r%u, r%u, pos %u, len %u%s", rd, ra, imm & 31, ((imm >> 5) & 31) + 1, imm & 0x400 ? ", signed" : ""); return; }
        if (f > GVM_I_COMMON_LAST) break;
        if (f == GVM_I_ADD && ra == 0) { snprintf(o, sz, "li r%u, %s", rd, is); return; }
        snprintf(o, sz, "%si r%u, r%u, %s", int_name[f], rd, ra, is);
        return;
    case 2: {
        uint32_t mode = (opc >> 3) & 7, wd = opc & 7;
        const char *wn = width_name[wd];
        switch (mode) {
        case GVM_M_LOAD: snprintf(o, sz, "l%s r%u, [r%u + %s]", wn, rd, ra, is); return;
        case GVM_M_STORE: snprintf(o, sz, "s%s r%u, [r%u + %s]", wn, rd, ra, is); return;
        case GVM_M_LOADX: snprintf(o, sz, "l%sx r%u, [r%u + r%u << %u]", wn, rd, ra, rb, rc & 31); return;
        case GVM_M_STOREX: snprintf(o, sz, "s%sx r%u, [r%u + r%u << %u]", wn, rd, ra, rb, rc & 31); return;
        case GVM_M_AMO: snprintf(o, sz, "amo%s r%u, [r%u], r%u", amo_name[wd], rd, ra, rb); return;
        case GVM_M_AMO2: if (wd <= 1) { snprintf(o, sz, "amo%s r%u, [r%u], r%u", wd ? "maxu" : "minu", rd, ra, rb); return; } break;
        }
        break;
    }
    case 3:
        if (GVM_IFORM(opc)) {   // the short immediate is a float's top 12 bits
            if (!is_long) snprintf(is, sizeof is, "0x%08x", (uint32_t)imm << 20);
            snprintf(o, sz, "%si r%u, r%u, %s", flt_name[f], rd, ra, is);
            return;
        }
        if (f == GVM_F_MAD || f == GVM_F_MSB || f == GVM_F_NMSB) { snprintf(o, sz, "%s r%u, r%u, r%u, r%u", flt_name[f], rd, ra, rb, rc); return; }
        snprintf(o, sz, "%s r%u, r%u, r%u", flt_name[f], rd, ra, rb);
        return;
    default: {
        const char *n = ext_name(opc);
        if (!n) break;
        switch (opc) {
        case GVM_X_SYSCALL: case GVM_X_BREAK: case GVM_X_WAIT: case GVM_X_SYNC: case GVM_X_FENCE: case GVM_X_ILL:
            snprintf(o, sz, "%s", n); return;
        case GVM_X_MFSR: snprintf(o, sz, "mfsr r%u, sr%d", rd, imm & 0xfff); return;
        case GVM_X_MTSR: snprintf(o, sz, "mtsr sr%d, r%u", imm & 0xfff, ra); return;
        case GVM_X_XSR: case GVM_X_SRSET: case GVM_X_SRCLR: snprintf(o, sz, "%s r%u, sr%d, r%u", n, rd, imm & 0xfff, ra); return;
        case GVM_X_BFI: snprintf(o, sz, "bfi r%u, r%u, pos %u, len %u", rd, ra, imm & 31, ((imm >> 5) & 31) + 1); return;
        case GVM_X_LPC: snprintf(o, sz, "lpc r%u, %s", rd, is); return;
        }
        if (opc >= GVM_X_CLZ && opc <= GVM_X_BREV) { snprintf(o, sz, "%s r%u, r%u", n, rd, ra); return; }
        snprintf(o, sz, "%s r%u, r%u, r%u", n, rd, ra, rb);
        return;
    }
    }
    snprintf(o, sz, ".word 0x%08x", w);
}

static void dis_ctl(char *o, size_t sz, uint32_t base, uint32_t ctl) {
    static const char *cc_name[16] = {0, "addbnz", "bltu", "bgeu", "beq", "bne", "blt", "bge", "bbs", "bbc", "bltui",
        "bgeui", "beqi", "bnei", "blti", "bgei"};
    uint32_t cc = GVM_CC(ctl), to = base + (uint32_t)GVM_OFF9(ctl) * 16;
    if (cc == GVM_CC_SPECIAL) {
        uint32_t sub = GVM_SUB(ctl), rs = GVM_CRS(ctl), far = base + (uint32_t)GVM_OFF18(ctl) * 16;
        switch (sub) {
        case GVM_SUB_NONE: snprintf(o, sz, "-"); return;
        case GVM_SUB_JR: snprintf(o, sz, rs == 1 ? "ret" : "jr r%u", rs); return;
        case GVM_SUB_CALLR: snprintf(o, sz, "callr r%u", rs); return;
        case GVM_SUB_ERET: snprintf(o, sz, "eret"); return;
        case GVM_SUB_J: snprintf(o, sz, "j %08x", far); return;
        case GVM_SUB_CALL: snprintf(o, sz, "call %08x", far); return;
        case GVM_SUB_ADD: snprintf(o, sz, "add r%u += %d", rs, GVM_CIMM12(ctl)); return;
        }
        snprintf(o, sz, "illegal");
        return;
    }
    if (cc == GVM_CC_BNE && GVM_CRA(ctl) == 0 && GVM_CRB(ctl) == 0) { snprintf(o, sz, "-"); return; }
    if (cc == GVM_CC_BEQ && GVM_CRA(ctl) == 0 && GVM_CRB(ctl) == 0) { snprintf(o, sz, "j %08x", to); return; }
    if (cc == GVM_CC_ADDBNZ || cc >= GVM_CC_BLTUI || cc == GVM_CC_BBS || cc == GVM_CC_BBC)
        snprintf(o, sz, "%s r%u, %d -> %08x", cc_name[cc], GVM_CRA(ctl), cc == GVM_CC_BBS || cc == GVM_CC_BBC ? (int)(GVM_CRB(ctl) & 31) : GVM_CIMM6(ctl), to);
    else
        snprintf(o, sz, "%s r%u, r%u -> %08x", cc_name[cc], GVM_CRA(ctl), GVM_CRB(ctl), to);
}

// One bundle as text: its ops (from word `from`, where a run of it starts) and its control word
static void dis_bundle(char *o, size_t sz, uint32_t pc) {
    uint32_t base = pc & ~15u, t[4], k;
    if (base + 16 > H.m.ram_end) { snprintf(o, sz, "(outside RAM)"); return; }
    for (k = 0; k < 4; k++) t[k] = H.m.ram[(base >> 2) + k];
    uint32_t n = GVM_N(t[3]), from = (pc & 15) >> 2;
    size_t len = 0;
    char buf[96];
    for (k = from; k < n && k < 2; k++) {
        dis_op(buf, sizeof buf, t[k], k == 0 ? GVM_LONG0(t[3]) : GVM_LONG1(t[3]), t[2]);
        len += snprintf(o + len, len < sz ? sz - len : 0, "%s%s", k > from ? " ; " : "", buf);
    }
    if (n == 3) len += snprintf(o + len, len < sz ? sz - len : 0, "(n 3: illegal)");
    dis_ctl(buf, sizeof buf, base, t[3]);
    snprintf(o + len, len < sz ? sz - len : 0, "%s| %s", len ? " " : "", buf);
}

static void trace_flush(void) {
    if (!H.tr_open) return;
    gvm_core *c = &H.cores[H.tr_core];
    int any = 0;
    for (int r = 1; r < 64; r++)
        if (c->x[r] != H.tr_x[r]) { fprintf(stderr, "%s r%d=%08x", any ? "" : "      ", r, c->x[r]); any = 1; }
    if (any) fputc('\n', stderr);
    H.tr_open = 0;
}

static int host_step(gvm_core *c) {
    if (H.trace) {
        char text[320];
        trace_flush();
        dis_bundle(text, sizeof text, c->pc);
        uint32_t ctl = (c->pc & ~15u) + 16 <= H.m.ram_end ? H.m.ram[((c->pc & ~15u) >> 2) + 3] : 0;
        fprintf(stderr, "[%u] %08x %08x  %s\n", c->id, c->pc, ctl, text);
        memcpy(H.tr_x, c->x, sizeof H.tr_x);
        H.tr_core = c->id;
        H.tr_open = 1;
    }
    if (H.max_bundles && H.bundles >= H.max_bundles) { H.stop = 1; H.exit_code = EXIT_LIMIT; return 1; }
    H.bundles++;
    return 0;
}

// ---- Traps with no handler: semihosting for a syscall, else a report and the end
static const char *cause_name(uint32_t cause) {
    switch (cause) {
    case GVM_CAUSE_ILLEGAL: return "illegal instruction";
    case GVM_CAUSE_BREAK: return "break";
    case GVM_CAUSE_SYSCALL: return "syscall";
    case GVM_CAUSE_LOAD: return "load fault";
    case GVM_CAUSE_STORE: return "store fault";
    case GVM_CAUSE_FETCH: return "fetch fault";
    case GVM_CAUSE_PRIV: return "privileged";
    }
    return cause & GVM_CAUSE_IRQ ? "interrupt" : "unknown";
}

static void dump_core(gvm_core *c) {
    char text[320];
    dis_bundle(text, sizeof text, c->pc);
    fprintf(stderr, "  bundle %08x: %s\n", c->pc & ~15u, text);
    for (int r = 0; r < 64; r++) fprintf(stderr, "%sr%-2d %08x%s", r % 8 ? " " : "  ", r, c->x[r], r % 8 == 7 ? "\n" : "");
    fprintf(stderr, "  status %08x epc %08x cause %08x tval %08x tvec %08x  bundles %llu ops %llu\n", c->sr[GVM_SR_STATUS],
            c->sr[GVM_SR_EPC], c->sr[GVM_SR_CAUSE], c->sr[GVM_SR_TVAL], c->sr[GVM_SR_TVEC],
            (unsigned long long)c->bundles_run, (unsigned long long)c->ops_run);
}

static void stop_all(int code) { H.stop = 1; H.exit_code = code; for (uint32_t i = 0; i < H.ncores; i++) H.cores[i].halted = 1; }

static int32_t sys_write(gvm_core *c, uint32_t fd, uint32_t buf, uint32_t len) {
    FILE *f = fd == 1 ? stdout : fd == 2 ? stderr : NULL;
    if (!f) return -E_BADF;
    uint8_t tmp[4096];
    uint32_t done = 0;
    while (done < len) {
        uint32_t k = len - done < sizeof tmp ? len - done : (uint32_t)sizeof tmp;
        if (!guest_read(c, buf + done, tmp, k)) return done ? (int32_t)done : -E_FAULT;
        fwrite(tmp, 1, k, f);
        done += k;
    }
    if (f == stderr || H.trace) out_flush();
    return (int32_t)done;
}

static int32_t sys_call(gvm_core *c) {
    uint32_t n = c->x[12], a0 = c->x[5], a1 = c->x[6], a2 = c->x[7];
    switch (n) {
    case 64: return sys_write(c, a0, a1, a2);   // write(fd, buf, len)
    case 66: {   // writev(fd, iov, n): {base, len} pairs
        int32_t total = 0;
        for (uint32_t i = 0; i < a2; i++) {
            uint8_t v[8];
            if (!guest_read(c, a1 + 8 * i, v, 8)) return -E_FAULT;
            int32_t r = sys_write(c, a0, rd32(v), rd32(v + 4));
            if (r < 0) return total ? total : r;
            total += r;
        }
        return total;
    }
    case 63: {   // read(fd, buf, len): fd 0 is the host's stdin
        if (a0 != 0) return -E_BADF;
        if (!a2) return 0;
        uint8_t tmp[4096];
        fflush(stdout);
        int got = (int)host_read_fd(0, tmp, a2 < sizeof tmp ? a2 : (unsigned)sizeof tmp);
        if (got < 0) return -E_INVAL;
        if (!guest_write(c, a1, tmp, (uint32_t)got)) return -E_FAULT;
        return got;
    }
    case 93: case 94:   // exit, exit_group: the whole machine stops
        stop_all((int)(a0 & 255));
        return 0;
    case 57: return 0;          // close
    case 80: return -E_NOSYS;   // fstat
    case 29: return -E_NOTTY;   // ioctl (musl's isatty on stdout)
    case 96: return 1;          // set_tid_address: the tid
    case 172: case 178: return 1;   // getpid, gettid
    case 214:   // brk(addr): the break, moved when addr lies between the heap's start and the mmap area
        if (a0 >= H.heap_start && a0 <= H.mmap_top) H.brk = a0;
        return (int32_t)H.brk;
    case 222: {   // mmap(addr, len, prot, flags, fd, off): anonymous only, from below the stack down; never reused
        uint32_t len = (a1 + 4095) & ~4095u;
        if (!(c->x[8] & 0x20) || !len) return -E_INVAL;   // MAP_ANONYMOUS
        if (H.mmap_top - H.brk < len) return -E_NOMEM;
        H.mmap_top -= len;
        return (int32_t)H.mmap_top;
    }
    case 215: return 0;   // munmap: kept
    case 113: case 403: {   // clock_gettime(clk, ts): TIME (100 kHz); 403 is the time64 form ({s64 sec, s64 nsec})
        uint8_t ts[16] = {0};
        uint64_t sec = H.m.time / 100000, nsec = (H.m.time % 100000) * 10000;
        if (n == 113) { wr32(ts, (uint32_t)sec); wr32(ts + 4, (uint32_t)nsec); }
        else { wr32(ts, (uint32_t)sec); wr32(ts + 4, (uint32_t)(sec >> 32)); wr32(ts + 8, (uint32_t)nsec); }
        return guest_write(c, a1, ts, n == 113 ? 8 : 16) ? 0 : -E_FAULT;
    }
    }
    return -E_NOSYS;
}

static int host_trap(gvm_core *c, uint32_t cause, uint32_t epc, uint32_t tval) {
    trace_flush();
    if (cause == GVM_CAUSE_SYSCALL) {
        uint32_t n = c->x[12], a[3] = {c->x[5], c->x[6], c->x[7]};
        int32_t r = sys_call(c);
        if (H.strace) { out_flush(); fprintf(stderr, "[%u] syscall %u(%#x, %#x, %#x) = %d\n", c->id, n, a[0], a[1], a[2], r); }
        c->x[5] = (uint32_t)r;
        c->pc = epc + 4;   // after the op: the rest of the bundle runs from there (the same as a handler's eret to EPC + 4)
        return 1;
    }
    out_flush();
    fprintf(stderr, "gvm_cpu: core %u: %s (cause %#x) at pc %08x, tval %08x, with no handler (TVEC 0)\n", c->id,
            cause_name(cause), cause, epc, tval);
    c->pc = epc;
    dump_core(c);
    stop_all(EXIT_TRAP);
    return 1;
}

// ---- Loading
static uint8_t *slurp(const char *path, uint32_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "gvm_cpu: cannot open %s\n", path); exit(EXIT_USAGE); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = (uint8_t *)malloc(n > 0 ? (size_t)n : 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "gvm_cpu: cannot read %s\n", path); exit(EXIT_USAGE); }
    fclose(f);
    *size = (uint32_t)n;
    return p;
}

static uint8_t *g_rom;
static uint32_t g_rom_cap;

// Bytes into RAM (from address 0: the boot area and the device window are RAM words too) or into the ROM
static void place(uint32_t at, const uint8_t *src, uint32_t filesz, uint32_t memsz, const char *what) {
    if (memsz == 0) return;
    if ((uint64_t)at + memsz <= H.m.ram_end) {
        uint8_t *ram = (uint8_t *)H.m.ram;   // little-endian words: a byte copy on a little-endian host
        memcpy(ram + at, src, filesz);
        memset(ram + at + filesz, 0, memsz - filesz);
    } else if (at >= GVM_ROM && (uint64_t)at + memsz <= GVM_ROM_END) {
        uint32_t o = at - GVM_ROM, end = o + memsz;
        if (end > g_rom_cap) { g_rom = (uint8_t *)realloc(g_rom, end); memset(g_rom + g_rom_cap, 0, end - g_rom_cap); g_rom_cap = end; }
        memcpy(g_rom + o, src, filesz);
        memset(g_rom + o + filesz, 0, memsz - filesz);
        if (end > H.m.rom_size) H.m.rom_size = end;
    } else {
        fprintf(stderr, "gvm_cpu: %s at %08x (%u bytes) is outside RAM (to %08x) and the ROM\n", what, at, memsz, H.m.ram_end);
        exit(EXIT_USAGE);
    }
    if (at >= GVM_RAM && at + memsz <= H.m.ram_end && at + memsz > H.heap_start) H.heap_start = at + memsz;
}

static uint32_t load_elf(const char *path) {
    uint32_t size;
    uint8_t *p = slurp(path, &size);
    if (size < 52 || memcmp(p, "\177ELF", 4) || p[4] != 1 || p[5] != 1) { fprintf(stderr, "gvm_cpu: %s: not a 32-bit little-endian ELF\n", path); exit(EXIT_USAGE); }
    uint32_t mach = p[18] | p[19] << 8, type = p[16] | p[17] << 8;
    if (mach != EM_GVM && mach != 0) { fprintf(stderr, "gvm_cpu: %s: e_machine %#x is not gpu_vm's (%#x)\n", path, mach, EM_GVM); exit(EXIT_USAGE); }
    if (type != 2) fprintf(stderr, "gvm_cpu: %s: e_type %u, not ET_EXEC: loaded as linked\n", path, type);
    uint32_t entry = rd32(p + 24), phoff = rd32(p + 28), phentsize = p[42] | p[43] << 8, phnum = p[44] | p[45] << 8;
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = p + phoff + i * phentsize;
        if (phoff + (i + 1) * phentsize > size) { fprintf(stderr, "gvm_cpu: %s: program headers past the file's end\n", path); exit(EXIT_USAGE); }
        if (rd32(ph) != 1) continue;   // PT_LOAD
        uint32_t off = rd32(ph + 4), vaddr = rd32(ph + 8), filesz = rd32(ph + 16), memsz = rd32(ph + 20);
        if (off + filesz > size || filesz > memsz) { fprintf(stderr, "gvm_cpu: %s: a segment past the file's end or larger than its memsz\n", path); exit(EXIT_USAGE); }
        place(vaddr, p + off, filesz, memsz, "a segment");
    }
    free(p);
    return entry;
}

// The initial stack, as Linux's for a program: argc, argv[], 0, envp[] (none), 0, auxv (AT_PAGESZ, AT_NULL); r5 argc,
// r6 argv, r7 envp too
static void setup_stack(gvm_core *c, int argc, char **argv) {
    uint32_t sp = H.m.ram_end - 16, ptr[256], heap = H.heap_start;
    if (argc > 250) argc = 250;
    for (int i = argc - 1; i >= 0; i--) {
        uint32_t len = (uint32_t)strlen(argv[i]) + 1;
        sp -= len;
        place(sp, (const uint8_t *)argv[i], len, len, "an argument");
        ptr[i] = sp;
    }
    uint32_t words = (uint32_t)argc + 1 + 1 + 1 + 4, k = 0, v[260];
    sp = (sp - words * 4) & ~15u;
    v[k++] = (uint32_t)argc;
    for (int i = 0; i < argc; i++) v[k++] = ptr[i];
    v[k++] = 0;                 // argv's end
    v[k++] = 0;                 // envp's end
    v[k++] = 6; v[k++] = 4096;  // AT_PAGESZ
    v[k++] = 0; v[k++] = 0;     // AT_NULL
    for (uint32_t i = 0; i < k; i++) H.m.ram[(sp >> 2) + i] = v[i];
    c->x[2] = sp;
    c->x[5] = (uint32_t)argc;
    c->x[6] = sp + 4;
    c->x[7] = sp + 4 + 4 * ((uint32_t)argc + 1);
    H.stack_limit = sp;
    H.heap_start = heap;   // (place() moved it past the arguments)
}

static void usage(void) {
    fprintf(stderr,
        "usage: gvm_cpu [options] PROGRAM.elf [ARGS...]\n"
        "       gvm_cpu [options] --bin FILE --at ADDR [--entry ADDR] [ARGS...]\n"
        "  --ram MB            RAM from 0x02000000 (default 64)\n"
        "  --rom FILE          the ROM at 0x40000000\n"
        "  --dtb FILE          a device tree at 0x1000\n"
        "  --budget N          bundles a pass, a core (default 8192)\n"
        "  --pass-time N       TIME's ticks (100 kHz) a pass (default 20: 0.2 ms)\n"
        "  --cores N           cores (default 1; the others are parked: workers are not made yet)\n"
        "  --max-bundles N     stop with exit code 3 after N bundles\n"
        "  --trace             each bundle on stderr: core, pc, control word, ops, registers written\n"
        "  --strace            each semihosted call on stderr\n"
        "  --stats             bundles, ops and passes at the end, on stderr\n");
    exit(EXIT_USAGE);
}

static uint32_t num(const char *s) { return (uint32_t)strtoul(s, NULL, 0); }

int main(int argc, char **argv) {
    const char *elf = NULL, *bin = NULL, *rom = NULL, *dtb = NULL;
    uint32_t at = 0, entry = 0, ram_mb = 64;
    int have_at = 0, have_entry = 0, i;
    H.budget = 8192; H.pass_time = 20; H.ncores = 1;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--trace")) H.trace = 1;
        else if (!strcmp(a, "--strace")) H.strace = 1;
        else if (!strcmp(a, "--stats")) H.stats = 1;
        else if (!strcmp(a, "--ram") && more) ram_mb = num(argv[++i]);
        else if (!strcmp(a, "--rom") && more) rom = argv[++i];
        else if (!strcmp(a, "--dtb") && more) dtb = argv[++i];
        else if (!strcmp(a, "--bin") && more) bin = argv[++i];
        else if (!strcmp(a, "--at") && more) { at = num(argv[++i]); have_at = 1; }
        else if (!strcmp(a, "--entry") && more) { entry = num(argv[++i]); have_entry = 1; }
        else if (!strcmp(a, "--budget") && more) H.budget = num(argv[++i]);
        else if (!strcmp(a, "--pass-time") && more) H.pass_time = num(argv[++i]);
        else if (!strcmp(a, "--cores") && more) H.ncores = num(argv[++i]);
        else if (!strcmp(a, "--max-bundles") && more) H.max_bundles = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(a, "--")) { i++; break; }
        else if (a[0] == '-' && a[1] == '-') { fprintf(stderr, "gvm_cpu: unknown option %s\n", a); usage(); }
        else break;
    }
    if (!bin) { if (i >= argc) usage(); elf = argv[i]; }   // the program's argv[0] is the ELF's path, or the raw image's
    if (bin && !have_at) { fprintf(stderr, "gvm_cpu: --bin needs --at\n"); usage(); }
    if (H.ncores < 1 || H.ncores > CPU_MAX_CORES || !H.budget || ram_mb < 1) usage();
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    if (H.trace) setvbuf(stdout, NULL, _IONBF, 0);

    // the machine: RAM is address 0 up (the boot area and the device window are words of it), to GVM_RAM + its size
    if ((uint64_t)GVM_RAM + (uint64_t)ram_mb * (1u << 20) > GVM_ROM) { fprintf(stderr, "gvm_cpu: --ram %u is past the ROM\n", ram_mb); return EXIT_USAGE; }
    H.m.ram_end = GVM_RAM + ram_mb * (1u << 20);
    H.m.ram = (uint32_t *)calloc(H.m.ram_end / 4, 4);
    H.cores = (gvm_core *)calloc(H.ncores, sizeof(gvm_core));
    if (!H.m.ram || !H.cores) { fprintf(stderr, "gvm_cpu: out of memory\n"); return EXIT_USAGE; }
    H.m.cores = H.cores;
    H.m.ncores = H.ncores;
    H.heap_start = GVM_RAM;
    if (rom) {
        uint32_t n;
        uint8_t *p = slurp(rom, &n);
        if (n > GVM_ROM_END - GVM_ROM) { fprintf(stderr, "gvm_cpu: the ROM is larger than 512 MB\n"); return EXIT_USAGE; }
        place(GVM_ROM, p, n, n, "the ROM");
        free(p);
    }
    if (dtb) {   // TODO(Linux): the kernel's entry convention for the device tree's address (fixed at GVM_DTB for now)
        uint32_t n;
        uint8_t *p = slurp(dtb, &n);
        if (GVM_DTB + n > GVM_DEV) { fprintf(stderr, "gvm_cpu: the device tree does not fit below 0x10000\n"); return EXIT_USAGE; }
        place(GVM_DTB, p, n, n, "the device tree");
        free(p);
    }
    H.heap_start = GVM_RAM;
    if (elf) entry = load_elf(elf);
    else {
        uint32_t n;
        uint8_t *p = slurp(bin, &n);
        place(at, p, n, n, "the image");
        free(p);
        if (!have_entry) entry = at;
    }
    if (elf && have_entry) fprintf(stderr, "gvm_cpu: --entry overrides the ELF's\n");
    H.m.rom = g_rom;

    for (uint32_t c = 0; c < H.ncores; c++) {
        gvm_core *k = &H.cores[c];
        gvm_core_reset(k, c, entry);
        k->host_trap = host_trap;
        k->host_step = host_step;
        k->host = &H;
        // TODO(workers): cores past 0 wait parked until a mailbox gives them a job (docs/machine/multicore.md's layout)
        if (c) k->halted = 1;
    }
    setup_stack(&H.cores[0], argc - i, argv + i);
    H.heap_start = (H.heap_start + 15) & ~15u;
    H.brk = H.heap_start;
    H.mmap_top = (H.stack_limit - (1u << 20)) & ~4095u;   // a megabyte of stack below the arguments
    if (H.mmap_top < H.heap_start) { fprintf(stderr, "gvm_cpu: no room for the stack\n"); return EXIT_USAGE; }

    // the passes: each core runs its budget from the RAM the last commit left, then the commit
    H.m.time = 0;
    while (!H.stop) {
        int running = 0, all_waiting = 1;
        for (uint32_t c = 0; c < H.ncores && !H.stop; c++) {
            gvm_core *k = &H.cores[c];
            if (k->halted) continue;
            running = 1;
            gvm_run(&H.m, k, H.budget);
            if (!k->waiting) all_waiting = 0;
        }
        trace_flush();
        gvm_commit(&H.m);
        H.passes++;
        // TODO(Linux): the console device (a word in the device window read after the commit), the external
        // interrupt line (gvm_core.ext) for input, and the mailboxes for workers go here.
        if (!running) {   // every core halted with no exit: a break with GVM_REF_BREAK_HALTS, say
            fprintf(stderr, "gvm_cpu: every core halted\n");
            H.exit_code = EXIT_TRAP;
            break;
        }
        if (H.stop) break;
        H.m.time += H.pass_time;
        if (all_waiting) {   // every core in a supervisor's wait: on to the earliest timer it can be woken by
            uint64_t next = 0;
            for (uint32_t c = 0; c < H.ncores; c++) {
                gvm_core *k = &H.cores[c];
                uint64_t cmp = (uint64_t)k->sr[GVM_SR_TCMP_HI] << 32 | k->sr[GVM_SR_TCMP_LO];
                if (k->halted) continue;
                if ((k->sr[GVM_SR_IEN] & (1u << GVM_IRQ_SOFT | 1u << GVM_IRQ_EXT)) && (k->soft || k->ext)) { next = H.m.time; break; }
                if ((k->sr[GVM_SR_IEN] & 1u << GVM_IRQ_TIMER) && cmp && (!next || cmp < next)) next = cmp;
            }
            if (!next) {
                out_flush();
                fprintf(stderr, "gvm_cpu: every core waits for an interrupt that cannot come\n");
                dump_core(&H.cores[0]);
                H.exit_code = EXIT_TRAP;
                break;
            }
            if (next > H.m.time) H.m.time = next;
        }
    }
    out_flush();
    if (H.exit_code == EXIT_LIMIT && H.max_bundles) fprintf(stderr, "gvm_cpu: %llu bundles run: stopped (--max-bundles), pc %08x\n", (unsigned long long)H.bundles, H.cores[0].pc);
    if (H.stats) {
        uint64_t b = 0, o = 0;
        for (uint32_t c = 0; c < H.ncores; c++) { b += H.cores[c].bundles_run; o += H.cores[c].ops_run; }
        fprintf(stderr, "gvm_cpu: %llu bundles, %llu ops (%.2f a bundle), %llu passes, time %llu (100 kHz)\n",
                (unsigned long long)b, (unsigned long long)o, b ? (double)o / (double)b : 0.0, (unsigned long long)H.passes,
                (unsigned long long)H.m.time);
    }
    return H.exit_code;
}
