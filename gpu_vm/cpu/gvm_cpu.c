// gvm_cpu: programs for gpu_vm (gpu_vm/GPU_VM.md, gpu_vm/isa.h) on the host, around the reference machine
// (gpu_vm/ref/gvm_ref.h): passes of gvm_run and gvm_commit as the card makes them, an ELF or raw image loaded, and
// bare-metal semihosting (a syscall while TVEC is 0 is made by the host). The compiler's tests run here, and later
// Linux. gpu_vm/cpu/README.md has the options and the conventions.
//
//   gvm_cpu [options] PROGRAM.elf [ARGS...]
//   gvm_cpu [options] --bin FILE --at ADDR [--entry ADDR] [ARGS...]
//   gvm_cpu [options] --dtb gvm.dtb vmlinux   (Linux: r5 = 0x1000, the device tree; the console device on stdin/stdout)
#define _CRT_SECURE_NO_WARNINGS
#define _POSIX_C_SOURCE 200809L
#define GVM_REF_IMPL
#include "../ref/gvm_ref.h"
#include "../platform/worker_fw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <conio.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define host_read_fd _read
#else
#include <unistd.h>
#include <poll.h>
#define host_read_fd read
#endif

#define CPU_MAX_CORES 256   // core 0 and 255 workers, as the RISC-V machine (gpu_vm/platform.md)

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
    int console, console_in;     // the console device (on unless --no-console), its input (off with --no-stdin)
    uint32_t con_printed;        // the guest's output bytes printed so far
    int stop, exit_code;
    uint32_t heap_start, brk, mmap_top, stack_limit;
    // the trace: what the bundle being traced found, to print what it wrote
    int tr_open;
    uint32_t tr_core, tr_x[64];
    // the devices (gpu_vm/platform.md): the control words a host gives each pass, on with --dtb or --devices
    int devices, desktop;
    uint32_t workers;            // worker cores started (1 to workers), by the geometry's control words
    uint8_t end[CPU_MAX_CORES];  // why each core's last pass ended (GVM_END_*)
    // --profile: bundles run by address and contents, from --profile-from bundles on; written at the exit
    const char *prof_path;
    uint64_t prof_from;
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
        uint32_t sub = GVM_SUB(ctl), rs = GVM_CRS(ctl), far_to = base + (uint32_t)GVM_OFF18(ctl) * 16;
        switch (sub) {
        case GVM_SUB_NONE: snprintf(o, sz, "-"); return;
        case GVM_SUB_JR: snprintf(o, sz, rs == 1 ? "ret" : "jr r%u", rs); return;
        case GVM_SUB_CALLR: snprintf(o, sz, "callr r%u", rs); return;
        case GVM_SUB_ERET: snprintf(o, sz, "eret"); return;
        case GVM_SUB_J: snprintf(o, sz, "j %08x", far_to); return;
        case GVM_SUB_CALL: snprintf(o, sz, "call %08x", far_to); return;
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

// ---- --profile: a table of (pc, the bundle's four words) -> runs, core 0's and the workers'
typedef struct { uint32_t pc, w[4]; uint64_t n0, nw; } prof_ent;
static struct { prof_ent *t; uint32_t mask, used; } P;

static uint32_t prof_hash(uint32_t pc, const uint32_t *w) { return ((pc >> 4) ^ w[0] ^ w[3]) * 2654435761u; }

static void prof_count(gvm_core *c) {
    uint32_t pc = c->pc, *w;
    if ((pc & 15u) || pc + 16 > H.m.ram_end || H.bundles < H.prof_from) return;   // (a bundle resumed: counted once)
    w = &H.m.ram[pc >> 2];
    if (P.used * 2 >= P.mask) {   // grown at half full
        prof_ent *old = P.t;
        uint32_t om = P.mask;
        P.mask = om ? om * 2 + 1 : (1u << 16) - 1;
        P.t = (prof_ent *)calloc((size_t)P.mask + 1, sizeof(prof_ent));
        if (!P.t) { fprintf(stderr, "gvm_cpu: --profile: out of memory\n"); exit(EXIT_USAGE); }
        for (uint32_t i = 0; old && i <= om; i++)
            if (old[i].n0 || old[i].nw) {
                uint32_t h = prof_hash(old[i].pc, old[i].w);
                while (P.t[h & P.mask].n0 || P.t[h & P.mask].nw) h++;
                P.t[h & P.mask] = old[i];
            }
        free(old);
    }
    for (uint32_t h = prof_hash(pc, w);; h++) {
        prof_ent *e = &P.t[h & P.mask];
        if (!e->n0 && !e->nw) { e->pc = pc; memcpy(e->w, w, 16); P.used++; }
        else if (e->pc != pc || memcmp(e->w, w, 16)) continue;
        if (c->id) e->nw++; else e->n0++;
        return;
    }
}

static void prof_write(void) {
    FILE *f;
    if (!H.prof_path || !(f = fopen(H.prof_path, "w"))) return;
    fprintf(f, "# pc w0 w1 w2 ctl core0 workers\n");
    for (uint32_t i = 0; P.t && i <= P.mask; i++) {
        prof_ent *e = &P.t[i];
        if (e->n0 || e->nw)
            fprintf(f, "%08x %08x %08x %08x %08x %llu %llu\n", e->pc, e->w[0], e->w[1], e->w[2], e->w[3],
                    (unsigned long long)e->n0, (unsigned long long)e->nw);
    }
    fclose(f);
}

// SIGTERM or SIGINT while profiling: the run stops at the next bundle and the profile is written
static volatile sig_atomic_t prof_killed;
static void prof_stop(int sig) { prof_killed = 1; }

static int host_step(gvm_core *c) {
    if (H.prof_path) {
        if (prof_killed) { H.stop = 1; return 1; }
        prof_count(c);
    }
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

// ---- The console device (gpu_vm/linux/README.md, "Console"): words in the device window, plain memory both sides,
// read and written by the host after each commit (the RISC-V machine's console words moved down by 0x86000000)
#define CON_BASE 0x016b8280u
#define CON_DELIVERED (CON_BASE + 0x00)   // host: characters delivered so far
#define CON_LAST (CON_BASE + 0x04)        // host: the last delivery, up to 4 characters, the first in the low byte
#define CON_TAKEN (CON_BASE + 0x10)       // guest: characters taken so far
#define CON_WRITTEN (CON_BASE + 0x14)     // guest: bytes written so far
#define CON_RING (CON_BASE + 0x20)        // guest: the output ring, byte n at n mod 256

// Up to max bytes of stdin that are there now (0: none yet; -1: its end)
static int stdin_ready(uint8_t *buf, int max) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD type = GetFileType(h), avail = 0;
    if (type == FILE_TYPE_CHAR) {   // a console
        int n = 0;
        while (n < max && _kbhit()) { int ch = _getch(); buf[n++] = (uint8_t)(ch == '\r' ? '\n' : ch); }
        return n;
    }
    if (type == FILE_TYPE_PIPE) {
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL)) return -1;   // (the writer has gone)
        if (!avail) return 0;
        if ((DWORD)max > avail) max = (int)avail;
    }
    int got = _read(0, buf, (unsigned)max);
    return got > 0 ? got : -1;
#else
    struct pollfd p = {0, POLLIN, 0};
    if (poll(&p, 1, 0) <= 0) return 0;
    ssize_t got = read(0, buf, (size_t)max);
    return got > 0 ? (int)got : -1;
#endif
}

static void console_poll(void) {
    uint32_t *ram = H.m.ram, written = ram[CON_WRITTEN >> 2];
    if (written != H.con_printed) {
        uint32_t n = written - H.con_printed;
        if (n > 256) {   // more than the ring holds: the oldest are gone
            fprintf(stderr, "gvm_cpu: console: %u bytes lost\n", n - 256);
            H.con_printed = written - 256;
        }
        for (; H.con_printed != written; H.con_printed++) {
            uint32_t a = CON_RING + (H.con_printed & 255);
            fputc((int)(ram[a >> 2] >> ((a & 3) * 8)) & 255, stdout);
        }
        fflush(stdout);
    }
    // input once the guest has used the console (written or taken): stdin stays the semihosted read's until then
    if (H.console_in && (written || ram[CON_TAKEN >> 2]) && ram[CON_TAKEN >> 2] == ram[CON_DELIVERED >> 2]) {
        uint8_t in[4];
        int n = stdin_ready(in, 4);
        if (n < 0) H.console_in = 0;   // stdin's end: no more input
        else if (n > 0) {
            uint32_t w = 0;
            for (int k = n - 1; k >= 0; k--) w = w << 8 | in[k];
            ram[CON_LAST >> 2] = w;
            ram[CON_DELIVERED >> 2] += (uint32_t)n;
        }
    }
}

// The memory node's size in a flattened device tree (the first "memory" or "memory@..." node under the root: its
// reg's size, one cell or two), or 0
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint64_t fdt_memory_size(const uint8_t *f, uint32_t size) {
    if (size < 40 || be32(f) != 0xd00dfeedu) return 0;
    uint32_t st = be32(f + 8), strs = be32(f + 12), st_size = be32(f + 36), o = st, depth = 0, in_mem = 0;
    if ((uint64_t)st + st_size > size || strs >= size) return 0;
    while (o + 4 <= st + st_size) {
        uint32_t tok = be32(f + o);
        o += 4;
        if (tok == 1) {   // FDT_BEGIN_NODE
            const char *name = (const char *)f + o;
            size_t len = 0;
            while (o + len < size && name[len]) len++;
            depth++;
            if (depth == 2 && (!strcmp(name, "memory") || !strncmp(name, "memory@", 7))) in_mem = depth;
            o += ((uint32_t)len + 4) & ~3u;
        } else if (tok == 2) {   // FDT_END_NODE
            if (in_mem == depth) in_mem = 0;
            depth--;
        } else if (tok == 3) {   // FDT_PROP
            if (o + 8 > size) return 0;
            uint32_t len = be32(f + o), nameoff = be32(f + o + 4);
            const uint8_t *v = f + o + 8;
            if (in_mem && in_mem == depth && strs + nameoff < size && !strcmp((const char *)f + strs + nameoff, "reg") && len >= 8 && o + 8 + len <= size)
                return len == 16 ? (uint64_t)be32(v + 8) << 32 | be32(v + 12) : be32(v + len - 4);
            o += 8 + ((len + 3) & ~3u);
        } else if (tok == 9) break;   // FDT_END
    }
    return 0;
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
// r6 argv, r7 envp too. The argument strings start 64 bytes below RAM's end: the compiler may load up to 16 bytes past
// an access through the same register, so the last 64 bytes of RAM are never used (the kernel reserves them too).
static void setup_stack(gvm_core *c, int argc, char **argv) {
    uint32_t sp = H.m.ram_end - 64, ptr[256], heap = H.heap_start;
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

// ---- The devices and the workers (gpu_vm/platform.md): the RISC-V machine's control words at 0x01000000 (its
// 0x87000000 less 0x86000000), given as rvc_cpu's control pass gives them; the worker cores started at the firmware
// (gpu_vm/platform/worker.S) when the kernel lays them out
#define CTRL 0x01000000u
#define STATS_ALL 0x016ba000u   // every core's count, then a texel for every 32 cores: which run, which sleep
#define W(a) H.m.ram[(a) >> 2]

static void worker_start(uint32_t i) {
    gvm_core *k = &H.cores[i];
    gvm_core_reset(k, i, GVM_FW_ENTRY);
    k->host_trap = host_trap;
    k->host_step = host_step;
    k->host_access = H.cores[0].host_access;
    k->host = &H;
    k->x[5] = i;
}

// The geometry's words (CTRL + 0x3d0: bit 0 while it is laid out anew, how many workers, their size) say which
// cores are workers: those are started at the firmware, the others halted. Then what each core ran.
static void workers_pass(void) {
    if (H.ncores < 2) return;
    uint32_t shaping = W(CTRL + 0x3d0) & 1, there = W(CTRL + 0x3d4), bits = W(CTRL + 0x3d8), want = 0;
    if (!shaping && bits >= 3 && bits <= 6) want = there < H.ncores - 1 ? there : H.ncores - 1;
    if (want != H.workers) {
        for (uint32_t i = 1; i < H.ncores; i++) {
            if (i <= want && i > H.workers) worker_start(i);
            if (i > want) H.cores[i].halted = 1;
        }
        H.workers = want;
    }
    if (!H.devices) return;
    uint32_t running[8] = {1}, asleep[8] = {0};
    for (uint32_t i = 0; i < H.ncores; i++) {
        uint32_t count = (uint32_t)H.cores[i].ops_run;
        W(STATS_ALL + 4 * i) = count;
        if (i < 16) W(CTRL + 0x380 + 4 * i) = count;
        if (i == 0 || i > H.workers) continue;
        uint32_t pc = H.cores[i].pc;
        int parked = pc >= GVM_FW_PARK && pc < GVM_FW_PARK_END;
        running[i / 32] |= (parked ? 0u : 1u) << (i % 32);
        asleep[i / 32] |= (!parked && H.end[i] == GVM_END_WAIT ? 1u : 0u) << (i % 32);
    }
    W(CTRL + 0x3c0) = 1 + H.workers;
    W(CTRL + 0x3c4) = running[0];
    W(CTRL + 0x3c8) = asleep[0];
    W(CTRL + 0x3cc) = H.ncores;
    for (uint32_t n = 0; n < 8; n++) { W(STATS_ALL + 0x400 + 16 * n) = running[n]; W(STATS_ALL + 0x404 + 16 * n) = asleep[n]; }
}

// GVM_SOUND=1 (rvc_cpu's RVC_SOUND): the sound card's clock and voices move as the card's control pass moves them
// (experiments/rvc_opt/src/sound.h's sound_control), so that a program's sound code runs as on the card; nothing is
// mixed. Every 5 ms of TIME, at 48,000 samples a second.
static void sound_pass(void) {
    static int on = -1;
    const uint32_t GUEST = CTRL + 0x220, DEVICE = CTRL + 0x230, RATE = 48000;
    if (on < 0) on = getenv("GVM_SOUND") && atoi(getenv("GVM_SOUND")) != 0;
    if (!on || !W(GUEST)) return;
    uint32_t cursor = (uint32_t)(H.m.time * RATE / 100000), moved = cursor - W(DEVICE);
    if (W(DEVICE + 4) && moved < 240) return;
    if (!W(DEVICE + 4)) moved = 0;
    for (uint32_t n = 0, voices = W(GUEST + 8) < 32 ? W(GUEST + 8) : 32; n < voices; n++) {
        uint32_t *a = &W(CTRL + 0x800 + 32 * n), *b = a + 4, *c = &W(CTRL + 0xc00 + 32 * n), *d = c + 4;
        uint32_t on_ = c[1] & 1, kind = c[1] >> 8, address = d[0], length = d[1], restart = d[2], aux = d[3], pos = c[2], frac = c[3], k = moved;
        if (a[0] != c[0]) on_ = a[0] & 1, kind = a[1], address = a[2], length = a[3], restart = b[0], aux = b[3], pos = frac = 0, k = 0;
        if ((kind & 0xff) == 3) restart = b[0];
        if (on_) {
            uint32_t kl = k & 0xffff, kh = k >> 16, pl = b[1] & 0xffff, ph = b[1] >> 16, low = kl * pl + frac;
            pos += (low >> 16) + kl * ph + kh * pl + ((kh * ph) << 16);
            frac = low & 0xffff;
            if ((kind & 0xff) == 3) {
                if ((int32_t)(restart - pos) <= 0) pos = restart, frac = 0;
            } else if (pos >= length) {
                if ((kind & 0x100) && restart < length) pos = restart + (pos - restart) % (length - restart);
                else on_ = 0;
            }
        }
        c[0] = a[0], c[1] = (kind << 8) | on_, c[2] = pos, c[3] = frac;
        d[0] = address, d[1] = length, d[2] = restart, d[3] = aux;
    }
    W(DEVICE) = cursor;
    W(DEVICE + 4) = RATE;
}

// The rest of rvc_cpu's control pass with no host devices: a submitted list is drawn at once (nothing is drawn: the
// submit word is taken back, the list counted and its copy counted), the clock, the host's flags, the passes
static void devices_pass(void) {
    sound_pass();
    uint32_t submit = W(CTRL + 0x10);
    if (submit) {
        W(CTRL + 0x10) = 0;
        W(CTRL + 0x1c)++;
        if ((submit & 1) && (submit & 6)) W(CTRL + 0x38)++;
    }
    W(CTRL + 0x34) = (uint32_t)(H.m.time / 100);   // milliseconds
    W(CTRL + 0x3c) = (H.desktop ? 1u : 0u) | (128u << 8) | (128u << 16);
    W(CTRL + 0x32c) = (uint32_t)H.passes;
}

// ---- GVM_RACES=FILE (gpu_vm/platform.md, "Stores"; rvc_cpu's RVC_RACES): every core here reads RAM as the last
// commit left it, as on the card, so a program runs as it does there; this says where that mattered. "undone": a word
// a lower core changed in the pass and a higher core stored too (its value as it saw the word is what the commit
// leaves); "as it was": a core read a word another core stored in the same pass (and was given it as the pass began:
// a core waiting on another's word does this every round, and is right to). The mailboxes, the firmware's words and
// the counters are left out: several cores write them by design.
static struct {
    FILE *f;
    uint64_t undone, asitwas;
    uint32_t *reads[CPU_MAX_CORES], nreads[CPU_MAX_CORES], capreads[CPU_MAX_CORES];   // (word, pc) pairs
    uint32_t *spc[CPU_MAX_CORES];   // a core's first store's pc to each word of this pass: word + 1, pc
    uint32_t *wmap, *wmapused, nused;   // word + 1 -> the first writer's entry; entries chained
    uint32_t *ent, nent, capent;        // (core, value, next)
    uint32_t *place, nplace;            // (kind, pc, count)
} RC;
#define RC_SPC_BITS 13
#define RC_SPC (1u << RC_SPC_BITS)
#define RC_MAP_BITS 20
#define RC_MAP (1u << RC_MAP_BITS)
#define RC_NONE 0xffffffffu
static int race_left_out(uint32_t a) {
    return (a >= 0x00c00000u && a < 0x00c08000u) || (a >= 0x00c0c000u && a < 0x00d11000u);
}
static void race_note(uint32_t kind, uint32_t pc) {
    for (uint32_t i = 0; i < RC.nplace; i++)
        if (RC.place[3 * i] == kind && RC.place[3 * i + 1] == pc) { RC.place[3 * i + 2]++; return; }
    RC.place = (uint32_t *)realloc(RC.place, 12 * (size_t)(RC.nplace + 1));
    RC.place[3 * RC.nplace] = kind; RC.place[3 * RC.nplace + 1] = pc; RC.place[3 * RC.nplace + 2] = 1;
    RC.nplace++;
}
static uint32_t *race_spc(uint32_t id, uint32_t w) {
    uint32_t *t = RC.spc[id], h = (w * 0x9e3779b1u) >> (32 - RC_SPC_BITS), n = 0;
    while (t[2 * h] && t[2 * h] != w + 1 && n++ < RC_SPC) h = (h + 1) & (RC_SPC - 1);
    return &t[2 * h];
}
static void race_access(gvm_core *c, uint32_t a, uint32_t bytes, int store) {
    if (race_left_out(a)) return;
    uint32_t id = c->id;
    for (uint32_t w = a >> 2; w <= (a + bytes - 1) >> 2; w++) {
        if (store) {
            uint32_t *e = race_spc(id, w);
            if (!e[0]) { e[0] = w + 1; e[1] = c->pc; }
        } else {
            if (RC.nreads[id] + 2 > RC.capreads[id]) {
                RC.capreads[id] = RC.capreads[id] ? 2 * RC.capreads[id] : 4096;
                RC.reads[id] = (uint32_t *)realloc(RC.reads[id], 4 * (size_t)RC.capreads[id]);
            }
            RC.reads[id][RC.nreads[id]++] = w;
            RC.reads[id][RC.nreads[id]++] = c->pc;
        }
    }
}
static uint32_t *race_slot(uint32_t w) {
    uint32_t h = (w * 0x9e3779b1u) >> (32 - RC_MAP_BITS);
    while (RC.wmap[2 * h] && RC.wmap[2 * h] != w + 1) h = (h + 1) & (RC_MAP - 1);
    return &RC.wmap[2 * h];
}
// After every core's part of a pass, before the commit
static void race_pass(void) {
    RC.nent = 0;
    for (uint32_t i = 0; i < H.ncores; i++) {
        gvm_wlog *wl = &H.cores[i].wl;
        for (uint32_t j = 0; j < wl->n; j++) {
            uint32_t h = wl->order[j], w = wl->key[h] - 1;
            if (race_left_out(w << 2)) continue;
            uint32_t *s = race_slot(w);
            if (!s[0]) { s[0] = w + 1; s[1] = RC_NONE; RC.wmapused[RC.nused++] = (uint32_t)(s - RC.wmap); }
            if (RC.nent + 1 > RC.capent) {
                RC.capent = RC.capent ? 2 * RC.capent : 65536;
                RC.ent = (uint32_t *)realloc(RC.ent, 12 * (size_t)RC.capent);
            }
            RC.ent[3 * RC.nent] = i; RC.ent[3 * RC.nent + 1] = wl->val[h]; RC.ent[3 * RC.nent + 2] = RC_NONE;
            if (s[1] == RC_NONE) s[1] = RC.nent;
            else { uint32_t e = s[1]; while (RC.ent[3 * e + 2] != RC_NONE) e = RC.ent[3 * e + 2]; RC.ent[3 * e + 2] = RC.nent; }
            RC.nent++;
        }
    }
    for (uint32_t u = 0; u < RC.nused; u++) {
        uint32_t *s = &RC.wmap[RC.wmapused[u]], w = s[0] - 1, start = H.m.ram[w];
        for (uint32_t e = s[1]; e != RC_NONE && RC.ent[3 * e + 2] != RC_NONE; e = RC.ent[3 * e + 2]) {
            uint32_t f = RC.ent[3 * e + 2], a = RC.ent[3 * e], b = RC.ent[3 * f], av = RC.ent[3 * e + 1], bv = RC.ent[3 * f + 1];
            if (av != start && bv != av) {
                uint32_t apc = race_spc(a, w)[1], bpc = race_spc(b, w)[1];
                RC.undone++;
                race_note(0, apc);
                if (RC.f) fprintf(RC.f, "undone %08x: core %u (pc %08x) stored %08x, then core %u (pc %08x) %08x, it was %08x, pass %llu\n",
                                  w << 2, a, apc, av, b, bpc, bv, start, (unsigned long long)H.passes);
            }
        }
    }
    for (uint32_t i = 0; i < H.ncores; i++) {
        for (uint32_t r = 0; r < RC.nreads[i]; r += 2) {
            uint32_t w = RC.reads[i][r], *s = race_slot(w);
            if (!s[0]) continue;
            int other = 0;
            for (uint32_t e = s[1]; e != RC_NONE; e = RC.ent[3 * e + 2]) other |= RC.ent[3 * e] != i;
            if (other) { RC.asitwas++; race_note(1, RC.reads[i][r + 1]); }
        }
        RC.nreads[i] = 0;
        memset(RC.spc[i], 0, 8 * (size_t)RC_SPC);
    }
    for (uint32_t u = 0; u < RC.nused; u++) RC.wmap[RC.wmapused[u]] = 0, RC.wmap[RC.wmapused[u] + 1] = 0;
    RC.nused = 0;
}
static int race_cmp(const void *x, const void *y) {
    const uint32_t *a = (const uint32_t *)x, *b = (const uint32_t *)y;
    return a[0] != b[0] ? (a[0] < b[0] ? -1 : 1) : a[2] != b[2] ? (a[2] > b[2] ? -1 : 1) : 0;
}
static void race_report(void) {
    uint32_t shown[2] = {0, 0};
    qsort(RC.place, RC.nplace, 12, race_cmp);
    fprintf(stderr, "gvm_cpu: races: %llu undone, %llu read as they were\n", (unsigned long long)RC.undone, (unsigned long long)RC.asitwas);
    for (uint32_t k = 0; k < RC.nplace; k++) {
        uint32_t kind = RC.place[3 * k];
        if (RC.f) fprintf(RC.f, "%s at %08x: %u\n", kind ? "as it was" : "undone", RC.place[3 * k + 1], RC.place[3 * k + 2]);
        if (shown[kind]++ < 8) fprintf(stderr, "  %s at %08x: %u\n", kind ? "as it was" : "undone", RC.place[3 * k + 1], RC.place[3 * k + 2]);
    }
    if (RC.f) fclose(RC.f);
}
static void race_init(const char *path) {
    RC.f = fopen(path, "w");
    if (!RC.f) { fprintf(stderr, "gvm_cpu: GVM_RACES: cannot write %s\n", path); exit(EXIT_USAGE); }
    for (uint32_t i = 0; i < H.ncores; i++) RC.spc[i] = (uint32_t *)calloc(2 * (size_t)RC_SPC, 4);
    RC.wmap = (uint32_t *)calloc(2 * (size_t)RC_MAP, 4);
    RC.wmapused = (uint32_t *)calloc(RC_MAP, 4);
    for (uint32_t i = 0; i < H.ncores; i++) H.cores[i].host_access = race_access;
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
        "  --cores N           cores (default 1): core 0, and workers up to N - 1 (gpu_vm/platform.md)\n"
        "  --devices           the control words a host gives (on with --dtb; gpu_vm/platform.md)\n"
        "  --desktop           the host's flag that the guest starts its desktop\n"
        "  --max-bundles N     stop with exit code 3 after N bundles\n"
        "  --trace             each bundle on stderr: core, pc, control word, ops, registers written\n"
        "  --strace            each semihosted call on stderr\n"
        "  --stats             bundles, ops and passes at the end, on stderr\n"
        "  --profile FILE      the bundles run, a line each address and contents with its runs, to FILE at the end\n"
        "  --profile-from N    count from the Nth bundle on\n"
        "  --no-console        no console device (its words at 0x016b8280 are plain RAM)\n"
        "  --no-stdin          the console delivers no input\n");
    exit(EXIT_USAGE);
}

static uint32_t num(const char *s) { return (uint32_t)strtoul(s, NULL, 0); }

int main(int argc, char **argv) {
    const char *elf = NULL, *bin = NULL, *rom = NULL, *dtb = NULL;
    uint32_t at = 0, entry = 0, ram_mb = 64;
    int have_at = 0, have_entry = 0, i;
    H.budget = 8192; H.pass_time = 20; H.ncores = 1; H.console = H.console_in = 1;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(a, "--trace")) H.trace = 1;
        else if (!strcmp(a, "--strace")) H.strace = 1;
        else if (!strcmp(a, "--stats")) H.stats = 1;
        else if (!strcmp(a, "--no-console")) H.console = 0;
        else if (!strcmp(a, "--no-stdin")) H.console_in = 0;
        else if (!strcmp(a, "--devices")) H.devices = 1;
        else if (!strcmp(a, "--desktop")) H.desktop = 1;
        else if (!strcmp(a, "--ram") && more) ram_mb = num(argv[++i]);
        else if (!strcmp(a, "--rom") && more) rom = argv[++i];
        else if (!strcmp(a, "--dtb") && more) dtb = argv[++i];
        else if (!strcmp(a, "--bin") && more) bin = argv[++i];
        else if (!strcmp(a, "--at") && more) { at = num(argv[++i]); have_at = 1; }
        else if (!strcmp(a, "--entry") && more) { entry = num(argv[++i]); have_entry = 1; }
        else if (!strcmp(a, "--budget") && more) H.budget = num(argv[++i]);
        else if (!strcmp(a, "--pass-time") && more) H.pass_time = num(argv[++i]);
        else if (!strcmp(a, "--cores") && more) H.ncores = num(argv[++i]);
        else if (!strcmp(a, "--profile") && more) H.prof_path = argv[++i];
        else if (!strcmp(a, "--profile-from") && more) H.prof_from = strtoull(argv[++i], NULL, 0);
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
    if (H.prof_path) { atexit(prof_write); signal(SIGTERM, prof_stop); signal(SIGINT, prof_stop); }

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
    if (dtb) {   // at GVM_DTB; the kernel finds its address in r5
        uint32_t n;
        uint8_t *p = slurp(dtb, &n);
        if (GVM_DTB + n > GVM_DEV) { fprintf(stderr, "gvm_cpu: the device tree does not fit below 0x10000\n"); return EXIT_USAGE; }
        uint64_t mem = fdt_memory_size(p, n);
        if (!mem) fprintf(stderr, "gvm_cpu: %s: no memory node found\n", dtb);
        else if (mem != (uint64_t)ram_mb << 20)
            fprintf(stderr, "gvm_cpu: %s: the memory node says %llu MB, --ram %u\n", dtb, (unsigned long long)(mem >> 20), ram_mb);
        place(GVM_DTB, p, n, n, "the device tree");
        free(p);
    }
    // the worker firmware in the boot area (gpu_vm/platform/worker.S): a worker core starts there
    place(GVM_WORKER_FW, (const uint8_t *)gvm_worker_fw, sizeof gvm_worker_fw, sizeof gvm_worker_fw, "the worker firmware");
    if (dtb) H.devices = 1;
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
        if (c) k->halted = 1;   // (a worker from when the kernel lays the workers out: workers_pass)
    }
    if (getenv("GVM_RACES")) race_init(getenv("GVM_RACES"));
    if (H.devices && H.ncores > 1) W(CTRL + 0x3cc) = H.ncores;   // how many cores there could be (the kernel reads it at once)
    if (dtb) {   // a kernel's boot: r5 the device tree's address, every other register 0 (no argument stack)
        H.cores[0].x[5] = GVM_DTB;
        H.stack_limit = H.m.ram_end;
    } else setup_stack(&H.cores[0], argc - i, argv + i);
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
            H.end[c] = (uint8_t)gvm_run(&H.m, k, H.budget);
            // (a worker parked in the firmware, or whose job's loop ended its pass with a user's wait, is idle)
            int idle = c && ((k->pc >= GVM_FW_PARK && k->pc < GVM_FW_PARK_END) || (H.end[c] == GVM_END_WAIT && !(k->sr[GVM_SR_STATUS] & GVM_ST_S)));
            if (!k->waiting && !idle) all_waiting = 0;
        }
        trace_flush();
        if (RC.wmap) race_pass();
        gvm_commit(&H.m);
        H.passes++;
        if (H.console) console_poll();
        workers_pass();
        if (H.devices) devices_pass();
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
                if (k->halted || (c && !k->waiting)) continue;   // (an idle worker's timer is the next pass's)
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
    if (RC.wmap) race_report();
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
