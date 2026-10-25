/*
 * q1_hull: Quake's server moving things through the world's clip hull (SV_Move for every walk, fall, missile
 * and SV_CheckWater). Copied from WinQuake's world.c: SV_HullPointContents and SV_RecursiveHullCheck as they
 * are. The hull is made here: a tree of planes (axial, every sixth oblique) cut to 11 levels over a 4096-unit
 * world, a leaf solid where its middle lies in one of 300 boxes, water under a height, empty elsewhere.
 * A unit: 600 moves of 8 to 64 units (the player hull's walks), 150 long traces, 1,000 point contents tests.
 */
#include <string.h>
#include "wl.h"

#define CONTENTS_EMPTY -1
#define CONTENTS_SOLID -2
#define CONTENTS_WATER -3
#define DIST_EPSILON (0.03125f)
#define DEPTH 11
#define MAXNODES (1 << DEPTH)
#define BOXES 300
#define MOVES 600
#define LONGS 150
#define POINTS 1000

typedef float vec3_t[3];
typedef struct { vec3_t normal; float dist; int type; } mplane_t;
typedef struct { int planenum; short children[2]; } dclipnode_t;
typedef struct { dclipnode_t *clipnodes; mplane_t *planes; int firstclipnode, lastclipnode; } hull_t;
typedef struct { vec3_t normal; float dist; } plane_t;
typedef struct { int allsolid, startsolid, inopen, inwater; float fraction; vec3_t endpos; plane_t plane; } trace_t;

static dclipnode_t clipnodes[MAXNODES];
static mplane_t planes[MAXNODES];
static hull_t hull;
static float boxes[BOXES][2][3];
static vec3_t moves[MOVES + LONGS][2], points[POINTS];
static int nodes_used;

#define DotProduct(a, b) ((a)[0] * (b)[0] + (a)[1] * (b)[1] + (a)[2] * (b)[2])
static const vec3_t vec3_origin = {0, 0, 0};

static int SV_HullPointContents(hull_t *h, int num, const vec3_t p)
{
    float d;
    dclipnode_t *node;
    mplane_t *plane;
    while (num >= 0) {
        node = h->clipnodes + num;
        plane = h->planes + node->planenum;
        if (plane->type < 3) d = p[plane->type] - plane->dist;
        else d = DotProduct(plane->normal, p) - plane->dist;
        if (d < 0) num = node->children[1];
        else num = node->children[0];
    }
    return num;
}

static int SV_RecursiveHullCheck(hull_t *h, int num, float p1f, float p2f, vec3_t p1, vec3_t p2, trace_t *trace)
{
    dclipnode_t *node;
    mplane_t *plane;
    float t1, t2, frac, midf;
    int i, side;
    vec3_t mid;
    if (num < 0) {
        if (num != CONTENTS_SOLID) {
            trace->allsolid = 0;
            if (num == CONTENTS_EMPTY) trace->inopen = 1;
            else trace->inwater = 1;
        } else trace->startsolid = 1;
        return 1;
    }
    node = h->clipnodes + num;
    plane = h->planes + node->planenum;
    if (plane->type < 3) {
        t1 = p1[plane->type] - plane->dist;
        t2 = p2[plane->type] - plane->dist;
    } else {
        t1 = DotProduct(plane->normal, p1) - plane->dist;
        t2 = DotProduct(plane->normal, p2) - plane->dist;
    }
    if (t1 >= 0 && t2 >= 0) return SV_RecursiveHullCheck(h, node->children[0], p1f, p2f, p1, p2, trace);
    if (t1 < 0 && t2 < 0) return SV_RecursiveHullCheck(h, node->children[1], p1f, p2f, p1, p2, trace);
    if (t1 < 0) frac = (t1 + DIST_EPSILON) / (t1 - t2);
    else frac = (t1 - DIST_EPSILON) / (t1 - t2);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    midf = p1f + (p2f - p1f) * frac;
    for (i = 0; i < 3; i++) mid[i] = p1[i] + frac * (p2[i] - p1[i]);
    side = (t1 < 0);
    if (!SV_RecursiveHullCheck(h, node->children[side], p1f, midf, p1, mid, trace)) return 0;
    if (SV_HullPointContents(h, node->children[side ^ 1], mid) != CONTENTS_SOLID)
        return SV_RecursiveHullCheck(h, node->children[side ^ 1], midf, p2f, mid, p2, trace);
    if (trace->allsolid) return 0;
    if (!side) {
        memcpy(trace->plane.normal, plane->normal, sizeof(vec3_t));
        trace->plane.dist = plane->dist;
    } else {
        for (i = 0; i < 3; i++) trace->plane.normal[i] = vec3_origin[i] - plane->normal[i];
        trace->plane.dist = -plane->dist;
    }
    while (SV_HullPointContents(h, h->firstclipnode, mid) == CONTENTS_SOLID) {
        frac -= 0.1f;
        if (frac < 0) {
            trace->fraction = midf;
            memcpy(trace->endpos, mid, sizeof mid);
            return 0;
        }
        midf = p1f + (p2f - p1f) * frac;
        for (i = 0; i < 3; i++) mid[i] = p1[i] + frac * (p2[i] - p1[i]);
    }
    trace->fraction = midf;
    memcpy(trace->endpos, mid, sizeof mid);
    return 0;
}

static int leaf_contents(const float lo[3], const float hi[3])
{
    float c[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    for (int b = 0; b < BOXES; b++)
        if (c[0] > boxes[b][0][0] && c[0] < boxes[b][1][0] && c[1] > boxes[b][0][1] && c[1] < boxes[b][1][1] &&
            c[2] > boxes[b][0][2] && c[2] < boxes[b][1][2])
            return CONTENTS_SOLID;
    return c[2] < -1500 ? CONTENTS_WATER : CONTENTS_EMPTY;
}

static int build(const float lo[3], const float hi[3], int depth, uint32_t *seed)
{
    if (depth == DEPTH) return leaf_contents(lo, hi);
    int n = nodes_used++, axis = depth % 3;
    float at = lo[axis] + (hi[axis] - lo[axis]) * (0.3f + 0.4f * wl_randf(seed));
    mplane_t *p = &planes[n];
    memset(p, 0, sizeof *p);
    p->normal[axis] = 1, p->dist = at, p->type = axis;
    if (n % 6 == 5) {   /* oblique: leans a little toward the next axis; the boxes below are cut as if it did not */
        float a = 0.25f, len = 1.0307764f;   /* sqrt(1 + a * a) */
        float c[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
        c[axis] = at;
        p->normal[axis] = 1 / len, p->normal[(axis + 1) % 3] = a / len, p->type = 3;
        p->dist = DotProduct(p->normal, c);
    }
    clipnodes[n].planenum = n;
    for (int side = 0; side < 2; side++) {
        float l2[3] = {lo[0], lo[1], lo[2]}, h2[3] = {hi[0], hi[1], hi[2]};
        if (side == 0) l2[axis] = at; else h2[axis] = at;
        clipnodes[n].children[side] = (short)build(l2, h2, depth + 1, seed);
    }
    return n;
}

static void setup(void)
{
    uint32_t seed = 0x9e11;
    const float lo[3] = {-2048, -2048, -2048}, hi[3] = {2048, 2048, 2048};
    for (int b = 0; b < BOXES; b++)
        for (int i = 0; i < 3; i++) {
            float c = -1900 + 3800 * wl_randf(&seed), h = 60 + 260 * wl_randf(&seed);
            boxes[b][0][i] = c - h, boxes[b][1][i] = c + h;
        }
    nodes_used = 0;
    build(lo, hi, 0, &seed);
    hull = (hull_t){clipnodes, planes, 0, nodes_used - 1};
    for (int m = 0; m < MOVES + LONGS; m++) {
        for (int i = 0; i < 3; i++) moves[m][0][i] = -1900 + 3800 * wl_randf(&seed);
        float len = m < MOVES ? 8 + 56 * wl_randf(&seed) : 3000;
        for (int i = 0; i < 3; i++) moves[m][1][i] = moves[m][0][i] + len * (wl_randf(&seed) - 0.5f);
    }
    for (int p = 0; p < POINTS; p++)
        for (int i = 0; i < 3; i++) points[p][i] = -2000 + 4000 * wl_randf(&seed);
}

static uint32_t run(void *ctx)
{
    uint32_t sum = 0;
    (void)ctx;
    for (int m = 0; m < MOVES + LONGS; m++) {
        trace_t trace;
        memset(&trace, 0, sizeof trace);
        trace.fraction = 1, trace.allsolid = 1;
        memcpy(trace.endpos, moves[m][1], sizeof(vec3_t));
        SV_RecursiveHullCheck(&hull, hull.firstclipnode, 0, 1, moves[m][0], moves[m][1], &trace);
        sum = sum * 31 + wl_fold(trace.fraction * 64) + (uint32_t)(trace.allsolid * 2 + trace.startsolid * 4 + trace.inwater * 8);
    }
    for (int p = 0; p < POINTS; p++) sum = sum * 7 + (uint32_t)SV_HullPointContents(&hull, hull.firstclipnode, points[p]);
    return sum;
}

/* run() stores only to its stack: a scratch of nothing, so that every worker may run it */
static void *inst(void) { return wl_alloc(16); }

const wl_kernel wl_q1_hull = {"q1_hull", "Quake server: SV_RecursiveHullCheck and SV_HullPointContents", setup, inst, run};
