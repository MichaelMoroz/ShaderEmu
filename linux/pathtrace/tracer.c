/* The path tracer's own part (docs/apps/raytrace.md): the scene's world under its lights and its
 * sky (scene.h), and one sample of every pixel of a tile as a job for a worker core, or of one pixel as a
 * lane's index in steps (pt_batch_step). A job writes its tile's sums and pixels and its stack: a tile is 32 across, the
 * width a multiple of four and a pixel's sum 16 bytes, so no two cores store to the same 16 bytes. The
 * scene is only read. */
#include <stdlib.h>
#include "scene.h"
#include "mcw.h"

struct pt_picture pt;

struct vec { float x, y, z; };
/* What a ray meets: nothing, a triangle of the world, or a light (and which). */
enum { MET_NONE, MET_WORLD, MET_LIGHT };
/* Where in the world: the triangle, and how far along its two edges. */
struct met { const struct sc_triangle *triangle; float u, w; };

static inline struct vec v(float x, float y, float z) { struct vec r = {x, y, z}; return r; }
static inline struct vec add(struct vec a, struct vec b) { return v(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline struct vec sub(struct vec a, struct vec b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline struct vec mul(struct vec a, struct vec b) { return v(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline struct vec scale(struct vec a, float s) { return v(a.x * s, a.y * s, a.z * s); }
static inline float dot(struct vec a, struct vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline struct vec unit(struct vec a) { return scale(a, 1.0f / __builtin_sqrtf(dot(a, a))); }
static inline struct vec cross(struct vec a, struct vec b) { return v(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline struct vec of(const float *p) { return v(p[0], p[1], p[2]); }

/* The most light a sample may bring: a light itself, seen in a mirror or through glass. */
#define PT_MOST 2.5f
/* What a lane's step keeps for (pt_batch_step): the place between two boxes, and a turn's shading
 * after its search (a light's ray through the world is not stopped in) */
#define PT_LEAF_LEFT 2500
#define PT_SHADE_LEFT 2500

/* (a product and a sum; chance() takes the upper bits, which are the good ones) */
static inline uint32_t
next(uint32_t *state)
{
	return *state = *state * 1664525u + 1013904223u;
}

static inline float
chance(uint32_t *state)
{
	return (float)(next(state) >> 8) * (1.0f / 16777216.0f);
}

/* A point of the unit sphere, evenly (Marsaglia, 1972): a point of the disc, which four draws
 * in five are, lifted onto it. */
static inline struct vec
in_ball(uint32_t *state)
{
	for (;;) {
		float a = 2.0f * chance(state) - 1.0f, b = 2.0f * chance(state) - 1.0f, s = a * a + b * b;

		if (s < 1.0f) {
			float f = 2.0f * __builtin_sqrtf(1.0f - s);

			return v(a * f, b * f, 1.0f - 2.0f * s);
		}
	}
}

/* Where a ray is inside a box, if it is before `far`: the distance it comes in at (0 from inside). */
static inline int
box_met(const struct sc_node *node, struct vec from, struct vec over, float far, float *at)
{
	float a = (node->low[0] - from.x) * over.x, b = (node->high[0] - from.x) * over.x, low, high;

	/* (the lesser and the greater as an instruction each: no number here is not one) */
	low = __builtin_fminf(a, b), high = __builtin_fmaxf(a, b);
	a = (node->low[1] - from.y) * over.y, b = (node->high[1] - from.y) * over.y;
	low = __builtin_fmaxf(low, __builtin_fminf(a, b)), high = __builtin_fminf(high, __builtin_fmaxf(a, b));
	a = (node->low[2] - from.z) * over.z, b = (node->high[2] - from.z) * over.z;
	low = __builtin_fmaxf(low, __builtin_fminf(a, b)), high = __builtin_fminf(high, __builtin_fmaxf(a, b));
	*at = low;
	return (high >= low) & (high > 0.0f) & (low < far);
}

/* A ray and a triangle (Moeller and Trumbore's test): how far, and where in it. */
static inline int
triangle_met(const struct sc_triangle *tri, struct vec from, struct vec way, float *t, float *u, float *w)
{
	struct vec e1 = of(tri->edge1), e2 = of(tri->edge2), p = cross(way, e2), s, q;
	float det = dot(e1, p), inv;

	if (det > -1e-12f && det < 1e-12f)
		return 0;
	inv = 1.0f / det;
	s = sub(from, of(tri->corner));
	*u = dot(s, p) * inv;
	if (*u < 0.0f || *u > 1.0f)
		return 0;
	q = cross(s, e1);
	*w = dot(way, q) * inv;
	if (*w < 0.0f || *u + *w > 1.0f)
		return 0;
	*t = dot(e2, q) * inv;
	return 1;
}

/* The nearest triangle of the world a ray meets nearer than *nearest: down the tree of boxes,
 * the nearer of two first, so that the farther is mostly not looked into. `over` is one over
 * the ray's way (a ray along an axis has an infinity there, which leaves it in a box or out).
 * `limited` (a lane's step): -1 where the pass runs low, *trail the turns to the next box, *nearest
 * and *met kept; a search resumed from them goes down those turns again (Laine's restart trail). */
static inline __attribute__((always_inline)) int
world_nearest(struct vec from, struct vec way, struct vec over, uint64_t *trail, float *nearest, struct met *met, int limited)
{
	const struct sc_node *nodes = sc_world.nodes;
	const struct sc_triangle *triangles = sc_world.triangles, *found = met->triangle;
	uint32_t stack[64], at = 0;
	float entered[64];	/* where the ray comes into each box put by */
	uint8_t level[64];	/* and the depth of the turn it was put by at */
	uint64_t back = *trail, turns = 0;	/* a bit a depth: 1 the farther child, or the only one */
	int held = 0, depth = 0, top = back ? 63 - __builtin_clzll(back) : -1;
	float fu = met->u, fw = met->w, enter, best = *nearest;

	if (!box_met(&nodes[0], from, over, best, &enter))
		goto done;
	for (;;) {
		const struct sc_node *node = &nodes[at];

		if (node->b) {
			const struct sc_triangle *tri = triangles + node->a, *end = tri + node->b;

			for (; tri < end; tri++) {
				float t, u, w;

				if (triangle_met(tri, from, way, &t, &u, &w) && t > 0.0005f && t < best)
					best = t, found = tri, fu = u, fw = w;
			}
		} else if (!limited) {
			float t0, t1;
			int in0 = box_met(&nodes[node->a], from, over, best, &t0), in1 = box_met(&nodes[node->a + 1], from, over, best, &t1);

			if (in0 && in1) {
				entered[held] = t0 <= t1 ? t1 : t0;
				stack[held++] = t0 <= t1 ? node->a + 1 : node->a;
				at = t0 <= t1 ? node->a : node->a + 1;
				continue;
			}
			if (in0 || in1) {
				at = in0 ? node->a : node->a + 1;
				continue;
			}
		} else {
			/* (as above, and the turns taken kept: bit d of `turns` the turn at depth d) */
			float t0, t1;
			int in0 = box_met(&nodes[node->a], from, over, best, &t0), in1 = box_met(&nodes[node->a + 1], from, over, best, &t1);
			uint32_t near = t0 <= t1 ? node->a : node->a + 1, far = 2 * node->a + 1 - near;
			int far_in = t0 <= t1 ? in1 : in0, near_in = in0 + in1 - far_in;

			if (depth <= top && (back >> depth & 1)) {
				/* (resumed: the near child was searched; one the ray no longer meets is let be) */
				if (in0 || in1) {
					turns |= 1ull << depth++;
					at = far_in ? far : near;
					continue;
				}
			} else if (near_in) {
				if (far_in) {
					entered[held] = t0 <= t1 ? t1 : t0, level[held] = (uint8_t)depth;
					stack[held++] = far;
					turns &= ~(1ull << depth);
				} else
					turns |= 1ull << depth;
				depth++, at = near;
				continue;
			} else if (far_in) {
				top = -1;	/* (a resumed search's near child behind what it found) */
				turns |= 1ull << depth++;
				at = far;
				continue;
			}
		}
		/* (a box put by may be behind what was found since: only one the ray enters nearer is taken) */
		do {
			if (!held)
				goto done;
			at = stack[--held];
		} while (!(entered[held] < best));
		if (limited) {
			depth = level[held], top = -1;	/* (a resumed search is past its turns) */
			turns = (turns & ((1ull << depth) - 1)) | 1ull << depth;
			/* (a lane's step stops between boxes) */
			if (mcw_steps_left() < PT_LEAF_LEFT) {
				*trail = turns, *nearest = best;
				met->triangle = found, met->u = fu, met->w = fw;
				return -1;
			}
			depth++;
		}
	}
done:
	if (!found)
		return 0;
	*nearest = best;
	met->triangle = found, met->u = fu, met->w = fw;
	return 1;
}

/* Whether a triangle of the world that is not mostly glass is in a ray's way before `far`: every box
 * the ray is in before then is gone into, a box's two tried together as world_nearest() does.
 * Glass in the way: *through is how much it takes of each colour, *crossed its surfaces met. */
static int
world_between(struct vec from, struct vec way, struct vec over, float far, struct vec *through, int *crossed)
{
	const struct sc_node *nodes = sc_world.nodes;
	const struct sc_triangle *triangles = sc_world.triangles;
	uint32_t stack[64], at = 0;
	int held = 0;
	float enter;

	if (!box_met(&nodes[0], from, over, far, &enter))
		return 0;
	for (;;) {
		const struct sc_node *node = &nodes[at];

		if (node->b) {
			const struct sc_triangle *tri = triangles + node->a, *end = tri + node->b;

			for (; tri < end; tri++) {
				float t, u, w;

				if (triangle_met(tri, from, way, &t, &u, &w) && t > 0.0005f && t < far) {
					const struct sc_object *glass = &sc.objects[tri->object];
					struct vec flat = cross(of(tri->edge1), of(tri->edge2));
					float out = dot(way, flat);

					if (glass->glass < 0.5f)
						return 1;
					/* (the way inside it is as much longer as where the ray leaves, and shorter as where it came in) */
					t = (dot(flat, of(tri->normal[0])) < 0.0f ? -out : out) > 0.0f ? t * glass->density : -t * glass->density;
					*through = add(*through, v(t * (1.0f - glass->colour[0]), t * (1.0f - glass->colour[1]), t * (1.0f - glass->colour[2])));
					(*crossed)++;
				}
			}
		} else {
			int in0 = box_met(&nodes[node->a], from, over, far, &enter), in1 = box_met(&nodes[node->a + 1], from, over, far, &enter);

			if (in0 && in1) {
				stack[held++] = node->a + 1;
				at = node->a;
				continue;
			}
			if (in0 || in1) {
				at = in0 ? node->a : node->a + 1;
				continue;
			}
		}
		if (!held)
			return 0;
		at = stack[--held];
	}
}

/* A light's ray in a lane's step (world_between in turns): where it stopped, and the glass it had met. */
struct pt_shadow {
	uint64_t trail;		/* the turns to the next box (0: from the start) */
	struct vec through;
	int crossed, stopped;
};

/* world_between() that stops where the pass runs low (-1, sh->stopped) and goes on from sh->trail:
 * a box's two in their order, so a search resumed goes down the same turns. */
static int
world_between_steps(struct vec from, struct vec way, struct vec over, float far, struct pt_shadow *sh)
{
	const struct sc_node *nodes = sc_world.nodes;
	const struct sc_triangle *triangles = sc_world.triangles;
	uint32_t stack[64], at = 0;
	uint8_t level[64];
	uint64_t back = sh->trail, turns = 0;
	int held = 0, depth = 0, top = back ? 63 - __builtin_clzll(back) : -1;
	float enter;

	if (!box_met(&nodes[0], from, over, far, &enter))
		return 0;
	for (;;) {
		const struct sc_node *node = &nodes[at];

		if (node->b) {
			const struct sc_triangle *tri = triangles + node->a, *end = tri + node->b;

			for (; tri < end; tri++) {
				float t, u, w;

				if (triangle_met(tri, from, way, &t, &u, &w) && t > 0.0005f && t < far) {
					const struct sc_object *glass = &sc.objects[tri->object];
					struct vec flat = cross(of(tri->edge1), of(tri->edge2));
					float out = dot(way, flat);

					if (glass->glass < 0.5f)
						return 1;
					t = (dot(flat, of(tri->normal[0])) < 0.0f ? -out : out) > 0.0f ? t * glass->density : -t * glass->density;
					sh->through = add(sh->through, v(t * (1.0f - glass->colour[0]), t * (1.0f - glass->colour[1]), t * (1.0f - glass->colour[2])));
					sh->crossed++;
				}
			}
		} else {
			int in0 = box_met(&nodes[node->a], from, over, far, &enter), in1 = box_met(&nodes[node->a + 1], from, over, far, &enter);

			if (depth <= top && (back >> depth & 1)) {
				/* (resumed: the first was searched) */
				if (in0 || in1) {
					turns |= 1ull << depth++;
					at = in1 ? node->a + 1 : node->a;
					continue;
				}
			} else if (in0 && in1) {
				level[held] = (uint8_t)depth;
				stack[held++] = node->a + 1;
				turns &= ~(1ull << depth++);
				at = node->a;
				continue;
			} else if (in0 || in1) {
				turns |= 1ull << depth++;
				at = in0 ? node->a : node->a + 1;
				continue;
			}
		}
		if (!held)
			return 0;
		at = stack[--held];
		depth = level[held], top = -1;
		turns = (turns & ((1ull << depth) - 1)) | 1ull << depth;
		if (mcw_steps_left() < PT_LEAF_LEFT) {
			sh->trail = turns, sh->stopped = 1;
			return -1;
		}
		depth++;
	}
}

/* Whether the world is in a light's ray before `far`, and the glass it crosses (sh: in a lane's step). */
static inline int
light_blocked(struct vec at, struct vec to, float far, struct vec *through, int *crossed, struct pt_shadow *sh)
{
	struct vec over = v(1.0f / to.x, 1.0f / to.y, 1.0f / to.z);
	int r;

	if (!sh)
		return world_between(at, to, over, far, through, crossed);
	r = world_between_steps(at, to, over, far, sh);
	*through = sh->through, *crossed = sh->crossed;
	return r != 0;
}

/* The nearest thing a ray meets (MET_NONE: the sky) and how far; *met: where in the world. A search
 * resumed (world_nearest) by *trail, *met and *distance what it had found; -1 where it stopped. */
static inline __attribute__((always_inline)) int
nearest_from(struct vec from, struct vec way, uint64_t *trail, float *distance, struct met *met, int limited)
{
	struct vec over = v(1.0f / way.x, 1.0f / way.y, 1.0f / way.z);
	int i, lights = sc.light_count, what = MET_NONE, world;
	float best = 1e30f, t;

	/* (the lights first: the tree is then looked into no farther than the nearest of them) */
	for (i = 0; i < lights; i++) {
		struct vec to = sub(of(sc.lights[i].place), from);
		float along = dot(to, way), off2 = dot(to, to) - along * along, r2 = sc.lights[i].radius * sc.lights[i].radius;

		if (off2 < r2) {
			float half = __builtin_sqrtf(r2 - off2);

			t = along - half;
			if (t < 0.0005f)
				t = along + half;	/* from inside it */
			if (t > 0.0005f && t < best)
				best = t, what = MET_LIGHT + i;
		}
	}
	/* (a triangle found before is nearer than any light: it was found nearer than them) */
	if (met->triangle)
		best = *distance;
	world = world_nearest(from, way, over, trail, &best, met, limited);
	if (world < 0) {
		*distance = best;
		return -1;
	}
	if (world)
		what = MET_WORLD;
	*distance = best;
	return what;
}

static inline __attribute__((always_inline)) int
nearest(struct vec from, struct vec way, float *distance, struct met *met)
{
	uint64_t trail = 0;

	met->triangle = 0;
	return nearest_from(from, way, &trail, distance, met, 0);
}

/* The way from the eye through a point of a picture `width` by `height`, x and y in pixels. */
static inline struct vec
eye_way(float x, float y, float width, float height)
{
	const struct sc_camera *camera = &sc.camera;
	float open = 2.0f * camera->lens / height, across = camera->aspect > 0.0f ? 2.0f * camera->lens * camera->aspect / width : open;

	return unit(add(of(camera->ahead), add(scale(of(camera->right), (x - 0.5f * width) * across), scale(of(camera->up), (0.5f * height - y) * open))));
}

/* (1 - x/8) to the eighth: e to the -x near enough for a colour, and nothing from 8 on */
static inline float
fade(float x)
{
	float e = 1.0f - x * 0.125f;

	if (e <= 0.0f || x <= 0.0f)
		return x <= 0.0f ? 1.0f : 0.0f;
	e *= e, e *= e;
	return e * e;
}

static inline float
power_of(float c, int n)
{
	float r = 1.0f;

	for (; n; n >>= 1, c *= c)
		if (n & 1)
			r *= c;
	return r;
}

/* Two ways square to a unit way and to each other (Duff and others, 2017). */
static inline void
square_to(struct vec n, struct vec *a, struct vec *b)
{
	float s = n.z >= 0.0f ? 1.0f : -1.0f, p = -1.0f / (s + n.z), q = n.x * n.y * p;

	*a = v(1.0f + s * n.x * n.x * p, s * q, -s * n.x);
	*b = v(q, s + n.y * n.y * p, -n.y);
}

/* What a light's ball sends out, each way from each point: its power is of the whole ball. */
static inline float
shine(const struct sc_light *light)
{
	return light->power * 0.25f / (light->radius * light->radius);
}

/* A way put a little off, by how rough the surface is. */
static inline struct vec
roughened(struct vec way, float rough, uint32_t *state)
{
	return rough > 0.0f ? unit(add(way, scale(in_ball(state), rough * rough))) : way;
}

/* Light through glass is less by its colour, and by a twelfth a surface. */
static inline struct vec
through_glass(struct vec comes, struct vec through, int crossed)
{
	return crossed ? scale(mul(comes, v(fade(through.x), fade(through.y), fade(through.z))), power_of(0.92f, crossed)) : comes;
}

/* The glowing things' light at a point: a point of a glowing triangle, picked by its area, and
 * a ray to it. `picks`: how many lights the one stands for. */
static struct vec
glow_at(struct vec at, uint32_t *state, uint32_t *rays, struct vec *to, float picks, struct pt_shadow *sh)
{
	const float *upto = pt.glow_upto;
	int low = 0, high = pt.glowing_count - 1;
	float r = chance(state) * upto[high], a = chance(state), b = chance(state), d2, d, facing;
	const struct sc_triangle *tri;
	const struct sc_object *object;
	struct vec flat, through = v(0, 0, 0);
	int crossed = 0;

	while (low < high) {
		int mid = (low + high) >> 1;

		if (upto[mid] > r)
			high = mid;
		else
			low = mid + 1;
	}
	tri = &sc_world.triangles[pt.glowing[low]];
	object = &sc.objects[tri->object];
	if (a + b > 1.0f)
		a = 1.0f - a, b = 1.0f - b;
	*to = sub(add(of(tri->corner), add(scale(of(tri->edge1), a), scale(of(tri->edge2), b))), at);
	d2 = dot(*to, *to);
	if (d2 < 1e-10f)
		return *to = v(0, 0, 0);
	d = __builtin_sqrtf(d2);
	*to = scale(*to, 1.0f / d);
	flat = cross(of(tri->edge1), of(tri->edge2));
	facing = __builtin_fabsf(dot(flat, *to)) / __builtin_sqrtf(dot(flat, flat));
	(*rays)++;
	if (light_blocked(at, *to, d - 0.002f, &through, &crossed, sh))
		return v(0, 0, 0);
	/* (one over the chance of that way: the area of them all, turned, over the distance squared) */
	return through_glass(scale(of(object->colour), object->glow * facing * upto[pt.glowing_count - 1] / d2 * picks), through, crossed);
}

/* One light's light at a point (next event estimation): a ray into the cone its ball fills
 * from there, evenly by solid angle, or to a glowing thing. *to: the way taken. Nothing when
 * something is between. */
static inline struct vec
light_at(struct vec at, uint32_t *state, uint32_t *rays, struct vec *to, struct pt_shadow *sh)
{
	int lights = sc.light_count + (pt.glowing_count > 0), pick = (int)(chance(state) * (float)lights);
	const struct sc_light *light = &sc.lights[pick];
	struct vec axis, ta, tb;
	struct vec through = v(0, 0, 0), comes;
	float d2, r2, d, top, less, c, s, a, b, q, reach;
	int crossed = 0;

	if (pick >= sc.light_count)
		return glow_at(at, state, rays, to, (float)lights, sh);
	axis = sub(of(light->place), at);
	d2 = dot(axis, axis), r2 = light->radius * light->radius;
	*to = v(0, 0, 0);
	if (d2 <= r2)
		return v(0, 0, 0);
	d = __builtin_sqrtf(d2);
	axis = scale(axis, 1.0f / d);
	top = __builtin_sqrtf(1.0f - r2 / d2);	/* the cosine at the cone's edge */
	less = r2 / d2 / (1.0f + top);		/* one less that, with its digits */
	c = 1.0f - chance(state) * less;
	s = __builtin_sqrtf(1.0f - c * c);
	do
		a = 2.0f * chance(state) - 1.0f, b = 2.0f * chance(state) - 1.0f, q = a * a + b * b;
	while (q >= 1.0f || q < 1e-6f);
	q = s / __builtin_sqrtf(q);
	square_to(axis, &ta, &tb);
	*to = add(scale(axis, c), add(scale(ta, a * q), scale(tb, b * q)));
	reach = r2 - d2 * s * s;
	reach = d * c - __builtin_sqrtf(reach > 0.0f ? reach : 0.0f);
	(*rays)++;
	if (light_blocked(at, *to, reach - 0.002f, &through, &crossed, sh))
		return v(0, 0, 0);
	/* (the cone's solid angle, and the one light stands for them all) */
	comes = scale(of(light->colour), shine(light) * 6.2831853f * less * (float)lights);
	return through_glass(comes, through, crossed);
}

/* A path where it is (a lane's step stops and goes on: pt_batch_step): its ray, the light it has
 * brought and what it still carries, its random numbers and rays, and its nearest search so far. */
struct pt_path {
	struct vec from, way, seen, weight;
	uint32_t state, rays;
	int bounce, straight, matt;	/* straight: the last turn asked no light for its light, one met now is seen; matt: but not after a matt surface (such a path is one in thousands, and a speck) */
	uint64_t trail;		/* where its search goes on (0: a new turn's), or when shading its light's ray */
	int shading;		/* its search through, its hit kept */
	struct vec through;	/* (the glass the light's ray had met) */
	int crossed;
	float best;
	int tri;		/* and what it found: a triangle of the world (-1 none), how far, where in it */
	float u, w;
};

static inline void
path_begin(struct pt_path *p, struct vec from, struct vec way, uint32_t state)
{
	p->from = from, p->way = way, p->seen = v(0, 0, 0), p->weight = v(1, 1, 1);
	p->state = state, p->rays = 1;
	p->bounce = 0, p->straight = 1, p->matt = 0;
	p->trail = 0, p->shading = 0, p->through = v(0, 0, 0), p->crossed = 0;
	p->best = 1e30f, p->tri = -1, p->u = p->w = 0.0f;
}

/* The light one path brings back into p->seen, *rays its rays, *state its random numbers (apart from p,
 * so that a plain path's p stays in registers); `limited` (a lane): 0 where its pass runs low, p kept. */
static inline __attribute__((always_inline)) int
path_run(struct pt_path *p, uint32_t *state, uint32_t *rays, int limited)
{
	int lights = sc.light_count + (pt.glowing_count > 0);

	while (p->bounce < pt.bounces) {
		const struct sc_object *object;
		struct met met = {p->tri >= 0 ? sc_world.triangles + p->tri : 0, p->u, p->w};
		struct vec at, normal, flat, colour, to, got, mirror, from = p->from, way = p->way, seen = p->seen, weight = p->weight;
		float t = p->best, facing, c, keep;
		int what, back, shiny = 0, straight = p->straight, matt = p->matt;

		if (!p->shading) {
			uint64_t trail = p->trail;

			what = nearest_from(from, way, &trail, &t, &met, limited);
			if (what < 0 || (limited && what == MET_WORLD && mcw_steps_left() < PT_SHADE_LEFT)) {
				p->trail = what < 0 ? trail : 0, p->shading = what >= 0, p->best = t;
				p->tri = met.triangle ? (int)(met.triangle - sc_world.triangles) : -1, p->u = met.u, p->w = met.w;
				return 0;
			}
		} else
			what = MET_WORLD;	/* (searched in an earlier step: its hit) */
		/* (a light's ray stopped: the turn is shaded again from here, with the same random numbers) */
		uint32_t state0 = *state, rays0 = *rays;
		struct pt_shadow sh = {p->shading ? p->trail : 0, p->through, p->crossed, 0}, *shp = limited ? &sh : 0;
		if (what == MET_NONE) {
			p->seen = add(seen, mul(weight, scale(of(sc.sky), sc.sky_power)));
			return 1;
		}
		if (what >= MET_LIGHT) {
			if (straight && !matt)
				p->seen = add(seen, mul(weight, scale(of(sc.lights[what - MET_LIGHT].colour), shine(&sc.lights[what - MET_LIGHT]))));
			return 1;
		}
		at = add(from, scale(way, t));
		object = &sc.objects[met.triangle->object];
		colour = of(object->colour);
		if (object->glow > 0.0f) {
			/* (after a matt surface its light was asked for: light_at) */
			if (straight && !matt)
				seen = add(seen, mul(weight, scale(colour, object->glow)));
			p->seen = seen;
			return 1;
		}
		/* the surface there: towards the mean of the triangle's corners' normals; which side the
		 * ray is on is the flat triangle's to say, turned the way its normals are */
		normal = unit(add(add(scale(of(met.triangle->normal[0]), 1.0f - met.u - met.w), scale(of(met.triangle->normal[1]), met.u)), scale(of(met.triangle->normal[2]), met.w)));
		flat = cross(of(met.triangle->edge1), of(met.triangle->edge2));
		if (dot(flat, normal) < 0.0f)
			flat = scale(flat, -1.0f);
		back = dot(way, flat) > 0.0f;
		if (back)
			normal = scale(normal, -1.0f), flat = scale(flat, -1.0f);
		c = -dot(way, normal);
		if (c <= 0.0f)	/* (a ray that skims a round thing's edge: the flat triangle is what it met) */
			normal = unit(flat), c = -dot(way, normal);
		if (object->glass > 0.0f && (object->glass >= 1.0f || chance(state) < object->glass)) {
			float ratio = back ? object->ior : 1.0f / object->ior, k = 1.0f - ratio * ratio * (1.0f - c * c);
			float f0 = (object->ior - 1.0f) / (object->ior + 1.0f), turned;

			/* what the glass took of the light on the way through it, by its colour (Beer and Lambert) */
			if (back && object->density > 0.0f) {
				float through = object->density * t;

				weight = mul(weight, v(fade(through * (1.0f - colour.x)), fade(through * (1.0f - colour.y)), fade(through * (1.0f - colour.z))));
			}
			/* in or out through the surface, or back off it: more of the latter the flatter the ray */
			f0 *= f0;
			turned = f0 + (1.0f - f0) * (1.0f - c) * (1.0f - c) * (1.0f - c) * (1.0f - c) * (1.0f - c);
			if (k < 0.0f || chance(state) < turned)
				way = add(way, scale(normal, 2.0f * c));
			else
				way = unit(add(scale(way, ratio), scale(normal, ratio * c - __builtin_sqrtf(k))));
			way = roughened(way, object->rough, state);
			straight = 1;
		} else {
			if (object->metal > 0.0f && (object->metal >= 1.0f || chance(state) < object->metal))
				shiny = 1, weight = mul(weight, colour);
			else if (object->rough < 0.99f) {
				/* a coat over the colour: it mirrors more the flatter the ray */
				float f0 = (object->ior - 1.0f) / (object->ior + 1.0f);

				f0 *= f0;
				shiny = chance(state) < f0 + (1.0f - f0) * (1.0f - c) * (1.0f - c) * (1.0f - c) * (1.0f - c) * (1.0f - c);
			}
			if (shiny && object->rough < 0.15f) {
				way = roughened(add(way, scale(normal, 2.0f * c)), object->rough, state);
				straight = 1;
			} else if (shiny) {
				/* a rough shine: a light's light by how near the mirrored way it comes */
				mirror = add(way, scale(normal, 2.0f * c));
				if (lights) {
					got = light_at(at, state, rays, &to, shp);
					if (limited && sh.stopped)
						goto shade_stopped;
					facing = dot(normal, to);
					keep = dot(mirror, to);
					if (facing > 0.0f && keep > 0.0f) {
						int n = (int)(2.0f / (object->rough * object->rough * object->rough * object->rough)) - 2;

						n = n < 1 ? 1 : n;
						seen = add(seen, mul(weight, scale(got, (float)(n + 2) * 0.15915494f * power_of(keep, n) * facing)));
					}
				}
				way = roughened(mirror, object->rough, state);
				if (dot(way, normal) <= 0.0f)
					way = sub(way, scale(normal, 2.0f * dot(way, normal)));
				straight = 0, matt = 1;
			} else {
				/* a matt surface: one light's light on it, then on in a way that favours the surface's own */
				weight = mul(weight, colour);
				if (lights) {
					got = light_at(at, state, rays, &to, shp);
					if (limited && sh.stopped)
						goto shade_stopped;
					facing = dot(normal, to);
					if (facing > 0.0f)
						seen = add(seen, mul(weight, scale(got, facing * 0.31830989f)));
				}
				way = unit(add(normal, in_ball(state)));
				straight = 0, matt = 1;
			}
		}
		/* the next turn: its search from the start */
		p->from = at, p->way = way, p->seen = seen, p->straight = straight, p->matt = matt;
		p->trail = 0, p->shading = 0, p->through = v(0, 0, 0), p->crossed = 0;
		p->best = 1e30f, p->tri = -1;
		if (p->bounce >= 2) {
			/* a path that carries little ends by chance, and the others count for it */
			keep = weight.x > weight.y ? (weight.x > weight.z ? weight.x : weight.z) : (weight.y > weight.z ? weight.y : weight.z);
			if (keep < 1.0f) {
				if (chance(state) >= keep)
					return 1;
				weight = scale(weight, 1.0f / keep);
			}
		}
		p->weight = weight;
		if (++p->bounce < pt.bounces)
			(*rays)++;
		if (limited && p->bounce < pt.bounces && mcw_steps_left() < PT_LEAF_LEFT)
			return 0;	/* (between two turns: p is all there is) */
		continue;
shade_stopped:
		*state = state0, *rays = rays0, p->shading = 1;
		p->trail = sh.trail, p->through = sh.through, p->crossed = sh.crossed, p->best = t;
		p->tri = (int)(met.triangle - sc_world.triangles), p->u = met.u, p->w = met.w;
		return 0;
	}
	return 1;
}

/* The light one path brings back; *rays counts its rays. */
static inline __attribute__((always_inline)) struct vec
path(struct vec from, struct vec way, uint32_t *state, uint32_t *rays)
{
	struct pt_path p;

	path_begin(&p, from, way, 0);
	(*rays)++;
	path_run(&p, state, rays, 0);
	return p.seen;
}

static inline uint32_t
shown(float sum, float samples)
{
	float c = sum / samples;

	c = c <= 0.0f ? 0.0f : c >= 1.0f ? 1.0f : __builtin_sqrtf(c);	/* (the screen's curve, near enough) */
	return (uint32_t)(c * 255.0f);
}

/* Whether the picture is made of the sums after sample `sample` (from 0): after the first two,
 * when their number has doubled or is a multiple of sixteen, and at the last. A sample between
 * costs a pixel no quotients and roots; the first core alone (core 0) shows every one. */
static inline int
shows(uint32_t sample, uint32_t core)
{
	uint32_t count = sample + 1;

	return core == 0 || count <= 2 || !(count & sample) || !(count & 15) || count >= (uint32_t)pt.samples;
}

static const uint32_t owner_colour[16] = {
	0xffffff, 0xff4040, 0x40ff40, 0x4080ff, 0xffff40, 0xff40ff, 0x40ffff, 0xff8000,
	0x80ff00, 0x0080ff, 0xff0080, 0x8000ff, 0x00ff80, 0xc0c0c0, 0xff8080, 0x80ff80,
};

/* Sample `sample` of pixel x, y: the light its path brings back, held to PT_MOST. */
static inline __attribute__((always_inline)) struct vec
pixel_light(int x, int y, uint32_t sample, uint32_t *rays)
{
	uint32_t state = ((uint32_t)(y * pt.width + x) * 9781u + sample * 6271u + pt.seed) * 2654435761u | 1u;
	float px, py;
	struct vec c;

	/* (mixed again: every pixel's numbers are one sequence from another place, and from
	 * places a step apart a pixel's n-th would be its neighbour's and a constant) */
	state ^= state >> 15, state *= 0x2c1b3c6du, state ^= state >> 12;
	/* through a point of the pixel, not always its middle: edges come out smooth */
	px = (float)x + chance(&state);
	py = (float)y + chance(&state);
	c = path(of(sc.camera.eye), eye_way(px, py, (float)pt.width, (float)pt.height), &state, rays);
	/* (held to PT_MOST, and a sample that is no number with it) */
	c.x = __builtin_fminf(c.x, PT_MOST), c.y = __builtin_fminf(c.y, PT_MOST), c.z = __builtin_fminf(c.z, PT_MOST);
	return c;
}

uint32_t
pt_tile(uint32_t tile, uint32_t how)
{
	uint32_t sample = how & 0xffffff, core = how >> 24, rays = 0;
	int tw = pt.tile_w, th = pt.tile_h;
	int across = (pt.width + tw - 1) / tw, x0 = (int)(tile % (uint32_t)across) * tw, y0 = (int)(tile / (uint32_t)across) * th;
	int x1 = x0 + tw < pt.width ? x0 + tw : pt.width, y1 = y0 + th < pt.height ? y0 + th : pt.height, x, y;
	float samples = (float)(sample + 1);
	int show = shows(sample, core);

	for (y = y0; y < y1; y++) {
		float *sum = pt.sums + (y * pt.width + x0) * 4;
		uint32_t *out = pt.pixels + y * pt.width + x0;

		for (x = x0; x < x1; x++, sum += 4, out++) {
			uint32_t had = rays;
			struct vec c = pixel_light(x, y, sample, &rays);

			if (sample == 0)
				sum[0] = c.x, sum[1] = c.y, sum[2] = c.z, sum[3] = (float)(rays - had);
			else
				sum[0] += c.x, sum[1] += c.y, sum[2] += c.z, sum[3] += (float)(rays - had);
			if (!show)
				continue;
			if (pt.owners && (x == x0 || y == y0))
				*out = owner_colour[core & 15];
			else
				*out = shown(sum[0], samples) << 16 | shown(sum[1], samples) << 8 | shown(sum[2], samples);
		}
	}
	return rays;
}

/* A tile's pixels made of its sums as they are after `count` samples. */
static void
show_tile(uint32_t tile, uint32_t count, uint32_t core)
{
	int tw = pt.tile_w, th = pt.tile_h;
	int across = (pt.width + tw - 1) / tw, x0 = (int)(tile % (uint32_t)across) * tw, y0 = (int)(tile / (uint32_t)across) * th;
	int x1 = x0 + tw < pt.width ? x0 + tw : pt.width, y1 = y0 + th < pt.height ? y0 + th : pt.height, x, y;
	float samples = (float)count;

	for (y = y0; y < y1; y++) {
		const float *sum = pt.sums + (y * pt.width + x0) * 4;
		uint32_t *out = pt.pixels + y * pt.width + x0;

		for (x = x0; x < x1; x++, sum += 4, out++)
			if (pt.owners && (x == x0 || y == y0))
				*out = owner_colour[core & 15];
			else
				*out = shown(sum[0], samples) << 16 | shown(sum[1], samples) << 8 | shown(sum[2], samples);
	}
}

/* The tracer's code and the world are read: a worker stops at a page that no core has run or
 * read, and with workers the first core runs none of the code. A tile's making is some 8 KB
 * and the rest little: four pages either side of these is all of this file's code. */
void
pt_read_all(void)
{
	extern char __executable_start[], etext[];
	uintptr_t at[5] = {(uintptr_t)pt_tile, (uintptr_t)pt_share, (uintptr_t)world_between, (uintptr_t)pt_batch_step, (uintptr_t)pt_batch_fold}, low = at[0], high = at[0], page;
	const volatile char *part[4] = {(const char *)sc_world.nodes, (const char *)sc_world.triangles, (const char *)pt.glowing, (const char *)pt.glow_upto};
	uint32_t bytes[4] = {(uint32_t)sc_world.node_count * sizeof(struct sc_node), (uint32_t)sc_world.triangle_count * sizeof(struct sc_triangle),
			     (uint32_t)pt.glowing_count * 4, (uint32_t)pt.glowing_count * 4}, i;
	int k;

	for (k = 1; k < 5; k++) {
		if (at[k] < low) low = at[k];
		if (at[k] > high) high = at[k];
	}
	low &= ~(uintptr_t)4095, high += 0x4000;
	low = low - (uintptr_t)__executable_start > 0x4000 ? low - 0x4000 : (uintptr_t)__executable_start;
	if (high > (uintptr_t)etext)
		high = (uintptr_t)etext;
	for (page = low; page < high; page += 4096)
		(void)*(const volatile char *)page;
	for (k = 0; k < 4; k++) {
		for (i = 0; i < bytes[k]; i += 4096)
			(void)part[k][i];
		if (bytes[k])
			(void)part[k][bytes[k] - 1];
	}
}

int
pt_glowing(void)
{
	static uint32_t *triangles;
	static float *upto;
	static int room;
	float area = 0.0f;
	int i, n = 0;

	pt.glowing_count = 0;
	if (room < sc_world.triangle_count) {
		free(triangles), free(upto);
		room = sc_world.triangle_count;
		triangles = malloc((size_t)room * 4), upto = malloc((size_t)room * 4);
		if (!triangles || !upto) {
			free(triangles), free(upto);
			triangles = 0, upto = 0, room = 0;
			return 0;
		}
	}
	for (i = 0; i < sc_world.triangle_count; i++) {
		const struct sc_triangle *tri = &sc_world.triangles[i];
		struct vec flat = cross(of(tri->edge1), of(tri->edge2));

		if (sc.objects[tri->object].glow > 0.0f && dot(flat, flat) > 0.0f) {
			area += 0.5f * __builtin_sqrtf(dot(flat, flat));
			triangles[n] = (uint32_t)i, upto[n++] = area;
		}
	}
	pt.glowing = triangles, pt.glow_upto = upto, pt.glowing_count = n;
	return 1;
}

/* A worker's share of the picture (scene.h): it is never without work while the picture is
 * traced. Handed a tile at a time, a worker waited some passes for its next. */
uint32_t
pt_share(uint32_t which, uint32_t how)
{
	uint32_t k = how >> 24, limit = how & 0xffffff, n = which >> 16, w = which & 0xffff, tiles = (uint32_t)pt.tiles, did = 0;
	struct pt_slot *slot = &pt.slots[k];
	uint32_t sample, last = 0, j, at;

	for (sample = slot->samples; sample < (uint32_t)pt.samples && !pt.stop; sample++) {
		uint32_t rays = 0;

		at = w * pt.stride % tiles;
		last = sample;
		for (j = w; j < tiles; j += n) {
			if (*(volatile int *)&pt.stop > 1)
				return did;
			rays += pt_tile(at, sample | k << 24);
			if ((at += pt.step) >= tiles)
				at -= tiles;
		}
		slot->rays += rays;
		slot->samples = sample + 1;
		if (++did == limit || mcw_recalled((int)k))
			break;
	}
	/* (it ends between two samples that are shown: the picture of what its tiles have) */
	if (did && !shows(last, k))
		for (j = w, at = w * pt.stride % tiles; j < tiles; j += n) {
			show_tile(at, last + 1, k);
			if ((at += pt.step) >= tiles)
				at -= tiles;
		}
	return did;
}

/* ---- on lanes (docs/machine/lanes.md): a sample of the whole picture one job, a pixel an index ---- */

/* A step of index i of a batch: pixel i % pixels of sample s0 + i / pixels. Through: its light and its
 * rays in the first of its PT_STEP_TEXELS (nothing else written); not through: where its path is in all
 * five, the first's last word a NaN (PT_GOING) with its rays, flags and turn, the fifth what its search
 * found (not found again from the triangle: an inlined test may round otherwise). */
#define PT_GOING 0xffc00000u
uint32_t
pt_batch_step(uint32_t i, void *ctx, uint32_t round)
{
	const struct pt_batch *b = ctx;
	uint32_t p = i % b->pixels, *o = (uint32_t *)b->vals + 4 * PT_STEP_TEXELS * i, w;
	float *f = (float *)o;
	struct pt_path path;

	if (round == 0) {
		int x = (int)(p % (uint32_t)pt.width), y = (int)(p / (uint32_t)pt.width);
		uint32_t state = ((uint32_t)(y * pt.width + x) * 9781u + (b->s0 + i / b->pixels) * 6271u + pt.seed) * 2654435761u | 1u;
		float px, py;

		/* (as pixel_light() begins) */
		state ^= state >> 15, state *= 0x2c1b3c6du, state ^= state >> 12;
		px = (float)x + chance(&state);
		py = (float)y + chance(&state);
		path_begin(&path, of(sc.camera.eye), eye_way(px, py, (float)pt.width, (float)pt.height), state);
	} else {
		if ((o[3] & PT_GOING) != PT_GOING)
			return 0;	/* (through in an earlier round) */
		w = o[3];
		path.shading = (w >> 21) & 1, path.through = v(0, 0, 0), path.crossed = 0;
		if (w & 1u << 20)
			path.through = v(f[20], f[21], f[22]), path.crossed = (int)o[23];
		path.from = v(f[0], f[1], f[2]), path.way = v(f[4], f[5], f[6]), path.state = o[7];
		path.seen = v(f[8], f[9], f[10]), path.tri = (int)o[11];
		path.weight = v(f[12], f[13], f[14]), path.trail = (uint64_t)o[15] << 32 | o[19];
		path.bounce = (int)(w & 31), path.straight = (w >> 5) & 1, path.matt = (w >> 6) & 1, path.rays = (w >> 7) & 0x1fff;
		path.best = f[16], path.u = f[17], path.w = f[18];
	}
	if (!path_run(&path, &path.state, &path.rays, 1)) {
		f[0] = path.from.x, f[1] = path.from.y, f[2] = path.from.z;
		o[3] = PT_GOING | (uint32_t)path.shading << 21 | (path.crossed ? 1u << 20 : 0) | path.rays << 7 | (uint32_t)path.matt << 6 | (uint32_t)path.straight << 5 | (uint32_t)path.bounce;
		f[4] = path.way.x, f[5] = path.way.y, f[6] = path.way.z, o[7] = path.state;
		f[8] = path.seen.x, f[9] = path.seen.y, f[10] = path.seen.z, o[11] = (uint32_t)path.tri;
		f[12] = path.weight.x, f[13] = path.weight.y, f[14] = path.weight.z, o[15] = (uint32_t)(path.trail >> 32);
		f[16] = path.best, f[17] = path.u, f[18] = path.w, o[19] = (uint32_t)path.trail;
		if (path.crossed)
			f[20] = path.through.x, f[21] = path.through.y, f[22] = path.through.z, o[23] = (uint32_t)path.crossed;
		return 1;
	}
	/* (held to PT_MOST, as pixel_light() holds it) */
	f[0] = __builtin_fminf(path.seen.x, PT_MOST), f[1] = __builtin_fminf(path.seen.y, PT_MOST), f[2] = __builtin_fminf(path.seen.z, PT_MOST);
	f[3] = (float)path.rays;
	return 0;
}

/* How many of indices first.. of a batch made (in steps) are not what pixel_light() makes of them. */
uint32_t
pt_batch_check(const struct pt_batch *b, uint32_t first, uint32_t count)
{
	uint32_t i, wrong = 0;

	for (i = first; i < first + count; i++) {
		uint32_t p = i % b->pixels, rays = 0;
		const float *f = b->vals + 4 * PT_STEP_TEXELS * i;
		struct vec c = pixel_light((int)(p % (uint32_t)pt.width), (int)(p / (uint32_t)pt.width), b->s0 + i / b->pixels, &rays);

		wrong += f[0] != c.x || f[1] != c.y || f[2] != c.z || f[3] != (float)rays;
	}
	return wrong;
}

/* Pixels p0 to p1 of a batch made: into their sums in the samples' order (as a worker's tile adds
 * them), and the picture of them when `show`; a path the field left going is finished here first */
void
pt_batch_fold(const struct pt_batch *b, uint32_t p0, uint32_t p1, int show)
{
	float samples = (float)(b->s0 + b->count);
	uint32_t p, k;

	for (p = p0; p < p1; p++) {
		float *sum = pt.sums + 4 * p;
		const float *v = b->vals + 4 * PT_STEP_TEXELS * p;

		for (k = 0; k < b->count; k++, v += 4 * PT_STEP_TEXELS * b->pixels) {
			uint32_t r = 1;

			while ((((const uint32_t *)v)[3] & PT_GOING) == PT_GOING && pt_batch_step(k * b->pixels + p, (void *)b, r))
				r++;
			if (b->s0 + k == 0)
				sum[0] = v[0], sum[1] = v[1], sum[2] = v[2], sum[3] = v[3];
			else
				sum[0] += v[0], sum[1] += v[1], sum[2] += v[2], sum[3] += v[3];
		}
		if (show)
			pt.pixels[p] = shown(sum[0], samples) << 16 | shown(sum[1], samples) << 8 | shown(sum[2], samples);
	}
}

/* Whether the picture is made of the sums after sample `sample` on the lanes: as shows() has it for a worker. */
int
pt_shown_after(uint32_t sample)
{
	return shows(sample, 1);
}

void
pt_way(float x, float y, int width, int height, float *way)
{
	struct vec v = eye_way(x, y, (float)width, (float)height);

	way[0] = v.x, way[1] = v.y, way[2] = v.z;
}

int
pt_pick(float x, float y, int width, int height)
{
	struct met met = {0, 0.0f, 0.0f};
	float t;
	int what = nearest(of(sc.camera.eye), eye_way(x, y, (float)width, (float)height), &t, &met);

	return what == MET_NONE ? SC_OBJECTS : what == MET_WORLD ? (int)met.triangle->object : -1 - (what - MET_LIGHT);
}
