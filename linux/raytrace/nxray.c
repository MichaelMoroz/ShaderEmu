/*
 * nxray: a ray tracer in tiles, every tile traced by whichever worker core is free
 * (docs/raytrace.md, docs/multicore.md). The work this machine's small cores are for: a pixel
 * is a few thousand instructions of float arithmetic and four bytes stored.
 *
 * Core 0 traces nothing. It lays the workers out (the machine's geometry), hands tiles to
 * the cores that have none, shows the picture as it fills, and reads the keys:
 *   a   all the workers the geometry has      1   one worker      0   core 0 alone
 *   s   the next geometry (15 small, 3 large and 12 small, 7 large, 15 of the smallest)
 *   w   each tile framed in its core's colour  space  stop and go   q, Escape  leave
 * For tests: RAY_FRAMES=N leaves after N pictures, RAY_WORKERS=N and RAY_SHAPE=4,4,... set
 * the start, RAY_SIZE=WxH the picture (320x240), RAY_STILL=1 keeps the scene where it is.
 * Each picture's line (raystat:) has its time, its rays and a sum of its pixels, which is the
 * same whichever cores traced it.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GLES/segl.h>
#include "nano-X.h"
#include "mcw.h"

#define ONE 65536
#define TILE 32
#define SPHERES 6
#define DEPTH 3

struct vec { float x, y, z; };
struct sphere { struct vec centre; float radius, radius2; struct vec colour; float mirror; };

/* The scene and the camera of the picture being traced: core 0 writes them between two
 * pictures, the workers only read. */
static struct {
	struct sphere spheres[SPHERES];
	struct vec eye, right, up, forward, light;
	int width, height, owners;
	uint32_t *pixels;
} scene __attribute__((aligned(16)));

static inline struct vec v(float x, float y, float z) { struct vec r = {x, y, z}; return r; }
static inline struct vec add(struct vec a, struct vec b) { return v(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline struct vec sub(struct vec a, struct vec b) { return v(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline struct vec scale(struct vec a, float s) { return v(a.x * s, a.y * s, a.z * s); }
static inline float dot(struct vec a, struct vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline struct vec unit(struct vec a) { return scale(a, 1.0f / __builtin_sqrtf(dot(a, a))); }

/* The nearest thing a ray meets: a sphere's number, SPHERES for the ground, -1 for the sky. */
static int
nearest(struct vec from, struct vec way, float *distance)
{
	float best = 1e30f;
	int what = -1, i;

	for (i = 0; i < SPHERES; i++) {
		const struct sphere *s = &scene.spheres[i];
		struct vec to = sub(s->centre, from);
		float along = dot(to, way), off2 = dot(to, to) - along * along;

		if (along > 0.0f && off2 < s->radius2) {
			float t = along - __builtin_sqrtf(s->radius2 - off2);

			if (t > 0.001f && t < best)
				best = t, what = i;
		}
	}
	if (way.y < -0.0001f) {
		float t = -from.y / way.y;

		if (t > 0.001f && t < best)
			best = t, what = SPHERES;
	}
	*distance = best;
	return what;
}

/* Whether anything stands between a point and the light. */
static int
shadowed(struct vec from, struct vec way)
{
	int i;

	for (i = 0; i < SPHERES; i++) {
		const struct sphere *s = &scene.spheres[i];
		struct vec to = sub(s->centre, from);
		float along = dot(to, way);

		if (along > 0.0f && dot(to, to) - along * along < s->radius2)
			return 1;
	}
	return 0;
}

/* The colour a ray brings back; *rays counts every ray cast for it. */
static struct vec
trace(struct vec from, struct vec way, uint32_t *rays)
{
	struct vec seen = v(0, 0, 0);
	float weight = 1.0f;
	int bounce;

	for (bounce = 0; bounce < DEPTH; bounce++) {
		struct vec at, normal, colour;
		float distance, mirror, lit;
		int what = nearest(from, way, &distance);

		(*rays)++;
		if (what < 0) {
			/* the sky: paler towards the horizon */
			float high = way.y > 0.0f ? way.y : 0.0f;

			seen = add(seen, scale(v(0.75f - 0.45f * high, 0.85f - 0.35f * high, 1.0f), weight));
			break;
		}
		at = add(from, scale(way, distance));
		if (what == SPHERES) {
			/* the ground: squares of two greys */
			int cx = (int)(at.x + 1000.0f), cz = (int)(at.z + 1000.0f);

			normal = v(0, 1, 0);
			colour = (cx ^ cz) & 1 ? v(0.85f, 0.85f, 0.8f) : v(0.25f, 0.27f, 0.3f);
			mirror = 0.25f;
		} else {
			const struct sphere *s = &scene.spheres[what];

			normal = scale(sub(at, s->centre), 1.0f / s->radius);
			colour = s->colour;
			mirror = s->mirror;
		}
		lit = dot(normal, scene.light);
		(*rays)++;
		if (lit < 0.0f || shadowed(at, scene.light))
			lit = 0.0f;
		seen = add(seen, scale(colour, weight * (1.0f - mirror) * (0.2f + 0.8f * lit)));
		weight *= mirror;
		if (weight < 0.03f)
			break;
		way = sub(way, scale(normal, 2.0f * dot(way, normal)));
		from = at;
	}
	return seen;
}

static inline uint32_t
byte_of(float c)
{
	return c <= 0.0f ? 0 : c >= 1.0f ? 255 : (uint32_t)(c * 255.0f);
}

static const uint32_t owner_colour[16] = {
	0xffffff, 0xff4040, 0x40ff40, 0x4080ff, 0xffff40, 0xff40ff, 0x40ffff, 0xff8000,
	0x80ff00, 0x0080ff, 0xff0080, 0x8000ff, 0x00ff80, 0xc0c0c0, 0xff8080, 0x80ff80,
};

/*
 * A worker's job: one tile. Its rows of the picture begin and end on 16 bytes (a tile is 32
 * pixels of four bytes across, and the picture's width is a multiple of four), so no two
 * cores store to the same 16 bytes; everything else it writes is its own stack.
 */
static uint32_t
trace_tile(uint32_t tile, uint32_t core)
{
	int across = (scene.width + TILE - 1) / TILE, x0 = (int)(tile % (uint32_t)across) * TILE, y0 = (int)(tile / (uint32_t)across) * TILE;
	int x1 = x0 + TILE < scene.width ? x0 + TILE : scene.width, y1 = y0 + TILE < scene.height ? y0 + TILE : scene.height, x, y;
	float half_w = (float)scene.width * 0.5f, half_h = (float)scene.height * 0.5f, zoom = 1.0f / half_h;
	uint32_t rays = 0;

	for (y = y0; y < y1; y++) {
		uint32_t *out = scene.pixels + y * scene.width + x0;

		for (x = x0; x < x1; x++) {
			float sx = ((float)x + 0.5f - half_w) * zoom, sy = (half_h - (float)y - 0.5f) * zoom;
			struct vec way = unit(add(add(scale(scene.right, sx), scale(scene.up, sy)), scale(scene.forward, 1.6f)));
			struct vec c = trace(scene.eye, way, &rays);

			if (scene.owners && (x == x0 || y == y0))
				*out++ = owner_colour[core & 15];
			else
				*out++ = byte_of(c.x) << 16 | byte_of(c.y) << 8 | byte_of(c.z);
		}
	}
	return rays;
}

/* ---- core 0 ---- */

static GR_WINDOW_ID window;
static int workers, wanted = 99, stopped, frames_left = -1, still;
static int shape_now;
static const char *const shape_names[] = {"15 small", "3 large and 12 small", "7 large", "15 of the smallest", "the machine's own"};
static const unsigned char shapes[4][64] = {
	{4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4},
	{6, 6, 6, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4},
	{6, 6, 6, 6, 6, 6, 6, 0, 0, 0, 0, 0, 0, 0, 0},
	{3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},
};

static unsigned
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
}

/* The workers given back, the geometry set (when one is asked for), and as many taken as wanted. */
static void
take_workers(const unsigned char *shape)
{
	extern char __DATA_BEGIN__[], _end[];
	int n;

	mcw_close();
	if (shape) {
		for (n = 0; n < 63 && shape[n]; n++)
			;
		if (mcw_shape(shape, n) != 0)
			shape_now = 4;	/* a machine that is as it is, or workers someone else has */
	}
	workers = wanted > 0 ? mcw_open(wanted > 63 ? 63 : wanted) : 0;
	if (workers)
		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
}

static void
set_scene(float t)
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
	float turn = 0.25f * t;
	int i;

	for (i = 0; i < SPHERES; i++) {
		float a = made[i].speed * t + made[i].phase;

		scene.spheres[i].centre = v(made[i].ring * cosf(a), made[i].height + (i ? 0.25f * sinf(2.0f * a) + 0.25f : 0.0f), made[i].ring * sinf(a));
		scene.spheres[i].radius = made[i].radius;
		scene.spheres[i].radius2 = made[i].radius * made[i].radius;
		scene.spheres[i].colour = v(made[i].r, made[i].g, made[i].b);
		scene.spheres[i].mirror = made[i].mirror;
	}
	scene.eye = v(7.0f * cosf(turn), 2.6f, 7.0f * sinf(turn));
	scene.forward = unit(sub(target, scene.eye));
	scene.right = unit(v(-scene.forward.z, 0, scene.forward.x));
	scene.up = v(scene.right.y * scene.forward.z - scene.right.z * scene.forward.y, scene.right.z * scene.forward.x - scene.right.x * scene.forward.z,
		scene.right.x * scene.forward.y - scene.right.y * scene.forward.x);
	scene.light = unit(v(0.5f, 0.9f, -0.3f));
}

/* Keys; true when the picture being traced is to be given up for a new one. */
static int
keys(void)
{
	GR_EVENT e;
	int again = 0;

	for (;;) {
		GrCheckNextEvent(&e);
		if (e.type == GR_EVENT_TYPE_NONE)
			return again;
		if (e.type == GR_EVENT_TYPE_CLOSE_REQ)
			exit(0);
		if (e.type == GR_EVENT_TYPE_UPDATE && e.update.utype == GR_UPDATE_SIZE)
			seglWindowChanged();
		if (e.type != GR_EVENT_TYPE_KEY_DOWN)
			continue;
		switch (e.keystroke.ch) {
		case 'q': case MWKEY_ESCAPE: exit(0);
		case ' ': stopped = !stopped; break;
		case 'w': scene.owners = !scene.owners; again = 1; break;
		case 'a': wanted = 99; again = 2; break;
		case '1': wanted = 1; again = 2; break;
		case '0': wanted = 0; again = 2; break;
		case 's': shape_now = (shape_now + 1) % 4; again = 3; break;
		}
	}
}

static void
pass(void)
{
	__asm__ volatile(".word 0x0100000f");	/* pause: the machine's pass ends */
}

int
main(void)
{
	static const GLfixed whole[16] = {2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE};
	static const GLfixed corners[12] = {0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0}, uv[8] = {0, 0, ONE, 0, ONE, ONE, 0, ONE};
	const char *text;
	GR_SCREEN_INFO info;
	GLuint texture;
	int width = 320, height = 240, scale_by, tiles, frame = 0, i, view_w, view_h;
	unsigned started = 0;
	float t = 0.0f;

	if ((text = getenv("RAY_SIZE")) != NULL && sscanf(text, "%dx%d", &width, &height) == 2)
		width = (width + 3) & ~3;
	if (width < 32 || height < 32 || width > 1024 || height > 768)
		width = 320, height = 240;
	if ((text = getenv("RAY_FRAMES")) != NULL)
		frames_left = atoi(text);
	if ((text = getenv("RAY_WORKERS")) != NULL)
		wanted = atoi(text);
	still = getenv("RAY_STILL") != NULL;
	if (GrOpen() < 0) {
		fprintf(stderr, "nxray: no Nano-X server (start one: nano-X -p &)\n");
		return 1;
	}
	GrGetScreenInfo(&info);
	for (scale_by = 3; scale_by > 1 && (width * scale_by > info.cols - 20 || height * scale_by > info.rows - 60); scale_by--)
		;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Rays", GR_ROOT_WINDOW_ID, -1, -1, width * scale_by, height * scale_by, 0);
	GrSelectEvents(window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE);
	GrMapWindow(window);
	GrSetFocus(window);
	if (seglInit(window) < 0) {
		fprintf(stderr, "nxray: this machine has no GPU\n");
		return 1;
	}
	scene.width = width;
	scene.height = height;
	scene.pixels = seglMemory((unsigned)(width * height * 4));
	if (!scene.pixels) {
		fprintf(stderr, "nxray: no GPU memory left\n");
		return 1;
	}
	memset(scene.pixels, 0, (size_t)(width * height * 4));
	/* the picture is a texture where it lies: one rectangle over the window, drawn again as it fills */
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	seglTexturePointer(scene.pixels, width, height, GL_RGBA);
	seglSize(&view_w, &view_h);
	glViewport(0, 0, view_w, view_h);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	seglQuad(corners, uv, texture, 255, 255, 0);
	seglSwap();

	atexit(mcw_close);
	if ((text = getenv("RAY_SHAPE")) != NULL) {
		unsigned char bits[64] = {0};	/* (a machine of 64 cores has 63 workers) */

		for (i = 0; *text && i < 63; text++)
			if (*text >= '0' && *text <= '9')
				bits[i++] = (unsigned char)(*text - '0');
		shape_now = 4;
		take_workers(bits);
	} else {
		take_workers(shapes[shape_now]);
	}
	tiles = ((width + TILE - 1) / TILE) * ((height + TILE - 1) / TILE);
	while (frames_left != 0) {
		int busy[64], next = 0, done = 0, k, again = 0;
		unsigned began = now_ms(), shown = began, rays = 0, sum = 0, took;
		char title[120];

		set_scene(t);
		for (k = 0; k < 64; k++)
			busy[k] = -1;
		while (done < tiles && !again) {
			int idle = 1;

			if (workers == 0) {
				/* core 0 alone: a tile, then a look at the window */
				rays += trace_tile((uint32_t)next++, 0);
				done++;
				idle = 0;
			}
			for (k = 1; k <= workers; k++) {
				if (busy[k] >= 0 && mcw_done(k)) {
					rays += mcw_result(k);
					busy[k] = -1;
					done++;
				}
				if (busy[k] < 0 && next < tiles) {
					busy[k] = next;
					mcw_post(k, trace_tile, (uint32_t)next++, (uint32_t)k);
					idle = 0;
				}
			}
			if (now_ms() - shown >= 200) {
				seglSwapAgain();
				shown = now_ms();
				again = keys();
			} else if (idle) {
				pass();
			}
		}
		/* a picture given up: the tiles still out are waited for (their cores must be free) */
		for (k = 1; k <= workers; k++)
			if (busy[k] >= 0) {
				mcw_wait(k);
				rays += mcw_result(k);
			}
		took = now_ms() - began;
		seglSwapAgain();
		if (again >= 2) {
			take_workers(again == 3 ? shapes[shape_now] : NULL);
			continue;
		}
		if (again)
			continue;
		for (i = 0; i < width * height; i++)
			sum = sum * 31 + scene.pixels[i];
		snprintf(title, sizeof title, "Rays - %d workers (%s): %u.%u s a picture, %u thousand rays a second", workers, shape_names[shape_now],
			took / 1000, took % 1000 / 100, took ? rays / took : 0);
		GrSetWindowTitle(window, title);
		GrFlush();
		fprintf(stderr, "raystat: picture %d, %d x %d: %u ms with %d workers (%s), %u rays, %u thousand rays a second, sum %08x\n", frame, width,
			height, took, workers, shape_names[shape_now], rays, took ? rays / took : 0, sum);
		frame++;
		if (frames_left > 0)
			frames_left--;
		if (!still)
			t += 0.35f;
		(void)started;
		while (stopped && frames_left != 0) {
			pass();
			if (keys())
				break;
		}
	}
	return 0;
}
