/*
 * The path tracer's own part (docs/raytrace.md): a Cornell box, and one sample of every
 * pixel of a tile as a job for a worker core (docs/multicore.md). A pixel is a few thousand
 * instructions of float arithmetic and sixteen bytes stored, which is the work the machine's
 * small cores are for.
 *
 * What a job writes: its tile's part of the sums (three floats a pixel) and of the picture
 * (a word a pixel), and its own stack. A tile is 32 pixels across and the picture's width a
 * multiple of four, so a tile's rows begin and end on 16 bytes of both and no two cores store
 * to the same 16. The scene is only read.
 */
#include "tracer.h"

struct pt_scene pt;

struct vec { float x, y, z; };
struct hit { float t; struct vec normal, colour; int material; float glow; };

static inline struct vec v(float x, float y, float z) { struct vec r = {x, y, z}; return r; }
static inline struct vec add(struct vec a, struct vec b) { return v(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline struct vec sub(struct vec a, struct vec b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline struct vec mul(struct vec a, struct vec b) { return v(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline struct vec scale(struct vec a, float s) { return v(a.x * s, a.y * s, a.z * s); }
static inline float dot(struct vec a, struct vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline struct vec unit(struct vec a) { return scale(a, 1.0f / __builtin_sqrtf(dot(a, a))); }

/* The box is the unit cube, open towards the eye (z = 0); the lamp a square of the ceiling. */
#define PT_MOST 2.5f
#define LAMP_X0 0.35f
#define LAMP_X1 0.65f
#define LAMP_Z0 0.35f
#define LAMP_Z1 0.65f
static const struct vec white = {0.75f, 0.75f, 0.75f}, red = {0.70f, 0.12f, 0.10f}, green = {0.14f, 0.55f, 0.16f};
/* two balls and a block */
static const struct { struct vec centre; float radius; } balls[2] = {{{0.30f, 0.18f, 0.62f}, 0.18f}, {{0.72f, 0.14f, 0.35f}, 0.14f}};
static const struct vec block_low = {0.55f, 0.0f, 0.60f}, block_high = {0.82f, 0.50f, 0.86f};

static inline uint32_t
next(uint32_t *state)
{
	uint32_t x = *state;

	x ^= x << 13, x ^= x >> 17, x ^= x << 5;
	return *state = x;
}

static inline float
chance(uint32_t *state)
{
	return (float)(next(state) >> 8) * (1.0f / 16777216.0f);
}

/* A point of the unit ball, by throwing away the cube's corners. */
static struct vec
in_ball(uint32_t *state)
{
	for (;;) {
		struct vec p = v(2.0f * chance(state) - 1.0f, 2.0f * chance(state) - 1.0f, 2.0f * chance(state) - 1.0f);
		float d = dot(p, p);

		if (d < 1.0f && d > 0.0001f)
			return scale(p, 1.0f / __builtin_sqrtf(d));
	}
}

static inline void
wall(float t, struct vec normal, struct vec colour, struct hit *h)
{
	if (t > 0.0005f && t < h->t) {
		h->t = t;
		h->normal = normal;
		h->colour = colour;
		h->material = PT_DIFFUSE;
		h->glow = 0.0f;
	}
}

/* The nearest thing a ray meets; false for the open side. */
static int
nearest(struct vec from, struct vec way, struct hit *h)
{
	int i, axis;
	float low = -1e30f, high = 1e30f;
	struct vec low_normal = v(0, 0, 0);

	h->t = 1e30f;
	h->glow = 0.0f;
	h->material = PT_DIFFUSE;
	h->normal = h->colour = v(0, 0, 0);
	/* the room: a ray inside it leaves by one wall of each pair */
	if (way.x < 0.0f) wall(-from.x / way.x, v(1, 0, 0), red, h);
	else if (way.x > 0.0f) wall((1.0f - from.x) / way.x, v(-1, 0, 0), green, h);
	if (way.y < 0.0f) wall(-from.y / way.y, v(0, 1, 0), white, h);
	else if (way.y > 0.0f) wall((1.0f - from.y) / way.y, v(0, -1, 0), white, h);
	if (way.z > 0.0f) wall((1.0f - from.z) / way.z, v(0, 0, -1), white, h);
	if (h->t < 1e29f && h->normal.y < -0.5f) {
		/* the ceiling: the lamp is a square of it */
		float x = from.x + way.x * h->t, z = from.z + way.z * h->t;

		if (x > LAMP_X0 && x < LAMP_X1 && z > LAMP_Z0 && z < LAMP_Z1)
			h->glow = pt.light;
	}
	for (i = 0; i < 2; i++) {
		struct vec to = sub(balls[i].centre, from);
		float along = dot(to, way), off2 = dot(to, to) - along * along, r2 = balls[i].radius * balls[i].radius;

		if (off2 < r2) {
			float half = __builtin_sqrtf(r2 - off2), t = along - half;

			if (t < 0.0005f)
				t = along + half;	/* from inside it (glass) */
			if (t > 0.0005f && t < h->t) {
				h->t = t;
				h->normal = scale(sub(add(from, scale(way, t)), balls[i].centre), 1.0f / balls[i].radius);
				h->colour = v(0.92f, 0.92f, 0.92f);
				h->material = pt.material[i];
				h->glow = 0.0f;
			}
		}
	}
	/* the block: where the ray is inside all three pairs of its sides */
	for (axis = 0; axis < 3; axis++) {
		float o = axis == 0 ? from.x : axis == 1 ? from.y : from.z, d = axis == 0 ? way.x : axis == 1 ? way.y : way.z;
		float a = axis == 0 ? block_low.x : axis == 1 ? block_low.y : block_low.z;
		float b = axis == 0 ? block_high.x : axis == 1 ? block_high.y : block_high.z, t0, t1, sign = -1.0f;

		if (d > -1e-6f && d < 1e-6f) {
			if (o < a || o > b)
				return h->t < 1e29f;
			continue;
		}
		t0 = (a - o) / d, t1 = (b - o) / d;
		if (t0 > t1) {
			float swap = t0;

			t0 = t1, t1 = swap, sign = 1.0f;
		}
		if (t0 > low) {
			low = t0;
			low_normal = axis == 0 ? v(sign, 0, 0) : axis == 1 ? v(0, sign, 0) : v(0, 0, sign);
		}
		if (t1 < high)
			high = t1;
	}
	if (low < high && low > 0.0005f && low < h->t) {
		h->t = low;
		h->normal = low_normal;
		h->colour = v(0.72f, 0.68f, 0.50f);
		h->material = PT_DIFFUSE;
		h->glow = 0.0f;
	}
	return h->t < 1e29f;
}

/* Whether a ball or the block stands between a point and a point of the lamp, `far` away. */
static int
shadowed(struct vec from, struct vec way, float far)
{
	int i, axis;
	float low = 0.0005f, high = far;

	for (i = 0; i < 2; i++) {
		struct vec to = sub(balls[i].centre, from);
		float along = dot(to, way), off2 = dot(to, to) - along * along, r2 = balls[i].radius * balls[i].radius;

		if (pt.material[i] != PT_GLASS && along > 0.0f && off2 < r2 && along - __builtin_sqrtf(r2 - off2) < far && along > 0.001f)
			return 1;
	}
	for (axis = 0; axis < 3; axis++) {
		float o = axis == 0 ? from.x : axis == 1 ? from.y : from.z, d = axis == 0 ? way.x : axis == 1 ? way.y : way.z;
		float a = axis == 0 ? block_low.x : axis == 1 ? block_low.y : block_low.z;
		float b = axis == 0 ? block_high.x : axis == 1 ? block_high.y : block_high.z, t0, t1;

		if (d > -1e-6f && d < 1e-6f) {
			if (o < a || o > b)
				return 0;
			continue;
		}
		t0 = (a - o) / d, t1 = (b - o) / d;
		if (t0 > t1) {
			float swap = t0;

			t0 = t1, t1 = swap;
		}
		if (t0 > low) low = t0;
		if (t1 < high) high = t1;
	}
	return low < high;
}

/* The light one path brings back; *rays counts its rays. */
static struct vec
path(struct vec from, struct vec way, uint32_t *state, uint32_t *rays)
{
	struct vec seen = v(0, 0, 0), weight = v(1, 1, 1);
	int bounce, straight = 1;	/* the last turn was a mirror's or the eye's: a lamp met now is seen */

	for (bounce = 0; bounce < pt.bounces; bounce++) {
		struct hit h;
		struct vec at;

		(*rays)++;
		if (!nearest(from, way, &h))
			break;
		if (h.glow > 0.0f) {
			if (straight)
				seen = add(seen, scale(weight, h.glow));
			break;
		}
		at = add(from, scale(way, h.t));
		if (h.material == PT_MIRROR) {
			way = sub(way, scale(h.normal, 2.0f * dot(way, h.normal)));
			weight = mul(weight, h.colour);
			from = at;
			straight = 1;
			continue;
		}
		if (h.material == PT_GLASS) {
			/* in or out through the surface, or back off it: more of the latter the flatter the ray */
			float c = -dot(way, h.normal), ratio = 1.0f / 1.5f, k, mirror;
			struct vec normal = h.normal;

			if (c < 0.0f)
				c = -c, normal = scale(normal, -1.0f), ratio = 1.5f;
			k = 1.0f - ratio * ratio * (1.0f - c * c);
			mirror = 0.04f + 0.96f * (1.0f - c) * (1.0f - c) * (1.0f - c) * (1.0f - c) * (1.0f - c);
			if (k < 0.0f || chance(state) < mirror)
				way = add(way, scale(normal, 2.0f * c));
			else
				way = unit(add(scale(way, ratio), scale(normal, ratio * c - __builtin_sqrtf(k))));
			from = at;
			straight = 1;
			continue;
		}
		/* a matt surface: the lamp's light on it, by a ray to a point of the lamp */
		weight = mul(weight, h.colour);
		{
			struct vec lamp = v(LAMP_X0 + (LAMP_X1 - LAMP_X0) * chance(state), 0.999f, LAMP_Z0 + (LAMP_Z1 - LAMP_Z0) * chance(state));
			struct vec to = sub(lamp, at);
			float d2 = dot(to, to), d = __builtin_sqrtf(d2), facing, down;

			to = scale(to, 1.0f / d);
			facing = dot(h.normal, to);
			down = to.y;	/* the lamp faces down: how squarely this ray leaves it */
			(*rays)++;
			if (facing > 0.0f && down > 0.0f && !shadowed(at, to, d - 0.002f)) {
				float area = (LAMP_X1 - LAMP_X0) * (LAMP_Z1 - LAMP_Z0);

				seen = add(seen, scale(weight, pt.light * facing * down * area / (3.14159265f * d2)));
			}
		}
		/* and on in a direction that favours the surface's own */
		way = unit(add(h.normal, in_ball(state)));
		from = at;
		straight = 0;
		if (bounce >= 2) {
			/* a path that carries little ends by chance, and the others count for it */
			float keep = weight.x > weight.y ? (weight.x > weight.z ? weight.x : weight.z) : (weight.y > weight.z ? weight.y : weight.z);

			if (chance(state) >= keep)
				break;
			weight = scale(weight, 1.0f / keep);
		}
	}
	return seen;
}

static inline uint32_t
shown(float sum, float samples)
{
	float c = sum / samples;

	c = c <= 0.0f ? 0.0f : c >= 1.0f ? 1.0f : __builtin_sqrtf(c);	/* (the screen's curve, near enough) */
	return (uint32_t)(c * 255.0f);
}

static const uint32_t owner_colour[16] = {
	0xffffff, 0xff4040, 0x40ff40, 0x4080ff, 0xffff40, 0xff40ff, 0x40ffff, 0xff8000,
	0x80ff00, 0x0080ff, 0xff0080, 0x8000ff, 0x00ff80, 0xc0c0c0, 0xff8080, 0x80ff80,
};

/* One more sample of every pixel of a tile. how: the sample's number, and the core's in the top byte. */
uint32_t
pt_tile(uint32_t tile, uint32_t how)
{
	uint32_t sample = how & 0xffffff, core = how >> 24, rays = 0;
	int across = (pt.width + PT_TILE - 1) / PT_TILE, x0 = (int)(tile % (uint32_t)across) * PT_TILE, y0 = (int)(tile / (uint32_t)across) * PT_TILE;
	int x1 = x0 + PT_TILE < pt.width ? x0 + PT_TILE : pt.width, y1 = y0 + PT_TILE < pt.height ? y0 + PT_TILE : pt.height, x, y;
	float half_w = (float)pt.width * 0.5f, half_h = (float)pt.height * 0.5f, zoom = 0.5f / half_h, samples = (float)(sample + 1);
	const struct vec eye = v(0.5f, 0.5f, -1.35f);

	for (y = y0; y < y1; y++) {
		float *sum = pt.sums + (y * pt.width + x0) * 3;
		uint32_t *out = pt.pixels + y * pt.width + x0;

		for (x = x0; x < x1; x++, sum += 3) {
			uint32_t state = ((uint32_t)(y * pt.width + x) * 9781u + sample * 6271u + pt.seed) * 2654435761u | 1u;
			float sx, sy;
			struct vec c;

			next(&state);
			/* through a point of the pixel, not always its middle: edges come out smooth */
			sx = ((float)x + chance(&state) - half_w) * zoom;
			sy = (half_h - (float)y - chance(&state)) * zoom;
			c = path(eye, unit(v(sx, sy, 1.35f)), &state, &rays);
			/* one path in thousands finds the lamp by a mirror or through glass and is worth a
			 * hundred others: held to what a bright wall is, or it is a white speck for good */
			if (c.x > PT_MOST) c.x = PT_MOST;
			if (c.y > PT_MOST) c.y = PT_MOST;
			if (c.z > PT_MOST) c.z = PT_MOST;
			if (sample == 0)
				sum[0] = c.x, sum[1] = c.y, sum[2] = c.z;
			else
				sum[0] += c.x, sum[1] += c.y, sum[2] += c.z;
			if (pt.owners && (x == x0 || y == y0))
				*out++ = owner_colour[core & 15];
			else
				*out++ = shown(sum[0], samples) << 16 | shown(sum[1], samples) << 8 | shown(sum[2], samples);
		}
	}
	return rays;
}
