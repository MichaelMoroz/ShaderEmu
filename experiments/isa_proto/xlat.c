/* The toolchain's back end: RV32IMAF code (as GCC and tcc write it) made the machine's slots (isa.h, ISA.md).
 * The code is translated where it lies in a linked program; every place that names code (branches, calls,
 * pc-relative addresses, switch tables) is made to name its slot. Also built for the guest. */
#include "xlat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- one RISC-V instruction, decoded, with the native op it makes alone ---- */
enum { C_OP, C_LOAD, C_STORE, C_BRANCH, C_JAL, C_JALR, C_AUIPC, C_GEN, C_BAD, C_DATA };
typedef struct {
    uint32_t ins, pc;
    int cls;
    uint32_t x, rd, ra, rb, rc, w, opg;   /* rd: the register written (SINK: none), or a store's data */
    uint64_t reads, writes;               /* x1-31 bits 1-31, f0-31 bits 32-63 */
    uint32_t target;                      /* a branch's or jal's target */
    int mem, store, end;                  /* touches memory; stores; ends a block */
} Ins;

#define SINK ISA_SINK
static uint32_t sx(uint32_t v, int bits) { return (uint32_t)((int32_t)(v << (32 - bits)) >> (32 - bits)); }
static uint64_t R(uint32_t r) { return r ? 1ull << r : 0; }
static uint64_t F(uint32_t r) { return 1ull << (32 + r); }
static void gen(Ins* d, uint32_t opg) { d->cls = C_GEN; d->x = K_GEN; d->opg = opg; }

static void decode(Ins* d, uint32_t ins, uint32_t pc) {
    memset(d, 0, sizeof *d);
    d->ins = ins, d->pc = pc;
    uint32_t op = ins & 0x7f, rd = (ins >> 7) & 31, f3 = (ins >> 12) & 7, rs1 = (ins >> 15) & 31, rs2 = (ins >> 20) & 31, f7 = ins >> 25;
    uint32_t immI = sx(ins >> 20, 12), immS = sx(((ins >> 25) << 5) | rd, 12);
    d->rd = rd ? rd : SINK, d->ra = rs1, d->rb = rs2;
    d->cls = C_BAD;
    switch (op) {
    case 0x37: d->cls = C_OP, d->x = K_INT, d->ra = d->rb = 0, d->w = ins & 0xfffff000, d->writes = R(rd); return;
    case 0x17: d->cls = C_AUIPC, d->x = K_INT | X_PCA, d->ra = d->rb = 0, d->w = ins & 0xfffff000, d->writes = R(rd); return;
    case 0x6f:
        d->cls = C_JAL, d->x = K_INT | X_BR | X_ALW | (rd ? X_LINK : 0), d->ra = d->rb = 0;
        d->target = pc + sx(((ins >> 31) << 20) | (ins & 0xff000) | ((ins >> 9) & 0x800) | ((ins >> 20) & 0x7fe), 21);
        d->writes = R(rd), d->end = 1;
        return;
    case 0x67:
        if (f3) return;
        d->cls = C_JALR, d->x = K_INT | X_BR | X_ALW | X_IND | (rd ? X_LINK : 0), d->rb = 0, d->w = immI;
        d->reads = R(rs1), d->writes = R(rd), d->end = 1;
        return;
    case 0x63: {
        static const uint32_t how[8] = {X_EQ, X_EQ | X_INV, ~0u, ~0u, 0, X_INV, X_UNS, X_UNS | X_INV};
        if (how[f3] == ~0u) return;
        d->cls = C_BRANCH, d->x = K_INT | X_BR | SEL_CMP << X_SEL_SHIFT | how[f3], d->rd = SINK;
        d->target = pc + sx(((ins >> 31) << 12) | ((ins << 4) & 0x800) | ((ins >> 20) & 0x7e0) | ((ins >> 7) & 0x1e), 13);
        d->reads = R(rs1) | R(rs2), d->end = 1;
        return;
    }
    case 0x03: {
        static const uint32_t mw[8] = {1u << X_MW_SHIFT, 2u << X_MW_SHIFT, 0, ~0u, (1u << X_MW_SHIFT) | X_MU, (2u << X_MW_SHIFT) | X_MU, ~0u, ~0u};
        if (mw[f3] == ~0u) return;
        d->cls = C_LOAD, d->x = K_MEM | mw[f3], d->rb = 0, d->w = immI, d->mem = 1;
        d->reads = R(rs1), d->writes = R(rd);
        return;
    }
    case 0x23:
        if (f3 > 2) return;
        d->cls = C_STORE, d->x = K_MEM | X_STORE | (f3 == 0 ? 1u : f3 == 1 ? 2u : 0u) << X_MW_SHIFT;
        d->rd = rs2, d->rb = 0, d->w = immS, d->mem = d->store = 1;
        d->reads = R(rs1) | R(rs2);
        return;
    case 0x13: {
        uint32_t sel = 0, x = 0;
        switch (f3) {
        case 0: sel = SEL_SUM; break;
        case 1: if (f7) return; sel = SEL_SLL, immI &= 31; break;
        case 2: sel = SEL_CMP; break;
        case 3: sel = SEL_CMP, x = X_UNS; break;
        case 4: sel = SEL_XOR; break;
        case 5: if (f7 & ~0x20u) return; sel = f7 ? SEL_SRA : SEL_SRL, immI &= 31; break;
        case 6: sel = SEL_OR; break;
        case 7: sel = SEL_AND; break;
        }
        d->cls = C_OP, d->x = K_INT | sel << X_SEL_SHIFT | x, d->rb = 0, d->w = immI;
        d->reads = R(rs1), d->writes = R(rd);
        return;
    }
    case 0x33:
        d->reads = R(rs1) | R(rs2), d->writes = R(rd);
        if (f7 == 1) { gen(d, G_MUL + f3); return; }
        if ((f7 != 0 && f7 != 0x20) || (f7 == 0x20 && f3 != 0 && f3 != 5)) return;
        {
            static const uint32_t sel[8] = {SEL_SUM, SEL_SLL, SEL_CMP, SEL_CMP, SEL_XOR, SEL_SRL, SEL_OR, SEL_AND};
            uint32_t s = f3 == 5 && f7 ? SEL_SRA : sel[f3];
            d->cls = C_OP, d->x = K_INT | s << X_SEL_SHIFT | (f3 == 3 ? X_UNS : 0) | (f3 == 0 && f7 ? X_NEG : 0), d->w = 0;
        }
        return;
    case 0x0f:
        if (ins == 0x0000100f) { gen(d, G_FENCEI); d->end = 1; d->reads = d->writes = ~0ull; d->mem = d->store = 1; return; }
        if (ins == 0x0100000f) { gen(d, G_PAUSE); d->end = 1; d->reads = d->writes = ~0ull; d->mem = d->store = 1; return; }
        if (f3 == 0) { d->cls = C_OP, d->x = K_INT, d->rd = SINK, d->ra = d->rb = 0, d->w = 0, d->mem = d->store = 1; return; }   /* fence */
        return;
    case 0x2f: {
        static const uint8_t amo[32] = {
            [0] = G_AMOADD, [1] = G_AMOSWAP, [2] = G_LR, [3] = G_SC, [4] = G_AMOXOR, [8] = G_AMOOR, [12] = G_AMOAND,
            [16] = G_AMOMIN, [20] = G_AMOMAX, [24] = G_AMOMINU, [28] = G_AMOMAXU};
        uint32_t k = f7 >> 2;
        if (f3 != 2 || !amo[k] || (k == 2 && rs2)) return;
        gen(d, amo[k]);
        d->reads = R(rs1) | R(rs2), d->writes = R(rd), d->mem = d->store = 1;
        return;
    }
    case 0x73:
        if (f3 == 0) {
            if (ins == 0x00000073) gen(d, G_ECALL);
            else if (ins == 0x00100073) gen(d, G_EBREAK);
            else if (ins == 0x10200073) gen(d, G_SRET);
            else if (ins == 0x10500073) gen(d, G_WFI);
            else if (f7 == 0x09 && rd == 0) gen(d, G_SFENCE);
            else if (ins == 0x30200073 || ins == 0x00200073) gen(d, G_SFENCE);   /* mret, uret: nothing, as on the tick */
            else return;
        } else {
            if (f3 == 4) return;
            gen(d, (f3 & 4 ? G_CSRRWI : G_CSRRW) + (f3 & 3) - 1);
            d->w = ins >> 20;
        }
        d->reads = d->writes = ~0ull, d->end = 1, d->mem = d->store = 1;   /* alone, and last in its block */
        return;
    case 0x07:
        if (f3 != 2) return;
        gen(d, G_FLW); d->w = immI, d->rb = 0, d->rd = rd, d->mem = 1, d->reads = R(rs1), d->writes = F(rd);
        return;
    case 0x27:
        if (f3 != 2) return;
        gen(d, G_FSW); d->w = immS, d->rd = 0, d->mem = d->store = 1, d->reads = R(rs1) | F(rs2);
        return;
    case 0x43: case 0x47: case 0x4b: case 0x4f:
        if (f7 & 3) return;
        gen(d, G_FMADD + ((op >> 2) & 3)); d->rc = ins >> 27, d->rd = rd, d->w = f3;
        d->reads = F(rs1) | F(rs2) | F(ins >> 27), d->writes = F(rd);
        return;
    case 0x53: {
        uint32_t g = 0;
        uint64_t rf = F(rs1) | F(rs2), wf = F(rd);
        uint32_t ird = rd ? rd : SINK;
        d->rd = rd, d->w = f3;
        switch (f7) {
        case 0x00: g = G_FADD; break;
        case 0x04: g = G_FSUB; break;
        case 0x08: g = G_FMUL; break;
        case 0x0c: g = G_FDIV; break;
        case 0x2c: if (rs2) return; g = G_FSQRT, rf = F(rs1); break;
        case 0x10: if (f3 > 2) return; g = G_FSGNJ + f3; break;
        case 0x14: if (f3 > 1) return; g = G_FMIN + f3; break;
        case 0x60: if (rs2 > 1) return; g = rs2 ? G_FCVTWUS : G_FCVTWS, rf = F(rs1), wf = R(rd), d->rd = ird; break;
        case 0x68: if (rs2 > 1) return; g = rs2 ? G_FCVTSWU : G_FCVTSW, rf = R(rs1); break;
        case 0x70: if (rs2 || f3 > 1) return; g = f3 ? G_FCLASS : G_FMVXW, rf = F(rs1), wf = R(rd), d->rd = ird; break;
        case 0x78: if (rs2 || f3) return; g = G_FMVWX, rf = R(rs1); break;
        case 0x50: if (f3 > 2) return; g = f3 == 2 ? G_FEQ : f3 == 1 ? G_FLT : G_FLE, wf = R(rd), d->rd = ird; break;
        default: return;
        }
        gen(d, g);
        d->reads = rf, d->writes = wf;
        return;
    }
    }
}

/* ---- the translation's state ---- */
typedef struct { uint32_t x, y, z, w; } Slot;
typedef struct {
    const XlatCode* in;
    Ins* ins;            /* by word */
    uint32_t n;
    uint8_t* lead;       /* the word starts a block */
    uint32_t* native;    /* each word's slot address */
    uint32_t base;
    Slot* out;
    uint32_t nslots, cap;
    XlatStats* st;
    XlatPatch* tp;
    uint32_t ntp, ctp;
    uint32_t *pstart, *plist, *seen[4], *stack[4], gen[4];   /* the branches into each word; backward walks' marks */
    uint8_t* entry;                                         /* entered from elsewhere: an entry, a call's target */
    uint8_t* jmap;                                          /* a jump through a table that was not found */
    uint32_t* jbase;                                        /* its base when that lies in code (an offset into it) */
    uint8_t* keep2;                                         /* auipcs that name such a base: RISC-V addresses */
} X;

static int in_code(const X* t, uint32_t a) {
    if (a - t->in->addr >= t->in->size || (a & 3)) return 0;
    return !t->in->is_code || t->in->is_code[(a - t->in->addr) >> 2];
}
static uint32_t idx(const X* t, uint32_t a) { return (a - t->in->addr) >> 2; }
static int kept(const X* t, uint32_t i) { return (t->in->keep && t->in->keep[i]) || (t->keep2 && t->keep2[i]); }

uint32_t xlat_map(const XlatCode* in, const XlatResult* r, uint32_t a) {
    if (a - in->addr >= in->size || (a & 3)) return a;
    uint32_t n = r->native[(a - in->addr) >> 2];
    return n ? n : a;
}

/* The low parts an auipc's register is used with, until it is written again or the code goes elsewhere. */
static int auipc_uses(const X* t, uint32_t i, uint32_t* lo, int max) {
    uint32_t r = (t->ins[i].ins >> 7) & 31;
    int n = 0;
    for (uint32_t j = i + 1; j < t->n && j < i + 48; j++) {
        const Ins* d = &t->ins[j];
        if (d->cls == C_DATA) break;
        uint32_t op = d->ins & 0x7f, rs1 = (d->ins >> 15) & 31;
        if (rs1 == r && (d->reads & R(r)) && n < max) {
            if ((op == 0x13 && ((d->ins >> 12) & 7) == 0) || op == 0x03 || op == 0x07 || op == 0x67) lo[n++] = sx(d->ins >> 20, 12);
            else if (op == 0x23 || op == 0x27) lo[n++] = sx(((d->ins >> 25) << 5) | ((d->ins >> 7) & 31), 12);
        }
        if ((d->writes & R(r)) || d->cls == C_JAL || d->cls == C_JALR) break;
    }
    return n;
}

/* The branches into each word (conditional ones and plain jumps), for walking the code backwards. */
static void make_preds(X* t) {
    t->pstart = (uint32_t*)calloc(t->n + 2, 4);
    for (uint32_t i = 0; i < t->n; i++) {
        const Ins* d = &t->ins[i];
        if ((d->cls == C_BRANCH || (d->cls == C_JAL && d->rd == SINK)) && in_code(t, d->target)) t->pstart[idx(t, d->target) + 1]++;
    }
    for (uint32_t i = 0; i < t->n; i++) t->pstart[i + 1] += t->pstart[i];
    t->plist = (uint32_t*)malloc((t->pstart[t->n] + 1) * 4);
    uint32_t* fill = (uint32_t*)calloc(t->n + 1, 4);
    for (uint32_t i = 0; i < t->n; i++) {
        const Ins* d = &t->ins[i];
        if ((d->cls == C_BRANCH || (d->cls == C_JAL && d->rd == SINK)) && in_code(t, d->target)) {
            uint32_t k = idx(t, d->target);
            t->plist[t->pstart[k] + fill[k]++] = i;
        }
    }
    free(fill);
    /* where code is entered other than by a branch: the entries given and every call's target */
    t->entry = (uint8_t*)calloc(t->n + 1, 1);
    for (uint32_t k = 0; k < t->in->nentries; k++) if (in_code(t, t->in->entries[k])) t->entry[idx(t, t->in->entries[k])] = 1;
    for (uint32_t i = 0; i < t->n; i++) {
        const Ins* d = &t->ins[i];
        if (d->cls == C_JAL && d->rd != SINK && in_code(t, d->target)) t->entry[idx(t, d->target)] = 1;
        if (d->cls == C_AUIPC && !kept(t, i)) {   /* (an address taken with auipc may be called) */
            uint32_t lo[8];
            int n = auipc_uses(t, i, lo, 8);
            for (int q = 0; q < n; q++) if (in_code(t, d->pc + d->w + lo[q])) { uint8_t* e = &t->entry[idx(t, d->pc + d->w + lo[q])]; if (!*e) *e = 2; }   /* (2: named by an auipc only) */
        }
    }
    for (int k = 0; k < 4; k++) t->seen[k] = (uint32_t*)calloc(t->n + 1, 4), t->stack[k] = (uint32_t*)malloc((t->n + 1) * 4);
}

static int saved_reg(uint32_t r) { return r == 2 || r == 3 || r == 4 || r == 8 || r == 9 || (r >= 18 && r <= 27); }

/* What register r holds just before word at on every path that reaches it, when each path's last write to it is
 * an auipc (or an addi on such a value) and they agree: 1 and the address. */
static int value_before(X* t, uint32_t at, uint32_t r, int depth, uint32_t* value) {
    if (depth > 3 || !r) return 0;
    uint32_t gen = ++t->gen[depth], sp = 0, got = 0, v = 0, visits = 0;
    uint32_t *seen = t->seen[depth], *stack = t->stack[depth];
    stack[sp++] = at;
    seen[at] = gen;
    while (sp) {
        uint32_t p = stack[--sp];
        if (++visits > 200000) { if (getenv("XLAT_WHY")) fprintf(stderr, "  %#x r%u: too far\n", t->ins[at].pc, r); return 0; }
        /* the instructions that run just before p: the one above it, unless it goes elsewhere, and branches to p */
        uint32_t from[1 + 64], nf = 0;
        if (t->entry[p]) {   /* a function's start: what its caller had is not known (and nothing falls into it) */
            if (getenv("XLAT_WHY")) fprintf(stderr, "  %#x r%u: reaches entry %#x\n", t->ins[at].pc, r, t->ins[p].pc);
            return 0;
        }
        if (p > 0) {
            const Ins* q = &t->ins[p - 1];
            int falls = q->cls != C_DATA && q->cls != C_JALR && !(q->cls == C_JAL && q->rd == SINK) && !(q->cls == C_GEN && q->opg == G_ILLEGAL);
            if (q->cls == C_JALR && q->rd != SINK) falls = 1;   /* (a call returns here) */
            if (falls) from[nf++] = p - 1;
        }
        for (uint32_t k = t->pstart[p]; k < t->pstart[p + 1] && nf < 65; k++) from[nf++] = t->plist[k];
        if (!nf) {   /* a function's start fails (what its caller had is not known); a block only a table enters is passed */
            if (t->entry[p]) { if (getenv("XLAT_WHY")) fprintf(stderr, "  %#x r%u: reaches entry %#x\n", t->ins[at].pc, r, t->ins[p].pc); return 0; }
            continue;
        }
        for (uint32_t f = 0; f < nf; f++) {
            uint32_t q = from[f];
            const Ins* d = &t->ins[q];
            uint32_t found;
            if (d->writes & R(r)) {
                if (d->cls == C_AUIPC) found = d->pc + d->w;
                else if ((d->ins & 0x707f) == 0x0013 && value_before(t, q, (d->ins >> 15) & 31, depth + 1, &found)) found += sx(d->ins >> 20, 12);
                else { if (getenv("XLAT_WHY")) fprintf(stderr, "  %#x r%u: written at %#x\n", t->ins[at].pc, r, d->pc); return 0; }
                if (got && found != v) { if (getenv("XLAT_WHY")) fprintf(stderr, "  %#x r%u: %#x and %#x\n", t->ins[at].pc, r, v, found); return 0; }
                got = 1, v = found;
                continue;
            }
            if ((d->cls == C_JAL || d->cls == C_JALR) && d->rd != SINK && !saved_reg(r)) {   /* a call may change it */
                if (getenv("XLAT_WHY")) fprintf(stderr, "  %#x r%u: call at %#x\n", t->ins[at].pc, r, d->pc);
                return 0;
            }
            if (seen[q] != gen) { seen[q] = gen; stack[sp++] = q; }
        }
    }
    if (got) *value = v;
    return (int)got;
}
static int lla_before(X* t, uint32_t at, uint32_t r, uint32_t* value) { return value_before(t, at, r, 0, value); }

/* The last auipc (+ addi) to r above word at in the code's order: a guess, taken only where a range check bounds the table. */
static int lla_above(const X* t, uint32_t at, uint32_t r, uint32_t* value) {
    uint32_t lo = 0;
    int addi = 0;
    for (uint32_t k = 1; k <= 4096 && k <= at; k++) {
        const Ins* d = &t->ins[at - k];
        if (d->cls == C_DATA) return 0;
        if (!(d->writes & R(r))) continue;
        if (!addi && (d->ins & 0x707f) == 0x0013 && ((d->ins >> 15) & 31) == r) { lo = sx(d->ins >> 20, 12); addi = 1; continue; }
        if (d->cls == C_AUIPC) { *value = d->pc + d->w + lo; return 1; }
        return 0;
    }
    return 0;
}

static void patch_add(X* t, uint32_t addr, uint32_t value, uint32_t base) {
    if (t->ntp == t->ctp) { t->ctp = t->ctp ? t->ctp * 2 : 256; t->tp = (XlatPatch*)realloc(t->tp, t->ctp * sizeof *t->tp); }
    t->tp[t->ntp++] = (XlatPatch){addr, value, base, XLAT_REL};
}

/* ---- a switch's table: lla b; ... lw e, 0(b + 4 i); add j, e, b; jr j. Its entries name code relative to b ---- */
static void missed(X* t, uint32_t at, int why) {
    t->st->tables_missed++;
    t->jmap[at] = 1;   /* its jump goes through a map of its function's code (G_JMAP) */
    if (getenv("XLAT_MISSED")) fprintf(stderr, "table missed at %#x (%d)\n", t->ins[at].pc, why);
}
typedef struct { uint32_t at, base, limit, guessed; } Table;
static int table_cmp(const void* a, const void* b) {
    uint32_t x = ((const Table*)a)->base, y = ((const Table*)b)->base;
    return x < y ? -1 : x > y;
}
/* A jump through a switch's table: its base and how many entries the range check before it allows (0: none found). */
static int find_table(X* t, uint32_t at, Table* tb) {
    uint32_t rj = t->ins[at].ra, base = 0, got = 0, shape = 0, guessed = 0;
    for (uint32_t k = 1; k < 12 && k <= at; k++) {
        const Ins* d = &t->ins[at - k];
        if ((d->ins & 0xfe00707f) == 0x00000033 && ((d->ins >> 7) & 31) == rj) {
            uint32_t r1 = (d->ins >> 15) & 31, r2 = (d->ins >> 20) & 31;
            /* a table's shape: one operand an entry loaded just before (lw e, 0(...)) */
            for (uint32_t q = k + 1; q < k + 12 && q <= at && !shape; q++) {
                const Ins* l = &t->ins[at - q];
                if ((l->ins & 0x707f) == 0x2003 && (((l->ins >> 7) & 31) == r1 || ((l->ins >> 7) & 31) == r2)) shape = 1;
                else if ((l->writes & (R(r1) | R(r2))) || l->cls == C_DATA) break;
            }
            if (lla_before(t, at - k, r2, &base) || lla_before(t, at - k, r1, &base)) got = k;
            else if (shape && (lla_above(t, at - k, r2, &base) || lla_above(t, at - k, r1, &base))) got = k, guessed = 1;
            break;
        }
        if (d->writes & R(rj)) break;
    }
    if (!got && !shape) return 0;   /* (a jump through a pointer: it names a slot already) */
    if (!got || in_code(t, base) || !t->in->read_data) {
        if (got && in_code(t, base)) t->jbase[at] = base;   /* (a jump into a run of code by an offset: memset's) */
        missed(t, at, !got ? 1 : 2);
        return 0;
    }
    /* the range check: the first unsigned branch back in code that falls through to here (bltu c, i: c + 1
     * entries; bgeu i, c: c), its constant an li */
    uint32_t limit = 0;
    for (uint32_t k = got + 1; k < 64 && k <= at; k++) {
        const Ins* d = &t->ins[at - k];
        if (d->cls == C_DATA || d->cls == C_JALR || (d->cls == C_JAL && d->rd == SINK)) break;
        if (d->cls != C_BRANCH || !(d->x & X_UNS)) continue;
        uint32_t rc = (d->x & X_INV) ? ((d->ins >> 20) & 31) : ((d->ins >> 15) & 31);
        for (uint32_t q = k + 1; q < 4096 && q <= at; q++) {
            const Ins* l = &t->ins[at - q];
            if (l->cls == C_DATA) break;
            if ((l->ins & 0xff07f) == 0x00013 && ((l->ins >> 7) & 31) == rc) { limit = sx(l->ins >> 20, 12) + ((d->x & X_INV) ? 0 : 1); break; }
            if (l->writes & R(rc)) break;
        }
        break;
    }
    if (guessed && !limit) { missed(t, at, 5); return 0; }
    *tb = (Table){at, base, limit, guessed};
    return 1;
}

/* The tables' entries: as many as the check allows, else while they name code, never into the next table. */
static void take_tables(X* t, Table* tb, uint32_t n) {
    qsort(tb, n, sizeof *tb, table_cmp);
    for (uint32_t i = 0; i < n; i++) {
        if (i && tb[i].base == tb[i - 1].base) continue;
        uint32_t j = i + 1, limit = tb[i].limit;
        for (; j < n && tb[j].base == tb[i].base; j++) if (tb[j].limit > limit) limit = tb[j].limit;   /* (jumps sharing a table) */
        uint32_t next = j < n ? tb[j].base : ~0u, room = (next - tb[i].base) / 4;
        tb[i].limit = limit;
        uint32_t cap = limit ? limit : 4096, k = 0;
        if (cap > room) cap = room;
        uint32_t jr = t->ins[tb[i].at].pc;
        for (; k < cap; k++) {   /* (a case lies in the switch's function: within a megabyte of its jump) */
            uint32_t e, c;
            if (!t->in->read_data(t->in->ctx, tb[i].base + 4 * k, &e) || !in_code(t, c = tb[i].base + e)) break;
            if ((!limit || tb[i].guessed) && c - jr + 0x100000u > 0x200000u) break;
        }
        if (tb[i].guessed) t->st->tables_guessed++;
        if (!k || (tb[i].limit && k < cap)) { if (getenv("XLAT_MISSED")) fprintf(stderr, "base %#x limit %u room %u\n", tb[i].base, tb[i].limit, room); missed(t, tb[i].at, k ? 3 : 4); continue; }
        if (!tb[i].limit) t->st->tables_unbounded++;
        for (uint32_t q = 0; q < k; q++) {
            uint32_t e;
            t->in->read_data(t->in->ctx, tb[i].base + 4 * q, &e);
            patch_add(t, tb[i].base + 4 * q, tb[i].base + e, tb[i].base);
            t->lead[idx(t, tb[i].base + e)] = 1;
        }
        t->st->tables++, t->st->table_entries += k;
    }
}

/* ---- slots ---- */
static Slot* slot_new(X* t) {
    if (t->nslots == t->cap) { t->cap = t->cap ? t->cap * 2 : 4096; t->out = (Slot*)realloc(t->out, (size_t)t->cap * sizeof(Slot)); }
    Slot* s = &t->out[t->nslots++];
    memset(s, 0, sizeof *s);
    return s;
}

/* The second unit's op for d (sum and or xor sll srl sra sub), or -1. */
static int unit2(const Ins* d) {
    if (d->cls != C_OP || (d->x & (X_BR | X_PCA | X_UNS | X_EQ | X_INV)) || d->mem) return -1;
    uint32_t sel = (d->x & X_SEL) >> X_SEL_SHIFT;
    if (sel == SEL_CMP) return -1;
    return sel == SEL_SUM && (d->x & X_NEG) ? 7 : (int)sel;
}
static int fits8(uint32_t v) { return (int32_t)v >= -128 && (int32_t)v < 128; }
static int plain_lw(const Ins* d) { return d->cls == C_LOAD && (d->x & (X_MW | X_MU)) == 0; }
static int plain_sw(const Ins* d) { return d->cls == C_STORE && (d->x & X_MW) == 0; }
static int reg_op(const Ins* d) { return (d->ins & 0x7f) == 0x33; }

static int dep(const Ins* a, const Ins* b) {
    if ((a->writes & b->reads) || (a->reads & b->writes) || (a->writes & b->writes)) return 1;
    return a->mem && b->mem && (a->store || b->store);
}

/* How c goes in m's second place: 0 not, 1 the unit, 2 the unit on W, 3 a word load, 4 the next word's store.
 * after: c follows m in the program (it may read m's result, but not as an address); else c comes before m. */
static int how2(const Ins* m, const Ins* c, int after) {
    if (m->cls == C_GEN || c->end || c->cls == C_GEN || c->cls == C_AUIPC) return 0;
    int u = unit2(c);
    if (after) {
        if (m->end) return 0;
        if (u >= 0) {
            if (reg_op(c) || fits8(c->w)) return 1;
            return m->w == 0 && m->cls == C_OP ? 2 : 0;
        }
        if (plain_lw(c) && fits8(c->w) && !(m->writes & c->reads) && !(m->store && !plain_sw(m))) return 3;
        if (plain_sw(m) && plain_sw(c) && c->ra == m->ra && c->w == m->w + 4 && !(m->writes & c->reads)) return 4;
        return 0;
    }
    if (dep(m, c)) return 0;
    if (u >= 0) return reg_op(c) || fits8(c->w) ? 1 : 0;
    if (plain_lw(c) && fits8(c->w) && !m->mem) return 3;
    return 0;
}

static void put_main(Slot* s, const Ins* m) {
    s->x = m->x;
    s->y = m->ra | SINK << 10;   /* (no second op: its result to the sink) */
    s->z = m->rd | m->rb << 6 | m->opg << 16 | m->rc << 24;
    s->w = m->w;
}

static void put2(Slot* s, const Ins* c, int h) {
    uint32_t op = c->ins & 0x7f, re = c->rd, rf = c->ra, rg = c->rb, imm8 = c->w;
    if (op == 0x13) rg = 0;
    if (op == 0x37) rf = rg = 0;
    if (op == 0x33) imm8 = 0;
    if (h == 1 || h == 2) s->x |= (uint32_t)unit2(c) << X_SEL2_SHIFT;
    if (h == 2) s->x |= X_BIG, s->w = c->w, imm8 = 0;
    if (h == 3) s->x |= X_LD2, rg = 0;
    if (h == 4) s->x |= X_ST2, re = SINK, rf = 0, rg = c->rd, imm8 = 0;
    s->x |= rg << X_RG_SHIFT;
    s->y = (s->y & 31u) | rf << 5 | re << 10 | (imm8 & 0xff) << 24;
}

/* what each slot's address fields name, fixed once every block is laid out */
enum { A_NONE, A_TARGET, A_ADDR, A_HI, A_MAP };
typedef struct { uint32_t slot, kind, value, word; } Addr;

XlatResult xlat_code(const XlatCode* in, uint32_t base, XlatStats* st) {
    XlatResult res;
    memset(&res, 0, sizeof res);
    X t;
    memset(&t, 0, sizeof t);
    t.in = in, t.base = base, t.st = st;
    t.n = in->size / 4;
    t.ins = (Ins*)calloc(t.n + 1, sizeof(Ins));
    t.lead = (uint8_t*)calloc(t.n + 1, 1);
    t.native = (uint32_t*)calloc(t.n + 1, 4);
    for (uint32_t i = 0; i < t.n; i++) {
        if (in->is_code && !in->is_code[i]) { t.ins[i].cls = C_DATA, t.ins[i].end = 1, t.ins[i].pc = in->addr + 4 * i; continue; }
        uint32_t w;
        memcpy(&w, in->bytes + 4 * i, 4);
        decode(&t.ins[i], w, in->addr + 4 * i);
        if (t.ins[i].cls == C_BAD) gen(&t.ins[i], G_ILLEGAL), t.ins[i].end = 1, t.ins[i].reads = t.ins[i].writes = ~0ull, st->illegal++;
        st->words++;
    }
    Table* tb = NULL;
    uint32_t ntb = 0, ctb = 0;
    make_preds(&t);
    t.jmap = (uint8_t*)calloc(t.n + 1, 1);
    t.jbase = (uint32_t*)calloc(t.n + 1, 4);
    t.keep2 = (uint8_t*)calloc(t.n + 1, 1);
    /* block starts: the entries, branches' targets, after any control op, code named by an auipc, switch cases */
    for (uint32_t k = 0; k < in->nentries; k++) if (in_code(&t, in->entries[k])) t.lead[idx(&t, in->entries[k])] = 1;
    for (uint32_t i = 0; i < t.n; i++) {
        const Ins* d = &t.ins[i];
        if (d->cls == C_DATA) { t.lead[i + 1] = 1; continue; }
        if (i == 0 || t.ins[i - 1].cls == C_DATA) t.lead[i] = 1;
        if (d->end) t.lead[i + 1] = 1;
        if (d->cls == C_BRANCH || d->cls == C_JAL) {
            if (in_code(&t, d->target)) t.lead[idx(&t, d->target)] = 1;
            else st->outside++;
        }
        if (d->cls == C_AUIPC && !kept(&t, i)) {
            uint32_t lo[8];
            int n = auipc_uses(&t, i, lo, 8);
            for (int k = 0; k < n; k++) if (in_code(&t, d->pc + d->w + lo[k])) t.lead[idx(&t, d->pc + d->w + lo[k])] = 1;
        }
        if (d->cls == C_JALR && d->rd == SINK && d->ra != 1 && d->w == 0) {
            if (ntb == ctb) { ctb = ctb ? 2 * ctb : 64; tb = (Table*)realloc(tb, ctb * sizeof *tb); }
            ntb += (uint32_t)find_table(&t, i, &tb[ntb]);
        }
    }
    take_tables(&t, tb, ntb);
    free(tb);
    /* a jump that goes through a map: every word of its function a block, so that any case is a slot's start */
    typedef struct { uint32_t site, lo, hi, at; } Map;
    Map* maps = NULL;
    uint32_t nmaps = 0;
    for (uint32_t i = 0; i < t.n; i++) {
        if (!t.jmap[i]) continue;
        uint32_t lo = i, hi = i + 1;
        while (lo > 0 && t.entry[lo] != 1 && t.ins[lo - 1].cls != C_DATA) lo--;
        while (hi < t.n && t.entry[hi] != 1 && t.ins[hi].cls != C_DATA) hi++;
        for (uint32_t k = lo; k <= hi && k <= t.n; k++) t.lead[k] = 1;
        for (uint32_t k = lo; k < hi && t.jbase[i]; k++) {   /* the base's auipc gives the RISC-V address the map takes */
            uint32_t l12[8];
            int nu = t.ins[k].cls == C_AUIPC ? auipc_uses(&t, k, l12, 8) : 0;
            for (int q = 0; q < nu; q++) if (t.ins[k].pc + t.ins[k].w + l12[q] == t.jbase[i]) t.keep2[k] = 1;
        }
        maps = (Map*)realloc(maps, (nmaps + 1) * sizeof *maps);
        maps[nmaps++] = (Map){i, lo, hi, 0};
    }
    /* each block's ops in pairs, in order */
    Addr* addr = (Addr*)calloc(t.n + 1, sizeof(Addr));
    uint32_t naddr = 0;
    for (uint32_t s = 0; s < t.n;) {
        uint32_t e = s + 1;
        while (e < t.n && !t.lead[e]) e++;
        if (t.ins[s].cls == C_DATA) { s = e; continue; }
        st->blocks++;
        uint8_t done[256];
        for (uint32_t b0 = s; b0 < e; b0 += 256) {   /* (a very long block in windows) */
            uint32_t k = (b0 + 256 < e ? b0 + 256 : e) - b0;
            memset(done, 0, sizeof done);
            for (uint32_t i = 0; i < k; i++) {
                if (done[i]) continue;
                const Ins* m = &t.ins[b0 + i];
                Slot* sl = slot_new(&t);
                uint32_t at = base + 16 * (t.nslots - 1), cnt = 1, me = b0 + i;
                t.native[me] = at;
                done[i] = 1;
                Ins f = *m;
                Addr a = {t.nslots - 1, A_NONE, 0, me};
                if (m->cls == C_BRANCH || m->cls == C_JAL) a.kind = A_TARGET, a.value = m->target;
                if (m->cls == C_AUIPC) a.kind = A_HI;
                if (t.jmap[me]) {
                    f.cls = C_GEN, f.x = K_GEN, f.opg = G_JMAP, f.rd = SINK, f.rb = 0, f.w = 0;
                    a.kind = A_MAP;
                    for (uint32_t q = 0; q < nmaps; q++) if (maps[q].site == me) a.value = q;
                }
                int32_t step = 0;
                /* two ops as one: lui or auipc and its addi, auipc and jalr, a step and the branch on it */
                if (i + 1 < k) {
                    const Ins* q = &t.ins[b0 + i + 1];
                    uint32_t qop = q->ins & 0x7f, q3 = (q->ins >> 12) & 7;
                    int on = ((q->ins >> 15) & 31) == ((m->ins >> 7) & 31) && m->rd != SINK;
                    if ((m->ins & 0x7f) == 0x37 && qop == 0x13 && q3 == 0 && on && q->rd == m->rd) {
                        f.w = m->w + q->w, cnt = 2;
                    } else if (m->cls == C_AUIPC && qop == 0x13 && q3 == 0 && on && q->rd == m->rd) {
                        a.kind = A_ADDR, a.value = m->pc + m->w + q->w, cnt = 2;
                    } else if (m->cls == C_AUIPC && q->cls == C_JALR && on && q->rd == m->rd) {
                        f = *q, f.x &= ~X_IND, f.ra = 0, f.reads = 0;
                        a.kind = A_TARGET, a.value = m->pc + m->w + q->w, cnt = 2;
                    } else if (unit2(m) == 0 && (m->ins & 0x707f) == 0x13 && m->ra == m->rd && m->rd != SINK && (int32_t)m->w >= -8 &&
                               (int32_t)m->w < 8 && q->cls == C_BRANCH && (q->x & X_EQ) && q->ra == m->rd && q->rb != m->rd) {
                        f = *q, f.x |= X_STEP, f.rd = m->rd, f.writes = m->writes, f.reads |= m->reads, step = (int32_t)m->w;
                        a.kind = A_TARGET, a.value = q->target, cnt = 2;
                    }
                    if (cnt == 2) { done[i + 1] = 1, t.native[me + 1] = at, st->fused++; }
                }
                /* the second place: a later op that may come up, or for the block's control op an earlier one */
                int h = 0;
                uint32_t ci = 0;
                if (f.cls != C_GEN && !(f.x & X_STEP) && !f.end) {
                    for (uint32_t j = i + cnt; j < k && j < i + 12 && !h; j++) {
                        if (done[j]) continue;
                        const Ins* c = &t.ins[b0 + j];
                        if (c->end) break;
                        int hh = how2(&f, c, 1);
                        if (hh == 2 && (f.w != 0 || a.kind != A_NONE)) hh = 0;
                        if (hh) {
                            int ok = 1;
                            for (uint32_t q = i + 1; q < j && ok; q++) if (!done[q] && dep(&t.ins[b0 + q], c)) ok = 0;
                            if (ok) h = hh, ci = j;
                        }
                    }
                }
                put_main(sl, &f);
                if (step) sl->z |= ((uint32_t)step & 15) << 12;
                if (h) {
                    put2(sl, &t.ins[b0 + ci], h);
                    done[ci] = 1, t.native[b0 + ci] = at, cnt++, st->paired++;
                }
                sl->x |= cnt << X_CNT_SHIFT;
                st->ops += cnt;
                if (a.kind != A_NONE) addr[naddr++] = a;
            }
        }
        s = e;
    }
    /* the maps, after the slots: (the function's first address, its words, each word's slot from the map) */
    for (uint32_t q = 0; q < nmaps; q++) {
        uint32_t words = 2 + maps[q].hi - maps[q].lo, first = t.nslots;
        for (uint32_t k = 0; k < (words + 3) / 4; k++) slot_new(&t);
        uint32_t* w = (uint32_t*)&t.out[first];
        maps[q].at = base + 16 * first;
        w[0] = t.ins[maps[q].lo].pc, w[1] = maps[q].hi - maps[q].lo;
        for (uint32_t k = maps[q].lo; k < maps[q].hi; k++) w[2 + k - maps[q].lo] = t.native[k] - maps[q].at;
        st->maps++;
    }
    /* names: branches and calls to slots, pc-relative addresses to code as slots, to data as they were */
    for (uint32_t k = 0; k < naddr; k++) {
        Addr* a = &addr[k];
        Slot* sl = &t.out[a->slot];
        uint32_t at = base + 16 * a->slot, value;
        if (a->kind == A_MAP) {
            value = maps[a->value].at;
        } else if (a->kind == A_TARGET) {
            value = in_code(&t, a->value) ? t.native[idx(&t, a->value)] : a->value;
            if (!in_code(&t, a->value)) st->outside++;
        } else if (a->kind == A_ADDR) {
            value = in_code(&t, a->value) && !kept(&t, a->word) ? t.native[idx(&t, a->value)] : a->value;
            if (in_code(&t, a->value)) st->code_addresses++;
        } else {
            const Ins* d = &t.ins[a->word];
            uint32_t lo[8], hi = d->pc + d->w;
            int n = auipc_uses(&t, a->word, lo, 8);
            value = hi;
            for (int q = 0; q < n && !kept(&t, a->word); q++)
                if (in_code(&t, hi + lo[q])) { value = t.native[idx(&t, hi + lo[q])] - lo[q]; st->code_addresses++; }
        }
        sl->w = value - at;
    }
    for (uint32_t k = 0; k < t.ntp; k++) t.tp[k].value = t.native[idx(&t, t.tp[k].value)];
    st->slots += t.nslots;
    res.slots = (uint8_t*)t.out, res.nslots = t.nslots, res.native = t.native, res.patches = t.tp, res.npatches = t.ntp;
    free(addr); free(t.lead); free(t.ins); free(t.pstart); free(t.plist); free(t.entry); free(t.jmap); free(t.jbase); free(t.keep2); free(maps);
    for (int k = 0; k < 4; k++) free(t.seen[k]), free(t.stack[k]);
    return res;
}

void xlat_free(XlatResult* r) { free(r->slots); free(r->native); free(r->patches); memset(r, 0, sizeof *r); }
