/*
 * path_bvh: the path tracer's search of its tree of boxes (nxpath, linux/pathtrace). Copied from tracer.c:
 * next() and chance(), in_ball(), box_met(), triangle_met() (Moeller and Trumbore) and world_nearest()'s plain
 * path (no lane steps), with scene.h's sc_triangle and sc_node. The tree is built here by halving at the
 * middle triangle on the longest axis (bvh.c's own builder bins by area); the mesh is a torus knot of 6,144
 * triangles over a floor of two. A unit: 64 paths from the eye through an 8 x 8 tile, each its first hit, a
 * ray to the light and two diffuse bounces (four searches a path).
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "wl.h"

struct vec { float x, y, z; };
struct sc_triangle { float corner[3], edge1[3], edge2[3], normal[3][3]; uint32_t object, unused; };
struct sc_node { float low[3]; uint32_t a; float high[3]; uint32_t b; };
struct met { const struct sc_triangle *triangle; float u, w; };

#define RINGS 96
#define AROUND 32
#define TRIANGLES (RINGS * AROUND * 2 + 2)
#define LEAF 4
#define PATHS 64

static struct sc_triangle *triangles;
static struct sc_node *nodes;
static uint32_t node_count;

static inline struct vec v(float x, float y, float z) { struct vec r = {x, y, z}; return r; }
static inline struct vec add(struct vec a, struct vec b) { return v(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline struct vec sub(struct vec a, struct vec b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline struct vec scale(struct vec a, float s) { return v(a.x * s, a.y * s, a.z * s); }
static inline float dot(struct vec a, struct vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline struct vec unit(struct vec a) { return scale(a, 1.0f / __builtin_sqrtf(dot(a, a))); }
static inline struct vec cross(struct vec a, struct vec b) { return v(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline struct vec of(const float *p) { return v(p[0], p[1], p[2]); }

static inline uint32_t next(uint32_t *state) { return *state = *state * 1664525u + 1013904223u; }
static inline float chance(uint32_t *state) { return (float)(next(state) >> 8) * (1.0f / 16777216.0f); }

static inline struct vec in_ball(uint32_t *state)
{
    for (;;) {
        float a = 2.0f * chance(state) - 1.0f, b = 2.0f * chance(state) - 1.0f, s = a * a + b * b;
        if (s < 1.0f) {
            float f = 2.0f * __builtin_sqrtf(1.0f - s);
            return v(a * f, b * f, 1.0f - 2.0f * s);
        }
    }
}

static inline int box_met(const struct sc_node *node, struct vec from, struct vec over, float far, float *at)
{
    float a = (node->low[0] - from.x) * over.x, b = (node->high[0] - from.x) * over.x, low, high;
    low = __builtin_fminf(a, b), high = __builtin_fmaxf(a, b);
    a = (node->low[1] - from.y) * over.y, b = (node->high[1] - from.y) * over.y;
    low = __builtin_fmaxf(low, __builtin_fminf(a, b)), high = __builtin_fminf(high, __builtin_fmaxf(a, b));
    a = (node->low[2] - from.z) * over.z, b = (node->high[2] - from.z) * over.z;
    low = __builtin_fmaxf(low, __builtin_fminf(a, b)), high = __builtin_fminf(high, __builtin_fmaxf(a, b));
    *at = low;
    return (high >= low) & (high > 0.0f) & (low < far);
}

static inline int triangle_met(const struct sc_triangle *tri, struct vec from, struct vec way, float *t, float *u, float *w)
{
    struct vec e1 = of(tri->edge1), e2 = of(tri->edge2), p = cross(way, e2), s, q;
    float det = dot(e1, p), inv;
    if (det > -1e-12f && det < 1e-12f) return 0;
    inv = 1.0f / det;
    s = sub(from, of(tri->corner));
    *u = dot(s, p) * inv;
    if (*u < 0.0f || *u > 1.0f) return 0;
    q = cross(s, e1);
    *w = dot(way, q) * inv;
    if (*w < 0.0f || *u + *w > 1.0f) return 0;
    *t = dot(e2, q) * inv;
    return 1;
}

static int world_nearest(struct vec from, struct vec way, float *nearest, struct met *met)
{
    struct vec over = v(1.0f / way.x, 1.0f / way.y, 1.0f / way.z);
    const struct sc_triangle *found = 0;
    uint32_t stack[64], at = 0;
    float entered[64], fu = 0, fw = 0, enter, best = *nearest;
    int held = 0;
    if (!box_met(&nodes[0], from, over, best, &enter)) return 0;
    for (;;) {
        const struct sc_node *node = &nodes[at];
        if (node->b) {
            const struct sc_triangle *tri = triangles + node->a, *end = tri + node->b;
            for (; tri < end; tri++) {
                float t, u, w;
                if (triangle_met(tri, from, way, &t, &u, &w) && t > 0.0005f && t < best) best = t, found = tri, fu = u, fw = w;
            }
        } else {
            float t0, t1;
            int in0 = box_met(&nodes[node->a], from, over, best, &t0), in1 = box_met(&nodes[node->a + 1], from, over, best, &t1);
            if (in0 && in1) {
                entered[held] = t0 <= t1 ? t1 : t0;
                stack[held++] = t0 <= t1 ? node->a + 1 : node->a;
                at = t0 <= t1 ? node->a : node->a + 1;
                continue;
            }
            if (in0 || in1) { at = in0 ? node->a : node->a + 1; continue; }
        }
        do {
            if (!held) goto done;
            at = stack[--held];
        } while (!(entered[held] < best));
    }
done:
    if (!found) return 0;
    *nearest = best;
    met->triangle = found, met->u = fu, met->w = fw;
    return 1;
}

/* the tree: triangles [first, first + count) under node n; children made in pairs */
static int sort_axis;
static int by_centre(const void *a, const void *b)
{
    const struct sc_triangle *p = a, *q = b;
    float x = p->corner[sort_axis] * 3 + p->edge1[sort_axis] + p->edge2[sort_axis];
    float y = q->corner[sort_axis] * 3 + q->edge1[sort_axis] + q->edge2[sort_axis];
    return x < y ? -1 : x > y;
}
static void bound(struct sc_node *n, uint32_t first, uint32_t count)
{
    for (int k = 0; k < 3; k++) n->low[k] = 1e30f, n->high[k] = -1e30f;
    for (uint32_t i = first; i < first + count; i++)
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++) {
                const struct sc_triangle *t = &triangles[i];
                float p = t->corner[k] + (c == 1 ? t->edge1[k] : c == 2 ? t->edge2[k] : 0);
                if (p < n->low[k]) n->low[k] = p;
                if (p > n->high[k]) n->high[k] = p;
            }
}
static void grow(uint32_t n, uint32_t first, uint32_t count)
{
    struct sc_node *node = &nodes[n];
    bound(node, first, count);
    if (count <= LEAF) { node->a = first, node->b = count; return; }
    int axis = 0;
    for (int k = 1; k < 3; k++)
        if (node->high[k] - node->low[k] > node->high[axis] - node->low[axis]) axis = k;
    sort_axis = axis;
    qsort(triangles + first, count, sizeof(struct sc_triangle), by_centre);
    uint32_t half = count / 2, child = node_count;
    node_count += 2;
    node->a = child, node->b = 0;
    grow(child, first, half);
    grow(child + 1, first + half, count - half);
}

static void put_triangle(int i, struct vec a, struct vec b, struct vec c)
{
    struct sc_triangle *t = &triangles[i];
    struct vec n = unit(cross(sub(b, a), sub(c, a)));
    memset(t, 0, sizeof *t);
    t->corner[0] = a.x, t->corner[1] = a.y, t->corner[2] = a.z;
    t->edge1[0] = b.x - a.x, t->edge1[1] = b.y - a.y, t->edge1[2] = b.z - a.z;
    t->edge2[0] = c.x - a.x, t->edge2[1] = c.y - a.y, t->edge2[2] = c.z - a.z;
    for (int k = 0; k < 3; k++) t->normal[k][0] = n.x, t->normal[k][1] = n.y, t->normal[k][2] = n.z;
}

static struct vec knot(float s, float r)   /* a (2, 3) torus knot's tube */
{
    float p = s * 6.2831853f, q = r * 6.2831853f;
    struct vec c = v((2 + cosf(3 * p)) * cosf(2 * p), (2 + cosf(3 * p)) * sinf(2 * p) * 0.6f + 2.6f, sinf(3 * p));
    struct vec d = v((2 + cosf(3 * (p + 0.01f))) * cosf(2 * (p + 0.01f)), (2 + cosf(3 * (p + 0.01f))) * sinf(2 * (p + 0.01f)) * 0.6f + 2.6f,
                     sinf(3 * (p + 0.01f)));
    struct vec t = unit(sub(d, c)), n = unit(cross(t, v(0, 0, 1))), b = cross(t, n);
    return add(c, add(scale(n, 0.45f * cosf(q)), scale(b, 0.45f * sinf(q))));
}

static void setup(void)
{
    triangles = wl_alloc(sizeof(struct sc_triangle) * TRIANGLES);
    nodes = wl_alloc(sizeof(struct sc_node) * TRIANGLES * 2);
    int i = 0;
    for (int r = 0; r < RINGS; r++)
        for (int k = 0; k < AROUND; k++) {
            float s0 = (float)r / RINGS, s1 = (float)(r + 1) / RINGS, t0 = (float)k / AROUND, t1 = (float)(k + 1) / AROUND;
            put_triangle(i++, knot(s0, t0), knot(s1, t0), knot(s1, t1));
            put_triangle(i++, knot(s0, t0), knot(s1, t1), knot(s0, t1));
        }
    put_triangle(i++, v(-20, 0, -20), v(-20, 0, 20), v(20, 0, 20));
    put_triangle(i++, v(-20, 0, -20), v(20, 0, 20), v(20, 0, -20));
    node_count = 1;
    grow(0, 0, TRIANGLES);
}

static uint32_t run(void *ctx)
{
    uint32_t state = 12345, sum = 0;
    struct vec eye = v(0, 3, -9), light = v(4, 9, -4);
    (void)ctx;
    for (int p = 0; p < PATHS; p++) {
        float x = (float)(p % 8) / 8 - 0.3f, y = (float)(p / 8) / 8 - 0.4f;
        struct vec from = eye, way = unit(v(x, y, 1)), carried = v(1, 1, 1), seen = v(0, 0, 0);
        for (int bounce = 0; bounce < 3; bounce++) {
            float distance = 1e30f;
            struct met met = {0, 0, 0};
            if (!world_nearest(from, way, &distance, &met)) { seen = add(seen, scale(carried, 0.6f)); break; }
            struct vec at = add(from, scale(way, distance)), n = of(met.triangle->normal[0]);
            if (dot(n, way) > 0) n = scale(n, -1);
            if (bounce == 0) {   /* the light: one ray to it */
                struct vec to = sub(light, at);
                float far = __builtin_sqrtf(dot(to, to)), lit = dot(n, scale(to, 1 / far));
                struct met block = {0, 0, 0};
                if (lit > 0 && !world_nearest(add(at, scale(n, 0.001f)), scale(to, 1 / far), &far, &block)) seen = add(seen, scale(carried, lit));
            }
            carried = scale(carried, 0.7f);
            way = unit(add(n, in_ball(&state)));
            from = add(at, scale(n, 0.001f));
        }
        sum = sum * 31 + wl_fold(seen.x + seen.y + seen.z);
    }
    return sum;
}

/* run() stores only to its stack: a scratch of nothing, so that every worker may run it */
static void *inst(void) { return wl_alloc(16); }

const wl_kernel wl_path_bvh = {"path_bvh", "nxpath: world_nearest over its tree of boxes, triangle_met, paths", setup, inst, run};
