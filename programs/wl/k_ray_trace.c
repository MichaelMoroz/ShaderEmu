/*
 * ray_trace: the machine's ray tracer (linux/raytrace/nxray.c). Copied: struct vec and its helpers,
 * nearest(), shadowed(), nearest_seen(), nearest_mirrored(), shadowed_ground(), trace(), byte_of(),
 * trace_texel() (its owner colours and lane job left out, the output given apart), set_scene()
 * with sin_cos(), span() and set_spared(). The scene is nxray's at t = 1.5 in its 320 x 240 picture;
 * a unit traces a tile of 32 x 24 pixels (192 texels of four pixels) of the middle, every ray of it
 * at depth 3 with shadows, into the core's own tile.
 */
#include <stdint.h>
#include <string.h>
#include "wl.h"

#define SPHERES 6
#define DEPTH 3
#define SHADE 16
#define WIDTH 320
#define HEIGHT 240
#define TILE_X 144   /* the tile: from pixel (TILE_X, TILE_Y), TILE_W x TILE_H */
#define TILE_Y 100
#define TILE_W 32
#define TILE_H 24

struct vec { float x, y, z; };
struct sphere { struct vec centre; float radius, radius2; struct vec colour; float mirror; };

static struct picture {
	struct sphere spheres[SPHERES];
	struct vec eye, right, up, forward, light;
	int width, height;
	int depth, shadows;
	struct { struct vec to; float to2; int x0, x1, y0, y1; } seen[SPHERES];
	struct { int x0, x1, y0, y1; } mirrored[SPHERES];
	float shade_x, shade_z, shade_by;
	uint8_t shade[SHADE * SHADE];
} picture __attribute__((aligned(16)));

static inline struct vec v(float x, float y, float z) { struct vec r = {x, y, z}; return r; }
static inline struct vec add(struct vec a, struct vec b) { return v(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline struct vec sub(struct vec a, struct vec b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline struct vec scale(struct vec a, float s) { return v(a.x * s, a.y * s, a.z * s); }
static inline float dot(struct vec a, struct vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline struct vec unit(struct vec a) { return scale(a, 1.0f / __builtin_sqrtf(dot(a, a))); }

static int nearest(const struct picture *sc, struct vec from, struct vec way, float *distance)
{
	float best = 1e30f;
	int what = -1, i;
	for (i = 0; i < SPHERES; i++) {
		const struct sphere *s = &sc->spheres[i];
		struct vec to = sub(s->centre, from);
		float along = dot(to, way), off2 = dot(to, to) - along * along;
		if (along > 0.0f && off2 < s->radius2) {
			float t = along - __builtin_sqrtf(s->radius2 - off2);
			if (t > 0.001f && t < best) best = t, what = i;
		}
	}
	if (way.y < -0.0001f) {
		float t = -from.y / way.y;
		if (t > 0.001f && t < best) best = t, what = SPHERES;
	}
	*distance = best;
	return what;
}

static int shadowed(const struct picture *sc, struct vec from, struct vec way)
{
	for (int i = 0; i < SPHERES; i++) {
		const struct sphere *s = &sc->spheres[i];
		struct vec to = sub(s->centre, from);
		float along = dot(to, way);
		if (along > 0.0f && dot(to, to) - along * along < s->radius2) return 1;
	}
	return 0;
}

static inline int nearest_seen(const struct picture *sc, struct vec way, int x, const uint8_t *row, int rows, float *distance)
{
	float best = 1e30f;
	int what = -1, k;
	for (k = 0; k < rows; k++) {
		int i = row[k];
		float along, off2, radius2 = sc->spheres[i].radius2;
		if (x < sc->seen[i].x0 || x > sc->seen[i].x1) continue;
		along = dot(sc->seen[i].to, way), off2 = sc->seen[i].to2 - along * along;
		if (along > 0.0f && off2 < radius2) {
			float t = along - __builtin_sqrtf(radius2 - off2);
			if (t > 0.001f && t < best) best = t, what = i;
		}
	}
	if (way.y < -0.0001f) {
		float t = -sc->eye.y / way.y;
		if (t > 0.001f && t < best) best = t, what = SPHERES;
	}
	*distance = best;
	return what;
}

static inline int nearest_mirrored(const struct picture *sc, struct vec from, struct vec way, int x, const uint8_t *row, int rows, float *distance)
{
	float best = 1e30f;
	int what = -1, k;
	for (k = 0; k < rows; k++) {
		int i = row[k];
		const struct sphere *s = &sc->spheres[i];
		struct vec to;
		float along, off2;
		if (x < sc->mirrored[i].x0 || x > sc->mirrored[i].x1) continue;
		to = sub(s->centre, from);
		along = dot(to, way), off2 = dot(to, to) - along * along;
		if (along > 0.0f && off2 < s->radius2) {
			float t = along - __builtin_sqrtf(s->radius2 - off2);
			if (t > 0.001f && t < best) best = t, what = i;
		}
	}
	*distance = best;
	return what;
}

static inline int shadowed_ground(const struct picture *sc, struct vec from)
{
	float gx = (from.x - sc->shade_x) * sc->shade_by, gz = (from.z - sc->shade_z) * sc->shade_by;
	unsigned mask;
	int i;
	if (!(gx >= 0.0f && gx < (float)SHADE && gz >= 0.0f && gz < (float)SHADE)) return 0;
	for (i = 0, mask = sc->shade[(int)gz * SHADE + (int)gx]; mask; i++, mask >>= 1)
		if (mask & 1) {
			const struct sphere *s = &sc->spheres[i];
			struct vec to = sub(s->centre, from);
			float along = dot(to, sc->light);
			if (along > 0.0f && dot(to, to) - along * along < s->radius2) return 1;
		}
	return 0;
}

static inline struct vec trace(const struct picture *sc, struct vec from, struct vec way, uint32_t *rays, int x,
                               const uint8_t *row, int rows, int rows_mirrored)
{
	struct vec seen = v(0, 0, 0);
	float weight = 1.0f;
	int bounce, ground = 0;
	for (bounce = 0; bounce < sc->depth; bounce++) {
		struct vec at, normal, colour;
		float distance, mirror, lit;
		int what = bounce == 0 ? nearest_seen(sc, way, x, row, rows, &distance)
		           : ground ? nearest_mirrored(sc, from, way, x, row + rows, rows_mirrored, &distance) : nearest(sc, from, way, &distance);
		(*rays)++;
		if (what < 0) {
			float high = way.y > 0.0f ? way.y : 0.0f;
			seen = add(seen, scale(v(0.75f - 0.45f * high, 0.85f - 0.35f * high, 1.0f), weight));
			break;
		}
		at = add(from, scale(way, distance));
		ground = bounce == 0 && what == SPHERES;
		if (what == SPHERES) {
			int cx = (int)(at.x + 1000.0f), cz = (int)(at.z + 1000.0f);
			normal = v(0, 1, 0);
			colour = (cx ^ cz) & 1 ? v(0.85f, 0.85f, 0.8f) : v(0.25f, 0.27f, 0.3f);
			mirror = 0.25f;
		} else {
			const struct sphere *s = &sc->spheres[what];
			normal = scale(sub(at, s->centre), 1.0f / s->radius);
			colour = s->colour;
			mirror = s->mirror;
		}
		lit = dot(normal, sc->light);
		*rays += (uint32_t)sc->shadows;
		if (lit < 0.0f || (sc->shadows && (what == SPHERES ? shadowed_ground(sc, at) : shadowed(sc, at, sc->light))))
			lit = 0.0f;
		seen = add(seen, scale(colour, weight * (1.0f - mirror) * (0.2f + 0.8f * lit)));
		weight *= mirror;
		if (weight < 0.03f) break;
		way = sub(way, scale(normal, 2.0f * dot(way, normal)));
		from = at;
	}
	return seen;
}

static inline uint32_t byte_of(float c)
{
	return (uint32_t)(__builtin_fminf(__builtin_fmaxf(c, 0.0f), 1.0f) * 255.0f);
}

/* nxray's trace_texel(): pixels p to p + 3 of the picture, into out[0..3] */
static void trace_texel(const struct picture *sc, uint32_t p, uint32_t *out)
{
	uint32_t rays = 0, word[4];
	int y = (int)(p / (uint32_t)sc->width), x0 = (int)(p % (uint32_t)sc->width), x1 = x0 + 4, x, k;
	float half_w = (float)sc->width * 0.5f, half_h = (float)sc->height * 0.5f, zoom = 1.0f / half_h;
	uint8_t row[2 * SPHERES];
	int rows = 0, rows_mirrored = 0;
	for (k = 0; k < SPHERES; k++)
		if (y >= sc->seen[k].y0 && y <= sc->seen[k].y1 && x1 > sc->seen[k].x0 && x0 <= sc->seen[k].x1)
			row[rows++] = (uint8_t)k;
	for (k = 0; k < SPHERES; k++)
		if (y >= sc->mirrored[k].y0 && y <= sc->mirrored[k].y1 && x1 > sc->mirrored[k].x0 && x0 <= sc->mirrored[k].x1)
			row[rows + rows_mirrored++] = (uint8_t)k;
	for (x = x0; x < x1; x++) {
		float sx = ((float)x + 0.5f - half_w) * zoom, sy = (half_h - (float)y - 0.5f) * zoom;
		struct vec way = unit(add(add(scale(sc->right, sx), scale(sc->up, sy)), scale(sc->forward, 1.6f)));
		struct vec c = trace(sc, sc->eye, way, &rays, x, row, rows, rows_mirrored);
		word[x - x0] = byte_of(c.x) << 16 | byte_of(c.y) << 8 | byte_of(c.z);
	}
	out[0] = word[0] | rays << 24;
	out[1] = word[1];
	out[2] = word[2];
	out[3] = word[3];
}

static void sin_cos(float a, float *s, float *c)
{
	int k = (int)(a * 0.63661977f + (a >= 0.0f ? 0.5f : -0.5f));
	float r = ((a - (float)k * 1.5703125f) - (float)k * 4.83751296997e-4f) - (float)k * 7.54978995489e-8f, r2 = r * r;
	float sr = r + r * r2 * (-1.6666654611e-1f + r2 * (8.3321608736e-3f + r2 * -1.9515295891e-4f));
	float cr = 1.0f - 0.5f * r2 + r2 * r2 * (4.166664568298827e-2f + r2 * (-1.388731625493765e-3f + r2 * 2.443315711809948e-5f));
	switch (k & 3) {
	case 0: *s = sr, *c = cr; break;
	case 1: *s = cr, *c = -sr; break;
	case 2: *s = -sr, *c = -cr; break;
	default: *s = -cr, *c = sr; break;
	}
}

static void set_scene(struct picture *sc, float t)
{
	static const struct { float ring, height, radius, speed, phase, r, g, b, mirror; } made[SPHERES] = {
		{0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.9f, 0.9f, 0.95f, 0.75f},
		{2.3f, 0.6f, 0.6f, 0.9f, 0.0f, 0.95f, 0.25f, 0.2f, 0.3f},
		{2.3f, 0.6f, 0.6f, 0.9f, 2.1f, 0.2f, 0.8f, 0.3f, 0.3f},
		{2.3f, 0.6f, 0.6f, 0.9f, 4.2f, 0.25f, 0.4f, 0.95f, 0.3f},
		{3.8f, 0.4f, 0.4f, -0.6f, 1.0f, 0.95f, 0.8f, 0.2f, 0.5f},
		{3.8f, 0.4f, 0.4f, -0.6f, 4.1f, 0.8f, 0.3f, 0.9f, 0.5f},
	};
	struct vec target = v(0, 0.8f, 0);
	float turn = 0.25f * t, turn_s, turn_c;
	for (int i = 0; i < SPHERES; i++) {
		float a = made[i].speed * t + made[i].phase, s, c;
		sin_cos(a, &s, &c);
		sc->spheres[i].centre = v(made[i].ring * c, made[i].height + (i ? 0.5f * s * c + 0.25f : 0.0f), made[i].ring * s);
		sc->spheres[i].radius = made[i].radius;
		sc->spheres[i].radius2 = made[i].radius * made[i].radius;
		sc->spheres[i].colour = v(made[i].r, made[i].g, made[i].b);
		sc->spheres[i].mirror = made[i].mirror;
	}
	sin_cos(turn, &turn_s, &turn_c);
	sc->eye = v(7.0f * turn_c, 2.6f, 7.0f * turn_s);
	sc->forward = unit(sub(target, sc->eye));
	sc->right = unit(v(-sc->forward.z, 0, sc->forward.x));
	sc->up = v(sc->right.y * sc->forward.z - sc->right.z * sc->forward.y, sc->right.z * sc->forward.x - sc->right.x * sc->forward.z,
		sc->right.x * sc->forward.y - sc->right.y * sc->forward.x);
	sc->light = unit(v(0.5f, 0.9f, -0.3f));
}

static void span(float c, float z, float r, float per, float middle, int most, int *low, int *high)
{
	float den = z * z - r * r, root, a, b;
	*low = 0, *high = most;
	if (z < r * 1.01f + 0.01f) return;
	root = r * __builtin_sqrtf(c * c + den);
	a = middle + per * 1.6f * (c * z - root) / den, b = middle + per * 1.6f * (c * z + root) / den;
	if (a > b) { float swap = a; a = b, b = swap; }
	*low = a > (float)most + 8.0f ? most + 1 : a > -8.0f ? (int)a - 4 : 0;
	*high = b < -8.0f ? -1 : b < (float)most + 8.0f ? (int)b + 4 : most;
}

static void set_spared(struct picture *sc)
{
	float half_w = (float)sc->width * 0.5f, half_h = (float)sc->height * 0.5f, low_x = 1e30f, low_z = 1e30f, high_x = -1e30f, high_z = -1e30f;
	float by = 1.0f / sc->light.y, spot[SPHERES][3];
	int i, x, z;
	for (i = 0; i < SPHERES; i++) {
		const struct sphere *s = &sc->spheres[i];
		struct vec to = sub(s->centre, sc->eye), under = sub(v(s->centre.x, -s->centre.y, s->centre.z), sc->eye);
		float ahead = dot(to, sc->forward), under_ahead = dot(under, sc->forward);
		sc->seen[i].to = to;
		sc->seen[i].to2 = dot(to, to);
		span(dot(to, sc->right), ahead, s->radius, half_h, half_w - 0.5f, sc->width - 1, &sc->seen[i].x0, &sc->seen[i].x1);
		span(dot(to, sc->up), ahead, s->radius, -half_h, half_h - 0.5f, sc->height - 1, &sc->seen[i].y0, &sc->seen[i].y1);
		span(dot(under, sc->right), under_ahead, s->radius, half_h, half_w - 0.5f, sc->width - 1, &sc->mirrored[i].x0, &sc->mirrored[i].x1);
		span(dot(under, sc->up), under_ahead, s->radius, -half_h, half_h - 0.5f, sc->height - 1, &sc->mirrored[i].y0, &sc->mirrored[i].y1);
		spot[i][0] = s->centre.x - sc->light.x * s->centre.y * by;
		spot[i][1] = s->centre.z - sc->light.z * s->centre.y * by;
		spot[i][2] = s->radius * by * 1.01f + 0.01f;
		if (spot[i][0] - spot[i][2] < low_x) low_x = spot[i][0] - spot[i][2];
		if (spot[i][1] - spot[i][2] < low_z) low_z = spot[i][1] - spot[i][2];
		if (spot[i][0] + spot[i][2] > high_x) high_x = spot[i][0] + spot[i][2];
		if (spot[i][1] + spot[i][2] > high_z) high_z = spot[i][1] + spot[i][2];
	}
	sc->shade_x = low_x;
	sc->shade_z = low_z;
	sc->shade_by = (float)SHADE / (high_x - low_x > high_z - low_z ? high_x - low_x : high_z - low_z);
	memset(sc->shade, 0, sizeof sc->shade);
	for (i = 0; i < SPHERES; i++) {
		int x0 = (int)((spot[i][0] - spot[i][2] - low_x) * sc->shade_by), x1 = (int)((spot[i][0] + spot[i][2] - low_x) * sc->shade_by);
		int z0 = (int)((spot[i][1] - spot[i][2] - low_z) * sc->shade_by), z1 = (int)((spot[i][1] + spot[i][2] - low_z) * sc->shade_by);
		for (z = z0 < 0 ? 0 : z0; z <= z1 && z < SHADE; z++)
			for (x = x0 < 0 ? 0 : x0; x <= x1 && x < SHADE; x++)
				sc->shade[z * SHADE + x] |= (uint8_t)(1 << i);
	}
}

static void setup(void)
{
	memset(&picture, 0, sizeof picture);
	picture.width = WIDTH, picture.height = HEIGHT;
	picture.depth = DEPTH, picture.shadows = 1;
	set_scene(&picture, 1.5f);
	set_spared(&picture);
}

static void *inst(void) { return wl_alloc(TILE_W * TILE_H * 4); }

static uint32_t run(void *ctx)
{
	uint32_t *tile = ctx, sum = 0;
	for (int y = 0; y < TILE_H; y++)
		for (int x = 0; x < TILE_W; x += 4)
			trace_texel(&picture, (uint32_t)((TILE_Y + y) * WIDTH + TILE_X + x), tile + y * TILE_W + x);
	for (int i = 0; i < TILE_W * TILE_H; i++) sum = sum * 33 + tile[i];
	return sum;
}

const wl_kernel wl_ray_trace = {"ray_trace", "nxray: trace_texel, spheres, shadows, mirrors", setup, inst, run};
