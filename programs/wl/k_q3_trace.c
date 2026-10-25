/*
 * q3_trace: Quake III's collision traces, what its server spends most on (Pmove's moves, the bots' and
 * the weapons' traces). Copied from ioq3's qcommon/cm_trace.c: CM_TraceThroughBrush (its box path),
 * CM_TraceThroughLeaf (brushes only, the marks a core's own as in the machine's patch), CM_TraceThroughTree
 * and CM_Trace's set-up of the trace work. The map is made here: a tree of 255 nodes (axial planes, every
 * eighth oblique) over a 4096-unit world, 600 brushes (boxes, a third with an oblique seventh side), each
 * leaf listing the brushes that reach into it. A unit: 160 player-sized box moves of 8 to 72 units and 40
 * point traces across the world (sight and shots).
 */
#include <math.h>
#include <string.h>
#include "wl.h"

#define SURFACE_CLIP_EPSILON 0.125f
#define NODES 255
#define LEAFS 256
#define BRUSHES 600
#define BOXES 160
#define LINES 40

typedef float vec3_t[3];
typedef struct { vec3_t normal; float dist; unsigned char type, signbits; } cplane_t;
typedef struct { cplane_t *plane; int surfaceFlags; } cbrushside_t;
typedef struct { int contents; vec3_t bounds[2]; int numsides; cbrushside_t *sides; } cbrush_t;
typedef struct { cplane_t *plane; int children[2]; } cNode_t;
typedef struct { int firstLeafBrush, numLeafBrushes; } cLeaf_t;
typedef struct { float fraction; int startsolid, allsolid, contents, surfaceFlags; cplane_t plane; } trace_t;
typedef struct {
    vec3_t start, end, size[2], offsets[8], extents, bounds[2];
    float maxOffset;
    int contents, isPoint;
    trace_t trace;
    int *marks, count;
} traceWork_t;

static cplane_t planes[NODES + BRUSHES * 7];
static cbrushside_t sides[BRUSHES * 7];
static cbrush_t brushes[BRUSHES];
static cNode_t nodes[NODES];
static cLeaf_t leafs[LEAFS];
static int *leafbrushes;
static vec3_t moves[BOXES + LINES][2];

#define DotProduct(a, b) ((a)[0] * (b)[0] + (a)[1] * (b)[1] + (a)[2] * (b)[2])

static void set_plane(cplane_t *p, float x, float y, float z, float dist)
{
    p->normal[0] = x, p->normal[1] = y, p->normal[2] = z, p->dist = dist;
    p->type = x == 1 ? 0 : y == 1 ? 1 : z == 1 ? 2 : 3;
    p->signbits = (x < 0) | (y < 0) << 1 | (z < 0) << 2;
}

static void CM_TraceThroughBrush(traceWork_t *tw, cbrush_t *brush)
{
    int i, getout = 0, startout = 0;
    cplane_t *plane, *clipplane = 0;
    float dist, enterFrac = -1, leaveFrac = 1, d1, d2, f;
    cbrushside_t *side, *leadside = 0;
    for (i = 0; i < brush->numsides; i++) {
        side = brush->sides + i;
        plane = side->plane;
        dist = plane->dist - DotProduct(tw->offsets[plane->signbits], plane->normal);
        d1 = DotProduct(tw->start, plane->normal) - dist;
        d2 = DotProduct(tw->end, plane->normal) - dist;
        if (d2 > 0) getout = 1;
        if (d1 > 0) startout = 1;
        if (d1 > 0 && (d2 >= SURFACE_CLIP_EPSILON || d2 >= d1)) return;
        if (d1 <= 0 && d2 <= 0) continue;
        if (d1 > d2) {
            f = (d1 - SURFACE_CLIP_EPSILON) / (d1 - d2);
            if (f < 0) f = 0;
            if (f > enterFrac) enterFrac = f, clipplane = plane, leadside = side;
        } else {
            f = (d1 + SURFACE_CLIP_EPSILON) / (d1 - d2);
            if (f > 1) f = 1;
            if (f < leaveFrac) leaveFrac = f;
        }
    }
    if (!startout) {
        tw->trace.startsolid = 1;
        if (!getout) tw->trace.allsolid = 1, tw->trace.fraction = 0, tw->trace.contents = brush->contents;
        return;
    }
    if (enterFrac < leaveFrac && enterFrac > -1 && enterFrac < tw->trace.fraction) {
        if (enterFrac < 0) enterFrac = 0;
        tw->trace.fraction = enterFrac;
        if (clipplane) tw->trace.plane = *clipplane;
        if (leadside) tw->trace.surfaceFlags = leadside->surfaceFlags;
        tw->trace.contents = brush->contents;
    }
}

static void CM_TraceThroughLeaf(traceWork_t *tw, cLeaf_t *leaf)
{
    for (int k = 0; k < leaf->numLeafBrushes; k++) {
        int brushnum = leafbrushes[leaf->firstLeafBrush + k];
        cbrush_t *b = &brushes[brushnum];
        if (tw->marks[brushnum] == tw->count) continue;   /* already checked this brush in another leaf */
        tw->marks[brushnum] = tw->count;
        if (!(b->contents & tw->contents)) continue;
        if (tw->bounds[0][0] > b->bounds[1][0] || tw->bounds[0][1] > b->bounds[1][1] || tw->bounds[0][2] > b->bounds[1][2] ||
            tw->bounds[1][0] < b->bounds[0][0] || tw->bounds[1][1] < b->bounds[0][1] || tw->bounds[1][2] < b->bounds[0][2])
            continue;
        CM_TraceThroughBrush(tw, b);
        if (!tw->trace.fraction) return;
    }
}

static void CM_TraceThroughTree(traceWork_t *tw, int num, float p1f, float p2f, vec3_t p1, vec3_t p2)
{
    cNode_t *node;
    cplane_t *plane;
    float t1, t2, offset, frac, frac2, idist, midf;
    vec3_t mid;
    int side;
    if (tw->trace.fraction <= p1f) return;
    if (num < 0) { CM_TraceThroughLeaf(tw, &leafs[-1 - num]); return; }
    node = nodes + num;
    plane = node->plane;
    if (plane->type < 3) {
        t1 = p1[plane->type] - plane->dist;
        t2 = p2[plane->type] - plane->dist;
        offset = tw->extents[plane->type];
    } else {
        t1 = DotProduct(plane->normal, p1) - plane->dist;
        t2 = DotProduct(plane->normal, p2) - plane->dist;
        offset = tw->isPoint ? 0 : 2048;
    }
    if (t1 >= offset + 1 && t2 >= offset + 1) { CM_TraceThroughTree(tw, node->children[0], p1f, p2f, p1, p2); return; }
    if (t1 < -offset - 1 && t2 < -offset - 1) { CM_TraceThroughTree(tw, node->children[1], p1f, p2f, p1, p2); return; }
    if (t1 < t2) {
        idist = 1.0f / (t1 - t2), side = 1;
        frac2 = (t1 + offset + SURFACE_CLIP_EPSILON) * idist;
        frac = (t1 - offset + SURFACE_CLIP_EPSILON) * idist;
    } else if (t1 > t2) {
        idist = 1.0f / (t1 - t2), side = 0;
        frac2 = (t1 - offset - SURFACE_CLIP_EPSILON) * idist;
        frac = (t1 + offset + SURFACE_CLIP_EPSILON) * idist;
    } else side = 0, frac = 1, frac2 = 0;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    midf = p1f + (p2f - p1f) * frac;
    for (int i = 0; i < 3; i++) mid[i] = p1[i] + frac * (p2[i] - p1[i]);
    CM_TraceThroughTree(tw, node->children[side], p1f, midf, p1, mid);
    if (frac2 < 0) frac2 = 0;
    if (frac2 > 1) frac2 = 1;
    midf = p1f + (p2f - p1f) * frac2;
    for (int i = 0; i < 3; i++) mid[i] = p1[i] + frac2 * (p2[i] - p1[i]);
    CM_TraceThroughTree(tw, node->children[side ^ 1], midf, p2f, mid, p2);
}

static void CM_Trace(traceWork_t *tw, const vec3_t start, const vec3_t end, const vec3_t mins, const vec3_t maxs)
{
    vec3_t offset;
    int i;
    tw->count++;
    memset(&tw->trace, 0, sizeof tw->trace);
    tw->trace.fraction = 1;
    tw->contents = 1;
    for (i = 0; i < 3; i++) {
        offset[i] = (mins[i] + maxs[i]) * 0.5f;
        tw->size[0][i] = mins[i] - offset[i];
        tw->size[1][i] = maxs[i] - offset[i];
        tw->start[i] = start[i] + offset[i];
        tw->end[i] = end[i] + offset[i];
    }
    tw->maxOffset = tw->size[1][0] + tw->size[1][1] + tw->size[1][2];
    for (i = 0; i < 8; i++) {
        tw->offsets[i][0] = tw->size[i & 1][0];
        tw->offsets[i][1] = tw->size[i >> 1 & 1][1];
        tw->offsets[i][2] = tw->size[i >> 2 & 1][2];
    }
    for (i = 0; i < 3; i++) {
        if (tw->start[i] < tw->end[i]) tw->bounds[0][i] = tw->start[i] + tw->size[0][i], tw->bounds[1][i] = tw->end[i] + tw->size[1][i];
        else tw->bounds[0][i] = tw->end[i] + tw->size[0][i], tw->bounds[1][i] = tw->start[i] + tw->size[1][i];
    }
    tw->isPoint = start == end || (mins[0] == 0 && mins[1] == 0 && mins[2] == 0 && maxs[0] == 0 && maxs[1] == 0 && maxs[2] == 0);
    for (i = 0; i < 3; i++) tw->extents[i] = tw->isPoint ? 0 : -tw->size[0][i] > tw->size[1][i] ? -tw->size[0][i] : tw->size[1][i];
    CM_TraceThroughTree(tw, 0, 0, 1, tw->start, tw->end);
}

/* the map: node n splits its box; children 2n+1 and 2n+2, the last level leafs */
static float leafbox[LEAFS][2][3];
static void split(int n, float lo[3], float hi[3], int depth, uint32_t *seed)
{
    int axis = depth % 3;
    float at = lo[axis] + (hi[axis] - lo[axis]) * (0.35f + 0.3f * wl_randf(seed)), keep;
    if (n % 8 == 7) {   /* an oblique node: its plane leans, the boxes below split as if it did not */
        float a = 0.2f + 0.3f * wl_randf(seed), len = sqrtf(1 + a * a);
        float nx = axis == 0 ? 1 / len : a / len, ny = axis == 1 ? 1 / len : axis == 0 ? a / len : 0, nz = axis == 2 ? 1 / len : 0;
        float centre[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
        centre[axis] = at;
        set_plane(&planes[n], nx, ny, nz, DotProduct(((float[3]){nx, ny, nz}), centre));
    } else {
        float x = axis == 0, y = axis == 1, z = axis == 2;
        set_plane(&planes[n], x, y, z, at);
    }
    nodes[n].plane = &planes[n];
    for (int c = 0; c < 2; c++) {
        float l2[3] = {lo[0], lo[1], lo[2]}, h2[3] = {hi[0], hi[1], hi[2]};
        if (c == 0) l2[axis] = at; else h2[axis] = at;
        int child = 2 * n + 1 + c;
        if (child >= NODES) {
            int leaf = child - NODES;
            nodes[n].children[c] = -1 - leaf;
            memcpy(leafbox[leaf][0], l2, sizeof l2), memcpy(leafbox[leaf][1], h2, sizeof h2);
        } else {
            nodes[n].children[c] = child;
            split(child, l2, h2, depth + 1, seed);
        }
    }
    (void)keep;
}

static void setup(void)
{
    uint32_t seed = 0x3a3a3a3a;
    float lo[3] = {-2048, -2048, -2048}, hi[3] = {2048, 2048, 2048};
    int planes_used = NODES, sides_used = 0, total = 0;
    split(0, lo, hi, 0, &seed);
    for (int b = 0; b < BRUSHES; b++) {
        cbrush_t *br = &brushes[b];
        float c[3], h[3];
        for (int i = 0; i < 3; i++) c[i] = -1900 + 3800 * wl_randf(&seed), h[i] = 16 + 180 * wl_randf(&seed);
        br->contents = 1;
        br->sides = &sides[sides_used];
        for (int i = 0; i < 3; i++) {
            br->bounds[0][i] = c[i] - h[i], br->bounds[1][i] = c[i] + h[i];
            float x = i == 0, y = i == 1, z = i == 2;
            set_plane(&planes[planes_used], x, y, z, c[i] + h[i]);
            sides[sides_used++] = (cbrushside_t){&planes[planes_used++], i};
            set_plane(&planes[planes_used], -x, -y, -z, -(c[i] - h[i]));
            sides[sides_used++] = (cbrushside_t){&planes[planes_used++], i + 3};
        }
        if (b % 3 == 0) {   /* a bevel: a corner cut off */
            float n[3] = {0.57735f, 0.57735f, 0.57735f};
            set_plane(&planes[planes_used], n[0], n[1], n[2], DotProduct(n, c) + 0.5f * (h[0] + h[1] + h[2]) * 0.57735f);
            sides[sides_used++] = (cbrushside_t){&planes[planes_used++], 7};
        }
        br->numsides = (int)(&sides[sides_used] - br->sides);
    }
    for (int pass = 0; pass < 2; pass++) {
        total = 0;
        for (int l = 0; l < LEAFS; l++) {
            leafs[l].firstLeafBrush = total;
            for (int b = 0; b < BRUSHES; b++) {
                int in = 1;
                for (int i = 0; i < 3; i++)
                    if (brushes[b].bounds[0][i] > leafbox[l][1][i] || brushes[b].bounds[1][i] < leafbox[l][0][i]) in = 0;
                if (in) { if (pass) leafbrushes[total] = b; total++; }
            }
            leafs[l].numLeafBrushes = total - leafs[l].firstLeafBrush;
        }
        if (!pass) leafbrushes = wl_alloc(sizeof(int) * (unsigned)total);
    }
    for (int m = 0; m < BOXES + LINES; m++) {
        for (int i = 0; i < 3; i++) moves[m][0][i] = -1900 + 3800 * wl_randf(&seed);
        if (m < BOXES) {
            float len = 8 + 64 * wl_randf(&seed);
            for (int i = 0; i < 3; i++) moves[m][1][i] = moves[m][0][i] + len * (wl_randf(&seed) - 0.5f) * (i == 2 ? 0.5f : 1.5f);
        } else
            for (int i = 0; i < 3; i++) moves[m][1][i] = -1900 + 3800 * wl_randf(&seed);
    }
}

static void *inst(void)
{
    traceWork_t *tw = wl_alloc(sizeof(traceWork_t));
    tw->marks = wl_alloc(sizeof(int) * BRUSHES);
    return tw;
}

static uint32_t run(void *ctx)
{
    static const vec3_t mins = {-15, -15, -24}, maxs = {15, 15, 32}, zero = {0, 0, 0};
    traceWork_t *tw = ctx;
    uint32_t sum = 0;
    tw->count = 0;
    memset(tw->marks, 0xff, sizeof(int) * BRUSHES);
    for (int m = 0; m < BOXES + LINES; m++) {
        CM_Trace(tw, moves[m][0], moves[m][1], m < BOXES ? mins : zero, m < BOXES ? maxs : zero);
        sum = sum * 31 + wl_fold(tw->trace.fraction * 1024) + (uint32_t)tw->trace.startsolid * 7 + (uint32_t)tw->trace.surfaceFlags;
    }
    return sum;
}

const wl_kernel wl_q3_trace = {"q3_trace", "Quake III server: CM_Trace box and point traces through the BSP", setup, inst, run};
