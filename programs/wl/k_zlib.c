/*
 * gzip_match: compressing, as gzip, unarc's writers and PNG saving do. Copied from zlib's deflate.c (the
 * kernel's copy, lib/zlib_deflate): INSERT_STRING, longest_match (its byte-by-byte path) and deflate_slow's lazy
 * matching at level 6 (good 8, lazy 16, nice 128, chain 128); zlib_tr_tally counts the symbols' frequencies as
 * trees.c does, the Huffman trees and the bits not made. The input: 16 KB of text-like words and binary-like
 * runs in a 64 KB window. A unit: that window searched whole.
 */
#include <string.h>
#include "wl.h"

#define MIN_MATCH 3
#define MAX_MATCH 258
#define WSIZE 32768
#define W_MASK (WSIZE - 1)
#define HASH_BITS 15
#define HASH_SIZE (1 << HASH_BITS)
#define HASH_MASK (HASH_SIZE - 1)
#define HASH_SHIFT ((HASH_BITS + MIN_MATCH - 1) / MIN_MATCH)
#define MIN_LOOKAHEAD (MAX_MATCH + MIN_MATCH + 1)
#define MAX_DIST (WSIZE - MIN_LOOKAHEAD)
#define NIL 0
#define TOO_FAR 4096
#define INPUT 16384

typedef unsigned IPos;
typedef unsigned short Pos;
typedef struct {
    unsigned char window[2 * WSIZE];
    Pos prev[WSIZE], head[HASH_SIZE];
    unsigned ins_h, strstart, lookahead, match_start, match_length, prev_length, prev_match, match_available;
    unsigned max_chain_length, max_lazy_match, good_match;
    int nice_match;
    unsigned short dyn_ltree[2 * 286], dyn_dtree[2 * 30];
    unsigned last_lit;
} deflate_state;

static unsigned char input[INPUT];

#define UPDATE_HASH(s, h, c) (h = (((h) << HASH_SHIFT) ^ (c)) & HASH_MASK)
#define INSERT_STRING(s, str, match_head) \
    (UPDATE_HASH(s, s->ins_h, s->window[(str) + (MIN_MATCH - 1)]), s->prev[(str) & W_MASK] = (Pos)(match_head = s->head[s->ins_h]), \
     s->head[s->ins_h] = (Pos)(str))

static int d_code(unsigned dist) { int c = 0; while ((1u << (c / 2 + 1)) <= dist) c++; return c; }   /* (trees.c's table, worked out) */

static int zlib_tr_tally(deflate_state *s, unsigned dist, unsigned lc)
{
    s->last_lit++;
    if (dist == 0) s->dyn_ltree[2 * lc]++;
    else {
        dist--;
        s->dyn_ltree[2 * (lc + 257 > 285 ? 285 : lc / 8 + 257)]++;
        s->dyn_dtree[2 * d_code(dist)]++;
    }
    return 0;
}

static unsigned longest_match(deflate_state *s, IPos cur_match)
{
    unsigned chain_length = s->max_chain_length;
    unsigned char *scan = s->window + s->strstart, *match;
    int len, best_len = (int)s->prev_length, nice_match = s->nice_match;
    IPos limit = s->strstart > (IPos)MAX_DIST ? s->strstart - (IPos)MAX_DIST : NIL;
    Pos *prev = s->prev;
    unsigned char *strend = s->window + s->strstart + MAX_MATCH;
    unsigned char scan_end1 = scan[best_len - 1], scan_end = scan[best_len];
    if (s->prev_length >= s->good_match) chain_length >>= 2;
    if ((unsigned)nice_match > s->lookahead) nice_match = (int)s->lookahead;
    do {
        match = s->window + cur_match;
        if (match[best_len] != scan_end || match[best_len - 1] != scan_end1 || *match != *scan || *++match != scan[1]) continue;
        scan += 2, match++;
        do {
        } while (*++scan == *++match && *++scan == *++match && *++scan == *++match && *++scan == *++match && *++scan == *++match &&
                 *++scan == *++match && *++scan == *++match && *++scan == *++match && scan < strend);
        len = MAX_MATCH - (int)(strend - scan);
        scan = strend - MAX_MATCH;
        if (len > best_len) {
            s->match_start = cur_match;
            best_len = len;
            if (len >= nice_match) break;
            scan_end1 = scan[best_len - 1];
            scan_end = scan[best_len];
        }
    } while ((cur_match = prev[cur_match & W_MASK]) > limit && --chain_length != 0);
    if ((unsigned)best_len <= s->lookahead) return (unsigned)best_len;
    return s->lookahead;
}

static void deflate_slow(deflate_state *s)
{
    IPos hash_head = NIL;
    for (;;) {
        if (s->lookahead == 0) break;
        if (s->lookahead >= MIN_MATCH) INSERT_STRING(s, s->strstart, hash_head);
        s->prev_length = s->match_length, s->prev_match = s->match_start;
        s->match_length = MIN_MATCH - 1;
        if (hash_head != NIL && s->prev_length < s->max_lazy_match && s->strstart - hash_head <= MAX_DIST) {
            s->match_length = longest_match(s, hash_head);
            if (s->match_length == MIN_MATCH && s->strstart - s->match_start > TOO_FAR) s->match_length = MIN_MATCH - 1;
        }
        if (s->prev_length >= MIN_MATCH && s->match_length <= s->prev_length) {
            unsigned max_insert = s->strstart + s->lookahead - MIN_MATCH;
            zlib_tr_tally(s, s->strstart - 1 - s->prev_match, s->prev_length - MIN_MATCH);
            s->lookahead -= s->prev_length - 1;
            s->prev_length -= 2;
            do {
                if (++s->strstart <= max_insert) INSERT_STRING(s, s->strstart, hash_head);
            } while (--s->prev_length != 0);
            s->match_available = 0;
            s->match_length = MIN_MATCH - 1;
            s->strstart++;
        } else if (s->match_available) {
            zlib_tr_tally(s, 0, s->window[s->strstart - 1]);
            s->strstart++;
            s->lookahead--;
        } else {
            s->match_available = 1;
            s->strstart++;
            s->lookahead--;
        }
    }
    if (s->match_available) zlib_tr_tally(s, 0, s->window[s->strstart - 1]), s->match_available = 0;
}

static void setup(void)
{
    static const char *const words[] = {"the ", "machine ", "shader ", "texel ", "pass ", "core ", "worker ", "cache ", "store ", "load ",
                                        "bundle ", "of ", "and ", "a ", "in ", "is ", "frame ", "game ", "quake ", "doom "};
    uint32_t seed = 0x6219;
    unsigned at = 0;
    while (at < INPUT) {
        if (wl_rand(&seed) % 6) {   /* text */
            const char *w = words[wl_rand(&seed) % 20];
            while (*w && at < INPUT) input[at++] = (unsigned char)*w++;
        } else {   /* binary: a run of small numbers, some repeating */
            unsigned n = 8 + wl_rand(&seed) % 40, base = wl_rand(&seed) & 0xff;
            for (unsigned k = 0; k < n && at < INPUT; k++) input[at++] = (unsigned char)(base + (k & 3) * (wl_rand(&seed) % 3));
        }
    }
}

static void *inst(void) { return wl_alloc(sizeof(deflate_state)); }

static uint32_t run(void *ctx)
{
    deflate_state *s = ctx;
    uint32_t sum = 0;
    memset(s->head, 0, sizeof s->head);
    memset(s->dyn_ltree, 0, sizeof s->dyn_ltree);
    memset(s->dyn_dtree, 0, sizeof s->dyn_dtree);
    memcpy(s->window, input, INPUT);
    memset(s->window + INPUT, 0, sizeof s->window - INPUT);
    s->strstart = 0, s->lookahead = INPUT, s->match_start = 0, s->match_available = 0, s->last_lit = 0;
    s->match_length = s->prev_length = MIN_MATCH - 1;
    s->max_chain_length = 128, s->max_lazy_match = 16, s->good_match = 8, s->nice_match = 128;
    s->ins_h = s->window[0];
    UPDATE_HASH(s, s->ins_h, s->window[1]);
    deflate_slow(s);
    for (int i = 0; i < 2 * 286; i += 2) sum = sum * 31 + s->dyn_ltree[i];
    for (int i = 0; i < 2 * 30; i += 2) sum = sum * 31 + s->dyn_dtree[i];
    return sum + s->last_lit;
}

const wl_kernel wl_gzip_match = {"gzip_match", "zlib deflate: deflate_slow's lazy matching and longest_match on 16 KB", setup, inst, run};
