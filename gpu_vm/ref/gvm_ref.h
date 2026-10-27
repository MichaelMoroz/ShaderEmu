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
#define GVM_WC_MAXNB 256          // the write cache model's tables at most (gvm_wlog.nb)
#define GVM_WC_MAXWAYS 8
#define GVM_OPQ 8                 // operations a core may ask for in a pass (a ninth waits for the commit)
#define GVM_XR_MAX 1024           // the core's file (isa.h, GVM_M_FREAD): words at most (gvm_core.xr, a power of two)
#define GVM_NPAIR_MAX 256         // its pair buffer at most (gvm_core.npair)

typedef struct gvm_wlog {
    uint32_t key[GVM_WLOG_SIZE];  // word address + 1 (0: empty)
    uint32_t val[GVM_WLOG_SIZE];
    uint32_t order[GVM_WLOG_SIZE];  // slots in the order first written, for the commit
    uint32_t n;
    uint32_t cap;                // words a pass before a store ends it (<= GVM_WLOG_SIZE * 3 / 4)
    // The tick's write cache as a model (gpu_vm/memory_model.md; off while nb is 0): a store whose texels the model
    // cannot place ends the pass as a full log does. Two tables of nb buckets of `ways` texels (x23: 32, 4) with x23's
    // hashes; reloc: an insertion that finds both buckets full may move one of their texels to its other bucket.
    uint32_t nb, ways, reloc;
    uint32_t tex[2 * GVM_WC_MAXNB * GVM_WC_MAXWAYS];   // texel number + 1 (0: empty), bucket b's ways from b * ways
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
    uint32_t op[8];          // the descriptor texels as stored at GVM_OPS (op[0] the kind; op[4-7] a blit's second)
    uint32_t opq[GVM_OPQ][8];   // the operations asked for in this pass, in order: the commit makes them
    uint32_t opage[GVM_OPQ];    // commits each has waited through (gvm_machine.op_passes)
    uint32_t nops;
    uint32_t refused;        // operations the commit refused, ever (GVM_OPS + 4 reads it)
    uint32_t ext;            // the external interrupt line (the host's)
    uint32_t soft;           // the software interrupt's pending bit
    uint32_t rnd;            // RANDOM's state
    uint32_t left;           // bundles left in this pass (LEFT)
    uint32_t halted;
    uint64_t ops_run, bundles_run;
    uint64_t e2_units, e1_steps, e1_words;   // the same bundles as E2's units and E1's steps and words (spec/ENCODINGS.md in GPU-VM)
    // The host's hooks (gvm_cpu; NULL by default, as in the tick's bench; gvm_core_reset clears them). host_trap is
    // called for a trap while TVEC is 0 (no handler) and returns nonzero when it dealt with it (pc set to go on, or
    // halted): then nothing of the trap is made. host_step is called before each bundle a pass runs (after an
    // interrupt is taken); nonzero halts the core there.
    int (*host_trap)(struct gvm_core *c, uint32_t cause, uint32_t epc, uint32_t tval);
    int (*host_step)(struct gvm_core *c);
    // host_access (gvm_cpu's race check, GVM_RACES) is told of every load and store a core makes of RAM: its address,
    // bytes and whether it stores (a store is told before it is made)
    void (*host_access)(struct gvm_core *c, uint32_t a, uint32_t bytes, int store);
    void *host;
    // the core's file and pair buffer (isa.h, GVM_M_FREAD; 0: none, the ops illegal): set by the host after the reset
    uint32_t xr, npair;
    uint32_t xf[GVM_XR_MAX];
    uint32_t xtag[GVM_XR_MAX];       // a slot's binding: the word's address | 1 bound | 2 dirty (this pass)
    uint32_t pa[GVM_NPAIR_MAX][2];   // this pass's pairs: address (word aligned), value
    uint32_t np;
    uint32_t xv[64][3];              // phase 3: lanes 1-3 of every register (gvm_machine.vec)
    uint32_t thin;                   // (gvm_run copies gvm_machine.thin: a trap's vector is 8-aligned)
    uint32_t enc;                    // (gvm_run copies gvm_machine.enc: 2 a vector 8-aligned, 1 4-aligned)
    uint32_t stale, stale_at;        // loads of a bound word or of one with a pair (the program's error), and the first's address
    uint32_t wait_k, wait_a;         // the last access an operation stopped: the operation's index (GVM_OPQ: the queue full), the address
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
    uint32_t vec;            // phase 3: 128-bit registers (isa.h GVM_X_VADD ...): lanes 1-3 in gvm_core.xv
    uint32_t late;           // phase 3 (gvm_asm.h's late_br): a conditional branch and addbnz read their registers after the bundle's ops
                             // (2: only in a bundle with a float op, which the tick runs in step12 or the general step; gvm_asm.h late_float)
    uint32_t wide3;          // phase 3 (gvm_asm.h's max_ops 3): n 3 is three ops, word 2 op 2 (no long immediate then)
    uint32_t thin;           // phase 3 (gvm_asm.h's thin8): 8-byte instructions {op, control} (16 with a long immediate:
                             // {op, control, imm, 0}, at a texel's start); branch offsets in 8 bytes; an instruction has 0 or 1 op
    uint32_t enc;            // the encoding (GPU-VM's spec/ENCODINGS.md): 0 E4's bundles, 2 E2's units of 8 bytes, 1 E1's words
    uint32_t silent;         // (the builder's sil, a tick's SIL) a store of the bytes RAM holds already, to a texel this core has
                             // not stored to in the pass, makes no entry (and needs no room): nothing it could change is seen
    uint32_t op_passes;      // 0 or 1: an operation is made at its pass's commit; N: N - 1 commits later, as the card's
                             // long ones land (its core waits on it as on any in flight; another core does not)
} gvm_machine;

#define GVM_REF_BREAK_HALTS 1u   // break stops the core (the reference's tests) instead of trapping

void gvm_core_reset(gvm_core *c, uint32_t id, uint32_t pc);
// Runs core c for up to budget bundles; returns GVM_END_*. Takes interrupts between bundles.
int gvm_run(gvm_machine *m, gvm_core *c, uint32_t budget);
// One bundle (or the rest of one from pc's word); GVM_END_BUDGET when it went on normally.
int gvm_bundle(gvm_machine *m, gvm_core *c);
// One step of E2 or E1 (gvm_machine.enc)
int gvm_step21(gvm_machine *m, gvm_core *c);
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

// A load of a word bound in the core's file, or with a pair this pass: counted (isa.h: the program's error)
static void gvm__stale(gvm_core *c, uint32_t a, uint32_t bytes) {
    uint32_t k, w0 = a & ~3u, w1 = (a + bytes - 1) & ~3u;
    if (!c || (!c->xr && !c->np)) return;
    for (k = 0; k < c->xr; k++)
        if ((c->xtag[k] & 1) && ((c->xtag[k] & ~3u) == w0 || (c->xtag[k] & ~3u) == w1)) { if (!c->stale++) c->stale_at = a; return; }
    for (k = 0; k < c->np; k++)
        if (c->pa[k][0] == w0 || c->pa[k][0] == w1) { if (!c->stale++) c->stale_at = a; return; }
}
// A load of `bytes` (1, 2, 4) at any alignment; 0 on a fault
static int gvm__load(gvm_machine *m, gvm_core *c, uint32_t a, uint32_t bytes, uint32_t *v) {
    if (a >= GVM_OPS && a - GVM_OPS <= 32 - bytes && !((a | bytes) & 3)) {   // in flight, refused, the latched words
        *v = a == GVM_OPS ? c->nops : a == GVM_OPS + 4 ? c->refused : c->op[(a - GVM_OPS) >> 2];
        return 1;
    }
    if (!gvm__readable(m, a) || !gvm__readable(m, a + bytes - 1)) return 0;
    if (c && c->host_access && a < m->ram_end) c->host_access(c, a, bytes, 0);
    if (a < m->ram_end) gvm__stale(c, a, bytes);
    uint32_t lo = gvm__word(m, c, a & ~3u), sh = (a & 3) * 8;
    uint64_t w = lo;
    if ((a & 3) + bytes > 4) w |= (uint64_t)gvm__word(m, c, (a & ~3u) + 4) << 32;
    w >>= sh;
    *v = bytes == 4 ? (uint32_t)w : (uint32_t)w & ((1u << (bytes * 8)) - 1);
    return 1;
}

int gvm_load32(gvm_machine *m, gvm_core *c, uint32_t a, uint32_t *v) { return gvm__load(m, c, a, 4, v); }

// The write cache model: x23's two buckets of texel t (wc_b1, wc_b2 in gpu_vm/tick/steps/x23.hlsl)
static uint32_t gvm__wc_b(const gvm_wlog *w, uint32_t t, int k) {
    return k == 0 ? (t ^ (t >> 5) ^ (t >> 10)) % w->nb : w->nb + ((t >> 3) ^ (t << 2) ^ (t >> 5)) % w->nb;
}
// 1: texel t is in the model or was placed there (place: 0 only asks)
static int gvm__wc_put(gvm_wlog *w, uint32_t t, int place) {
    uint32_t k, j, *q[2];
    for (k = 0; k < 2; k++) {
        q[k] = &w->tex[gvm__wc_b(w, t, (int)k) * w->ways];
        for (j = 0; j < w->ways; j++) if (q[k][j] == t + 1) return 1;
    }
    for (k = 0; k < 2; k++)
        for (j = 0; j < w->ways; j++)
            if (!q[k][j]) { if (place) q[k][j] = t + 1; return 1; }
    if (w->reloc)   // both full: a resident that has room in its other bucket moves there
        for (k = 0; k < 2; k++)
            for (j = 0; j < w->ways; j++) {
                uint32_t u = q[k][j] - 1, b0 = gvm__wc_b(w, u, 0), o = (q[k] == &w->tex[b0 * w->ways]) ? gvm__wc_b(w, u, 1) : b0, i;
                uint32_t *r = &w->tex[o * w->ways];
                for (i = 0; i < w->ways; i++)
                    if (!r[i]) { if (place) { r[i] = u + 1; q[k][j] = t + 1; } return 1; }
            }
    return 0;
}

// Room for the words a store of `bytes` at a makes, or 0: the pass ends and the op runs again
static int gvm__room(gvm_core *c, uint32_t a, uint32_t bytes) {
    uint32_t need = 0, w;
    if (c->wl.nb) {   // the model: the texels (two at most) one after the other, as the store will place them
        uint32_t t0 = a >> 4, t1 = (a + bytes - 1) >> 4;
        if (!gvm__wc_put(&c->wl, t0, 0)) return 0;
        if (t1 != t0) {   // (the first is placed to check the second; left placed if not, harmless to the model)
            gvm__wc_put(&c->wl, t0, 1);
            if (!gvm__wc_put(&c->wl, t1, 0)) return 0;
        }
    }
    for (w = a >> 2; w <= (a + bytes - 1) >> 2; w++) need += gvm__wl_find(&c->wl, w, 0) == NULL;
    return c->wl.n + need <= c->wl.cap;
}

// A store that changes nothing (gvm_machine.silent): every byte as RAM holds it, no word of its texels in the write log
static int gvm__silent(const gvm_machine *m, gvm_core *c, uint32_t a, uint32_t bytes, uint32_t v) {
    uint32_t i, t;
    if (!m->silent || bytes > 4 || a >= GVM_OPS || a + bytes > m->ram_end) return 0;
    for (t = a >> 4; t <= (a + bytes - 1) >> 4; t++)
        for (i = 0; i < 4; i++) if (gvm__wl_find(&c->wl, t * 4 + i, 0)) return 0;
    for (i = 0; i < bytes; i++)
        if (((m->ram[(a + i) >> 2] >> (((a + i) & 3) * 8)) & 0xffu) != ((v >> (8 * i)) & 0xffu)) return 0;
    return 1;
}
static uint32_t gvm__store_v;   // (the value of the store host_access is told of: gvm_cpu's GVM_STLOG_VALUES)
static void gvm__store(gvm_machine *m, gvm_core *c, uint32_t a, uint32_t bytes, uint32_t v) {
    if (a >= GVM_OPS) { c->op[(a - GVM_OPS) >> 2] = v; return; }   // (latched: a 16-byte store at GVM_OPS asks)
    gvm__store_v = v;
    if (c->host_access) c->host_access(c, a, bytes, 1);
    if (gvm__silent(m, c, a, bytes, v)) return;
    if (c->wl.nb) { gvm__wc_put(&c->wl, a >> 4, 1); gvm__wc_put(&c->wl, (a + bytes - 1) >> 4, 1); }
    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t b = a + i, word = b >> 2, sh = (b & 3) * 8;
        uint32_t *e = gvm__wl_find(&c->wl, word, 0);
        uint32_t old = e ? *e : m->ram[word];
        if (!e) e = gvm__wl_find(&c->wl, word, 1);
        *e = (old & ~(0xffu << sh)) | ((v >> (8 * i)) & 0xffu) << sh;
    }
}

// Operations asked for and not yet made do not stop the core, nor one another: an access stops it (the pass ends
// before the op, which runs again) only where one of them is still at work: a load of what it writes, a store to what
// it writes or reads; and a 16-byte store at GVM_OPS when GVM_OPQ are asked for already. Fetches are not looked at:
// code an operation wrote runs after a sync, which waits for them all.
static int gvm__in(uint32_t a, uint32_t bytes, uint32_t lo, uint32_t n) { return n && (a - lo < n || lo - a < bytes); }
static int gvm__op_waits(gvm_core *c, uint32_t a, uint32_t bytes, int store) {
    uint32_t k;
    if (a >= GVM_OPS) {
        if (!(store && a == GVM_OPS && bytes == 16 && c->nops == GVM_OPQ)) return 0;
        c->wait_k = GVM_OPQ; c->wait_a = a; return 1;
    }
    for (k = 0; k < c->nops; k++) {
        const uint32_t *d = c->opq[k];
        uint32_t dn = 0, sn = 0;
        if (d[0] == GVM_OP_COPY) dn = sn = d[3];
        else if (d[0] == GVM_OP_FILL) dn = d[3];
        else if (d[0] == GVM_OP_BLIT && d[7] && d[6]) { dn = d[4] * (d[7] - 1) + d[6]; sn = d[5] * (d[7] - 1) + d[6]; }
        if (gvm__in(a, bytes, d[1], dn) || (store && gvm__in(a, bytes, d[2], sn))) { c->wait_k = k; c->wait_a = a; return 1; }
    }
    return 0;
}

static int gvm__store_ok(const gvm_machine *m, uint32_t a, uint32_t bytes) {
    if (a >= GVM_OPS) return a - GVM_OPS <= 32 - bytes && !((a | bytes) & 3);
    return gvm__writable(m, a) && gvm__writable(m, a + bytes - 1);
}

static void gvm__trap(gvm_core *c, uint32_t cause, uint32_t epc, uint32_t tval) {
    if (c->host_trap && (c->sr[GVM_SR_TVEC] & ~15u) == 0 && c->host_trap(c, cause, epc, tval)) return;
    uint32_t st = c->sr[GVM_SR_STATUS];
    c->sr[GVM_SR_EPC] = epc;
    c->sr[GVM_SR_CAUSE] = cause;
    c->sr[GVM_SR_TVAL] = tval;
    st = (st & ~(GVM_ST_PIE | GVM_ST_PS | GVM_ST_IE)) | ((st & GVM_ST_IE) ? GVM_ST_PIE : 0) | ((st & GVM_ST_S) ? GVM_ST_PS : 0) | GVM_ST_S;
    c->sr[GVM_SR_STATUS] = st;
    c->pc = c->sr[GVM_SR_TVEC] & (c->enc == 1 ? ~3u : c->thin || c->enc == 2 ? ~7u : ~15u);
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
    if (n == GVM_SR_XR) return c->xr;
    if (n == GVM_SR_NPAIR) return c->npair;
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
        static const uint32_t bytes_of[8] = {4, 8, 2, 1, 16, 4, 2, 1};   // by GVM_W_* (x15)
        if (mode <= 3) {
            uint32_t by = bytes_of[wd];
            if (!by || (wd == GVM_W_NC && (mode & 1))) break;   // (nc: loads only)
            int pair = wd == GVM_W_D && mode <= 1;   // d: a pair, rd and rb, its offset rc's words (isa.h GVM_IMM_D)
            if (pair && !is_long) imm = (uint32_t)GVM_IMM_D(w);
            uint32_t r2 = pair ? rb : rd + 1;
            uint32_t addr = (mode <= 1) ? a + imm : a + (b << (rc & 31));
            // a byte, a half or a word stays in its address's word (since x17): the bytes past it are not accessed
            if (by <= 4 && (addr & 3) + by > 4) by = 4 - (addr & 3);
            if (gvm__op_waits(c, addr, by, mode & 1)) { c->ops_run--; c->pc = epc; *end = GVM_END_SYNC; return GVM__END; }
            if (mode == 0 || mode == 2) {
                uint32_t v[4] = {0, 0, 0, 0}, k, words = by > 4 ? by / 4 : 1;
                if (wd == GVM_W_NC && addr < m->ram_end && gvm__wl_find(&c->wl, addr >> 2, 0) && !c->stale++) c->stale_at = addr;   // (stored this pass)
                for (k = 0; k < words; k++) {
                    if (!gvm__load(m, c, addr + 4 * k, by > 4 ? 4 : by, &v[k])) { c->ops_run--; gvm__trap(c, GVM_CAUSE_LOAD, epc, addr); return GVM__TRAPPED; }
                }
                if (wd == GVM_W_B) v[0] = (uint32_t)(int32_t)(int8_t)v[0];
                if (wd == GVM_W_H) v[0] = (uint32_t)(int32_t)(int16_t)v[0];
                for (k = 0; k < words; k++) gvm__set(c, k == 1 ? r2 : rd + k, v[k]);
                return GVM__GO;
            }
            if (!gvm__store_ok(m, addr, by)) { c->ops_run--; gvm__trap(c, GVM_CAUSE_STORE, epc, addr); return GVM__TRAPPED; }
            if (addr < GVM_OPS && !(by <= 4 && gvm__silent(m, c, addr, by, c->x[rd])) && !gvm__room(c, addr, by)) { c->ops_run--; c->pc = epc; *end = GVM_END_FULL; return GVM__END; }
            if (by <= 4) gvm__store(m, c, addr, by, c->x[rd]);
            else for (uint32_t k = 0; k < by / 4; k++) gvm__store(m, c, addr + 4 * k, 4, c->x[(k == 1 ? r2 : rd + k) & 63]);
            if (addr == GVM_OPS && by == 16) c->opage[c->nops] = 0, memcpy(c->opq[c->nops++], c->op, sizeof c->op);   // an operation asked for
            return GVM__GO;
        }
        if (mode == 4 || (mode == 5 && wd <= GVM_AMO2_MAXU)) {
            uint32_t old, nw;
            if ((a & 3) || !gvm__store_ok(m, a, 4) || a >= GVM_OPS) { c->ops_run--; gvm__trap(c, GVM_CAUSE_STORE, epc, a); return GVM__TRAPPED; }
            if (gvm__op_waits(c, a, 4, 1)) { c->ops_run--; c->pc = epc; *end = GVM_END_SYNC; return GVM__END; }
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
        if (mode >= GVM_M_FREAD && (c->xr || c->npair)) {   // the core's file and its pairs
            uint32_t i = c->xr ? (a + imm) & (c->xr - 1) : 0;
            int pair = mode == GVM_M_FWRITE && wd == GVM_FK_PAIR;
            if (pair ? !c->npair : !c->xr) break;
            if (mode == GVM_M_FREAD && wd == GVM_FK_LF) { gvm__set(c, rd, c->xf[i]); return GVM__GO; }
            if (mode == GVM_M_FWRITE && wd == GVM_FK_SF) { c->xf[i] = c->x[rd]; if (c->xtag[i] & 1) c->xtag[i] |= 2; return GVM__GO; }
            if ((mode == GVM_M_FREAD && wd == GVM_FK_BIND) || (mode == GVM_M_FWRITE && wd == GVM_FK_UNBIND)) {
                uint32_t ad = c->x[rd] & ~3u;
                if (c->xtag[i] & 2) { c->ops_run--; c->pc = epc; *end = GVM_END_FULL; return GVM__END; }   // the commit writes it first
                if (mode == GVM_M_FWRITE) { c->xtag[i] = 0; return GVM__GO; }
                if (!gvm__writable(m, ad)) { c->ops_run--; gvm__trap(c, GVM_CAUSE_LOAD, epc, ad); return GVM__TRAPPED; }
                if (gvm__op_waits(c, ad, 4, 0)) { c->ops_run--; c->pc = epc; *end = GVM_END_SYNC; return GVM__END; }
                c->xtag[i] = 0;   // (its own old binding is no alias of the load)
                gvm__stale(c, ad, 4);
                c->xf[i] = gvm__word(m, c, ad);
                c->xtag[i] = ad | 1;
                return GVM__GO;
            }
            if (pair) {
                uint32_t ad = (a + imm) & ~3u;
                if (!gvm__writable(m, ad)) { c->ops_run--; gvm__trap(c, GVM_CAUSE_STORE, epc, ad); return GVM__TRAPPED; }
                if (gvm__op_waits(c, ad, 4, 1)) { c->ops_run--; c->pc = epc; *end = GVM_END_SYNC; return GVM__END; }
                if (c->np >= c->npair) { c->ops_run--; c->pc = epc; *end = GVM_END_FULL; return GVM__END; }
                c->pa[c->np][0] = ad; c->pa[c->np][1] = c->x[rd]; c->np++;
                return GVM__GO;
            }
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
        case GVM_X_VADD: case GVM_X_VSUB: case GVM_X_VMUL: case GVM_X_VAND: case GVM_X_VOR: case GVM_X_VXOR: case GVM_X_VFADD:
        case GVM_X_VFSUB: case GVM_X_VFMUL: case GVM_X_VFMA: case GVM_X_VFMIN: case GVM_X_VFMAX: case GVM_X_VFLT: case GVM_X_VFLE:
        case GVM_X_VEQ: case GVM_X_VSEL: case GVM_X_VDOT: case GVM_X_VSPLAT: case GVM_X_VSWZ: case GVM_X_VEXT: case GVM_X_VINS:
        case GVM_X_VLD: case GVM_X_VST: case GVM_X_VMASK: {
            if (!m->vec) break;
            uint32_t A[4], B[4], C[4], D[4], R[4], l, rc2 = GVM_RC(w);
            for (l = 0; l < 4; l++) {
                A[l] = l ? (ra ? c->xv[ra][l - 1] : 0) : c->x[ra]; B[l] = l ? (rb ? c->xv[rb][l - 1] : 0) : c->x[rb];
                C[l] = l ? (rc2 ? c->xv[rc2][l - 1] : 0) : c->x[rc2]; D[l] = l ? (rd ? c->xv[rd][l - 1] : 0) : c->x[rd];
            }
            int whole = 1;
            for (l = 0; l < 4; l++) {
                switch (opc) {
                case GVM_X_VADD: R[l] = A[l] + B[l]; break;
                case GVM_X_VSUB: R[l] = A[l] - B[l]; break;
                case GVM_X_VMUL: R[l] = A[l] * B[l]; break;
                case GVM_X_VAND: R[l] = A[l] & B[l]; break;
                case GVM_X_VOR: R[l] = A[l] | B[l]; break;
                case GVM_X_VXOR: R[l] = A[l] ^ B[l]; break;
                case GVM_X_VFADD: R[l] = gvm_fop(GVM_F_ADD, A[l], B[l], 0); break;
                case GVM_X_VFSUB: R[l] = gvm_fop(GVM_F_SUB, A[l], B[l], 0); break;
                case GVM_X_VFMUL: R[l] = gvm_fop(GVM_F_MUL, A[l], B[l], 0); break;
                case GVM_X_VFMA: R[l] = gvm_fop(GVM_F_MAD, A[l], B[l], C[l]); break;
                case GVM_X_VFMIN: R[l] = gvm_fop(GVM_F_MIN, A[l], B[l], 0); break;
                case GVM_X_VFMAX: R[l] = gvm_fop(GVM_F_MAX, A[l], B[l], 0); break;
                case GVM_X_VFLT: R[l] = gvm_fop(GVM_F_LT, A[l], B[l], 0) ? ~0u : 0; break;
                case GVM_X_VFLE: R[l] = gvm_fop(GVM_F_LE, A[l], B[l], 0) ? ~0u : 0; break;
                case GVM_X_VEQ: R[l] = A[l] == B[l] ? ~0u : 0; break;
                case GVM_X_VSEL: R[l] = C[l] ? A[l] : B[l]; break;
                case GVM_X_VSPLAT: R[l] = A[0]; break;
                case GVM_X_VSWZ: R[l] = A[(imm >> (2 * l)) & 3]; break;
                case GVM_X_VINS: R[l] = l == (imm & 3) ? A[0] : D[l]; break;
                default: R[l] = 0; break;
                }
            }
            if (opc == GVM_X_VDOT) {
                uint32_t s = gvm_fop(GVM_F_MUL, A[0], B[0], 0);
                for (l = 1; l < 4; l++) s = gvm_fop(GVM_F_MAD, A[l], B[l], s);
                R[0] = s; R[1] = R[2] = R[3] = 0;
            }
            if (opc == GVM_X_VEXT || opc == GVM_X_VMASK) { gvm__set(c, rd, opc == GVM_X_VEXT ? A[imm & 3] : (A[0] != 0) | (A[1] != 0) << 1 | (A[2] != 0) << 2 | (A[3] != 0) << 3); return GVM__GO; }
            if (opc == GVM_X_VLD || opc == GVM_X_VST) {
                uint32_t ad = a + imm;
                int st = opc == GVM_X_VST;
                if ((ad & 15) || (st ? !gvm__store_ok(m, ad, 16) : (!gvm__readable(m, ad) || !gvm__readable(m, ad + 15)))) {
                    c->ops_run--; gvm__trap(c, st ? GVM_CAUSE_STORE : GVM_CAUSE_LOAD, epc, ad); return GVM__TRAPPED;
                }
                if (gvm__op_waits(c, ad, 16, st)) { c->ops_run--; c->pc = epc; *end = GVM_END_SYNC; return GVM__END; }
                if (st) {
                    if (ad < GVM_OPS && !gvm__room(c, ad, 16)) { c->ops_run--; c->pc = epc; *end = GVM_END_FULL; return GVM__END; }
                    for (l = 0; l < 4; l++) gvm__store(m, c, ad + 4 * l, 4, D[l]);
                    return GVM__GO;
                }
                for (l = 0; l < 4; l++) gvm__load(m, c, ad + 4 * l, 4, &R[l]);
            }
            (void)whole;
            if (rd) { c->x[rd] = R[0]; c->xv[rd][0] = R[1]; c->xv[rd][1] = R[2]; c->xv[rd][2] = R[3]; }
            return GVM__GO;
        }
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
        case GVM_X_SYNC: c->pc = c->nops && m->op_passes > 1 ? epc : epc + 4; *end = GVM_END_SYNC; return GVM__END;
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
    const int T = m->thin != 0;
    const uint32_t AL = T ? 7u : 15u, CW = T ? 4u : 12u, U = T ? 8u : 16u;   // (thin: an instruction's alignment, its control word, offsets' unit)
    uint32_t pc = c->pc, base = pc & ~AL, s = (pc & AL) >> 2, t[4], k;
    int end = GVM_END_BUDGET;
    if (base >= m->ram_end) { gvm__trap(c, GVM_CAUSE_FETCH, pc, pc); return GVM_END_BUDGET; }
    if (T) {   // {op, control[, imm]}: as the bundle {op, -, imm, control}
        t[0] = m->ram[base >> 2]; t[1] = 0; t[3] = m->ram[(base >> 2) + 1];
        t[2] = GVM_LONG0(t[3]) ? m->ram[(base >> 2) + 2] : 0;
        s = s ? 3 : 0;
    } else for (k = 0; k < 4; k++) t[k] = m->ram[(base >> 2) + k];   // code is fetched from RAM as committed
    uint32_t ctl = t[3], n = GVM_N(ctl), size = T ? (GVM_LONG0(ctl) ? 16u : 8u) : 16u;
    if (T && n > 1) n = 4;
    c->bundles_run++;
    if (s == 0) {   // (E2 and E1: what this bundle would take; a control word that does nothing is left out)
        uint32_t nz = !((GVM_CC(ctl) == GVM_CC_SPECIAL && GVM_SUB(ctl) == GVM_SUB_NONE) || (GVM_CC(ctl) == GVM_CC_BNE && GVM_CRA(ctl) == 0 && GVM_CRB(ctl) == 0));
        uint32_t lg = (GVM_LONG0(ctl) | GVM_LONG1(ctl)) != 0 && n > 0, e1 = n + nz;
        c->e2_units += lg ? 1 + (n == 2 || nz) : n == 2 && nz ? 2 : 1;
        c->e1_steps += e1 ? e1 : 1;
        c->e1_words += (e1 ? e1 : 1) + lg;
    }
    if (n == 4 || (n == 3 && !m->wide3)) { gvm__trap(c, GVM_CAUSE_ILLEGAL, base | CW, ctl); return GVM_END_BUDGET; }
    // the branch's registers as this run of the bundle begins
    uint32_t ca0 = c->x[GVM_CRA(ctl)], cb0 = c->x[GVM_CRB(ctl)], cs0 = c->x[GVM_CRS(ctl)];
    for (; s < n; s++) {
        int lu = 0, r = gvm__op(m, c, base, s, t, n, (s == 0 ? GVM_LONG0(ctl) : s == 1 ? GVM_LONG1(ctl) : 0) != 0, &lu, &end);
        if (r == GVM__TRAPPED) return GVM_END_BUDGET;
        if (r == GVM__END) {
            // the pass ends inside the bundle: pc is where it goes on (past n: the control word)
            if (T ? (c->pc & 7) != 0 : (c->pc & 15) >> 2 >= n) c->pc = base | CW;
            return end;
        }
        (void)lu;
    }
    // the control word: a branch on the registers as the bundle found them, jr and callr on its ops' results
    int hasf = 0;
    for (k = 0; k < n; k++) hasf |= GVM_CLASS(GVM_OPC(t[k])) == 3;
    if (m->late && GVM_CC(ctl) != GVM_CC_SPECIAL && (m->late != 2 || hasf)) { ca0 = c->x[GVM_CRA(ctl)]; cb0 = c->x[GVM_CRB(ctl)]; }   // (late: as its ops left them)
    uint32_t cc = GVM_CC(ctl), next = base + size;
    int super = (c->sr[GVM_SR_STATUS] & GVM_ST_S) != 0;
    if (cc == GVM_CC_SPECIAL) {
        uint32_t sub = GVM_SUB(ctl), rs = GVM_CRS(ctl), to;
        switch (sub) {
        case GVM_SUB_NONE: break;
        case GVM_SUB_JR: next = c->x[rs] & ~AL; break;
        case GVM_SUB_CALLR: to = c->x[rs] & ~AL; c->x[1] = base + size; next = to; break;
        case GVM_SUB_ERET: {
            if (!super) { gvm__trap(c, GVM_CAUSE_PRIV, base | CW, ctl); return GVM_END_BUDGET; }
            uint32_t st = c->sr[GVM_SR_STATUS];
            next = c->sr[GVM_SR_EPC];
            st = (st & ~(GVM_ST_IE | GVM_ST_S | GVM_ST_PS)) | ((st & GVM_ST_PIE) ? GVM_ST_IE : 0) | ((st & GVM_ST_PS) ? GVM_ST_S : 0) | GVM_ST_PIE;
            c->sr[GVM_SR_STATUS] = st;
            break;
        }
        case GVM_SUB_J: next = base + (uint32_t)GVM_OFF18(ctl) * U; break;
        case GVM_SUB_CALL: c->x[1] = base + size; next = base + (uint32_t)GVM_OFF18(ctl) * U; break;
        case GVM_SUB_ADD:   // after the ops: it wins; r0 is illegal
            if (rs == 0) { gvm__trap(c, GVM_CAUSE_ILLEGAL, base | CW, ctl); return GVM_END_BUDGET; }
            gvm__set(c, rs, cs0 + (uint32_t)GVM_CIMM12(ctl));
            break;
        default: gvm__trap(c, GVM_CAUSE_ILLEGAL, base | CW, ctl); return GVM_END_BUDGET;
        }
    } else if (cc == GVM_CC_ADDBNZ) {   // ra += imm6 after the ops (it wins); taken when the sum is not 0
        uint32_t v = ca0 + (uint32_t)GVM_CIMM6(ctl);
        if (GVM_CRA(ctl) == 0) { gvm__trap(c, GVM_CAUSE_ILLEGAL, base | CW, ctl); return GVM_END_BUDGET; }   // r0: illegal
        gvm__set(c, GVM_CRA(ctl), v);
        if (v != 0) next = base + (uint32_t)GVM_OFF9(ctl) * U;
    } else {
        uint32_t ra = GVM_CRA(ctl), bf = GVM_CRB(ctl);
        (void)ra;
        if (gvm__cond(cc, ca0, bf, cb0)) next = base + (uint32_t)GVM_OFF9(ctl) * U;
    }
    c->pc = next;
    return GVM_END_BUDGET;
}

// ---- E2 and E1 (GPU-VM's spec/ENCODINGS.md): the same ops, laid out as units of 8 bytes or words of 4
#define GVM_E21_CTL(w) ((((w) >> 15) & 31u) == 7u)     // opcodes 0x38-0x3f: a control word
#define GVM_E21_PRE(w) ((((w) >> 12) & 255u) == 0x1fu)  // opcode 0x1f: a prefix
static uint32_t gvm__e21_const(uint32_t p, uint32_t op) { return (p & 0xfff00000u) | (p & 0xfffu) << 8 | ((op >> 20) & 0xffu); }

// The control word w at cw (its unit's or word's address), U the distance's unit, next the address after its step;
// ca0, cb0, cs0 its registers as the step found them. Returns 0, or 1 when it trapped.
static int gvm__control21(gvm_core *c, uint32_t w, uint32_t cw, uint32_t U, uint32_t next, uint32_t ca0, uint32_t cb0, uint32_t cs0) {
    uint32_t cc = ((w >> 12) & 7u) | ((w >> 17) & 8u), AL = U - 1;
    int super = (c->sr[GVM_SR_STATUS] & GVM_ST_S) != 0;
    int32_t off = (int32_t)w >> 21;
    if (cc == GVM_CC_SPECIAL) {
        uint32_t sub = (w >> 6) & 7u, rs = w & 63u, to;
        int32_t dist = (int32_t)(((uint32_t)off << 9) | ((w >> 9) & 7u) << 6 | (w & 63u));
        switch (sub) {
        case GVM_SUB_NONE: break;
        case GVM_SUB_JR: next = c->x[rs] & ~AL; break;
        case GVM_SUB_CALLR: to = c->x[rs] & ~AL; c->x[1] = next; next = to; break;
        case GVM_SUB_ERET: {
            if (!super) { gvm__trap(c, GVM_CAUSE_PRIV, cw, w); return 1; }
            uint32_t st = c->sr[GVM_SR_STATUS];
            next = c->sr[GVM_SR_EPC];
            st = (st & ~(GVM_ST_IE | GVM_ST_S | GVM_ST_PS)) | ((st & GVM_ST_PIE) ? GVM_ST_IE : 0) | ((st & GVM_ST_PS) ? GVM_ST_S : 0) | GVM_ST_PIE;
            c->sr[GVM_SR_STATUS] = st;
            break;
        }
        case GVM_SUB_J: next = cw + (uint32_t)dist * U; break;
        case GVM_SUB_CALL: c->x[1] = next; next = cw + (uint32_t)dist * U; break;
        case GVM_SUB_ADD:   // rs += imm14, after the step's op (it wins); r0 is illegal
            if (rs == 0) { gvm__trap(c, GVM_CAUSE_ILLEGAL, cw, w); return 1; }
            gvm__set(c, rs, cs0 + (((uint32_t)off << 3) | ((w >> 9) & 7u)));
            break;
        default: gvm__trap(c, GVM_CAUSE_ILLEGAL, cw, w); return 1;
        }
    } else if (cc == GVM_CC_ADDBNZ) {   // ra += imm6 after the step's op (it wins); taken when the sum is not 0
        uint32_t ra = w & 63u, v = ca0 + (uint32_t)((int32_t)(w << 20) >> 26);
        if (ra == 0) { gvm__trap(c, GVM_CAUSE_ILLEGAL, cw, w); return 1; }
        gvm__set(c, ra, v);
        if (v != 0) next = cw + (uint32_t)off * U;
    } else if (gvm__cond(cc, ca0, (w >> 6) & 63u, cb0)) next = cw + (uint32_t)off * U;
    c->pc = next;
    return 0;
}

// One step of E2 (a unit, or its second word after a pass that ended on its first) or E1 (a word; a prefix with its op)
int gvm_step21(gvm_machine *m, gvm_core *c) {
    uint32_t pc = c->pc, t[4] = {0, 0, 0, 0}, A, B;
    int end = GVM_END_BUDGET, lu = 0, r;
    if (pc >= m->ram_end || (pc & 3)) { gvm__trap(c, GVM_CAUSE_FETCH, pc, pc); return GVM_END_BUDGET; }
    c->bundles_run++;
    if (m->enc == 2) {
        uint32_t base = pc & ~7u;
        A = m->ram[base >> 2]; B = m->ram[(base >> 2) + 1];
        if (GVM_E21_PRE(A)) {   // {prefix, op}: one op with a 32-bit constant
            if (GVM_E21_CTL(B) || GVM_E21_PRE(B)) { gvm__trap(c, GVM_CAUSE_ILLEGAL, base, A); return GVM_END_BUDGET; }
            t[0] = B; t[2] = gvm__e21_const(A, B);
            r = gvm__op(m, c, base, 0, t, 1, 1, &lu, &end);
            if (r == GVM__TRAPPED) return GVM_END_BUDGET;
            if (r == GVM__END) { if (c->pc == (base | 4)) c->pc = base + 8; return end; }   // (past its op: the next unit)
            c->pc = base + 8;
            return GVM_END_BUDGET;
        }
        if (GVM_E21_CTL(B)) {   // {op, control}: the control word on the registers as the unit found them
            uint32_t ca0 = c->x[B & 63u], cb0 = c->x[(B >> 6) & 63u];
            if ((pc & 4) == 0) {
                if (GVM_E21_CTL(A) || GVM_E21_PRE(A)) { gvm__trap(c, GVM_CAUSE_ILLEGAL, base, A); return GVM_END_BUDGET; }
                t[0] = A;
                r = gvm__op(m, c, base, 0, t, 1, 0, &lu, &end);
                if (r == GVM__TRAPPED) return GVM_END_BUDGET;
                if (r == GVM__END) { if (c->pc != base) c->pc = base | 4; return end; }   // (past its op: the control word next)
            }
            gvm__control21(c, B, base, 8, base + 8, ca0, cb0, ca0);
            return GVM_END_BUDGET;
        }
        if (GVM_E21_CTL(A) || GVM_E21_PRE(A)) { gvm__trap(c, GVM_CAUSE_ILLEGAL, base, A); return GVM_END_BUDGET; }
        t[0] = A; t[1] = B;   // {op, op}
        for (uint32_t s = (pc & 4) ? 1 : 0; s < 2; s++) {
            r = gvm__op(m, c, base, s, t, 2, 0, &lu, &end);
            if (r == GVM__TRAPPED) return GVM_END_BUDGET;
            if (r == GVM__END) { if (c->pc == (base | 8)) c->pc = base + 8; return end; }
        }
        c->pc = base + 8;
        return GVM_END_BUDGET;
    }
    A = m->ram[pc >> 2];
    if (GVM_E21_CTL(A)) {
        uint32_t ca0 = c->x[A & 63u];
        gvm__control21(c, A, pc, 4, pc + 4, ca0, c->x[(A >> 6) & 63u], ca0);
        return GVM_END_BUDGET;
    }
    if (GVM_E21_PRE(A)) {   // the prefix and its op, one step (the next texel's first word when the prefix is a texel's last)
        if (pc + 4 >= m->ram_end) { gvm__trap(c, GVM_CAUSE_FETCH, pc, pc + 4); return GVM_END_BUDGET; }
        B = m->ram[(pc >> 2) + 1];
        if (GVM_E21_CTL(B) || GVM_E21_PRE(B)) { gvm__trap(c, GVM_CAUSE_ILLEGAL, pc, A); return GVM_END_BUDGET; }
        t[0] = B; t[2] = gvm__e21_const(A, B);
        r = gvm__op(m, c, pc, 0, t, 1, 1, &lu, &end);
        if (r == GVM__TRAPPED) return GVM_END_BUDGET;
        if (r == GVM__END) { if (c->pc == pc + 4) c->pc = pc + 8; return end; }   // (past its op: the word after it)
        c->pc = pc + 8;
        return GVM_END_BUDGET;
    }
    t[0] = A;
    r = gvm__op(m, c, pc, 0, t, 1, 0, &lu, &end);
    if (r == GVM__TRAPPED) return GVM_END_BUDGET;
    if (r == GVM__END) return end;
    c->pc = pc + 4;
    return GVM_END_BUDGET;
}

int gvm_run(gvm_machine *m, gvm_core *c, uint32_t budget) {
    c->left = budget;
    c->thin = m->thin;
    c->enc = m->enc;
    if (c->halted) return GVM_END_HALT;
    if (c->waiting) {
        if (!(gvm__ipend(m, c) & c->sr[GVM_SR_IEN])) return GVM_END_WAIT;
        c->waiting = 0;
    }
    while (c->left > 0) {
        // interrupts, between bundles only
        if ((c->pc & (m->enc == 1 ? 3u : m->thin || m->enc == 2 ? 7u : 15u)) == 0 && (c->sr[GVM_SR_STATUS] & GVM_ST_IE)) {
            uint32_t p = gvm__ipend(m, c) & c->sr[GVM_SR_IEN];
            if (p) {
                uint32_t bit = (p & 4) ? 2 : (p & 2) ? 1 : 0;
                gvm__trap(c, GVM_CAUSE_IRQ | bit, c->pc, 0);
                if (c->halted) return GVM_END_HALT;   // (by host_trap)
            }
        }
        if (c->host_step && c->host_step(c)) { c->halted = 1; return GVM_END_HALT; }
        c->left--;
        int r = m->enc ? gvm_step21(m, c) : gvm_bundle(m, c);
        if (r != GVM_END_BUDGET) return r;
        if (c->npair && c->np >= c->npair) return GVM_END_FULL;   // the bundle that filled the pair buffer ends the pass
        if (c->halted) return GVM_END_HALT;   // (by host_trap)
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

// An operation a core asked for, made at once (the tick takes passes; what lands is the same); 0: refused
static int gvm__operation(gvm_machine *m, const uint32_t *d) {
    uint32_t k = d[0], ok = 0;
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
    return ok;
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
        if (w->nb) memset(w->tex, 0, 2 * w->nb * w->ways * sizeof w->tex[0]);
        gvm_core *c = &m->cores[i];   // then the file's dirty bound slots, in slot order, then the pairs, in order
        for (j = 0; j < c->xr; j++)
            if ((c->xtag[j] & 3) == 3) { m->ram[c->xtag[j] >> 2] = c->xf[j]; c->xtag[j] &= ~2u; }
        for (j = 0; j < c->np; j++) m->ram[c->pa[j][0] >> 2] = c->pa[j][1];
        c->np = 0;
    }
    for (i = 0; i < m->ncores; i++) {
        gvm_core *c = &m->cores[i];
        uint32_t made = 0;
        for (j = 0; j < c->nops; j++) c->opage[j]++;
        for (; made < c->nops && c->opage[made] >= m->op_passes; made++)   // in the order asked for
            c->refused += !gvm__operation(m, c->opq[made]);
        c->nops -= made;
        memmove(c->opq, c->opq + made, c->nops * sizeof c->opq[0]);
        memmove(c->opage, c->opage + made, c->nops * sizeof c->opage[0]);
    }
}
#endif   // GVM_REF_IMPL
#endif
