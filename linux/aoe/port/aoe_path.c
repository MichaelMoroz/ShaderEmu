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

extern double g_path_distance_scale;
enum { N = 2 * 256 * 256 + 1 };
static unsigned short table[N];
static int made, check;
unsigned aoe_path_calls, aoe_path_wrong;

static int by_doubles(int d2) { return (int) (sqrt((double) d2) * g_path_distance_scale); }

static void make(void) {
    const char* e = getenv("AOE_PATH");   /* AOE_PATH=doubles: the way before */
    made = g_path_distance_scale == 16.0 && !(e && !strcmp(e, "doubles")) ? 1 : -1;
    check = getenv("AOE_PATH_CHECK") != 0;
    unsigned n = 0;
    for (unsigned d2 = 0; d2 < N; d2++) {
        unsigned m = 256 * d2;
        while ((n + 1) * (n + 1) <= m) n++;
        table[d2] = (unsigned short) n;
    }
}

int aoe_path_dist(int d2) {
    if (!made) make();
    if (made < 0 || (unsigned) d2 >= N) return by_doubles(d2);
    int r = table[d2];
    if (check) {
        aoe_path_calls++;
        if (r != by_doubles(d2)) {
            if (!aoe_path_wrong++) fprintf(stderr, "aoe: a path's distance differs: %d gives %d, the doubles %d\n", d2, r, by_doubles(d2));
        }
    }
    return r;
}
