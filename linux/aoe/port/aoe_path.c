/* The path finder's distances without doubles (docs/games/aoe.md, "Paths without doubles"): PathingSystem::findTilePath took
 * (int) (sqrt((double) d2) * 16.0) of a whole number of squared tiles at every step of its search, and that and its soft doubles
 * were two thirds of the first core with a dozen villagers walking. It is floor(sqrt(256 * d2)), the same number: 16 times a
 * correctly rounded root is exact, and a root that is not whole is farther from a whole number than any rounding. So a table up
 * to the largest map's diagonal, made once; past it, or with another scale, the doubles as before. AOE_PATH_CHECK=1 makes both
 * at every call and counts those that differ (aoephase: at a mark's end); AOE_PATH=doubles is the way before. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern double g_path_distance_scale, g_path_zero_terrain_value;
enum { N = 2 * 256 * 256 + 1 };
unsigned short aoe_path_table[N];
int aoe_path_made;   /* 1: the table (aoe_cores.h's aoe_path_dist reads it), -1: the doubles, 0: not yet */
static int check;
unsigned aoe_path_calls, aoe_path_wrong;

static int by_doubles(int d2) { return (int) (sqrt((double) d2) * g_path_distance_scale); }

static void make(void) {
    const char* e = getenv("AOE_PATH");   /* AOE_PATH=doubles: the way before */
    check = getenv("AOE_PATH_CHECK") != 0;
    /* (findTilePath and passable compare a terrain's cost with 0.0f, which is g_path_zero_terrain_value as a float) */
    if (g_path_zero_terrain_value != 0.0) fprintf(stderr, "aoe: g_path_zero_terrain_value is not 0: the path finder's floats are wrong\n");
    unsigned n = 0;
    for (unsigned d2 = 0; d2 < N; d2++) {
        unsigned m = 256 * d2;
        while ((n + 1) * (n + 1) <= m) n++;
        aoe_path_table[d2] = (unsigned short) n;
    }
    aoe_path_made = g_path_distance_scale == 16.0 && !(e && !strcmp(e, "doubles")) ? 1 : -1;
    if (check && aoe_path_made > 0) aoe_path_made = 2;   /* every call comes here */
}

/* (the first call, a check's, the doubles' and one past the table come here) */
int aoe_path_dist_slow(int d2) {
    if (!aoe_path_made) make();
    if (aoe_path_made < 0 || (unsigned) d2 >= N) return by_doubles(d2);
    int r = aoe_path_table[d2];
    if (check) {
        aoe_path_calls++;
        if (r != by_doubles(d2)) {
            if (!aoe_path_wrong++) fprintf(stderr, "aoe: a path's distance differs: %d gives %d, the doubles %d\n", d2, r, by_doubles(d2));
        }
    }
    return r;
}

/* The random map's roots without doubles (docs/games/aoe.md, "The load"). (long) sqrt((double) n) of a whole n is floor(sqrt(n)),
 * as above: a whole root found from the card's estimate (fsqrt.s is not correctly rounded there) and set right in whole numbers,
 * so the same number whatever the estimate. Callers pass dx * dx + dy * dy of map tiles. */
long aoe_isqrt_more(long n) {   /* (aoe_prefix.h's aoe_isqrt: the table inline once it is made) */
    if (n <= 0) return n < 0 ? (long) sqrt((double) n) : 0;
    if (n < N) {   /* (the path's table is floor(16 sqrt(n)): its sixteenth is floor(sqrt(n))) */
        if (!aoe_path_made) make();
        return aoe_path_table[n] >> 4;
    }
    unsigned u = (unsigned) n, s = (unsigned) __builtin_sqrtf((float) n);
    if (s > 65535) s = 65535;
    while (s && s * s > u) s--;
    while ((s + 1) * (s + 1) <= u && s < 65535) s++;
    return (long) s;
}
/* (float) sqrt((double) n) of a whole n: the same bits from a table filled at each value's first use by the doubles (find_path's
 * cost to the goal at every tile it takes); past the table, the doubles. */
float aoe_sqrt_whole(long n) {
    static float* table;
    if (n <= 0 || n >= N) return (float) sqrt((double) n);
    if (!table && !(table = (float*) calloc(N, sizeof(float)))) return (float) sqrt((double) n);
    float f = table[n];
    if (f == 0.0f) table[n] = f = (float) sqrt((double) n);
    return f;
}
