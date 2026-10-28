/*
 * A "shader" on the processors: shade() is called for every pixel of the picture, as a
 * fragment shader is, on the machine's lanes: the field's, some 3,900 pixels of the machine
 * at once each running it for pixels of its own, or else the worker cores' (mcw_shade, mcwx.h;
 * docs/machine/lanes.md). This one traces a ray from the eye to two balls over a chequered
 * floor, with their shadows. Change shade() and Run again. Keys: q leaves, space stops and goes.
 *
 * What a shader may do: compute and read; store nothing but its own variables (no globals, no
 * printf). What changes from picture to picture comes in `struct frame`. Four pixels side by
 * side are a lane's at a time and must be done in one pass of the machine, 8,192 instructions:
 * some 2,000 a pixel as this machine's own compiler makes them (four that take longer are made,
 * much more slowly, by the core that asked).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define MWINCLUDECOLORS
#include <nano-X.h>
#include <GLES/segl.h>
#include <mcw.h>
#include <mcwx.h>

#define WIDTH 240	/* (a multiple of four) */
#define HEIGHT 180
#define SCALE 3
#define ONE 65536
#define MIRRORED 0	/* how many times the floor throws a ray back (1 doubles a floor pixel's cost) */

/* What the shader is told for a picture: the time, the eye's place and ways, the balls. */
struct frame {
	float t;
	float eye[3], fwd[3], right[3], up[3];
	float ball[2][4];	/* centre and radius */
};

/* A wave from -1 to 1 that comes round once for every 1 of v. Not sinf(): the C library works
 * a sine out in doubles, which this machine has no instructions for (a double is a call of
 * hundreds of instructions each time). This is a few floats. */
static float wave(float v)
{
	float p = 2.0f * (v - (float)(int)v) - 1.0f;

	if (p < -1.0f)
		p += 2.0f;
	return 4.0f * p * (1.0f - (p < 0.0f ? -p : p));
}

/* How far along the ray from o the way d (one long) ball b is met, or -1. */
static float meet(const float *b, float ox, float oy, float oz, float dx, float dy, float dz)
{
	float cx = ox - b[0], cy = oy - b[1], cz = oz - b[2];
	float p = cx * dx + cy * dy + cz * dz, q = cx * cx + cy * cy + cz * cz - b[3] * b[3], s = p * p - q;

	if (s < 0.0f)
		return -1.0f;
	s = -p - sqrtf(s);
	return s > 0.001f ? s : -1.0f;
}

/* The colour a ray from o the way d brings back, at most `turns` times thrown back by a ball. */
static void look(const struct frame *f, float ox, float oy, float oz, float dx, float dy, float dz, int turns, float *rgb)
{
	float best = 1e9f, at, lx = 0.48f, ly = 0.8f, lz = 0.36f, nx, ny, nz, px, py, pz, lit, r, g, b;
	int what = -1, i;

	for (i = 0; i < 2; i++)
		if ((at = meet(f->ball[i], ox, oy, oz, dx, dy, dz)) > 0.0f && at < best)
			best = at, what = i;
	if (dy < 0.0f && (at = -oy / dy) < best)
		best = at, what = 2;
	if (what < 0) {
		/* the sky: paler towards the horizon */
		float high = dy > 0.0f ? dy : 0.0f;

		rgb[0] = 0.55f - 0.35f * high, rgb[1] = 0.7f - 0.3f * high, rgb[2] = 0.95f;
		return;
	}
	px = ox + dx * best, py = oy + dy * best, pz = oz + dz * best;
	if (what == 2) {
		int cx = (int)(px + 1000.0f), cz = (int)(pz + 1000.0f);

		nx = 0.0f, ny = 1.0f, nz = 0.0f;
		r = g = b = (cx ^ cz) & 1 ? 0.85f : 0.3f;
	} else {
		const float *c = f->ball[what];
		float k = 1.0f / c[3];

		nx = (px - c[0]) * k, ny = (py - c[1]) * k, nz = (pz - c[2]) * k;
		if (what == 0)
			r = 0.95f, g = 0.35f, b = 0.2f;
		else
			r = 0.25f, g = 0.6f + 0.3f * wave(0.1f * f->t), b = 0.9f;
	}
	lit = nx * lx + ny * ly + nz * lz;
	/* in the shade of a ball between the point and the light */
	if (lit > 0.0f && (meet(f->ball[0], px, py, pz, lx, ly, lz) > 0.0f || meet(f->ball[1], px, py, pz, lx, ly, lz) > 0.0f))
		lit = 0.0f;
	lit = 0.2f + 0.8f * (lit > 0.0f ? lit : 0.0f);
	r *= lit, g *= lit, b *= lit;
	if (turns > 0 && what == 2) {
		/* the floor throws a quarter of the light back */
		float m[3];

		look(f, px, py, pz, dx, -dy, dz, turns - 1, m);
		r = 0.75f * r + 0.25f * m[0], g = 0.75f * g + 0.25f * m[1], b = 0.75f * b + 0.25f * m[2];
	}
	/* far things fade into the sky */
	at = best * (1.0f / 30.0f);
	at = at > 1.0f ? 1.0f : at * at;
	rgb[0] = r + (0.55f - r) * at, rgb[1] = g + (0.7f - g) * at, rgb[2] = b + (0.95f - b) * at;
}

/* A pixel's colour, 0x00RRGGBB: x and y in pixels from the top left. */
static uint32_t shade(int x, int y, void *ctx)
{
	const struct frame *f = ctx;
	float u = ((float)x + 0.5f - WIDTH * 0.5f) * (2.0f / HEIGHT), v = (HEIGHT * 0.5f - (float)y - 0.5f) * (2.0f / HEIGHT);
	float dx = f->fwd[0] * 1.6f + f->right[0] * u + f->up[0] * v;
	float dy = f->fwd[1] * 1.6f + f->right[1] * u + f->up[1] * v;
	float dz = f->fwd[2] * 1.6f + f->right[2] * u + f->up[2] * v;
	float n = 1.0f / sqrtf(dx * dx + dy * dy + dz * dz), c[3];

	look(f, f->eye[0], f->eye[1], f->eye[2], dx * n, dy * n, dz * n, MIRRORED, c);
	return (uint32_t)(c[0] > 1.0f ? 255.0f : c[0] * 255.0f) << 16 | (uint32_t)(c[1] > 1.0f ? 255.0f : c[1] * 255.0f) << 8 |
	       (uint32_t)(c[2] > 1.0f ? 255.0f : c[2] * 255.0f);
}

static unsigned milliseconds(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned)t.tv_sec * 1000u + (unsigned)t.tv_nsec / 1000000u;
}

/* The picture at time t: the eye goes round the middle, looking at it, and the balls round each other. */
static void scene(struct frame *f, float t)
{
	float c = wave(0.02f * t + 0.25f), s = wave(0.02f * t), n;

	f->t = t;
	f->ball[0][0] = 1.4f * wave(0.07f * t + 0.25f), f->ball[0][1] = 1.0f + 0.4f * wave(0.11f * t), f->ball[0][2] = 1.4f * wave(0.07f * t);
	f->ball[0][3] = 1.0f;
	f->ball[1][0] = -f->ball[0][0], f->ball[1][1] = 0.6f, f->ball[1][2] = -f->ball[0][2], f->ball[1][3] = 0.6f;
	f->eye[0] = 6.5f * c, f->eye[1] = 2.4f + 0.6f * wave(0.05f * t), f->eye[2] = 6.5f * s;
	f->fwd[0] = -f->eye[0], f->fwd[1] = 0.8f - f->eye[1], f->fwd[2] = -f->eye[2];
	n = 1.0f / sqrtf(f->fwd[0] * f->fwd[0] + f->fwd[1] * f->fwd[1] + f->fwd[2] * f->fwd[2]);
	f->fwd[0] *= n, f->fwd[1] *= n, f->fwd[2] *= n;
	n = 1.0f / sqrtf(f->fwd[0] * f->fwd[0] + f->fwd[2] * f->fwd[2]);
	f->right[0] = -f->fwd[2] * n, f->right[1] = 0.0f, f->right[2] = f->fwd[0] * n;
	f->up[0] = f->right[1] * f->fwd[2] - f->right[2] * f->fwd[1];
	f->up[1] = f->right[2] * f->fwd[0] - f->right[0] * f->fwd[2];
	f->up[2] = f->right[0] * f->fwd[1] - f->right[1] * f->fwd[0];
}

int main(void)
{
	static const GLfixed whole[16] = { 2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE };
	static const GLfixed corners[12] = { 0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0 }, uv[8] = { 0, 0, ONE, 0, ONE, ONE, 0, ONE };
	static struct frame frames[2];	/* what the picture being made reads, and the next */
	uint32_t *pixels[2];
	GR_WINDOW_ID window;
	GR_EVENT event;
	GLuint texture[2];
	int width, height, workers, lanes, pictures = 0, at = 0, stopped = 0;
	unsigned since;
	float t = 0.0f;

	if (GrOpen() < 0)
		return 1;
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Shader on the machine's lanes", GR_ROOT_WINDOW_ID, -1, -1, WIDTH * SCALE, HEIGHT * SCALE, BLACK);
	GrSelectEvents(window, GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_EXPOSURE);
	GrMapWindow(window);
	do
		GrGetNextEvent(&event);
	while (event.type != GR_EVENT_TYPE_EXPOSURE);
	if (seglInit(window) < 0)
		return 1;
	/* two pictures in memory the GPU draws from: one shown while the other is made */
	pixels[0] = seglMemory(WIDTH * HEIGHT * 4);
	pixels[1] = seglMemory(WIDTH * HEIGHT * 4);
	if (!pixels[0] || !pixels[1])
		return 1;
	memset(pixels[0], 0, WIDTH * HEIGHT * 4);
	memset(pixels[1], 0, WIDTH * HEIGHT * 4);
	glGenTextures(2, texture);
	glBindTexture(GL_TEXTURE_2D, texture[0]);
	seglTexturePointer(pixels[0], WIDTH, HEIGHT, GL_RGBA);
	glBindTexture(GL_TEXTURE_2D, texture[1]);
	seglTexturePointer(pixels[1], WIDTH, HEIGHT, GL_RGBA);
	seglSize(&width, &height);
	glViewport(0, 0, width, height);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixx(whole);
	glDisable(GL_DEPTH_TEST);

	/* the lanes' field where the machine has one, else as many workers as it gives; what the lanes
	 * read is touched first */
	lanes = mcw_field_lanes();
	workers = lanes ? 0 : mcw_open(mcw_most());
	mcw_touch(frames, sizeof frames);
	if (lanes)
		printf("shader: the field's %d lanes, %d x %d pixels\n", lanes, WIDTH, HEIGHT);
	else
		printf("shader: %d worker cores, %d x %d pixels\n", workers, WIDTH, HEIGHT);
	fflush(stdout);
	since = milliseconds();
	scene(&frames[0], t);
	mcw_shade_begin(pixels[0], WIDTH, HEIGHT, shade, &frames[0]);
	for (;;) {
		GrCheckNextEvent(&event);
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ || (event.type == GR_EVENT_TYPE_KEY_DOWN && event.keystroke.ch == 'q'))
			break;
		if (event.type == GR_EVENT_TYPE_KEY_DOWN && event.keystroke.ch == ' ')
			stopped = !stopped;
		/* the picture being made is whole: it is shown, and the next is begun in the other memory */
		mcw_shade_wait();
		seglQuad(corners, uv, texture[at], 255, 255, 0);
		seglSwapEarly();
		if (!stopped)
			t += 0.25f;
		at ^= 1;
		scene(&frames[at], t);
		seglSettle();	/* (the list that shows this memory's last picture has been drawn) */
		mcw_shade_begin(pixels[at], WIDTH, HEIGHT, shade, &frames[at]);
		if (++pictures % 50 == 0) {
			unsigned ms = milliseconds() - since;

			printf("shader: 50 pictures in %u ms: %u a second, %u thousand pixels a second\n", ms, ms ? 50000 / ms : 0,
			       ms ? 50u * WIDTH * HEIGHT / ms : 0);
			fflush(stdout);
			since += ms;
		}
	}
	mcw_shade_wait();
	mcw_close();
	GrClose();
	return 0;
}
