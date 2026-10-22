// gpu_vm's reference machine (gpu_vm/GPU_VM.md): what every op, bundle, trap, pass and commit does, exactly. The
// tick (gpu_vm/tick) is checked against it, gvm_cpu runs Linux on it, and the compiler's tests run on it.
// C99 and C++, header only: one file defines GVM_REF_IMPL before including it.
//
// A machine is RAM (the state texture's texels from row 64: address 0 to ram_end), the ROM, and cores.
// A pass runs each core for up to `budget` bundles from the RAM the last commit left, a core seeing its
// own stores (its write log); the commit then writes every core's stores, the highest core's last, and
// makes the operations the cores asked for (gvm_commit). Nothing a core stores is seen by another core
// before the commit.
#ifndef GVM_REF_H
#define GVM_REF_H
#include <stdint.h>
#include <stddef.h>
#include "../isa.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GVM_WLOG_BITS 12          // the reference's write log: 4,096 words at most a pass, a core
#define GVM_WLOG_SIZE (1u << GVM_WLOG_BITS)

typedef struct gvm_wlog {
    uint32_t key[GVM_WLOG_SIZE];  // word address + 1 (0: empty)
    uint32_t val[GVM_WLOG_SIZE];
    uint32_t order[GVM_WLOG_SIZE];  // slots in the order first written, for the commit
    uint32_t n;
    uint32_t cap;                // words a pass before a store ends it (<= GVM_WLOG_SIZE * 3 / 4)
} gvm_wlog;

// Why a pass ended for a core (gvm_run's result)
enum {
    GVM_END_BUDGET = 0,   // its bundles ran out
    GVM_END_SYNC,         // sync, or the operation it waits for
    GVM_END_WAIT,         // wait: waiting for an interrupt, or a user's wait (the pass's end)
    GVM_END_FULL,         // a store found the write log full; that op runs again next pass
    GVM_END_PARKED,       // a worker parked (or a core with nothing to do)
    GVM_END_HALT,         // the reference's own: a test's break with GVM_REF_BREAK_HALTS
};

typedef struct gvm_core {
    uint32_t x[64];
    uint32_t pc;
    uint32_t sr[GVM_SR_COUNT];
    uint32_t id;
    uint32_t waiting;        // in a supervisor's wait until (IPEND & IEN) != 0
    uint32_t op[8];          // the operation's descriptor texels, as stored at GVM_OPS; op[0] the kind
    uint32_t op_pending;     // stored in this pass: the commit makes it
    uint32_t ext;            // the external interrupt line (the host's)
    uint32_t soft;           // the software interrupt's pending bit
    uint32_t rnd;            // RANDOM's state
    uint32_t left;           // bundles left in this pass (LEFT)
    uint32_t halted;
    uint64_t ops_run, bundles_run;
    gvm_wlog wl;
} gvm_core;

typedef struct gvm_machine {
    uint32_t *ram;           // ram_end / 4 words, address 0 up (the boot area and the devices too)
    uint32_t ram_end;        // bytes; GVM_RAM + RAM's size
    const uint8_t *rom;      // at GVM_ROM; past rom_size reads as 0
    uint32_t rom_size;
    uint64_t time;           // TIME: 100 kHz, the same for the whole of a pass
    uint32_t player;
    uint32_t flags;          // GVM_REF_*
    gvm_core *cores;
    uint32_t ncores;
} gvm_machine;

#define GVM_REF_BREAK_HALTS 1u   // break stops the core (the reference's tests) instead of trapping

void gvm_core_reset(gvm_core *c, uint32_t id, uint32_t pc);
// Runs core c for up to budget bundles; returns GVM_END_*. Takes interrupts between bundles.
int gvm_run(gvm_machine *m, gvm_core *c, uint32_t budget);
// One bundle (or the rest of one from pc's word); GVM_END_BUDGET when it went on normally.
int gvm_bundle(gvm_machine *m, gvm_core *c);
// The commit: every core's write log into RAM (core 0 first, the highest last), then the operations.
void gvm_commit(gvm_machine *m);
// Loads as a core sees them (its own write log first); 0 on a fault.
int gvm_load32(gvm_machine *m, gvm_core *c, uint32_t a, uint32_t *v);

// Floats as the machine makes them (also for the tick's checks)
uint32_t gvm_fop(uint32_t f, uint32_t a, uint32_t b, uint32_t c3);

#ifdef __cplusplus
}
#endif

#ifdef GVM_REF_IMPL
#include <math.h>
#include <string.h>

static uint32_t gvm__ftz(uint32_t a) { return ((a & 0x7f800000u) == 0) ? (a & 0x80000000u) : a; }
static float gvm__f(uint32_t a) { union { uint32_t u; float f; } x; x.u = gvm__ftz(a); return x.f; }
static uint32_t gvm__u(float f) {
    union { uint32_t u; float f; } x; x.f = f;
    if ((x.u & 0x7f800000u) == 0x7f800000u && (x.u & 0x007fffffu)) return 0x7fc00000u;   // the one NaN
    return gvm__ftz(x.u);
}
static int gvm__nan(uint32_t a) { return (a & 0x7f800000u) == 0x7f800000u && (a & 0x007fffffu) != 0; }

uint32_t gvm_fop(uint32_t f, uint32_t a, uint32_t b, uint32_t c3) {
    float x = gvm__f(a), y = gvm__f(b), z = gvm__f(c3);
    switch (f) {
    case GVM_F_ADD: return gvm__u(x + y);
    case GVM_F_SUB: return gvm__u(x - y);
    case GVM_F_MUL: return gvm__u(x * y);
    case GVM_F_DIV: return gvm__u(x / y);
    case GVM_F_MIN:
    case GVM_F_MAX: {
        // a NaN loses to a number; equal values (+0 and -0, a denormal and 0) by sign; an operand's bits as
        // they are
        if (gvm__nan(a) && gvm__nan(b)) return 0x7fc00000u;
        if (gvm__nan(a)) return b;
        if (gvm__nan(b)) return a;
        int less = x < y || (x == y && (a >> 31) > (b >> 31));
        int more = x > y || (x == y && (b >> 31) > (a >> 31));
        return (f == GVM_F_MIN ? less : more) ? a : b;
    }
    case GVM_F_MAD: { volatile float p = x * y; return gvm__u(gvm__f(gvm__u(p)) + z); }
    case GVM_F_MSB: { volatile float p = x * y; return gvm__u(gvm__f(gvm__u(p)) - z); }
    case GVM_F_NMSB: { volatile float p = x * y; return gvm__u(z - gvm__f(gvm__u(p))); }
    case GVM_F_EQ: return x == y;
    case GVM_F_LT: return x < y;
    case GVM_F_LE: return x <= y;
    case GVM_F_SGNJ: return (a & 0x7fffffffu) | (b & 0x80000000u);
    case GVM_F_SGNJN: return (a & 0x7fffffffu) | (~b & 0x80000000u);
    case GVM_F_SGNJX: return a ^ (b & 0x80000000u);
    case GVM_F_UNORD: return gvm__nan(a) || gvm__nan(b);
    case GVM_F_ITOF: return gvm__u((float)(int32_t)a);
    case GVM_F_UTOF: return gvm__u((float)a);
    case GVM_F_FTOI:
        if (gvm__nan(a)) return 0;
        if (x >= 2147483648.0f) return 0x7fffffffu;
        if (x <= -2147483648.0f) return 0x80000000u;
        return (uint32_t)(int32_t)x;
    case GVM_F_FTOU:
        if (gvm__nan(a) || x <= 0.0f) return 0;
        if (x >= 4294967296.0f) return 0xffffffffu;
        return (uint32_t)x;
    case GVM_F_FLOOR: return gvm__u(floorf(x));
    case GVM_F_CEIL: return gvm__u(ceilf(x));
    case GVM_F_TRUNC: return gvm__u(truncf(x));
    case GVM_F_ROUND: return gvm__u(nearbyintf(x));   // to even under the default mode
    case GVM_F_SQRT: return gvm__u(sqrtf(x));
    case GVM_F_RSQRT: return gvm__u(1.0f / sqrtf(x));
    case GVM_F_RCP: return gvm__u(1.0f / x);
    case GVM_F_SIN: return gvm__u(sinf(x));
    case GVM_F_COS: return gvm__u(cosf(x));
    case GVM_F_EXP2: return gvm__u(exp2f(x));
    case GVM_F_LOG2: return gvm__u(log2f(x));
    case GVM_F_FRAC: return gvm__u(x - floorf(x));
    }
    return 0;
}

void gvm_core_reset(gvm_core *c, uint32_t id, uint32_t pc) {
    memset(c, 0, sizeof *c);
    c->id = id;
    c->pc = pc;
    c->sr[GVM_SR_STATUS] = GVM_ST_S;
    c->rnd = 0x9e3779b9u ^ (id * 0x85ebca6bu);
    c->wl.cap = GVM_WLOG_SIZE * 3 / 4;
}

static uint32_t *gvm__wl_find(gvm_wlog *w, uint32_t word, int make) {
    uint32_t h = (word * 0x9e3779b1u) >> (32 - GVM_WLOG_BITS);
    for (;;) {
        if (w->key[h] == word + 1) return &w->val[h];
        if (w->key[h] == 0) {
            if (!make) return NULL;
            w->key[h] = word + 1;
            w->order[w->n++] = h;
            return &w->val[h];
        }
        h = (h + 1) & (GVM_WLOG_SIZE - 1);
    }
}

static int gvm__readable(const gvm_machine *m, uint32_t a) { return a < m->ram_end || (a >= GVM_ROM && a < GVM_ROM_END); }
static int gvm__writable(const gvm_machine *m, uint32_t a) { return a >= GVM_DEV && a < m->ram_end; }

// A word as the core sees it; a is word aligned and readable
static uint32_t gvm__word(gvm_machine *m, gvm_core *c, uint32_t a) {
    if (a >= GVM_ROM) {
        uint32_t o = a - GVM_ROM;
        if (o + 4 > m->rom_size) return 0;
        return (uint32_t)m->rom[o] | (uint32_t)m->rom[o + 1] << 8 | (uint32_t)m->rom[o + 2] << 16 | (uint32_t)m->rom[o + 3] << 24;
    }
    if (c) {
        uint32_t *e = gvm__wl_find(&c->wl, a >> 2, 0);
        if (e) return *e;
    }
    return m->ram[a >> 2];
}

// A load of `bytes` (1, 2, 4) at any alignment; 0 on a fault
static int gvm__load(gvm_machine *m, gvm_core *c, uint32_t a, uint32_t bytes, uint32_t *v) {
    if (a >= GVM_OPS && a + bytes <= GVM_OPS + 32 && !((a | bytes) & 3)) { *v = c->op[(a - GVM_OPS) >> 2]; return 1; }
    if (!gvm__readable(m, a) || !gvm__readable(m, a + bytes - 1)) return 0;
    uint32_t lo = gvm__word(m, c, a & ~3u), sh = (a & 3) * 8;
    uint64_t w = lo;
    if ((a & 3) + bytes > 4) w |= (uint64_t)gvm__word(m, c, (a & ~3u) + 4) << 32;
    w >>= sh;
    *v = bytes == 4 ? (uint32_t)w : (uint32_t)w & ((1u << (bytes * 8)) - 1);
    return 1;
}

int gvm_load32(gvm_machine *m, gvm_core *c, uint32_t a, uint32_t *v) { return gvm__load(m, c, a, 4, v); }

// Room for the words a store of `bytes` at a makes, or 0: the pass ends and the op runs again
static int gvm__room(gvm_core *c, uint32_t a, uint32_t bytes) {
    uint32_t need = 0, w;
    for (w = a >> 2; w <= (a + bytes - 1) >> 2; w++) need += gvm__wl_find(&c->wl, w, 0) == NULL;
    return c->wl.n + need <= c->wl.cap;
}

static void gvm__store(gvm_machine *m, gvm_core *c, uint32_t a, uint32_t bytes, uint32_t v) {
    if (a >= GVM_OPS) {
        c->op[(a - GVM_OPS) >> 2] = v;
        c->op_pending = 1;
        return;
    }
    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t b = a + i, word = b >> 2, sh = (b & 3) * 8;
        uint32_t *e = gvm__wl_find(&c->wl, word, 0);
        uint32_t old = e ? *e : m->ram[word];
        if (!e) e = gvm__wl_find(&c->wl, word, 1);
        *e = (old & ~(0xffu << sh)) | ((v >> (8 * i)) & 0xffu) << sh;
    }
}

static int gvm__store_ok(const gvm_machine *m, uint32_t a, uint32_t bytes) {
    if (a >= GVM_OPS) return a + bytes <= GVM_OPS + 32 && !((a | bytes) & 3);
    return gvm__writable(m, a) && gvm__writable(m, a + bytes - 1);
}

static void gvm__trap(gvm_core *c, uint32_t cause, uint32_t epc, uint32_t tval) {
    uint32_t st = c->sr[GVM_SR_STATUS];
    c->sr[GVM_SR_EPC] = epc;
    c->sr[GVM_SR_CAUSE] = cause;
    c->sr[GVM_SR_TVAL] = tval;
    st = (st & ~(GVM_ST_PIE | GVM_ST_PS | GVM_ST_IE)) | ((st & GVM_ST_IE) ? GVM_ST_PIE : 0) | ((st & GVM_ST_S) ? GVM_ST_PS : 0) | GVM_ST_S;
    c->sr[GVM_SR_STATUS] = st;
    c->pc = c->sr[GVM_SR_TVEC] & ~15u;
    c->waiting = 0;
}

static uint32_t gvm__ipend(const gvm_machine *m, const gvm_core *c) {
    uint64_t cmp = (uint64_t)c->sr[GVM_SR_TCMP_HI] << 32 | c->sr[GVM_SR_TCMP_LO];
    return (c->soft ? 1u << GVM_IRQ_SOFT : 0) | (cmp != 0 && m->time >= cmp ? 1u << GVM_IRQ_TIMER : 0) | (c->ext ? 1u << GVM_IRQ_EXT : 0);
}

static uint32_t gvm__sr_read(gvm_machine *m, gvm_core *c, uint32_t n) {
    switch (n) {
    case GVM_SR_IPEND: return gvm__ipend(m, c);
    case GVM_SR_TIME_LO: return (uint32_t)m->time;
    case GVM_SR_TIME_HI: return (uint32_t)(m->time >> 32);
    case GVM_SR_CYCLE: return (uint32_t)c->bundles_run;
    case GVM_SR_COREID: return c->id;
    case GVM_SR_PLAYER: return m->player;
    case GVM_SR_RANDOM: c->rnd ^= c->rnd << 13; c->rnd ^= c->rnd >> 17; c->rnd ^= c->rnd << 5; return c->rnd;
    case GVM_SR_LEFT: return c->left;
    case GVM_SR_OPS: return (uint32_t)c->ops_run;
    }
    return n < GVM_SR_COUNT ? c->sr[n] : 0;
}

static void gvm__sr_write(gvm_core *c, uint32_t n, uint32_t v) {
    switch (n) {
    case GVM_SR_STATUS: c->sr[n] = (v & (GVM_ST_IE | GVM_ST_PIE | GVM_ST_PS)) | (c->sr[n] & GVM_ST_S); return;
    case GVM_SR_EPC: case GVM_SR_CAUSE: case GVM_SR_TVAL: case GVM_SR_TVEC: case GVM_SR_SCRATCH: case GVM_SR_IEN:
    case GVM_SR_TCMP_LO: case GVM_SR_TCMP_HI:
        c->sr[n] = v; return;
    case GVM_SR_IPEND: c->soft = v & 1; return;   // the software bit alone is written
    }
}

static uint32_t gvm__ext(uint32_t f, uint32_t a, uint32_t b) {
    switch (f) {
    case GVM_X_MULH: return (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32);
    case GVM_X_MULHU: return (uint32_t)(((uint64_t)a * b) >> 32);
    case GVM_X_MULHSU: return (uint32_t)(((int64_t)(int32_t)a * (int64_t)(uint64_t)b) >> 32);
    case GVM_X_DIV: return b == 0 ? 0xffffffffu : (a == 0x80000000u && b == 0xffffffffu) ? a : (uint32_t)((int32_t)a / (int32_t)b);
    case GVM_X_DIVU: return b == 0 ? 0xffffffffu : a / b;
    case GVM_X_REM: return b == 0 ? a : (a == 0x80000000u && b == 0xffffffffu) ? 0 : (uint32_t)((int32_t)a % (int32_t)b);
    case GVM_X_REMU: return b == 0 ? a : a % b;
    case GVM_X_CLZ: { uint32_t n = 0; while (n < 32 && !(a & (0x80000000u >> n))) n++; return n; }
    case GVM_X_CTZ: { uint32_t n = 0; while (n < 32 && !(a & (1u << n))) n++; return n; }
    case GVM_X_POPC: { uint32_t n = 0; while (a) { n += a & 1; a >>= 1; } return n; }
    case GVM_X_BSWAP: return a >> 24 | (a >> 8 & 0xff00u) | (a << 8 & 0xff0000u) | a << 24;
    case GVM_X_BREV: { uint32_t r = 0; for (int i = 0; i < 32; i++) r |= ((a >> i) & 1) << (31 - i); return r; }
    }
    return 0;
}

static uint32_t gvm__int(uint32_t f, uint32_t a, uint32_t b) {
    uint32_t s = b & 31;
    switch (f) {
    case GVM_I_ADD: return a + b;
    case GVM_I_SUB: return a - b;
    case GVM_I_AND: return a & b;
    case GVM_I_OR: return a | b;
    case GVM_I_XOR: return a ^ b;
    case GVM_I_ANDN: return a & ~b;
    case GVM_I_ORN: return a | ~b;
    case GVM_I_XNOR: return ~(a ^ b);
    case GVM_I_SLL: return a << s;
    case GVM_I_SRL: return a >> s;
    case GVM_I_SRA: return (uint32_t)((int32_t)a >> s);
    case GVM_I_ROL: return s ? (a << s | a >> (32 - s)) : a;
    case GVM_I_ROR: return s ? (a >> s | a << (32 - s)) : a;
    case GVM_I_SLT: return (int32_t)a < (int32_t)b;
    case GVM_I_SLTU: return a < b;
    case GVM_I_SEQ: return a == b;
    case GVM_I_SNE: return a != b;
    case GVM_I_SGE: return (int32_t)a >= (int32_t)b;
    case GVM_I_SGEU: return a >= b;
    case GVM_I_MIN: return (int32_t)a < (int32_t)b ? a : b;
    case GVM_I_MAX: return (int32_t)a > (int32_t)b ? a : b;
    case GVM_I_MINU: return a < b ? a : b;
    case GVM_I_MAXU: return a > b ? a : b;
    case GVM_I_MUL: return a * b;
    }
    return 0;
}

static uint32_t gvm__bfe(uint32_t a, uint32_t imm) {
    uint32_t pos = imm & 31, len = ((imm >> 5) & 31) + 1, v = a >> pos;
    if (len < 32) {
        v &= (1u << len) - 1;
        if ((imm & 0x400) && (v >> (len - 1)) & 1) v |= ~0u << len;
    }
    return v;
}

static uint32_t gvm__bfi(uint32_t d, uint32_t a, uint32_t imm) {
    uint32_t pos = imm & 31, len = ((imm >> 5) & 31) + 1;
    uint32_t m = len >= 32 ? ~0u : (1u << len) - 1;
    return (d & ~(m << pos)) | ((a & m) << pos);
}

static void gvm__set(gvm_core *c, uint32_t r, uint32_t v) { if (r & 63) c->x[r & 63] = v; }

enum { GVM__GO = 0, GVM__TRAPPED = 1, GVM__END = 2 };

// One op word at slot s of the bundle at base; is_long: word 2 is its immediate (the control word says so).
static int gvm__op(gvm_machine *m, gvm_core *c, uint32_t base, uint32_t s, const uint32_t *t, uint32_t n, int is_long, int *long_used, int *end) {
    uint32_t w = t[s], opc = GVM_OPC(w), rd = GVM_RD(w), ra = GVM_RA(w), rb = GVM_RB(w), rc = GVM_RC(w);
    uint32_t epc = base | s * 4;
    uint32_t imm = (uint32_t)GVM_IMM12(w);
    int super = (c->sr[GVM_SR_STATUS] & GVM_ST_S) != 0;
    *long_used = 0;
    uint32_t cls = GVM_CLASS(opc);
    if (is_long) {
        imm = t[2];
        *long_used = 1;
    }
    uint32_t a = c->x[ra], b = c->x[rb];
    c->ops_run++;
    switch (cls) {
    case 1: {   // integer
        uint32_t f = opc & 31;
        if (!GVM_IFORM(opc)) {
            switch (f) {
            case GVM_I_SHADD: gvm__set(c, rd, a + (b << (rc & 31))); return GVM__GO;
            case GVM_I_SEL: gvm__set(c, rd, c->x[rc] != 0 ? a : b); return GVM__GO;
            case GVM_I_ADDCY: gvm__set(c, rd, a + b + (c->x[rc] & 1)); return GVM__GO;
            case GVM_I_CARRY: { uint64_t r = (uint64_t)a + b + (c->x[rc] & 1); gvm__set(c, rd, (uint32_t)(r >> 32)); return GVM__GO; }
            case GVM_I_SUBBY: gvm__set(c, rd, a - b - (c->x[rc] & 1)); return GVM__GO;
            case GVM_I_BORROW: { uint64_t r = (uint64_t)a - b - (c->x[rc] & 1); gvm__set(c, rd, (uint32_t)(r >> 63)); return GVM__GO; }
            }
            if (f > GVM_I_COMMON_LAST) break;
            gvm__set(c, rd, gvm__int(f, a, b));
            return GVM__GO;
        }
        if (f == GVM_I_BFE) { gvm__set(c, rd, gvm__bfe(a, imm)); return GVM__GO; }
        if (f > GVM_I_COMMON_LAST) break;
        gvm__set(c, rd, gvm__int(f, a, imm));   // (sub: ra - imm, since x15)
        return GVM__GO;
    }
    case 2: {   // memory
        uint32_t mode = (opc >> 3) & 7, wd = opc & 7;
        static const uint32_t bytes_of[8] = {4, 8, 2, 1, 16, 0, 2, 1};   // by GVM_W_* (x15)
        if (mode <= 3) {
            uint32_t by = bytes_of[wd];
            if (!by) break;
            uint32_t addr = (mode <= 1) ? a + imm : a + (b << (rc & 31));
            if (mode == 0 || mode == 2) {
                uint32_t v[4] = {0, 0, 0, 0}, k, words = by > 4 ? by / 4 : 1;
                for (k = 0; k < words; k++) {
                    if (!gvm__load(m, c, addr + 4 * k, by > 4 ? 4 : by, &v[k])) { c->ops_run--; gvm__trap(c, GVM_CAUSE_LOAD, epc, addr); return GVM__TRAPPED; }
                }
                if (wd == GVM_W_B) v[0] = (uint32_t)(int32_t)(int8_t)v[0];
                if (wd == GVM_W_H) v[0] = (uint32_t)(int32_t)(int16_t)v[0];
                for (k = 0; k < words; k++) gvm__set(c, rd + k, v[k]);
                return GVM__GO;
            }
            if (!gvm__store_ok(m, addr, by)) { c->ops_run--; gvm__trap(c, GVM_CAUSE_STORE, epc, addr); return GVM__TRAPPED; }
            if (addr < GVM_OPS && !gvm__room(c, addr, by)) { c->ops_run--; c->pc = epc; *end = GVM_END_FULL; return GVM__END; }
            if (by <= 4) gvm__store(m, c, addr, by, c->x[rd]);
            else for (uint32_t k = 0; k < by / 4; k++) gvm__store(m, c, addr + 4 * k, 4, c->x[(rd + k) & 63]);
            return GVM__GO;
        }
        if (mode == 4 || (mode == 5 && wd <= GVM_AMO2_MAXU)) {
            uint32_t old, nw;
            if ((a & 3) || !gvm__store_ok(m, a, 4) || a >= GVM_OPS) { c->ops_run--; gvm__trap(c, GVM_CAUSE_STORE, epc, a); return GVM__TRAPPED; }
            if (!gvm__room(c, a, 4)) { c->ops_run--; c->pc = epc; *end = GVM_END_FULL; return GVM__END; }
            old = gvm__word(m, c, a);
            if (mode == 5) nw = wd == GVM_AMO2_MINU ? (old < b ? old : b) : (old > b ? old : b);
            else switch (wd) {
            case GVM_AMO_SWAP: nw = b; break;
            case GVM_AMO_ADD: nw = old + b; break;
            case GVM_AMO_AND: nw = old & b; break;
            case GVM_AMO_OR: nw = old | b; break;
            case GVM_AMO_XOR: nw = old ^ b; break;
            case GVM_AMO_MIN: nw = (int32_t)old < (int32_t)b ? old : b; break;
            case GVM_AMO_MAX: nw = (int32_t)old > (int32_t)b ? old : b; break;
            default: nw = old == c->x[rd] ? b : old; break;   // cas
            }
            gvm__store(m, c, a, 4, nw);
            gvm__set(c, rd, old);
            return GVM__GO;
        }
        break;
    }
    case 3: {   // float
        uint32_t f = opc & 31;
        if (GVM_IFORM(opc)) {
            if (f == GVM_F_MAD || f == GVM_F_MSB || f == GVM_F_NMSB) break;
            b = *long_used ? imm : imm << 20;
        }
        gvm__set(c, rd, gvm_fop(f, a, b, c->x[rc]));
        return GVM__GO;
    }
    default:   // extended
        switch (opc) {
        case GVM_X_MULH: case GVM_X_MULHU: case GVM_X_MULHSU: case GVM_X_DIV: case GVM_X_DIVU: case GVM_X_REM: case GVM_X_REMU:
        case GVM_X_CLZ: case GVM_X_CTZ: case GVM_X_POPC: case GVM_X_BSWAP: case GVM_X_BREV:
            gvm__set(c, rd, gvm__ext(opc, a, b));
            return GVM__GO;
        case GVM_X_BFI: gvm__set(c, rd, gvm__bfi(c->x[rd], a, imm)); return GVM__GO;
        case GVM_X_LPC: gvm__set(c, rd, base + imm); return GVM__GO;
        case GVM_X_SYSCALL: c->ops_run--; gvm__trap(c, GVM_CAUSE_SYSCALL, epc, 0); return GVM__TRAPPED;
        case GVM_X_BREAK:
            c->ops_run--;
            if (m->flags & GVM_REF_BREAK_HALTS) { c->pc = epc; c->halted = 1; *end = GVM_END_HALT; return GVM__END; }
            gvm__trap(c, GVM_CAUSE_BREAK, epc, 0);
            return GVM__TRAPPED;
        case GVM_X_WAIT:
            c->pc = epc + 4;
            if (!super) { *end = GVM_END_WAIT; return GVM__END; }   // a user's wait: the pass's end
            if (gvm__ipend(m, c) & c->sr[GVM_SR_IEN]) return GVM__GO;
            c->waiting = 1;
            *end = GVM_END_WAIT;
            return GVM__END;
        case GVM_X_SYNC: c->pc = epc + 4; *end = GVM_END_SYNC; return GVM__END;
        case GVM_X_FENCE: return GVM__GO;
        case GVM_X_MFSR: case GVM_X_MTSR: case GVM_X_XSR: case GVM_X_SRSET: case GVM_X_SRCLR: {
            uint32_t n2 = imm & 0xfff;
            if (n2 >= GVM_SR_COUNT) break;
            if (!super && (opc != GVM_X_MFSR || n2 < GVM_SR_USER_FIRST)) { c->ops_run--; gvm__trap(c, GVM_CAUSE_PRIV, epc, w); return GVM__TRAPPED; }
            uint32_t old = gvm__sr_read(m, c, n2);
            if (opc == GVM_X_MTSR) gvm__sr_write(c, n2, a);
            if (opc == GVM_X_XSR) gvm__sr_write(c, n2, a);
            if (opc == GVM_X_SRSET) gvm__sr_write(c, n2, old | a);
            if (opc == GVM_X_SRCLR) gvm__sr_write(c, n2, old & ~a);
            if (opc != GVM_X_MTSR) gvm__set(c, rd, old);
            return GVM__GO;
        }
        }
        break;
    }
    c->ops_run--;
    gvm__trap(c, GVM_CAUSE_ILLEGAL, epc, w);
    return GVM__TRAPPED;
}

static int gvm__cond(uint32_t cc, uint32_t a, uint32_t bf, uint32_t b) {
    int32_t i6 = (int32_t)(bf << 26) >> 26;
    switch (cc) {
    case GVM_CC_BEQ: return a == b;
    case GVM_CC_BNE: return a != b;
    case GVM_CC_BLT: return (int32_t)a < (int32_t)b;
    case GVM_CC_BGE: return (int32_t)a >= (int32_t)b;
    case GVM_CC_BLTU: return a < b;
    case GVM_CC_BGEU: return a >= b;
    case GVM_CC_BBS: return (a >> (bf & 31)) & 1;
    case GVM_CC_BBC: return !((a >> (bf & 31)) & 1);
    case GVM_CC_BEQI: return (int32_t)a == i6;
    case GVM_CC_BNEI: return (int32_t)a != i6;
    case GVM_CC_BLTI: return (int32_t)a < i6;
    case GVM_CC_BGEI: return (int32_t)a >= i6;
    case GVM_CC_BLTUI: return a < (uint32_t)i6;   // imm6 sign-extended (x15)
    case GVM_CC_BGEUI: return a >= (uint32_t)i6;
    }
    return 0;
}

int gvm_bundle(gvm_machine *m, gvm_core *c) {
    uint32_t pc = c->pc, base = pc & ~15u, s = (pc & 15) >> 2, t[4], k;
    int end = GVM_END_BUDGET;
    if (base >= m->ram_end) { gvm__trap(c, GVM_CAUSE_FETCH, pc, pc); return GVM_END_BUDGET; }
    for (k = 0; k < 4; k++) t[k] = m->ram[(base >> 2) + k];   // code is fetched from RAM as committed
    uint32_t ctl = t[3], n = GVM_N(ctl);
    c->bundles_run++;
    if (n == 3) { gvm__trap(c, GVM_CAUSE_ILLEGAL, base | 12, ctl); return GVM_END_BUDGET; }
    // the branch's registers as this run of the bundle begins
    uint32_t ca0 = c->x[GVM_CRA(ctl)], cb0 = c->x[GVM_CRB(ctl)], cs0 = c->x[GVM_CRS(ctl)];
    for (; s < n; s++) {
        int lu = 0, r = gvm__op(m, c, base, s, t, n, (s == 0 ? GVM_LONG0(ctl) : GVM_LONG1(ctl)) != 0, &lu, &end);
        if (r == GVM__TRAPPED) return GVM_END_BUDGET;
        if (r == GVM__END) {
            // the pass ends inside the bundle: pc is where it goes on (past n: the control word)
            if ((c->pc & 15) >> 2 >= n) c->pc = base | 12;
            return end;
        }
        (void)lu;
    }
    // the control word: a branch on the registers as the bundle found them, jr and callr on its ops' results
    uint32_t cc = GVM_CC(ctl), next = base + 16;
    int super = (c->sr[GVM_SR_STATUS] & GVM_ST_S) != 0;
    if (cc == GVM_CC_SPECIAL) {
        uint32_t sub = GVM_SUB(ctl), rs = GVM_CRS(ctl), to;
        switch (sub) {
        case GVM_SUB_NONE: break;
        case GVM_SUB_JR: next = c->x[rs] & ~15u; break;
        case GVM_SUB_CALLR: to = c->x[rs] & ~15u; c->x[1] = base + 16; next = to; break;
        case GVM_SUB_ERET: {
            if (!super) { gvm__trap(c, GVM_CAUSE_PRIV, base | 12, ctl); return GVM_END_BUDGET; }
            uint32_t st = c->sr[GVM_SR_STATUS];
            next = c->sr[GVM_SR_EPC];
            st = (st & ~(GVM_ST_IE | GVM_ST_S | GVM_ST_PS)) | ((st & GVM_ST_PIE) ? GVM_ST_IE : 0) | ((st & GVM_ST_PS) ? GVM_ST_S : 0) | GVM_ST_PIE;
            c->sr[GVM_SR_STATUS] = st;
            break;
        }
        case GVM_SUB_J: next = base + (uint32_t)GVM_OFF18(ctl) * 16; break;
        case GVM_SUB_CALL: c->x[1] = base + 16; next = base + (uint32_t)GVM_OFF18(ctl) * 16; break;
        case GVM_SUB_ADD:   // after the ops: it wins; r0 is illegal
            if (rs == 0) { gvm__trap(c, GVM_CAUSE_ILLEGAL, base | 12, ctl); return GVM_END_BUDGET; }
            gvm__set(c, rs, cs0 + (uint32_t)GVM_CIMM12(ctl));
            break;
        default: gvm__trap(c, GVM_CAUSE_ILLEGAL, base | 12, ctl); return GVM_END_BUDGET;
        }
    } else if (cc == GVM_CC_ADDBNZ) {   // ra += imm6 after the ops (it wins); taken when the sum is not 0
        uint32_t v = ca0 + (uint32_t)GVM_CIMM6(ctl);
        if (GVM_CRA(ctl) == 0) { gvm__trap(c, GVM_CAUSE_ILLEGAL, base | 12, ctl); return GVM_END_BUDGET; }   // r0: illegal
        gvm__set(c, GVM_CRA(ctl), v);
        if (v != 0) next = base + (uint32_t)GVM_OFF9(ctl) * 16;
    } else {
        uint32_t ra = GVM_CRA(ctl), bf = GVM_CRB(ctl);
        (void)ra;
        if (gvm__cond(cc, ca0, bf, cb0)) next = base + (uint32_t)GVM_OFF9(ctl) * 16;
    }
    c->pc = next;
    return GVM_END_BUDGET;
}

int gvm_run(gvm_machine *m, gvm_core *c, uint32_t budget) {
    c->left = budget;
    if (c->halted) return GVM_END_HALT;
    if (c->op[0] != 0 && c->op[0] != 0xffffffffu && !c->op_pending) return GVM_END_SYNC;   // (an operation in flight)
    if (c->waiting) {
        if (!(gvm__ipend(m, c) & c->sr[GVM_SR_IEN])) return GVM_END_WAIT;
        c->waiting = 0;
    }
    while (c->left > 0) {
        // interrupts, between bundles only
        if ((c->pc & 15) == 0 && (c->sr[GVM_SR_STATUS] & GVM_ST_IE)) {
            uint32_t p = gvm__ipend(m, c) & c->sr[GVM_SR_IEN];
            if (p) {
                uint32_t bit = (p & 4) ? 2 : (p & 2) ? 1 : 0;
                gvm__trap(c, GVM_CAUSE_IRQ | bit, c->pc, 0);
            }
        }
        c->left--;
        int r = gvm_bundle(m, c);
        if (r != GVM_END_BUDGET) return r;
    }
    return GVM_END_BUDGET;
}

static uint8_t gvm__byte(gvm_machine *m, uint32_t a) {
    if (a >= GVM_ROM) return a - GVM_ROM < m->rom_size ? m->rom[a - GVM_ROM] : 0;
    return (uint8_t)(m->ram[a >> 2] >> ((a & 3) * 8));
}
static void gvm__put_byte(gvm_machine *m, uint32_t a, uint8_t v) {
    uint32_t sh = (a & 3) * 8;
    m->ram[a >> 2] = (m->ram[a >> 2] & ~(0xffu << sh)) | (uint32_t)v << sh;
}
static int gvm__range(gvm_machine *m, uint32_t a, uint32_t n, int dst) {
    if (n == 0) return 1;
    uint32_t e = a + n - 1;
    if (e < a) return 0;
    if (dst) return a >= GVM_DEV && e < m->ram_end;
    return e < m->ram_end || (a >= GVM_ROM && e < GVM_ROM_END);
}

// The operation a core asked for, made at once (the tick takes passes; what lands is the same)
static void gvm__operation(gvm_machine *m, gvm_core *c) {
    uint32_t *d = c->op, k = d[0], ok = 0;
    if (k == GVM_OP_COPY && gvm__range(m, d[1], d[3], 1) && gvm__range(m, d[2], d[3], 0)) {
        uint32_t i, n = d[3];
        if (d[1] <= d[2]) for (i = 0; i < n; i++) gvm__put_byte(m, d[1] + i, gvm__byte(m, d[2] + i));
        else for (i = n; i-- > 0;) gvm__put_byte(m, d[1] + i, gvm__byte(m, d[2] + i));
        ok = 1;
    } else if (k == GVM_OP_FILL && gvm__range(m, d[1], d[3], 1)) {
        for (uint32_t i = 0; i < d[3]; i++) gvm__put_byte(m, d[1] + i, (uint8_t)(d[2] >> ((i & 3) * 8)));
        ok = 1;
    } else if (k == GVM_OP_BLIT) {
        uint32_t dst = d[1], src = d[2], ds = d[4], ss = d[5], row = d[6], rows = d[7], r;
        ok = rows == 0 || row == 0 ||
             (gvm__range(m, dst, ds * (rows - 1) + row, 1) && gvm__range(m, src, ss * (rows - 1) + row, 0));
        for (r = 0; ok && r < rows; r++)
            for (uint32_t i = 0; i < row; i++) gvm__put_byte(m, dst + r * ds + i, gvm__byte(m, src + r * ss + i));
    }
    d[0] = ok ? 0 : 0xffffffffu;
}

void gvm_commit(gvm_machine *m) {
    uint32_t i, j;
    for (i = 0; i < m->ncores; i++) {
        gvm_wlog *w = &m->cores[i].wl;
        for (j = 0; j < w->n; j++) {
            uint32_t h = w->order[j];
            m->ram[w->key[h] - 1] = w->val[h];
            w->key[h] = 0;
        }
        w->n = 0;
    }
    for (i = 0; i < m->ncores; i++) {
        gvm_core *c = &m->cores[i];
        if (c->op_pending) { c->op_pending = 0; gvm__operation(m, c); }
    }
}
#endif   // GVM_REF_IMPL
#endif
