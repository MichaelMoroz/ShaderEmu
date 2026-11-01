// The striped write cache over a GVM_STLOG trace (trace.py). No tick here: everything this prints is modelled.
//   sim TRACE            read-back statistics of unlimited passes
//   sim TRACE CONFIG...  one line per config
// The stream is cut again by the simulated cache: only a logged sync, wait or halt is a real end, a logged budget or
// full end is not (the reference ran with no cache model), and the budget counts from the simulated pass's start.
//
// CONFIG: name=value,...
//   s=stripes  e=entries in a stripe (2 tables of e/8 buckets, 4 ways; a stripe's table is held by one block)
//   r=entries kept by all (replicated; 0: none)  p=f|l  f: the first r texels of a pass, no eviction;
//       l: 4-way LRU sets, a victim goes to its stripe (the holder), which can fill it
//   sil=0|1 silent stores take no entry
//   f=filter bits (0: exact, tags in every block), counted=1: the end by capacity is by a counter every block can keep
//       (filter misses a stripe; end at cap=percent of e); a false positive that a holder cannot place counts as `div`.
//   b=budget in bundles
//   pred=bits,g=shift: only a store whose texel >> shift is marked in a table every block keeps goes to the kept part. A read-back's end
//       marks it (all blocks see that end, so all learn alike), and the table outlives the pass; age=passes: cleared that often
//   stk=N: a store within N bytes around sp goes to the kept part too (sp is a register, the same in every block)
// A load of a texel marked stored and not kept by all ends the pass in all blocks.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint32_t a, sp, b, info, pc; } rec;
#define TAB (1u << 16)
typedef struct { uint32_t key[TAB]; uint32_t ep[TAB]; uint32_t ord[TAB]; uint32_t lastb[TAB]; uint8_t rb[TAB]; } map;

static inline uint32_t h1(uint32_t t) { return t * 0x9e3779b1u; }
static inline uint32_t h2(uint32_t t) { return t * 0x85ebca6bu + 0x165667b1u; }

static map M;
static uint32_t epoch = 1;
static inline int mfind(uint32_t t) {   // the texel's place; M.ep[i] == epoch if it is there
    uint32_t i = (h1(t) >> 16) & (TAB - 1);
    while (M.ep[i] == epoch && M.key[i] != t) i = (i + 1) & (TAB - 1);
    return (int)i;
}

typedef struct {
    int s, e, r, lru, sil, f, counted, cap, b, pred, g, age, stk;
    uint32_t *pt; uint64_t since;
    char name[96];
    uint32_t *tb;      // the tables of stripes: s * (2 * nb * 4)
    uint32_t *lr, *la; // replicated sets
    uint32_t *flt;
    int *cnt, *used;
    int nb, nr, rsets, firstn;
    uint64_t tick;
    uint64_t passes, bundles, e_budget, e_nat, e_full, e_read, e_div, b_full, b_read, b_budget, b_nat, wasted, placed;
    uint64_t bs;  // bundle at the pass's start
    uint32_t touched[4096 * 4]; int ntouched;
    uint32_t pass_ep;
    uint32_t *st_ep;   // texels of this pass: stored marks
    uint32_t *st_key;
} cfg;

#define STAB (1u << 15)
static inline int smark(cfg *c, uint32_t t, int set) {   // 1: was stored this pass
    uint32_t i = h1(t) >> 17;
    for (;;) {
        if (c->st_ep[i] != c->pass_ep) { if (set) { c->st_ep[i] = c->pass_ep; c->st_key[i] = t; } return 0; }
        if (c->st_key[i] == t) return 1;
        i = (i + 1) & (STAB - 1);
    }
}
static inline int fl_has(cfg *c, uint32_t t) { uint32_t b = (h2(t) >> 8) % (uint32_t)c->f; return (c->flt[b >> 5] >> (b & 31)) & 1; }
static inline void fl_set(cfg *c, uint32_t t) { uint32_t b = (h2(t) >> 8) % (uint32_t)c->f; c->flt[b >> 5] |= 1u << (b & 31); }

static inline int stripe_of(cfg *c, uint32_t t) { return c->s == 1 ? 0 : (int)(t & (uint32_t)(c->s - 1)); }
// 1: placed or there; 0: no room in the holder
static int place(cfg *c, uint32_t t) {
    int st = stripe_of(c, t), nb = c->nb;
    uint32_t tt = c->s == 1 ? t : t >> __builtin_ctz(c->s);
    uint32_t *T = c->tb + (size_t)st * (2 * nb * 4);
    uint32_t b0 = (((tt * 0x9e3779b1u) >> 16) * (uint32_t)nb) >> 16, b1 = nb + ((((tt * 0x85ebca6bu + 0x165667b1u) >> 16) * (uint32_t)nb) >> 16);
    uint32_t *q0 = T + b0 * 4, *q1 = T + b1 * 4;
    int j;
    for (j = 0; j < 4; j++) if (q0[j] == t + 1 || q1[j] == t + 1) return 1;
    for (j = 0; j < 4; j++) if (!q0[j]) { q0[j] = t + 1; goto put; }
    for (j = 0; j < 4; j++) if (!q1[j]) { q1[j] = t + 1; goto put; }
    return 0;
put:
    c->used[st]++; c->placed++;
    return 1;
}
static int in_tables(cfg *c, uint32_t t) {
    int st = stripe_of(c, t), nb = c->nb;
    uint32_t tt = c->s == 1 ? t : t >> __builtin_ctz(c->s);
    uint32_t *T = c->tb + (size_t)st * (2 * nb * 4);
    uint32_t b0 = (((tt * 0x9e3779b1u) >> 16) * (uint32_t)nb) >> 16, b1 = nb + ((((tt * 0x85ebca6bu + 0x165667b1u) >> 16) * (uint32_t)nb) >> 16);
    for (int j = 0; j < 4; j++) if (T[b0 * 4 + j] == t + 1 || T[b1 * 4 + j] == t + 1) return 1;
    return 0;
}
static void new_pass(cfg *c, uint64_t b) {
    if (c->nb) memset(c->tb, 0, (size_t)c->s * 2 * c->nb * 4 * 4);
    if (c->nr) memset(c->lr, 0, (size_t)c->nr * 4);
    if (c->f) memset(c->flt, 0, (size_t)(c->f / 32 + 1) * 4);
    memset(c->cnt, 0, sizeof(int) * c->s); memset(c->used, 0, sizeof(int) * c->s);
    c->firstn = 0; c->pass_ep++; c->bs = b;
    if (c->pred && c->age && ++c->since >= (uint64_t)c->age) { c->since = 0; memset(c->pt, 0, (size_t)(c->pred / 32 + 1) * 4); }
}
static void end_pass(cfg *c, uint64_t b, int why) {
    uint64_t n = b - c->bs;
    c->passes++; c->bundles += n;
    if (why == 0) c->e_budget++, c->b_budget += n; else if (why == 1) c->e_nat++, c->b_nat += n;
    else if (why == 2) c->e_full++, c->b_full += n; else if (why == 3) c->e_read++, c->b_read += n;
    new_pass(c, b);
}
// the kept-by-all part
static int r_has(cfg *c, uint32_t t, int touch) {
    if (!c->nr) return 0;
    if (!c->lru) { for (int i = 0; i < c->firstn; i++) if (c->lr[i] == t + 1) return 1; return 0; }
    uint32_t s = (h1(t) >> 12) % (uint32_t)c->rsets, *q = c->lr + s * 4, *a = c->la + s * 4;
    for (int j = 0; j < 4; j++) if (q[j] == t + 1) { if (touch) a[j] = (uint32_t)++c->tick; return 1; }
    return 0;
}
// a store new to the kept part: 1 kept. A victim goes to its stripe; 0 in *full when the holder had no room.
static int r_put(cfg *c, uint32_t t, int *full) {
    *full = 0;
    if (!c->nr) return 0;
    if (!c->lru) { if (c->firstn < c->nr) { c->lr[c->firstn++] = t + 1; return 1; } return 0; }
    uint32_t s = (h1(t) >> 12) % (uint32_t)c->rsets, *q = c->lr + s * 4, *a = c->la + s * 4;
    int v = 0;
    for (int j = 0; j < 4; j++) { if (!q[j]) { v = j; break; } if (a[j] < a[v]) v = j; }
    if (q[v]) { if (!place(c, q[v] - 1)) { *full = 1; return 0; } }
    q[v] = t + 1; a[v] = (uint32_t)++c->tick;
    return 1;
}

static inline uint32_t pslot(cfg *c, uint32_t t) { return (h2(t >> c->g) >> 8) % (uint32_t)c->pred; }
static void step(cfg *c, const rec *x) {
    uint32_t bytes = x->info & 0xff, t0 = x->a >> 4, t1 = (x->a + (bytes ? bytes : 1) - 1) >> 4;
    int load = (x->info >> 24) & 1, silent = (x->info >> 23) & 1;
    for (;;) {
        if (x->b - c->bs >= (uint64_t)c->b) { end_pass(c, c->bs + c->b, 0); continue; }   // (budget ends fall where they fall)
        break;
    }
    for (uint32_t t = t0;; t = t1) {
        if (load) {
            int stored = c->f ? fl_has(c, t) : smark(c, t, 0);
            if (c->s > 1 && stored && !r_has(c, t, 1)) { if (c->pred) { uint32_t q = pslot(c, t); c->pt[q >> 5] |= 1u << (q & 31); } end_pass(c, x->b - 1, 3); return step(c, x); }
        } else if (!(c->sil && silent)) {
            int was = c->f ? fl_has(c, t) : smark(c, t, 0);
            int kept = r_has(c, t, 1);
            if (!kept) {
                int full = 0;
                if (!was) {
                    if (c->counted && c->s > 0) {
                        int st = stripe_of(c, t);
                        int fits_r = c->nr && (c->lru || c->firstn < c->nr);
                        if (!fits_r && c->cnt[st] >= c->e * c->cap / 100) { end_pass(c, x->b - 1, 2); return step(c, x); }
                        if (!fits_r) c->cnt[st]++;
                    }
                    int want = 1;
                    if (c->pred || c->stk) {
                        uint32_t q = c->pred ? pslot(c, t) : 0;
                        want = (c->pred && ((c->pt[q >> 5] >> (q & 31)) & 1)) || (c->stk && x->a - x->sp + (uint32_t)c->stk < 2u * (uint32_t)c->stk);
                    }
                    if (!want || !r_put(c, t, &full)) {
                        if (full || !place(c, t)) {
                            if (c->counted) c->e_div++;
                            end_pass(c, x->b - 1, 2); return step(c, x);
                        }
                    }
                } else if (!in_tables(c, t)) {   // marked but not in the tables: the holder needs a slot (exact: it was in the kept part and went)
                    if (!place(c, t)) { if (c->counted) c->e_div++; end_pass(c, x->b - 1, 2); return step(c, x); }
                }
                if (c->f) fl_set(c, t); else smark(c, t, 1);
            }
        }
        if (t == t1) break;
    }
}

static cfg *parse(const char *a) {
    cfg *c = calloc(1, sizeof(cfg));
    c->s = 1; c->e = 168; c->b = 8192; c->sil = 0; c->cap = 70;
    snprintf(c->name, sizeof c->name, "%s", a);
    char *d = strdup(a);
    for (char *k = strtok(d, ","); k; k = strtok(NULL, ",")) {
        char *v = strchr(k, '='); if (!v) continue; *v++ = 0;
        if (!strcmp(k, "s")) c->s = atoi(v); else if (!strcmp(k, "e")) c->e = atoi(v); else if (!strcmp(k, "r")) c->r = atoi(v);
        else if (!strcmp(k, "p")) c->lru = *v == 'l'; else if (!strcmp(k, "sil")) c->sil = atoi(v); else if (!strcmp(k, "f")) c->f = atoi(v);
        else if (!strcmp(k, "counted")) c->counted = atoi(v); else if (!strcmp(k, "cap")) c->cap = atoi(v); else if (!strcmp(k, "b")) c->b = atoi(v);
        else if (!strcmp(k, "pred")) c->pred = atoi(v); else if (!strcmp(k, "g")) c->g = atoi(v); else if (!strcmp(k, "age")) c->age = atoi(v); else if (!strcmp(k, "stk")) c->stk = atoi(v);
    }
    c->nb = c->e / 8; if (c->nb < 1) c->nb = 1;
    c->nr = c->r; c->rsets = c->lru ? (c->r / 4 > 0 ? c->r / 4 : 1) : 0;
    c->tb = calloc((size_t)c->s * 2 * c->nb * 4, 4);
    c->lr = calloc((size_t)c->r + 8, 4); c->la = calloc((size_t)c->r + 8, 4);
    c->pt = calloc((size_t)c->pred / 32 + 2, 4);
    c->flt = calloc((size_t)c->f / 32 + 2, 4);
    c->cnt = calloc(c->s, sizeof(int)); c->used = calloc(c->s, sizeof(int));
    c->st_ep = calloc(STAB, 4); c->st_key = calloc(STAB, 4);
    return c;
}

// ---- Read-backs of unlimited passes: the trace cut only at the budget and the real ends
static const int CAPS[] = {170, 225, 370, 722};
#define NC 4
typedef struct { uint64_t passes, nat, stored, rb, puts, stores_silent, stores; uint64_t first_any_b, first_any_n, frb[NC], frbn[NC], ffull[NC], ffulln[NC], never_past[NC], past[NC], earlier[NC], both[NC]; uint64_t hist_any[10]; } rbstat;
static void read_backs(rec *R, size_t n, int sil, int budget, const char *label) {
    rbstat S; memset(&S, 0, sizeof S);
    uint32_t bs = R[0].b, nst = 0; int64_t first = -1; int64_t frb[NC], ff[NC];
    for (int i = 0; i < NC; i++) frb[i] = ff[i] = -1;
    uint64_t keep_rb = 0;
#define FLUSH(endb) do { S.passes++; S.stored += nst; S.rb += keep_rb; \
        if (first >= 0) { S.first_any_b += (uint64_t)first; S.first_any_n++; int h = (int)(first / 256); if (h > 9) h = 9; S.hist_any[h]++; } \
        for (int i = 0; i < NC; i++) { if (frb[i] >= 0) { S.frb[i] += (uint64_t)frb[i]; S.frbn[i]++; } if (ff[i] >= 0) { S.ffull[i] += (uint64_t)ff[i]; S.ffulln[i]++; \
            if (frb[i] >= 0) S.both[i]++; } } \
        epoch++; nst = 0; first = -1; keep_rb = 0; for (int i = 0; i < NC; i++) frb[i] = ff[i] = -1; bs = (endb); } while (0)
    for (size_t k = 0; k < n; k++) {
        const rec *x = &R[k];
        if (x->a == 0xffffffffu) {
            int why = x->info & 0xff;
            if (why != 0 && why != 3) { S.nat++; FLUSH(x->b); }
            continue;
        }
        while (x->b - bs >= (uint32_t)budget) FLUSH(bs + budget);
        uint32_t bytes = x->info & 0xff, t0 = x->a >> 4, t1 = (x->a + (bytes ? bytes : 1) - 1) >> 4;
        int load = (x->info >> 24) & 1, silent = (x->info >> 23) & 1;
        if (!load) { S.stores++; S.stores_silent += silent; }
        for (uint32_t t = t0;; t = t1) {
            int i = mfind(t), there = M.ep[i] == epoch;
            if (load) {
                if (there) {
                    if (!M.rb[i]) { M.rb[i] = 1; keep_rb++; }
                    if (first < 0) first = x->b - bs;
                    for (int c = 0; c < NC; c++) if ((int)M.ord[i] >= CAPS[c] && frb[c] < 0) frb[c] = x->b - bs;
                }
            } else if (!there && !(sil && silent)) {
                M.ep[i] = epoch; M.key[i] = t; M.ord[i] = nst; M.rb[i] = 0;
                for (int c = 0; c < NC; c++) if ((int)nst == CAPS[c] && ff[c] < 0) ff[c] = x->b - bs;
                nst++;
            }
            if (t == t1) break;
        }
    }
    printf("%s  passes %llu, real ends %.1f%%; texels stored a pass %.1f, read back in the pass %.1f (never read back %.1f%%); silent stores %.1f%%\n", label,
           (unsigned long long)S.passes, 100.0 * S.nat / S.passes, (double)S.stored / S.passes, (double)S.rb / S.passes, S.stored ? 100.0 - 100.0 * S.rb / S.stored : 0, S.stores ? 100.0 * S.stores_silent / S.stores : 0);
    printf("  first read-back of any stored texel: in %.1f%% of passes, at bundle %.0f; by 256-bundle bin %%:", 100.0 * S.first_any_n / S.passes, S.first_any_n ? (double)S.first_any_b / S.first_any_n : 0);
    for (int i = 0; i < 10; i++) printf(" %.1f", 100.0 * S.hist_any[i] / S.passes);
    printf("\n");
    for (int c = 0; c < NC; c++)
        printf("  past %4d: reached in %5.1f%% of passes at bundle %6.0f; a texel past it read back in %5.1f%% at bundle %6.0f\n", CAPS[c], 100.0 * S.ffulln[c] / S.passes, S.ffulln[c] ? (double)S.ffull[c] / S.ffulln[c] : 0,
               100.0 * S.frbn[c] / S.passes, S.frbn[c] ? (double)S.frb[c] / S.frbn[c] : 0);
}

int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f) / sizeof(rec); fseek(f, 0, SEEK_SET);
    rec *R = malloc(n * sizeof(rec));
    if (fread(R, sizeof(rec), n, f) != n) return 1;
    fclose(f);
    if (argc == 2) { read_backs(R, n, 0, 8192, "all stores  "); read_backs(R, n, 1, 8192, "with silent "); return 0; }
    for (int a = 2; a < argc; a++) {
        cfg *c = parse(argv[a]);
        c->pass_ep = 1; c->bs = R[0].b;
        for (size_t k = 0; k < n; k++) {
            const rec *x = &R[k];
            if (x->a == 0xffffffffu) { int why = x->info & 0xff; if (why != 0 && why != 3) { while (x->b - c->bs > (uint64_t)c->b) end_pass(c, c->bs + c->b, 0); end_pass(c, x->b, 1); } continue; }
            step(c, x);
        }
        uint64_t all = R[n - 1].b - R[0].b;
        printf("%-40s passes %8llu b/p %6.0f | budget %5.1f real %5.1f full %5.1f readback %5.1f | at full %5.0f at rb %5.0f | div %llu | stored a pass %.0f\n", c->name, (unsigned long long)c->passes, (double)all / c->passes,
               100.0 * c->e_budget / c->passes, 100.0 * c->e_nat / c->passes, 100.0 * c->e_full / c->passes, 100.0 * c->e_read / c->passes,
               c->e_full ? (double)c->b_full / c->e_full : 0, c->e_read ? (double)c->b_read / c->e_read : 0, (unsigned long long)c->e_div, (double)c->placed / c->passes);
        fflush(stdout);
    }
    return 0;
}
