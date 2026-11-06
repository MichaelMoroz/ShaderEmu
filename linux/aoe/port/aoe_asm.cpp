// What upstream has in Asm_draw.asm, Inf32.asm and Drawarea_asm.asm (13,839 lines, one piece for
// each span length and alignment): the same operations, one loop a kind of span.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../include/drawarea.h"
#include "aoe_cores.h"

struct Span { Span* next; Span* prev; int x0, x1; };

// A core's own: the first core's panels and the view on a worker draw at once (port/aoe_view.cpp)
enum { INDEX_FROM = 256, INDEX_STEP = 64, INDEXES = 8 };
struct RowMark { unsigned offset; int column; };
struct RowIndex { const unsigned* data; long width, height; unsigned check; int columns; RowMark* marks; };
struct AsmState {
    unsigned char** surface_offsets_;
    Span** span_heads_;
    Span** span_tails_;
    unsigned char* xlate_table_;
    long min_line_, max_line_, min_span_, max_span_;
    unsigned color_xform_;
    unsigned shadow_mask_[4], shadow_value_[4];
    RowIndex indexes_[INDEXES];
    int index_next_;
} __attribute__((aligned(64)));
// and the raster's band workers' (aoe_view.cpp's aoe_raster_levels: the view's last draws by bands of rows, each worker clipped to its
// band), each found by its stack
enum { ASM_BANDS = AOE_RASTER_BANDS };
static AsmState states[2 + ASM_BANDS];
struct AoeBandStack aoe_band_stack[ASM_BANDS];
int aoe_bands_on;
extern "C" int mcw_on_worker(void);
static inline AsmState* asm_state(void) {
    int c = aoe_view_core();
    if (c || !aoe_bands_on) return states + c;
    unsigned sp;
    __asm__("mv %0, sp" : "=r"(sp));
    for (int k = 0; k < ASM_BANDS; k++)
        if (sp - aoe_band_stack[k].lo < aoe_band_stack[k].hi - aoe_band_stack[k].lo) return states + 2 + k;
    return states;
}
#define surface_offsets A->surface_offsets_
#define span_heads A->span_heads_
#define span_tails A->span_tails_
#define xlate_table A->xlate_table_
#define min_line A->min_line_
#define max_line A->max_line_
#define min_span A->min_span_
#define max_span A->max_span_
#define color_xform A->color_xform_
#define shadow_mask A->shadow_mask_
#define shadow_value A->shadow_value_
#define indexes A->indexes_
#define index_next A->index_next_
#define AS AsmState* A = asm_state();
// A band worker starts as the view's worker stands at its last draws
void aoe_asm_for_band(int k) {
    AsmState* b = &states[2 + k];
    RowIndex ix[INDEXES]; memcpy(ix, b->indexes_, sizeof ix);
    int next = b->index_next_;
    *b = states[1];
    memcpy(b->indexes_, ix, sizeof ix);
    b->index_next_ = next;
}
// The worker starts as the first core stands
void aoe_asm_for_worker(void) {
    RowIndex keep[INDEXES]; memcpy(keep, states[1].indexes_, sizeof keep);
    int next = states[1].index_next_;
    states[1] = states[0];
    memcpy(states[1].indexes_, keep, sizeof keep);
    states[1].index_next_ = next;
}


extern "C" {
void ASMSet_Shadowing(unsigned long m1, unsigned long v1, unsigned long m2, unsigned long v2) { AS

    shadow_mask[0] = shadow_mask[2] = m1;
    shadow_value[0] = shadow_value[2] = v1;
    shadow_mask[1] = shadow_mask[3] = m2;
    shadow_value[1] = shadow_value[3] = v2;
}
void ASMSet_ShadowingEx(unsigned long* p) { AS

    for (int i = 0; i < 4; i++) { shadow_mask[i] = p[i]; shadow_value[i] = p[i + 4]; }
}
void ASMSet_Surface_Info(void* offsets, void* heads, void* tails, long minspan, long minline, long maxspan, long maxline) { AS
    surface_offsets = (unsigned char**) offsets;
    span_heads = (Span**) heads;
    span_tails = (Span**) tails;
    min_line = minline; max_line = maxline; min_span = minspan; max_span = maxspan;
}
void ASMSet_Color_Xform(int color) { AS color_xform = (unsigned char) color * 0x01010101u; }
int ASMGet_Color_Xform(void) { AS return color_xform & 0xff; }
void ASMSet_Xlate_Table(void* table) { AS xlate_table = (unsigned char*) table; }
void* ASMGet_Xlate_Table(void) { AS return xlate_table; }

void ASMDraw_HLine(long x1, long x2, long y, unsigned long color) { AS

    if (y < min_line || y > max_line) return;
    if (x1 > x2) { long t = x1; x1 = x2; x2 = t; }
    if (x2 < min_span || x1 > max_span) return;
    if (x1 < min_span) x1 = min_span;
    if (x2 > max_span) x2 = max_span;
    unsigned char* row = surface_offsets[y];
    for (Span* s = span_heads[y]; s; s = s->next) {
        if (x2 < s->x0) return;
        if (x1 > s->x1) continue;
        long a = x1 < s->x0 ? s->x0 : x1, b = x2 > s->x1 ? s->x1 : x2;
        memset(row + a, (unsigned char) color, b - a + 1);
        if (x2 <= s->x1) return;
    }
}
}

// the pattern a fogged sprite is drawn in: (pixel & mask) | value, by the address's place in its word
static inline unsigned char pat(unsigned char p, unsigned m, unsigned v, unsigned char* d) {
    int sh = ((unsigned long) d & 3) * 8;
    return (unsigned char) (((p | (p << 8) | (p << 16) | ((unsigned) p << 24)) & m | v) >> sh);
}

enum { OP_COPY, OP_FILL, OP_XFORM, OP_XFILL, OP_SHADOW };

// One run of a row. a to b are columns, left to right; src is what the column `from` takes
// (going by `dir`). The kinds' pixels go by the row's fog.
static inline void put_run(AsmState* A, int op, unsigned char* row, long a, long b, const unsigned char* src, unsigned char value,
                           int fog, unsigned m, unsigned v, int dir, long from) {
    unsigned char* d = row + a;
    long n = b - a + 1;
    unsigned char xf = (unsigned char) color_xform;
    if (!fog) {
        switch (op) {
            case OP_COPY:
                if (dir > 0) memcpy(d, src + (a - from), n);
                else { const unsigned char* s = src + (from - a); while (n--) *d++ = *s--; }
                return;
            case OP_FILL: memset(d, value, n); return;
            case OP_XFILL: memset(d, value | xf, n); return;
            case OP_XFORM:
                if (dir > 0) { const unsigned char* s = src + (a - from); while (n--) *d++ = *s++ | xf; }
                else { const unsigned char* s = src + (from - a); while (n--) *d++ = *s-- | xf; }
                return;
            case OP_SHADOW: { unsigned char* t = xlate_table; while (n--) { *d = t[*d]; d++; } return; }
        }
        return;
    }
    unsigned xv = (color_xform & m) | v;
    while (n > 0) {
        unsigned char p;
        switch (op) {
            case OP_COPY: p = pat(dir > 0 ? src[a - from] : src[from - a], m, v, d); break;
            case OP_FILL: p = pat(value, m, v, d); break;
            case OP_XFILL: p = pat(value, m, xv, d); break;
            case OP_XFORM: p = pat(dir > 0 ? src[a - from] : src[from - a], m, xv, d); break;
            default: {
                int sh = ((unsigned long) d & 3) * 8;
                p = (unsigned char) ((((unsigned) xlate_table[*d] << sh) | v) & m) >> 0;
                p = (unsigned char) ((((((unsigned) xlate_table[*d] << sh) | v) & m)) >> sh);
                break;
            }
        }
        *d++ = p;
        a++;
        n--;
    }
}

// A big picture's rows have hundreds of commands, and a button at the right of the screen has its piece of
// the picture behind it drawn from each row's start. Where each row stands every 64 columns, made once.

// One command of a row, left to right; 0 at the row's end.
static inline int row_command(const unsigned char*& s, long& at) {
    unsigned c = *s++;
    long n;
    switch (c & 15) {
        case 0: case 4: case 8: case 12: n = c >> 2; s += n; at += n; return 1;
        case 1: case 5: case 9: case 13: at += c >> 2; return 1;
        case 2: n = (c & 0xf0) << 4 | *s++; s += n; at += n; return 1;
        case 3: n = (c & 0xf0) << 4 | *s++; at += n; return 1;
        case 6: n = c >> 4; if (!n) n = *s++; s += n; at += n; return 1;
        case 7: case 10: n = c >> 4; if (!n) n = *s++; s++; at += n; return 1;
        case 11: n = c >> 4; if (!n) n = *s++; at += n; return 1;
        case 14:
            switch (c >> 4) {
                case 1: at += 1; return 1;
                case 2: at += *s++; return 1;
                case 4: {
                    unsigned e = *s++;
                    switch (e & 15) {
                        case 0: case 4: case 8: case 12: n = e >> 2; s += n; at += n; break;
                        case 1: case 5: case 9: case 13: at += e >> 2; break;
                        case 2: n = (e & 0xf0) << 4 | *s++; s += n; at += n; break;
                        case 3: n = (e & 0xf0) << 4 | *s++; at += n; break;
                        case 6: n = e >> 4; if (!n) n = *s++; s += n; at += n; break;
                        case 7: case 10: n = e >> 4; if (!n) n = *s++; s++; at += n; break;
                        case 11: n = e >> 4; if (!n) n = *s++; at += n; break;
                        case 14:
                            if ((e >> 4) == 1) at += 1;
                            else if ((e >> 4) == 2) at += *s++;
                            break;
                        default: break;
                    }
                    return 1;
                }
                default: return 1;
            }
        default: return 0;   // 15: the row's end
    }
}

static const RowIndex* row_index(AsmState* A, unsigned char* base, const unsigned* data, long width, long height) {
    unsigned check = data[0] * 31 + data[height / 2] * 7 + data[height - 1] + (unsigned) width * 65599 + (unsigned) height;
    for (int i = 0; i < INDEXES; i++)
        if (indexes[i].data == data && indexes[i].check == check) return &indexes[i];
    RowIndex* x = &indexes[index_next];
    index_next = (index_next + 1) % INDEXES;
    aoe_any_free(x->marks);
    x->data = data; x->width = width; x->height = height; x->check = check;
    x->columns = width / INDEX_STEP + 1;
    x->marks = (RowMark*) aoe_any_calloc(height * x->columns, sizeof(RowMark));
    if (!x->marks) { x->data = 0; return 0; }
    for (long row = 0; row < height; row++) {
        const unsigned char* start = base + data[row];
        const unsigned char* s = start;
        RowMark* marks = x->marks + row * x->columns;
        long at = 0, before_at = 0;
        const unsigned char* before = s;
        int c = 0;
        while (row_command(s, at)) {
            while (c < x->columns && c * INDEX_STEP < at) { marks[c].offset = before - start; marks[c].column = before_at; c++; }
            before = s; before_at = at;
        }
        for (; c < x->columns; c++) { marks[c].offset = before - start; marks[c].column = before_at; }
    }
    return x;
}

// A big picture drawn again (a menu's or a status screen's background, drawn three times as a panel comes: its focus, its redraw
// and UpdateWindow; the game screen's bands): its rows kept as pixels and runs the first time (docs/games/aoe.md, "The load"). A
// row of copies, fills and skips is then drawn as its runs of pixels, each copied within the spans: the same bytes put_run writes,
// and what a skip leaves untouched is untouched. Other rows (a player's colour, a transformed fill, a shadow), a fogged or mirrored
// drawing and the view's worker draw as before. The first core's only: AOE_PLAIN=0 is the way before.
enum { PLAINS = 16, PLAIN_NOT = 0x40000000 };
struct PlainRun { short a, b; };
struct Plain { const unsigned* data; long width, height; unsigned check, used, xf; unsigned char* pix; int* first; PlainRun* runs; };
static Plain plains[PLAINS];
static unsigned plain_clock;
static int plain_on = -1;
// A row into pixels and runs as the drawing below reads it (not mirrored): the number of runs, or -1 for a row drawn as before
static int plain_row(const unsigned char* s, long width, unsigned short lpad, unsigned char* out, PlainRun* runs, int room, unsigned char xf) {
    if (lpad & 0x8000) return 0;
    long at = lpad;
    int k = 0;
    for (;;) {
        unsigned c = *s++;
        long n, fill = -1;
        switch (c & 15) {
            case 0: case 4: case 8: case 12: n = c >> 2; break;
            case 2: n = (c & 0xf0) << 4 | *s++; break;
            case 7: n = c >> 4; if (!n) n = *s++; fill = *s++; break;
            case 6: {   // (a player's colour: the pixels or'd with the colour's transform, as put_run's OP_XFORM)
                n = c >> 4; if (!n) n = *s++;
                if (n <= 0) continue;
                if (at < 0 || at + n > width) return -1;
                for (long i = 0; i < n; i++) out[at + i] = s[i] | xf;
                s += n;
                if (k && runs[k - 1].b == at - 1) runs[k - 1].b = (short) (at + n - 1);
                else { if (k == room) return -1; runs[k].a = (short) at; runs[k].b = (short) (at + n - 1); k++; }
                at += n;
                continue;
            }
            case 10: n = c >> 4; if (!n) n = *s++; fill = *s++ | xf; break;   // (OP_XFILL)
            case 1: case 5: case 9: case 13: at += c >> 2; continue;
            case 3: at += (c & 0xf0) << 4 | *s++; continue;
            case 15: return k;
            default: return -1;   // a player's colour, a transformed fill, a shadow, an outline, a command passed over
        }
        if (n <= 0) continue;
        if (at < 0 || at + n > width) return -1;
        if (fill >= 0) memset(out + at, (int) fill, n);
        else { memcpy(out + at, s, n); s += n; }
        if (k && runs[k - 1].b == at - 1) runs[k - 1].b = (short) (at + n - 1);
        else { if (k == room) return -1; runs[k].a = (short) at; runs[k].b = (short) (at + n - 1); k++; }
        at += n;
    }
}
static const Plain* plain_get(unsigned char* base, const unsigned* data, const unsigned short* outline, long width, long height, unsigned char xf) {
    if (plain_on < 0) { const char* e = getenv("AOE_PLAIN"); plain_on = !(e && !strcmp(e, "0")); }
    if (!plain_on || height <= 0 || aoe_view_core()) return 0;
    unsigned check = data[0] * 31 + data[height / 2] * 7 + data[height - 1] + (unsigned) width * 65599 + (unsigned) height;
    Plain* old = &plains[0];
    for (int i = 0; i < PLAINS; i++) {
        Plain* p = &plains[i];
        if (p->data == data && p->check == check && p->width == width && p->height == height && p->xf == xf) { p->used = ++plain_clock; return p; }
        if (p->used < old->used) old = p;
    }
    free(old->pix); free(old->first); free(old->runs);
    memset(old, 0, sizeof *old);
    enum { ROOM = 64 };   // (runs a row at most: more and the row is drawn as before)
    old->pix = (unsigned char*) malloc(width * height);
    old->first = (int*) malloc((height + 1) * sizeof(int));
    PlainRun* tmp = (PlainRun*) malloc(height * ROOM * sizeof(PlainRun));
    if (!old->pix || !old->first || !tmp) { free(old->pix); free(old->first); free(tmp); memset(old, 0, sizeof *old); return 0; }
    long total = 0, kept_rows = 0;
    for (long r = 0; r < height; r++) {
        int k = plain_row(base + data[r], width, outline[r * 2], old->pix + r * width, tmp + total, ROOM, xf);
        old->first[r] = (int) total | (k < 0 ? PLAIN_NOT : 0);   // (a row drawn as before: marked, its runs none)
        if (k >= 0) { total += k; kept_rows++; }
    }
    old->first[height] = (int) total;
    old->runs = (PlainRun*) realloc(tmp, (total ? total : 1) * sizeof(PlainRun));
    old->data = data; old->width = width; old->height = height; old->check = check; old->used = ++plain_clock; old->xf = xf;
    if (getenv("AOE_DEBUG")) fprintf(stderr, "aoe: a picture of %ld x %ld kept, %ld rows of runs, %ld runs\n", width, height, kept_rows, total);
    return old;
}

// The rows of a sprite; with PLAIN, those a big picture keeps are copied (Plain): two copies of the loop, so that the drawing of
// every other sprite (the view's walk and raster) has no test of its own at each row
template <int PLAIN>
static inline void sprite_rows(AsmState* A, unsigned char* base, long x, long y, long width, long height, long skip, long right,
                               unsigned* data, unsigned short* outline, int fog, int mirror, const RowIndex* index, const Plain* plain) {
    unsigned m = 0xffffffff, v = 0;
    for (long row = 0; row < height; row++) {
        if (!(row & 15) && A == states && aoe_copy_asked.v == 1) aoe_view_copy_serve();   // (the view's scroll copy, made at once: aoe_view.cpp)
        long line = y + row;
        Span* span = (mirror ? span_tails : span_heads)[line];
        if (!span) continue;
        if (fog) { m = shadow_mask[line & 3]; v = shadow_value[line & 3]; }
        unsigned short lpad = outline[row * 2], rpad = outline[row * 2 + 1];
        if (mirror) { unsigned short t = lpad; lpad = rpad; rpad = t; if (rpad & 0x8000) continue; }
        else if (lpad & 0x8000) continue;
        long left = x + lpad, rt = right - rpad;
        unsigned char* line_base = surface_offsets[line];
        const unsigned char* s = base + data[row];
        // the first span that can show anything of this row
        if (!mirror) {
            while (span && span->x1 < left) span = span->next;
            if (!span || rt < span->x0) continue;
        } else {
            while (span && span->x0 > rt) span = span->prev;
            if (!span || left > span->x1) continue;
        }
        if (PLAIN && !(plain->first[row + skip] & PLAIN_NOT)) {   // (copies, fills and skips: its runs as kept)
            const unsigned char* px = plain->pix + (row + skip) * width - x;
            const PlainRun* r = plain->runs + plain->first[row + skip];
            const PlainRun* end = plain->runs + (plain->first[row + skip + 1] & ~PLAIN_NOT);
            Span* t = span;
            for (; r < end && t; r++) {
                long a = x + r->a, b = x + r->b;
                while (t && t->x1 < a) t = t->next;
                for (Span* u = t; u && u->x0 <= b; u = u->next) {
                    long p = a < u->x0 ? u->x0 : a, q = b > u->x1 ? u->x1 : b;
                    if (p <= q) memcpy(line_base + p, px + p, q - p + 1);
                    if (b <= u->x1) break;
                }
            }
            continue;
        }
        long at = mirror ? rt : left;  // the column the next command starts at
        if (index && span->x0 - left >= INDEX_STEP) {
            const RowMark* mark = index->marks + (row + skip) * index->columns + (span->x0 - left) / INDEX_STEP;
            s += mark->offset;
            at = left + mark->column;
        }
        for (;;) {
            unsigned c = *s++;
            long n;
            int op;
            const unsigned char* px = 0;
            unsigned char value = 0;
            switch (c & 15) {
                case 0: case 4: case 8: case 12: n = c >> 2; op = OP_COPY; px = s; s += n; break;
                case 1: case 5: case 9: case 13: at += mirror ? -(long) (c >> 2) : (long) (c >> 2); continue;
                case 2: n = (c & 0xf0) << 4 | *s++; op = OP_COPY; px = s; s += n; break;
                case 3: n = (c & 0xf0) << 4 | *s++; at += mirror ? -n : n; continue;
                case 6: n = c >> 4; if (!n) n = *s++; op = OP_XFORM; px = s; s += n; break;
                case 7: n = c >> 4; if (!n) n = *s++; op = OP_FILL; value = *s++; break;
                case 10: n = c >> 4; if (!n) n = *s++; op = OP_XFILL; value = *s++; break;
                case 11: n = c >> 4; if (!n) n = *s++; op = OP_SHADOW; break;
                case 14:
                    switch (c >> 4) {
                        case 1: at += mirror ? -1 : 1; continue;            // an outline, not drawn by this game
                        case 2: n = *s++; at += mirror ? -n : n; continue;
                        case 3: continue;
                        case 4: {                                            // the next command, passed over
                            unsigned e = *s++;
                            long k = 0;
                            switch (e & 15) {
                                case 0: case 4: case 8: case 12: k = e >> 2; s += k; break;
                                case 1: case 5: case 9: case 13: k = e >> 2; break;
                                case 2: k = (e & 0xf0) << 4 | *s++; s += k; break;
                                case 3: k = (e & 0xf0) << 4 | *s++; break;
                                case 6: k = e >> 4; if (!k) k = *s++; s += k; break;
                                case 7: case 10: k = e >> 4; if (!k) k = *s++; s++; break;
                                case 11: k = e >> 4; if (!k) k = *s++; break;
                                case 14:
                                    if ((e >> 4) == 1) k = 1;
                                    else if ((e >> 4) == 2) k = *s++;
                                    break;
                                default: break;
                            }
                            at += mirror ? k : k;
                            continue;
                        }
                        default: continue;
                    }
                default: goto next_row;  // 15: the row's end
            }
            if (!mirror) {
                long a = at, b = at + n - 1;
                at += n;
                while (span && span->x1 < a) span = span->next;
                if (!span) goto next_row;   // nothing more of this row shows
                for (Span* t = span; t && t->x0 <= b; t = t->next) {
                    long p = a < t->x0 ? t->x0 : a, q = b > t->x1 ? t->x1 : b;
                    if (p <= q) put_run(A, op, line_base, p, q, px, value, fog, m, v, 1, a);
                    if (b <= t->x1) break;
                }
            } else {
                long b = at, a = at - n + 1;
                at -= n;
                while (span && span->x0 > b) span = span->prev;
                for (Span* t = span; t && t->x1 >= a; t = t->prev) {
                    long p = a < t->x0 ? t->x0 : a, q = b > t->x1 ? t->x1 : b;
                    if (p <= q) put_run(A, op, line_base, p, q, px, value, fog, m, v, -1, b);
                    if (a >= t->x0) break;
                }
            }
        }
    next_row:;
    }
}

extern "C" void ASMDraw_Sprite(void* base_, long x, long y, long width, long height, long data_offsets, long outline_offset, int flags) { AS
    unsigned char* base = (unsigned char*) base_;
    long skip = 0;
    long last = height + y - 1;
    if (last < min_line || y > max_line) return;
    if (y < min_line) {
        skip = min_line - y;
        y = A >= states + 2 ? min_line : 0;   // (0 as the drawing always had it, where the first line is 0; a band of the raster's
                                               // begins lower: its first row at its first line)
        height -= skip;
    }
    if (last > max_line) height -= last - max_line;
    if (x > max_span) return;
    long right = x + width - 1;
    if (right < min_span) return;
    unsigned* data = (unsigned*) (base + data_offsets) + skip;
    unsigned short* outline = (unsigned short*) (base + outline_offset) + skip * 2;
    int fog = flags & 1, mirror = flags & 2;
    const RowIndex* index = A < states + 2 && !mirror && width >= INDEX_FROM && height + skip > 0 ? row_index(A, base, data - skip, width, height + (last > max_line ? last - max_line : 0) + skip) : 0;
    // (kept: the first core's big pictures, and since 6 November 2026 its sprites of 1,024 pixels or more, a player's colours with them)
    const Plain* plain = !fog && !mirror && A == states && (index || width * (height + skip) >= 1024)
                             ? plain_get(base, data - skip, outline - skip * 2, width, height + (last > max_line ? last - max_line : 0) + skip,
                                         (unsigned char) color_xform)
                             : 0;

    if (plain) sprite_rows<1>(A, base, x, y, width, height, skip, right, data, outline, fog, mirror, index, plain);
    else sprite_rows<0>(A, base, x, y, width, height, skip, right, data, outline, fog, mirror, index, 0);
}

// -----------------------------------------------------------------------------------------------
// Inf32: a whole stream at the first call (it is one file's, read whole by rge_fio.cpp), then 64 KB a call.

struct Inf { const unsigned char* in; const unsigned char* end; unsigned bits; int nbits; unsigned char* out; long len, cap; int fixed, bad; };

static int inf_bit(Inf* z, int n) {
    while (z->nbits < n) {
        z->bits |= (unsigned) (z->in < z->end ? *z->in++ : 0) << z->nbits;
        z->nbits += 8;
    }
    int v = z->bits & ((1u << n) - 1);
    z->bits >>= n;
    z->nbits -= n;
    return v;
}

struct Huff { unsigned short count[16], symbol[320]; };

static void huff_make(Huff* h, const unsigned char* lens, int n) {
    unsigned short offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++) if (lens[i]) h->symbol[offs[lens[i]]++] = i;
}

static inline int huff_get(Inf* z, const Huff* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= inf_bit(z, 1);
        int c = h->count[len];
        if (code - c < first) return h->symbol[index + (code - first)];
        index += c;
        first = (first + c) << 1;
        code <<= 1;
    }
    return -1;
}

// (a fixed buffer on a worker, which may not allocate: one too small leaves the stream to the first core, as before)
static int inf_room(Inf* z, long n) {
    if (z->len + n > z->cap) {
        if (z->fixed) { z->bad = 1; return 0; }
        z->cap = (z->len + n) * 2;
        z->out = (unsigned char*) realloc(z->out, z->cap);
    }
    return 1;
}

static void inflate_all(Inf* z) {
    static const unsigned short lbase[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const unsigned char lext[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const unsigned short dbase[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const unsigned char dext[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    static const unsigned char order[] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    Huff hl, hd;   // (on the stack: the stream ahead is inflated on a worker, docs/games/aoe.md "The load")
    int final;
    do {
        final = inf_bit(z, 1);
        int type = inf_bit(z, 2);
        if (type == 0) {
            z->bits = 0; z->nbits = 0;
            int n = z->in[0] | z->in[1] << 8;
            z->in += 4;
            if (!inf_room(z, n)) return;
            memcpy(z->out + z->len, z->in, n);
            z->in += n; z->len += n;
            continue;
        }
        unsigned char lens[320];
        if (type == 1) {
            int i = 0;
            for (; i < 144; i++) lens[i] = 8;
            for (; i < 256; i++) lens[i] = 9;
            for (; i < 280; i++) lens[i] = 7;
            for (; i < 288; i++) lens[i] = 8;
            huff_make(&hl, lens, 288);
            for (i = 0; i < 30; i++) lens[i] = 5;
            huff_make(&hd, lens, 30);
        } else if (type == 2) {
            int nl = inf_bit(z, 5) + 257, nd = inf_bit(z, 5) + 1, nc = inf_bit(z, 4) + 4, i;
            memset(lens, 0, 19);
            for (i = 0; i < nc; i++) lens[order[i]] = inf_bit(z, 3);
            huff_make(&hl, lens, 19);
            for (i = 0; i < nl + nd;) {
                int sym = huff_get(z, &hl);
                if (sym < 16) { lens[i++] = sym; continue; }
                int prev = 0, rep;
                if (sym == 16) { prev = lens[i - 1]; rep = 3 + inf_bit(z, 2); }
                else if (sym == 17) rep = 3 + inf_bit(z, 3);
                else rep = 11 + inf_bit(z, 7);
                while (rep--) lens[i++] = prev;
            }
            huff_make(&hd, lens + nl, nd);
            huff_make(&hl, lens, nl);
        } else return;
        for (;;) {
            int sym = huff_get(z, &hl);
            if (sym < 0) return;
            if (sym < 256) { if (!inf_room(z, 1)) return; z->out[z->len++] = sym; continue; }
            if (sym == 256) break;
            sym -= 257;
            int n = lbase[sym] + inf_bit(z, lext[sym]);
            int ds = huff_get(z, &hd);
            int dist = dbase[ds] + inf_bit(z, dext[ds]);
            if (!inf_room(z, n)) return;
            unsigned char* o = z->out + z->len;
            for (int i = 0; i < n; i++) o[i] = o[i - dist];
            z->len += n;
        }
    } while (!final);
}

static unsigned char* inf_data;
static long inf_len, inf_at;

// The game's data inflated ahead (docs/games/aoe.md, "The load"): Empires.dat is read and inflated at Start Game, 22 million
// instructions on the first core. At the main menu the first core reads the file and a worker inflates it into a buffer of its own;
// the first stream that is the same bytes takes that buffer (waiting for the job if it is not done), any other stream is inflated
// as before. AOE_AHEAD=0 is the way before.
extern "C" { int mcw_wait(int k); void mcw_post(int k, void (*fn)(uint32_t, uint32_t), uint32_t a0, uint32_t a1); void mcw_touch(void*, unsigned); }
static struct { unsigned char* in; long in_len; unsigned char* out; long cap, len; int bad, worker; } ahead __attribute__((aligned(16)));
static void ahead_job(uint32_t, uint32_t) {
    Inf z;
    memset(&z, 0, sizeof z);
    z.in = ahead.in; z.end = ahead.in + ahead.in_len;
    z.out = ahead.out; z.cap = ahead.cap; z.fixed = 1;
    inflate_all(&z);
    ahead.len = z.len;
    ahead.bad = z.bad || z.in > z.end;
}
void aoe_inflate_ahead(const char* path, int worker) {
    const char* e = getenv("AOE_AHEAD");
    if (ahead.worker || worker <= 0 || (e && !strcmp(e, "0"))) return;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* in = n > 0 ? (unsigned char*) malloc(n) : 0;
    long cap = n * 16 + 65536;   // (deflate's data ratio: the trial's 142,050 bytes are 758,114)
    unsigned char* out = in ? (unsigned char*) malloc(cap) : 0;
    if (!out || fread(in, 1, n, f) != (size_t) n) { fclose(f); free(in); free(out); return; }
    fclose(f);
    mcw_touch(out, cap);   // (stored to by the first core first: the worker never waits for a page)
    ahead.in = in; ahead.in_len = n; ahead.out = out; ahead.cap = cap; ahead.len = 0; ahead.bad = 0; ahead.worker = worker;
    mcw_post(worker, ahead_job, 0, 0);
}
static int ahead_take(const void* compressed, int size) {
    if (!ahead.worker || !ahead.in || ahead.in_len != size || memcmp(ahead.in, compressed, size)) return 0;
    mcw_wait(ahead.worker);
    free(ahead.in); ahead.in = 0;
    if (ahead.bad) { free(ahead.out); ahead.out = 0; return 0; }
    if (getenv("AOE_AHEAD") && !strcmp(getenv("AOE_AHEAD"), "check")) {   // (inflated here too, and compared)
        Inf z;
        memset(&z, 0, sizeof z);
        z.in = (const unsigned char*) compressed; z.end = z.in + size;
        inflate_all(&z);
        fprintf(stderr, "aoe: inflated ahead %ld bytes, here %ld: %s\n", ahead.len, z.len,
                z.len == ahead.len && !memcmp(z.out, ahead.out, z.len) ? "the same" : "DIFFER");
        free(z.out);
    }
    free(inf_data);
    inf_data = ahead.out; inf_len = ahead.len; inf_at = 0;
    ahead.out = 0;
    if (getenv("AOE_DEBUG")) fprintf(stderr, "aoe: %ld bytes inflated ahead taken\n", inf_len);
    return 1;
}

extern "C" int Inf32BufSize(void) { return 16; }
extern "C" int Inf32Decode(void* compressed, int offset, void* file_size, void* buffer, int zero, void* buffer_size, void* state, int one) {
    int* in_size = (int*) file_size;
    int* out_size = (int*) buffer_size;
    int* started = (int*) state;   // cleared by the file's opening
    if (!*started) {
        *started = 1;
        if (ahead_take(compressed, *in_size)) goto inflated;
        Inf z;
        memset(&z, 0, sizeof z);
        z.in = (const unsigned char*) compressed;
        z.end = z.in + *in_size;
        free(inf_data);
        inflate_all(&z);
        inf_data = z.out;
        inf_len = z.len;
        inf_at = 0;
    }
inflated:
    long n = inf_len - inf_at;
    if (n > *out_size) n = *out_size;
    memcpy(buffer, inf_data + inf_at, n);
    if (n < *out_size) memset((char*) buffer + n, 0, *out_size - n);
    inf_at += n;
    *in_size = 0;
    return 0;
}

// -----------------------------------------------------------------------------------------------
// Drawarea_asm.asm

void TDrawArea::PtrClear(tagRECT* rect, int color) {
    long x0 = 0, y0 = 0, x1 = this->Width, y1 = this->Height;
    if (rect) { x0 = rect->left; y0 = rect->top; x1 = rect->right; y1 = rect->bottom; }
    if (x1 < 0 || x0 > x1 || x0 >= this->Width || y1 < 0 || y0 > y1 || y0 >= this->Height) return;
    if (x0 < 0) x0 = 0;
    if (x1 >= this->Width) x1 = this->Width - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= this->Height) y1 = this->Height - 1;
    int locked = this->DrawSystem && this->DrawSystem->DrawType == 2;
    if (locked && !this->Lock((char*) "PtrClear", 1)) return;
    unsigned char** rows = (unsigned char**) this->CurDisplayOffsets;
    for (long y = y0; y <= y1; y++) memset(rows[y] + x0, color, x1 - x0 + 1);
    if (locked) this->Unlock((char*) "PtrClear");
}

// the receiver's pixels into `dest`, along the spans of `lines` (the source's rows)
void TDrawArea::PtrSpanCopy(TDrawArea* dest, long dx, long dy, tagRECT* r, unsigned char** lines) {
    if (r->left < 0 || r->left > r->right || r->right >= this->Width) return;
    if (r->top < 0 || r->top > r->bottom || r->bottom >= this->Height) return;
    long h = r->bottom - r->top + 1;
    if (dx < 0 || dx >= dest->Width || dy < 0 || dy >= dest->Height) return;
    if (dy + h > dest->Height) h = dest->Height - dy;
    if (this->DrawSystem && this->DrawSystem->DrawType == 2 && !dest->Lock((char*) "PtrSpanCopy", 1)) return;
    unsigned char** from = (unsigned char**) this->CurDisplayOffsets;
    unsigned char** to = (unsigned char**) dest->CurDisplayOffsets;
    Span** spans = (Span**) lines;
    for (long i = 0; i < h; i++) {
        long sy = r->top + i;
        unsigned char* s = from[sy] + r->left;
        unsigned char* d = to[dy + i] + dx;
        for (Span* n = spans[sy]; n; n = n->next) memcpy(d + n->x0, s + n->x0, n->x1 - n->x0 + 1);
    }
}

void TDrawArea::OverlayMemCopy(tagRECT& a, tagRECT& b, int dx, int dy, int xo, int yo) {
    if (!dx && !dy) return;
    int locked = this->DrawSystem && this->DrawSystem->DrawType == 2;
    if (locked && !this->Lock((char*) "OverlayMemCopy", 1)) return;
    long h = a.bottom - a.top + 1, words = (a.right - a.left + 4) >> 2;
    long sx = a.left + xo, tx = b.left + xo, sy, ty, step = 1;
    if (dy < 0) { step = -1; sy = a.bottom + yo; ty = b.bottom + yo; }
    else { sy = a.top + yo; ty = b.top + yo; }
    unsigned char** rows = (unsigned char**) this->CurDisplayOffsets;
    // The port: a scroll moves most of the view, 200 KB a frame, 106 thousand instructions of the view's worker by words. Rows one
    // after the other in memory are one block, which the machine's copy moves in a pass or two (CSRs 0x8b0 to 0x8b3; docs/machine/gpu.md,
    // "A program's copy": a destination inside its source is copied from its end, as memmove). The columns between the rows' pieces
    // get the neighbouring rows' pixels, where the old picture's were: both are the new strip, drawn again. The view keeps a scroll on
    // 16 pixels across for it (view.cpp). AOE_SCROLL_COPY=words is the way before (aoe_scroll_way).
    int by_words = aoe_scroll_way.words;
    long first_s = dy < 0 ? sy - (h - 1) : sy, first_t = dy < 0 ? ty - (h - 1) : ty;
    unsigned char* src = rows[first_s] + sx;
    unsigned char* dst = rows[first_t] + tx;
    long pitch = rows[1] - rows[0], bytes = (h - 1) * pitch + words * 4;
    int whole = !by_words && h > 1 && pitch > 0;
    for (long i = 1; whole && i < h; i++) whole = rows[first_s + i] == rows[first_s] + i * pitch && rows[first_t + i] == rows[first_t] + i * pitch;
    // Only the first core asks for the machine's copy (CSR 0x8b0 reads 1 there and 0 on a worker, docs/machine/gpu.md, "A program's
    // copy"): the view's job, on a worker, has the first core make it, which waits for the walk meanwhile (aoe_view_first_copy).
    if (whole && !(((uintptr_t) dst | (uintptr_t) src | (uintptr_t) bytes) & 15) && aoe_view_first_copy(src, dst, bytes)) {
    } else
    for (; h > 0; h--, sy += step, ty += step) memmove(rows[ty] + tx, rows[sy] + sx, words * 4);
    if (locked) this->Unlock((char*) "OverlayMemCopy");
}
