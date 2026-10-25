/*
 * q3_names, q3_sort: the string and sorting work of a game's load and of its renderer's every frame.
 * q3_names follows ioq3's renderer and file system on a level's load: generateHashValue (tr_shader.c, copied)
 * for shader names into a table of 1,024 chains, a lookup by Q_stricmp for each name asked (found, or added),
 * the names first made with snprintf and copied with a bounded copy (Com_sprintf, Q_strncpyz); qsort of the
 * names at the end (FS_SortFileList). 600 names a unit, a third asked twice.
 * q3_sort: R_SortDrawSurfs (tr_main.c, copied): R_RadixSort, four passes of R_Radix over 4,000 draw surfaces'
 * sort keys, then the walk that cuts them into runs of one shader (R_DecomposeSort's shifts).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wl.h"

#define NAMES 600
#define HASH 1024
#define SURFS 4000

/* ---- q3_names ---- */
struct entry { char name[64]; int next, used; };
struct names { struct entry entries[NAMES]; int heads[HASH], count; char line[96]; const char *sorted[NAMES]; };
static const char *const dirs[] = {"textures/base_wall/", "textures/gothic_block/", "models/players/sarge/", "sound/weapons/rocket/",
                                   "textures/sfx/", "models/mapobjects/", "gfx/2d/", "textures/base_floor/"};
static const char *const words[] = {"metal", "trim", "light", "blood", "pipe", "grate", "flame", "skin", "head", "upper", "lower",
                                    "rocket", "shell", "panel", "stone", "brick"};
static uint32_t name_seed[NAMES];

static long generateHashValue(const char *fname, const int size)
{
    int i = 0;
    long hash = 0;
    char letter;
    while (fname[i] != '\0') {
        letter = (char)tolower((unsigned char)fname[i]);
        if (letter == '.') break;
        if (letter == '\\') letter = '/';
        hash += (long)(letter) * (i + 119);
        i++;
    }
    hash = (hash ^ (hash >> 10) ^ (hash >> 20));
    return hash & (size - 1);
}

static int Q_stricmp(const char *s1, const char *s2)
{
    int c1, c2;
    do {
        c1 = *s1++, c2 = *s2++;
        if (c1 != c2) {
            if (c1 >= 'a' && c1 <= 'z') c1 -= ('a' - 'A');
            if (c2 >= 'a' && c2 <= 'z') c2 -= ('a' - 'A');
            if (c1 != c2) return c1 < c2 ? -1 : 1;
        }
    } while (c1);
    return 0;
}

static void Q_strncpyz(char *dest, const char *src, int destsize)
{
    strncpy(dest, src, (size_t)destsize - 1);
    dest[destsize - 1] = 0;
}

static int by_name(const void *a, const void *b) { return Q_stricmp(*(const char *const *)a, *(const char *const *)b); }

static void setup_names(void)
{
    uint32_t seed = 0x5a5e;
    for (int i = 0; i < NAMES; i++) name_seed[i] = (i % 3 == 2) ? name_seed[(i * 7) % (i - 1)] : wl_rand(&seed);
}

static void *inst_names(void) { return wl_alloc(sizeof(struct names)); }

static uint32_t run_names(void *ctx)
{
    struct names *s = ctx;
    uint32_t sum = 0;
    s->count = 0;
    for (int h = 0; h < HASH; h++) s->heads[h] = -1;
    for (int i = 0; i < NAMES; i++) {
        uint32_t r = name_seed[i];
        snprintf(s->line, sizeof s->line, "%s%s_%s%d.tga", dirs[r & 7], words[r >> 3 & 15], words[r >> 7 & 15], (int)(r >> 11 & 63));
        long hash = generateHashValue(s->line, HASH);
        int at = s->heads[hash];
        while (at >= 0 && Q_stricmp(s->entries[at].name, s->line)) at = s->entries[at].next;
        if (at < 0) {
            at = s->count++;
            Q_strncpyz(s->entries[at].name, s->line, sizeof s->entries[at].name);
            s->entries[at].next = s->heads[hash], s->entries[at].used = 0;
            s->heads[hash] = at;
        }
        s->entries[at].used++;
        sum += (uint32_t)hash;
    }
    for (int i = 0; i < s->count; i++) s->sorted[i] = s->entries[i].name;
    qsort(s->sorted, (size_t)s->count, sizeof s->sorted[0], by_name);
    for (int i = 0; i < s->count; i += 17) sum = sum * 31 + (uint32_t)(unsigned char)s->sorted[i][20] + (uint32_t)s->count;
    return sum;
}

/* ---- q3_sort ---- */
typedef struct { unsigned sort; void *surface; } drawSurf_t;
#define QSORT_FOGNUM_SHIFT 2
#define QSORT_REFENTITYNUM_SHIFT 7
#define QSORT_SHADERNUM_SHIFT (QSORT_REFENTITYNUM_SHIFT + 10)
struct surfs { drawSurf_t list[SURFS], scratch[SURFS]; };
static drawSurf_t surfs_made[SURFS];

static void R_Radix(int offset, int size, drawSurf_t *source, drawSurf_t *dest)
{
    int count[256] = {0}, index[256], i;
    unsigned char *sortKey = ((unsigned char *)&source[0].sort) + offset, *end = sortKey + (size * sizeof(drawSurf_t));
    for (; sortKey < end; sortKey += sizeof(drawSurf_t)) ++count[*sortKey];
    index[0] = 0;
    for (i = 1; i < 256; ++i) index[i] = index[i - 1] + count[i - 1];
    sortKey = ((unsigned char *)&source[0].sort) + offset;
    for (i = 0; i < size; ++i, sortKey += sizeof(drawSurf_t)) dest[index[*sortKey]++] = source[i];
}

static void setup_sort(void)
{
    uint32_t seed = 0x5047;
    for (int i = 0; i < SURFS; i++) {   /* shaders by area of the map, a few entities, some fogged */
        unsigned shader = (unsigned)(i / 40 + wl_rand(&seed) % 25) & 0x3fff, entity = wl_rand(&seed) % 8 ? 1023 : wl_rand(&seed) % 64;
        surfs_made[i].sort = shader << QSORT_SHADERNUM_SHIFT | entity << QSORT_REFENTITYNUM_SHIFT | (wl_rand(&seed) % 9 ? 0 : 1) << QSORT_FOGNUM_SHIFT;
        surfs_made[i].surface = (void *)(uintptr_t)(i * 16);
    }
}

static void *inst_sort(void) { return wl_alloc(sizeof(struct surfs)); }

static uint32_t run_sort(void *ctx)
{
    struct surfs *s = ctx;
    uint32_t sum = 0, runs = 0, last = ~0u;
    memcpy(s->list, surfs_made, sizeof s->list);
    R_Radix(0, SURFS, s->list, s->scratch);
    R_Radix(1, SURFS, s->scratch, s->list);
    R_Radix(2, SURFS, s->list, s->scratch);
    R_Radix(3, SURFS, s->scratch, s->list);
    for (int i = 0; i < SURFS; i++) {   /* R_AddDrawSurfCmd's walk: a run per shader and entity */
        unsigned shader = s->list[i].sort >> QSORT_SHADERNUM_SHIFT, entity = (s->list[i].sort >> QSORT_REFENTITYNUM_SHIFT) & 1023;
        if ((shader << 10 | entity) != last) runs++, last = shader << 10 | entity;
        sum += (uint32_t)(uintptr_t)s->list[i].surface * (uint32_t)(i & 7);
    }
    return sum ^ runs << 20;
}

const wl_kernel wl_q3_names = {"q3_names", "Quake III load: generateHashValue, Q_stricmp lookups, snprintf, qsort of names", setup_names, inst_names, run_names};
const wl_kernel wl_q3_sort = {"q3_sort", "Quake III renderer: R_RadixSort of 4,000 draw surfaces and their runs", setup_sort, inst_sort, run_sort};
