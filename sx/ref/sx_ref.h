// SX's reference machine (sx/SX.md): what every op, bundle, trap, pass and commit does, exactly. The
// tick (sx/tick) is checked against it, sx_cpu runs Linux on it, and the compiler's tests run on it.
// C99 and C++, header only: one file defines SX_REF_IMPL before including it.
//
// A machine is RAM (the state texture's texels from row 64: address 0 to ram_end), the ROM, and cores.
// A pass runs each core for up to `budget` bundles from the RAM the last commit left, a core seeing its
// own stores (its write log); the commit then writes every core's stores, the highest core's last, and
// makes the operations the cores asked for (sx_commit). Nothing a core stores is seen by another core
// before the commit.
#ifndef SX_REF_H
#define SX_REF_H
#include <stdint.h>
#include <stddef.h>
#include "../isa.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SX_WLOG_BITS 12          // the reference's write log: 4,096 words at most a pass, a core
#define SX_WLOG_SIZE (1u << SX_WLOG_BITS)

typedef struct sx_wlog {
    uint32_t key[SX_WLOG_SIZE];  // word address + 1 (0: empty)
    uint32_t val[SX_WLOG_SIZE];
    uint32_t order[SX_WLOG_SIZE];  // slots in the order first written, for the commit
    uint32_t n;
    uint32_t cap;                // words a pass before a store ends it (<= SX_WLOG_SIZE * 3 / 4)
} sx_wlog;

// Why a pass ended for a core (sx_run's result)
enum {
    SX_END_BUDGET = 0,   // its bundles ran out
    SX_END_SYNC,         // sync, or the operation it waits for
    SX_END_WAIT,         // wait: waiting for an interrupt, or a user's wait (the pass's end)
    SX_END_FULL,         // a store found the write log full; that op runs again next pass
    SX_END_PARKED,       // a worker parked (or a core with nothing to do)
    SX_END_HALT,         // the reference's own: a test's break with SX_REF_BREAK_HALTS
};

typedef struct sx_core {
    uint32_t x[64];
    uint32_t pc;
    uint32_t sr[SX_SR_COUNT];
    uint32_t id;
    uint32_t waiting;        // in a supervisor's wait until (IPEND & IEN) != 0
    uint32_t op[8];          // the operation's descriptor texels, as stored at SX_OPS; op[0] the kind
    uint32_t op_pending;     // stored in this pass: the commit makes it
    uint32_t ext;            // the external interrupt line (the host's)
    uint32_t soft;           // the software interrupt's pending bit
    uint32_t rnd;            // RANDOM's state
    uint32_t left;           // bundles left in this pass (LEFT)
    uint32_t halted;
    uint64_t ops_run, bundles_run;
    sx_wlog wl;
} sx_core;

typedef struct sx_machine {
    uint32_t *ram;           // ram_end / 4 words, address 0 up (the boot area and the devices too)
    uint32_t ram_end;        // bytes; SX_RAM + RAM's size
    const uint8_t *rom;      // at SX_ROM; past rom_size reads as 0
    uint32_t rom_size;
    uint64_t time;           // TIME: 100 kHz, the same for the whole of a pass
    uint32_t player;
    uint32_t flags;          // SX_REF_*
    sx_core *cores;
    uint32_t ncores;
} sx_machine;

#define SX_REF_BREAK_HALTS 1u   // break stops the core (the reference's tests) instead of trapping

void sx_core_reset(sx_core *c, uint32_t id, uint32_t pc);
// Runs core c for up to budget bundles; returns SX_END_*. Takes interrupts between bundles.
int sx_run(sx_machine *m, sx_core *c, uint32_t budget);
// One bundle (or the rest of one from pc's word); SX_END_BUDGET when it went on normally.
int sx_bundle(sx_machine *m, sx_core *c);
// The commit: every core's write log into RAM (core 0 first, the highest last), then the operations.
void sx_commit(sx_machine *m);
// Loads as a core sees them (its own write log first); 0 on a fault.
int sx_load32(sx_machine *m, sx_core *c, uint32_t a, uint32_t *v);

// Floats as the machine makes them (also for the tick's checks)
uint32_t sx_fop(uint32_t f, uint32_t a, uint32_t b, uint32_t c3);

#ifdef __cplusplus
}
#endif

#ifdef SX_REF_IMPL
#include <math.h>
#include <string.h>

static uint32_t sx__ftz(uint32_t a) { return ((a & 0x7f800000u) == 0) ? (a & 0x80000000u) : a; }
static float sx__f(uint32_t a) { union { uint32_t u; float f; } x; x.u = sx__ftz(a); return x.f; }
static uint32_t sx__u(float f) {
    union { uint32_t u; float f; } x; x.f = f;
    if ((x.u & 0x7f800000u) == 0x7f800000u && (x.u & 0x007fffffu)) return 0x7fc00000u;   // the one NaN
    return sx__ftz(x.u);
}
static int sx__nan(uint32_t a) { return (a & 0x7f800000u) == 0x7f800000u && (a & 0x007fffffu) != 0; }

uint32_t sx_fop(uint32_t f, uint32_t a, uint32_t b, uint32_t c3) {
    float x = sx__f(a), y = sx__f(b), z = sx__f(c3);
    switch (f) {
    case SX_F_ADD: return sx__u(x + y);
    case SX_F_SUB: return sx__u(x - y);
    case SX_F_MUL: return sx__u(x * y);
    case SX_F_DIV: return sx__u(x / y);
    case SX_F_MIN:
    case SX_F_MAX: {
        // a NaN loses to a number; equal values (+0 and -0, a denormal and 0) by sign; an operand's bits as
        // they are
        if (sx__nan(a) && sx__nan(b)) return 0x7fc00000u;
        if (sx__nan(a)) return b;
        if (sx__nan(b)) return a;
        int less = x < y || (x == y && (a >> 31) > (b >> 31));
        int more = x > y || (x == y && (b >> 31) > (a >> 31));
        return (f == SX_F_MIN ? less : more) ? a : b;
    }
    case SX_F_MAD: { volatile float p = x * y; return sx__u(sx__f(sx__u(p)) + z); }
    case SX_F_MSB: { volatile float p = x * y; return sx__u(sx__f(sx__u(p)) - z); }
    case SX_F_NMSB: { volatile float p = x * y; return sx__u(z - sx__f(sx__u(p))); }
    case SX_F_EQ: return x == y;
    case SX_F_LT: return x < y;
    case SX_F_LE: return x <= y;
    case SX_F_SGNJ: return (a & 0x7fffffffu) | (b & 0x80000000u);
    case SX_F_SGNJN: return (a & 0x7fffffffu) | (~b & 0x80000000u);
    case SX_F_SGNJX: return a ^ (b & 0x80000000u);
    case SX_F_UNORD: return sx__nan(a) || sx__nan(b);
    case SX_F_ITOF: return sx__u((float)(int32_t)a);
    case SX_F_UTOF: return sx__u((float)a);
    case SX_F_FTOI:
        if (sx__nan(a)) return 0;
        if (x >= 2147483648.0f) return 0x7fffffffu;
        if (x <= -2147483648.0f) return 0x80000000u;
        return (uint32_t)(int32_t)x;
    case SX_F_FTOU:
        if (sx__nan(a) || x <= 0.0f) return 0;
        if (x >= 4294967296.0f) return 0xffffffffu;
        return (uint32_t)x;
    case SX_F_FLOOR: return sx__u(floorf(x));
    case SX_F_CEIL: return sx__u(ceilf(x));
    case SX_F_TRUNC: return sx__u(truncf(x));
    case SX_F_ROUND: return sx__u(nearbyintf(x));   // to even under the default mode
    case SX_F_SQRT: return sx__u(sqrtf(x));
    case SX_F_RSQRT: return sx__u(1.0f / sqrtf(x));
    case SX_F_RCP: return sx__u(1.0f / x);
    case SX_F_SIN: return sx__u(sinf(x));
    case SX_F_COS: return sx__u(cosf(x));
    case SX_F_EXP2: return sx__u(exp2f(x));
    case SX_F_LOG2: return sx__u(log2f(x));
    case SX_F_FRAC: return sx__u(x - floorf(x));
    }
    return 0;
}

void sx_core_reset(sx_core *c, uint32_t id, uint32_t pc) {
    memset(c, 0, sizeof *c);
    c->id = id;
    c->pc = pc;
    c->sr[SX_SR_STATUS] = SX_ST_S;
    c->rnd = 0x9e3779b9u ^ (id * 0x85ebca6bu);
    c->wl.cap = SX_WLOG_SIZE * 3 / 4;
}

static uint32_t *sx__wl_find(sx_wlog *w, uint32_t word, int make) {
    uint32_t h = (word * 0x9e3779b1u) >> (32 - SX_WLOG_BITS);
    for (;;) {
        if (w->key[h] == word + 1) return &w->val[h];
        if (w->key[h] == 0) {
            if (!make) return NULL;
            w->key[h] = word + 1;
            w->order[w->n++] = h;
            return &w->val[h];
        }
        h = (h + 1) & (SX_WLOG_SIZE - 1);
    }
}

static int sx__readable(const sx_machine *m, uint32_t a) { return a < m->ram_end || (a >= SX_ROM && a < SX_ROM_END); }
static int sx__writable(const sx_machine *m, uint32_t a) { return a >= SX_DEV && a < m->ram_end; }

// A word as the core sees it; a is word aligned and readable
static uint32_t sx__word(sx_machine *m, sx_core *c, uint32_t a) {
    if (a >= SX_ROM) {
        uint32_t o = a - SX_ROM;
        if (o + 4 > m->rom_size) return 0;
        return (uint32_t)m->rom[o] | (uint32_t)m->rom[o + 1] << 8 | (uint32_t)m->rom[o + 2] << 16 | (uint32_t)m->rom[o + 3] << 24;
    }
    if (c) {
        uint32_t *e = sx__wl_find(&c->wl, a >> 2, 0);
        if (e) return *e;
    }
    return m->ram[a >> 2];
}

// A load of `bytes` (1, 2, 4) at any alignment; 0 on a fault
static int sx__load(sx_machine *m, sx_core *c, uint32_t a, uint32_t bytes, uint32_t *v) {
    if (a >= SX_OPS && a + bytes <= SX_OPS + 32 && !((a | bytes) & 3)) { *v = c->op[(a - SX_OPS) >> 2]; return 1; }
    if (!sx__readable(m, a) || !sx__readable(m, a + bytes - 1)) return 0;
    uint32_t lo = sx__word(m, c, a & ~3u), sh = (a & 3) * 8;
    uint64_t w = lo;
    if ((a & 3) + bytes > 4) w |= (uint64_t)sx__word(m, c, (a & ~3u) + 4) << 32;
    w >>= sh;
    *v = bytes == 4 ? (uint32_t)w : (uint32_t)w & ((1u << (bytes * 8)) - 1);
    return 1;
}

int sx_load32(sx_machine *m, sx_core *c, uint32_t a, uint32_t *v) { return sx__load(m, c, a, 4, v); }

// Room for the words a store of `bytes` at a makes, or 0: the pass ends and the op runs again
static int sx__room(sx_core *c, uint32_t a, uint32_t bytes) {
    uint32_t need = 0, w;
    for (w = a >> 2; w <= (a + bytes - 1) >> 2; w++) need += sx__wl_find(&c->wl, w, 0) == NULL;
    return c->wl.n + need <= c->wl.cap;
}

static void sx__store(sx_machine *m, sx_core *c, uint32_t a, uint32_t bytes, uint32_t v) {
    if (a >= SX_OPS) {
        c->op[(a - SX_OPS) >> 2] = v;
        c->op_pending = 1;
        return;
    }
    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t b = a + i, word = b >> 2, sh = (b & 3) * 8;
        uint32_t *e = sx__wl_find(&c->wl, word, 0);
        uint32_t old = e ? *e : m->ram[word];
        if (!e) e = sx__wl_find(&c->wl, word, 1);
        *e = (old & ~(0xffu << sh)) | ((v >> (8 * i)) & 0xffu) << sh;
    }
}

static int sx__store_ok(const sx_machine *m, uint32_t a, uint32_t bytes) {
    if (a >= SX_OPS) return a + bytes <= SX_OPS + 32 && !((a | bytes) & 3);
    return sx__writable(m, a) && sx__writable(m, a + bytes - 1);
}

static void sx__trap(sx_core *c, uint32_t cause, uint32_t epc, uint32_t tval) {
    uint32_t st = c->sr[SX_SR_STATUS];
    c->sr[SX_SR_EPC] = epc;
    c->sr[SX_SR_CAUSE] = cause;
    c->sr[SX_SR_TVAL] = tval;
    st = (st & ~(SX_ST_PIE | SX_ST_PS | SX_ST_IE)) | ((st & SX_ST_IE) ? SX_ST_PIE : 0) | ((st & SX_ST_S) ? SX_ST_PS : 0) | SX_ST_S;
    c->sr[SX_SR_STATUS] = st;
    c->pc = c->sr[SX_SR_TVEC] & ~15u;
    c->waiting = 0;
}

static uint32_t sx__ipend(const sx_machine *m, const sx_core *c) {
    uint64_t cmp = (uint64_t)c->sr[SX_SR_TCMP_HI] << 32 | c->sr[SX_SR_TCMP_LO];
    return (c->soft ? 1u << SX_IRQ_SOFT : 0) | (cmp != 0 && m->time >= cmp ? 1u << SX_IRQ_TIMER : 0) | (c->ext ? 1u << SX_IRQ_EXT : 0);
}

static uint32_t sx__sr_read(sx_machine *m, sx_core *c, uint32_t n) {
    switch (n) {
    case SX_SR_IPEND: return sx__ipend(m, c);
    case SX_SR_TIME_LO: return (uint32_t)m->time;
    case SX_SR_TIME_HI: return (uint32_t)(m->time >> 32);
    case SX_SR_CYCLE: return (uint32_t)c->bundles_run;
    case SX_SR_COREID: return c->id;
    case SX_SR_PLAYER: return m->player;
    case SX_SR_RANDOM: c->rnd ^= c->rnd << 13; c->rnd ^= c->rnd >> 17; c->rnd ^= c->rnd << 5; return c->rnd;
    case SX_SR_LEFT: return c->left;
    case SX_SR_OPS: return (uint32_t)c->ops_run;
    }
    return n < SX_SR_COUNT ? c->sr[n] : 0;
}

static void sx__sr_write(sx_core *c, uint32_t n, uint32_t v) {
    switch (n) {
    case SX_SR_STATUS: c->sr[n] = (v & (SX_ST_IE | SX_ST_PIE | SX_ST_PS)) | (c->sr[n] & SX_ST_S); return;
    case SX_SR_EPC: case SX_SR_CAUSE: case SX_SR_TVAL: case SX_SR_TVEC: case SX_SR_SCRATCH: case SX_SR_IEN:
    case SX_SR_TCMP_LO: case SX_SR_TCMP_HI:
        c->sr[n] = v; return;
    case SX_SR_IPEND: c->soft = v & 1; return;   // the software bit alone is written
    }
}

static uint32_t sx__ext(uint32_t f, uint32_t a, uint32_t b) {
    switch (f) {
    case SX_X_MULH: return (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32);
    case SX_X_MULHU: return (uint32_t)(((uint64_t)a * b) >> 32);
    case SX_X_MULHSU: return (uint32_t)(((int64_t)(int32_t)a * (int64_t)(uint64_t)b) >> 32);
    case SX_X_DIV: return b == 0 ? 0xffffffffu : (a == 0x80000000u && b == 0xffffffffu) ? a : (uint32_t)((int32_t)a / (int32_t)b);
    case SX_X_DIVU: return b == 0 ? 0xffffffffu : a / b;
    case SX_X_REM: return b == 0 ? a : (a == 0x80000000u && b == 0xffffffffu) ? 0 : (uint32_t)((int32_t)a % (int32_t)b);
    case SX_X_REMU: return b == 0 ? a : a % b;
    case SX_X_CLZ: { uint32_t n = 0; while (n < 32 && !(a & (0x80000000u >> n))) n++; return n; }
    case SX_X_CTZ: { uint32_t n = 0; while (n < 32 && !(a & (1u << n))) n++; return n; }
    case SX_X_POPC: { uint32_t n = 0; while (a) { n += a & 1; a >>= 1; } return n; }
    case SX_X_BSWAP: return a >> 24 | (a >> 8 & 0xff00u) | (a << 8 & 0xff0000u) | a << 24;
    case SX_X_BREV: { uint32_t r = 0; for (int i = 0; i < 32; i++) r |= ((a >> i) & 1) << (31 - i); return r; }
    }
    return 0;
}

static uint32_t sx__int(uint32_t f, uint32_t a, uint32_t b) {
    uint32_t s = b & 31;
    switch (f) {
    case SX_I_ADD: return a + b;
    case SX_I_SUB: return a - b;
    case SX_I_AND: return a & b;
    case SX_I_OR: return a | b;
    case SX_I_XOR: return a ^ b;
    case SX_I_ANDN: return a & ~b;
    case SX_I_ORN: return a | ~b;
    case SX_I_XNOR: return ~(a ^ b);
    case SX_I_SLL: return a << s;
    case SX_I_SRL: return a >> s;
    case SX_I_SRA: return (uint32_t)((int32_t)a >> s);
    case SX_I_ROL: return s ? (a << s | a >> (32 - s)) : a;
    case SX_I_ROR: return s ? (a >> s | a << (32 - s)) : a;
    case SX_I_SLT: return (int32_t)a < (int32_t)b;
    case SX_I_SLTU: return a < b;
    case SX_I_SEQ: return a == b;
    case SX_I_SNE: return a != b;
    case SX_I_SGE: return (int32_t)a >= (int32_t)b;
    case SX_I_SGEU: return a >= b;
    case SX_I_MIN: return (int32_t)a < (int32_t)b ? a : b;
    case SX_I_MAX: return (int32_t)a > (int32_t)b ? a : b;
    case SX_I_MINU: return a < b ? a : b;
    case SX_I_MAXU: return a > b ? a : b;
    case SX_I_MUL: return a * b;
    }
    return 0;
}

static uint32_t sx__bfe(uint32_t a, uint32_t imm) {
    uint32_t pos = imm & 31, len = ((imm >> 5) & 31) + 1, v = a >> pos;
    if (len < 32) {
        v &= (1u << len) - 1;
        if ((imm & 0x400) && (v >> (len - 1)) & 1) v |= ~0u << len;
    }
    return v;
}

static uint32_t sx__bfi(uint32_t d, uint32_t a, uint32_t imm) {
    uint32_t pos = imm & 31, len = ((imm >> 5) & 31) + 1;
    uint32_t m = len >= 32 ? ~0u : (1u << len) - 1;
    return (d & ~(m << pos)) | ((a & m) << pos);
}

static void sx__set(sx_core *c, uint32_t r, uint32_t v) { if (r & 63) c->x[r & 63] = v; }

enum { SX__GO = 0, SX__TRAPPED = 1, SX__END = 2 };

// One op word at slot s of the bundle at base; is_long: word 2 is its immediate (the control word says so).
static int sx__op(sx_machine *m, sx_core *c, uint32_t base, uint32_t s, const uint32_t *t, uint32_t n, int is_long, int *long_used, int *end) {
    uint32_t w = t[s], opc = SX_OPC(w), rd = SX_RD(w), ra = SX_RA(w), rb = SX_RB(w), rc = SX_RC(w);
    uint32_t epc = base | s * 4;
    uint32_t imm = (uint32_t)SX_IMM12(w);
    int super = (c->sr[SX_SR_STATUS] & SX_ST_S) != 0;
    *long_used = 0;
    uint32_t cls = SX_CLASS(opc);
    if (is_long) {
        imm = t[2];
        *long_used = 1;
    }
    uint32_t a = c->x[ra], b = c->x[rb];
    c->ops_run++;
    switch (cls) {
    case 1: {   // integer
        uint32_t f = opc & 31;
        if (!SX_IFORM(opc)) {
            switch (f) {
            case SX_I_SHADD: sx__set(c, rd, a + (b << (rc & 31))); return SX__GO;
            case SX_I_SEL: sx__set(c, rd, c->x[rc] != 0 ? a : b); return SX__GO;
            case SX_I_ADDCY: sx__set(c, rd, a + b + (c->x[rc] & 1)); return SX__GO;
            case SX_I_CARRY: { uint64_t r = (uint64_t)a + b + (c->x[rc] & 1); sx__set(c, rd, (uint32_t)(r >> 32)); return SX__GO; }
            case SX_I_SUBBY: sx__set(c, rd, a - b - (c->x[rc] & 1)); return SX__GO;
            case SX_I_BORROW: { uint64_t r = (uint64_t)a - b - (c->x[rc] & 1); sx__set(c, rd, (uint32_t)(r >> 63)); return SX__GO; }
            }
            if (f > SX_I_COMMON_LAST) break;
            sx__set(c, rd, sx__int(f, a, b));
            return SX__GO;
        }
        if (f == SX_I_BFE) { sx__set(c, rd, sx__bfe(a, imm)); return SX__GO; }
        if (f > SX_I_COMMON_LAST) break;
        sx__set(c, rd, sx__int(f, a, imm));   // (sub: ra - imm, since x15)
        return SX__GO;
    }
    case 2: {   // memory
        uint32_t mode = (opc >> 3) & 7, wd = opc & 7;
        static const uint32_t bytes_of[8] = {4, 8, 2, 1, 16, 0, 2, 1};   // by SX_W_* (x15)
        if (mode <= 3) {
            uint32_t by = bytes_of[wd];
            if (!by) break;
            uint32_t addr = (mode <= 1) ? a + imm : a + (b << (rc & 31));
            if (mode == 0 || mode == 2) {
                uint32_t v[4] = {0, 0, 0, 0}, k, words = by > 4 ? by / 4 : 1;
                for (k = 0; k < words; k++) {
                    if (!sx__load(m, c, addr + 4 * k, by > 4 ? 4 : by, &v[k])) { c->ops_run--; sx__trap(c, SX_CAUSE_LOAD, epc, addr); return SX__TRAPPED; }
                }
                if (wd == SX_W_B) v[0] = (uint32_t)(int32_t)(int8_t)v[0];
                if (wd == SX_W_H) v[0] = (uint32_t)(int32_t)(int16_t)v[0];
                for (k = 0; k < words; k++) sx__set(c, rd + k, v[k]);
                return SX__GO;
            }
            if (!sx__store_ok(m, addr, by)) { c->ops_run--; sx__trap(c, SX_CAUSE_STORE, epc, addr); return SX__TRAPPED; }
            if (addr < SX_OPS && !sx__room(c, addr, by)) { c->ops_run--; c->pc = epc; *end = SX_END_FULL; return SX__END; }
            if (by <= 4) sx__store(m, c, addr, by, c->x[rd]);
            else for (uint32_t k = 0; k < by / 4; k++) sx__store(m, c, addr + 4 * k, 4, c->x[(rd + k) & 63]);
            return SX__GO;
        }
        if (mode == 4 || (mode == 5 && wd <= SX_AMO2_MAXU)) {
            uint32_t old, nw;
            if ((a & 3) || !sx__store_ok(m, a, 4) || a >= SX_OPS) { c->ops_run--; sx__trap(c, SX_CAUSE_STORE, epc, a); return SX__TRAPPED; }
            if (!sx__room(c, a, 4)) { c->ops_run--; c->pc = epc; *end = SX_END_FULL; return SX__END; }
            old = sx__word(m, c, a);
            if (mode == 5) nw = wd == SX_AMO2_MINU ? (old < b ? old : b) : (old > b ? old : b);
            else switch (wd) {
            case SX_AMO_SWAP: nw = b; break;
            case SX_AMO_ADD: nw = old + b; break;
            case SX_AMO_AND: nw = old & b; break;
            case SX_AMO_OR: nw = old | b; break;
            case SX_AMO_XOR: nw = old ^ b; break;
            case SX_AMO_MIN: nw = (int32_t)old < (int32_t)b ? old : b; break;
            case SX_AMO_MAX: nw = (int32_t)old > (int32_t)b ? old : b; break;
            default: nw = old == c->x[rd] ? b : old; break;   // cas
            }
            sx__store(m, c, a, 4, nw);
            sx__set(c, rd, old);
            return SX__GO;
        }
        break;
    }
    case 3: {   // float
        uint32_t f = opc & 31;
        if (SX_IFORM(opc)) {
            if (f == SX_F_MAD || f == SX_F_MSB || f == SX_F_NMSB) break;
            b = *long_used ? imm : imm << 20;
        }
        sx__set(c, rd, sx_fop(f, a, b, c->x[rc]));
        return SX__GO;
    }
    default:   // extended
        switch (opc) {
        case SX_X_MULH: case SX_X_MULHU: case SX_X_MULHSU: case SX_X_DIV: case SX_X_DIVU: case SX_X_REM: case SX_X_REMU:
        case SX_X_CLZ: case SX_X_CTZ: case SX_X_POPC: case SX_X_BSWAP: case SX_X_BREV:
            sx__set(c, rd, sx__ext(opc, a, b));
            return SX__GO;
        case SX_X_BFI: sx__set(c, rd, sx__bfi(c->x[rd], a, imm)); return SX__GO;
        case SX_X_LPC: sx__set(c, rd, base + imm); return SX__GO;
        case SX_X_SYSCALL: c->ops_run--; sx__trap(c, SX_CAUSE_SYSCALL, epc, 0); return SX__TRAPPED;
        case SX_X_BREAK:
            c->ops_run--;
            if (m->flags & SX_REF_BREAK_HALTS) { c->pc = epc; c->halted = 1; *end = SX_END_HALT; return SX__END; }
            sx__trap(c, SX_CAUSE_BREAK, epc, 0);
            return SX__TRAPPED;
        case SX_X_WAIT:
            c->pc = epc + 4;
            if (!super) { *end = SX_END_WAIT; return SX__END; }   // a user's wait: the pass's end
            if (sx__ipend(m, c) & c->sr[SX_SR_IEN]) return SX__GO;
            c->waiting = 1;
            *end = SX_END_WAIT;
            return SX__END;
        case SX_X_SYNC: c->pc = epc + 4; *end = SX_END_SYNC; return SX__END;
        case SX_X_FENCE: return SX__GO;
        case SX_X_MFSR: case SX_X_MTSR: case SX_X_XSR: case SX_X_SRSET: case SX_X_SRCLR: {
            uint32_t n2 = imm & 0xfff;
            if (n2 >= SX_SR_COUNT) break;
            if (!super && (opc != SX_X_MFSR || n2 < SX_SR_USER_FIRST)) { c->ops_run--; sx__trap(c, SX_CAUSE_PRIV, epc, w); return SX__TRAPPED; }
            uint32_t old = sx__sr_read(m, c, n2);
            if (opc == SX_X_MTSR) sx__sr_write(c, n2, a);
            if (opc == SX_X_XSR) sx__sr_write(c, n2, a);
            if (opc == SX_X_SRSET) sx__sr_write(c, n2, old | a);
            if (opc == SX_X_SRCLR) sx__sr_write(c, n2, old & ~a);
            if (opc != SX_X_MTSR) sx__set(c, rd, old);
            return SX__GO;
        }
        }
        break;
    }
    c->ops_run--;
    sx__trap(c, SX_CAUSE_ILLEGAL, epc, w);
    return SX__TRAPPED;
}

static int sx__cond(uint32_t cc, uint32_t a, uint32_t bf, uint32_t b) {
    int32_t i6 = (int32_t)(bf << 26) >> 26;
    switch (cc) {
    case SX_CC_BEQ: return a == b;
    case SX_CC_BNE: return a != b;
    case SX_CC_BLT: return (int32_t)a < (int32_t)b;
    case SX_CC_BGE: return (int32_t)a >= (int32_t)b;
    case SX_CC_BLTU: return a < b;
    case SX_CC_BGEU: return a >= b;
    case SX_CC_BBS: return (a >> (bf & 31)) & 1;
    case SX_CC_BBC: return !((a >> (bf & 31)) & 1);
    case SX_CC_BEQI: return (int32_t)a == i6;
    case SX_CC_BNEI: return (int32_t)a != i6;
    case SX_CC_BLTI: return (int32_t)a < i6;
    case SX_CC_BGEI: return (int32_t)a >= i6;
    case SX_CC_BLTUI: return a < (uint32_t)i6;   // imm6 sign-extended (x15)
    case SX_CC_BGEUI: return a >= (uint32_t)i6;
    }
    return 0;
}

int sx_bundle(sx_machine *m, sx_core *c) {
    uint32_t pc = c->pc, base = pc & ~15u, s = (pc & 15) >> 2, t[4], k;
    int end = SX_END_BUDGET;
    if (base >= m->ram_end) { sx__trap(c, SX_CAUSE_FETCH, pc, pc); return SX_END_BUDGET; }
    for (k = 0; k < 4; k++) t[k] = m->ram[(base >> 2) + k];   // code is fetched from RAM as committed
    uint32_t ctl = t[3], n = SX_N(ctl);
    c->bundles_run++;
    if (n == 3) { sx__trap(c, SX_CAUSE_ILLEGAL, base | 12, ctl); return SX_END_BUDGET; }
    // the branch's registers as this run of the bundle begins
    uint32_t ca0 = c->x[SX_CRA(ctl)], cb0 = c->x[SX_CRB(ctl)], cs0 = c->x[SX_CRS(ctl)];
    for (; s < n; s++) {
        int lu = 0, r = sx__op(m, c, base, s, t, n, (s == 0 ? SX_LONG0(ctl) : SX_LONG1(ctl)) != 0, &lu, &end);
        if (r == SX__TRAPPED) return SX_END_BUDGET;
        if (r == SX__END) {
            // the pass ends inside the bundle: pc is where it goes on (past n: the control word)
            if ((c->pc & 15) >> 2 >= n) c->pc = base | 12;
            return end;
        }
        (void)lu;
    }
    // the control word: a branch on the registers as the bundle found them, jr and callr on its ops' results
    uint32_t cc = SX_CC(ctl), next = base + 16;
    int super = (c->sr[SX_SR_STATUS] & SX_ST_S) != 0;
    if (cc == SX_CC_SPECIAL) {
        uint32_t sub = SX_SUB(ctl), rs = SX_CRS(ctl), to;
        switch (sub) {
        case SX_SUB_NONE: break;
        case SX_SUB_JR: next = c->x[rs] & ~15u; break;
        case SX_SUB_CALLR: to = c->x[rs] & ~15u; c->x[1] = base + 16; next = to; break;
        case SX_SUB_ERET: {
            if (!super) { sx__trap(c, SX_CAUSE_PRIV, base | 12, ctl); return SX_END_BUDGET; }
            uint32_t st = c->sr[SX_SR_STATUS];
            next = c->sr[SX_SR_EPC];
            st = (st & ~(SX_ST_IE | SX_ST_S | SX_ST_PS)) | ((st & SX_ST_PIE) ? SX_ST_IE : 0) | ((st & SX_ST_PS) ? SX_ST_S : 0) | SX_ST_PIE;
            c->sr[SX_SR_STATUS] = st;
            break;
        }
        case SX_SUB_J: next = base + (uint32_t)SX_OFF18(ctl) * 16; break;
        case SX_SUB_CALL: c->x[1] = base + 16; next = base + (uint32_t)SX_OFF18(ctl) * 16; break;
        case SX_SUB_ADD:   // after the ops: it wins; r0 is illegal
            if (rs == 0) { sx__trap(c, SX_CAUSE_ILLEGAL, base | 12, ctl); return SX_END_BUDGET; }
            sx__set(c, rs, cs0 + (uint32_t)SX_CIMM12(ctl));
            break;
        default: sx__trap(c, SX_CAUSE_ILLEGAL, base | 12, ctl); return SX_END_BUDGET;
        }
    } else if (cc == SX_CC_ADDBNZ) {   // ra += imm6 after the ops (it wins); taken when the sum is not 0
        uint32_t v = ca0 + (uint32_t)SX_CIMM6(ctl);
        if (SX_CRA(ctl) == 0) { sx__trap(c, SX_CAUSE_ILLEGAL, base | 12, ctl); return SX_END_BUDGET; }   // r0: illegal
        sx__set(c, SX_CRA(ctl), v);
        if (v != 0) next = base + (uint32_t)SX_OFF9(ctl) * 16;
    } else {
        uint32_t ra = SX_CRA(ctl), bf = SX_CRB(ctl);
        (void)ra;
        if (sx__cond(cc, ca0, bf, cb0)) next = base + (uint32_t)SX_OFF9(ctl) * 16;
    }
    c->pc = next;
    return SX_END_BUDGET;
}

int sx_run(sx_machine *m, sx_core *c, uint32_t budget) {
    c->left = budget;
    if (c->halted) return SX_END_HALT;
    if (c->op[0] != 0 && c->op[0] != 0xffffffffu && !c->op_pending) return SX_END_SYNC;   // (an operation in flight)
    if (c->waiting) {
        if (!(sx__ipend(m, c) & c->sr[SX_SR_IEN])) return SX_END_WAIT;
        c->waiting = 0;
    }
    while (c->left > 0) {
        // interrupts, between bundles only
        if ((c->pc & 15) == 0 && (c->sr[SX_SR_STATUS] & SX_ST_IE)) {
            uint32_t p = sx__ipend(m, c) & c->sr[SX_SR_IEN];
            if (p) {
                uint32_t bit = (p & 4) ? 2 : (p & 2) ? 1 : 0;
                sx__trap(c, SX_CAUSE_IRQ | bit, c->pc, 0);
            }
        }
        c->left--;
        int r = sx_bundle(m, c);
        if (r != SX_END_BUDGET) return r;
    }
    return SX_END_BUDGET;
}

static uint8_t sx__byte(sx_machine *m, uint32_t a) {
    if (a >= SX_ROM) return a - SX_ROM < m->rom_size ? m->rom[a - SX_ROM] : 0;
    return (uint8_t)(m->ram[a >> 2] >> ((a & 3) * 8));
}
static void sx__put_byte(sx_machine *m, uint32_t a, uint8_t v) {
    uint32_t sh = (a & 3) * 8;
    m->ram[a >> 2] = (m->ram[a >> 2] & ~(0xffu << sh)) | (uint32_t)v << sh;
}
static int sx__range(sx_machine *m, uint32_t a, uint32_t n, int dst) {
    if (n == 0) return 1;
    uint32_t e = a + n - 1;
    if (e < a) return 0;
    if (dst) return a >= SX_DEV && e < m->ram_end;
    return e < m->ram_end || (a >= SX_ROM && e < SX_ROM_END);
}

// The operation a core asked for, made at once (the tick takes passes; what lands is the same)
static void sx__operation(sx_machine *m, sx_core *c) {
    uint32_t *d = c->op, k = d[0], ok = 0;
    if (k == SX_OP_COPY && sx__range(m, d[1], d[3], 1) && sx__range(m, d[2], d[3], 0)) {
        uint32_t i, n = d[3];
        if (d[1] <= d[2]) for (i = 0; i < n; i++) sx__put_byte(m, d[1] + i, sx__byte(m, d[2] + i));
        else for (i = n; i-- > 0;) sx__put_byte(m, d[1] + i, sx__byte(m, d[2] + i));
        ok = 1;
    } else if (k == SX_OP_FILL && sx__range(m, d[1], d[3], 1)) {
        for (uint32_t i = 0; i < d[3]; i++) sx__put_byte(m, d[1] + i, (uint8_t)(d[2] >> ((i & 3) * 8)));
        ok = 1;
    } else if (k == SX_OP_BLIT) {
        uint32_t dst = d[1], src = d[2], ds = d[4], ss = d[5], row = d[6], rows = d[7], r;
        ok = rows == 0 || row == 0 ||
             (sx__range(m, dst, ds * (rows - 1) + row, 1) && sx__range(m, src, ss * (rows - 1) + row, 0));
        for (r = 0; ok && r < rows; r++)
            for (uint32_t i = 0; i < row; i++) sx__put_byte(m, dst + r * ds + i, sx__byte(m, src + r * ss + i));
    }
    d[0] = ok ? 0 : 0xffffffffu;
}

void sx_commit(sx_machine *m) {
    uint32_t i, j;
    for (i = 0; i < m->ncores; i++) {
        sx_wlog *w = &m->cores[i].wl;
        for (j = 0; j < w->n; j++) {
            uint32_t h = w->order[j];
            m->ram[w->key[h] - 1] = w->val[h];
            w->key[h] = 0;
        }
        w->n = 0;
    }
    for (i = 0; i < m->ncores; i++) {
        sx_core *c = &m->cores[i];
        if (c->op_pending) { c->op_pending = 0; sx__operation(m, c); }
    }
}
#endif   // SX_REF_IMPL
#endif
