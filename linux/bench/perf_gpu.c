/*
 * nxperf's GPU tests (docs/apps/bench.md): lists drawn through programs/linux/gles.c into the
 * window perf_gpu_init() was given, each waited for (seglSwap), so a list's time is its making on
 * the first core, its drawing and the wait. The check is that every list came back and the
 * library said no error.
 */
#include <stdio.h>
#include <string.h>
#include <GLES/gl.h>
#include <GLES/segl.h>
#include "perf.h"

int perf_gpu_ready;
static int gw, gh;
static GLuint texture;

int perf_gpu_init(unsigned window)
{
	static uint32_t texels[64 * 64];
	int x, y;

	if (seglInit(window) < 0) return -1;
	seglSize(&gw, &gh);
	glViewport(0, 0, gw, gh);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthof(0, (float)gw, (float)gh, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	for (y = 0; y < 64; y++)
		for (x = 0; x < 64; x++) texels[y * 64 + x] = ((x ^ y) & 8) ? 0x00e0a040u : 0x002060c0u;
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	perf_gpu_ready = 1;
	return 0;
}

static int frames_check(struct perf_run *r, int frames, const char *what)
{
	GLenum e = glGetError();

	perf_check(r, e == GL_NO_ERROR, "%d lists of %s drawn, error %x", frames, what, (unsigned)e);
	return e == GL_NO_ERROR;
}

static void clear(int frame)
{
	glClearColor(0.1f + 0.02f * (float)(frame & 7), 0.1f, 0.15f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void perf_gpu_lists(struct perf_run *r)
{
	int frames = 40, f;

	glGetError();
	perf_begin(r);
	for (f = 0; f < frames; f++) {
		clear(f);
		seglSwap();
	}
	perf_end(r);
	perf_rate(r, (float)frames, 1.0f);
	perf_cpo(r, (float)frames);
	frames_check(r, frames, "a clear");
}

#define TRIS 2000
void perf_gpu_tris(struct perf_run *r)
{
	static float v[TRIS * 9];
	int frames = 20, f, k, cols = gw / 6 > 0 ? gw / 6 : 1;

	for (k = 0; k < TRIS; k++) {
		float x = (float)(k % cols * 6 % (gw > 6 ? gw - 6 : 1)), y = (float)(k / cols * 5 % (gh > 6 ? gh - 6 : 1));
		float *t = v + 9 * k;

		t[0] = x, t[1] = y, t[2] = 0, t[3] = x + 5, t[4] = y, t[5] = 0, t[6] = x, t[7] = y + 4, t[8] = 0;
	}
	glGetError();
	glDisable(GL_TEXTURE_2D);
	glEnableClientState(GL_VERTEX_ARRAY);
	perf_begin(r);
	for (f = 0; f < frames; f++) {
		clear(f);
		glColor4f(0.9f, 0.6f, 0.2f, 1.0f);
		glVertexPointer(3, GL_FLOAT, 0, v);
		glDrawArrays(GL_TRIANGLES, 0, 3 * TRIS);
		seglSwap();
	}
	perf_end(r);
	glDisableClientState(GL_VERTEX_ARRAY);
	perf_rate(r, (float)frames * TRIS, 1e6f);
	perf_cpo(r, (float)frames * TRIS);
	frames_check(r, frames, "2,000 triangles");
}

#define QUADS 400
void perf_gpu_quads(struct perf_run *r)
{
	static const GLfixed uv[8] = {0, 0, 65536, 0, 65536, 65536, 0, 65536};
	int frames = 20, f, k;
	GLfixed xyz[12];

	glGetError();
	perf_begin(r);
	for (f = 0; f < frames; f++) {
		clear(f);
		for (k = 0; k < QUADS; k++) {
			int x = (k * 37 + f) % (gw > 16 ? gw - 16 : 1), y = (k * 23) % (gh > 16 ? gh - 16 : 1);

			xyz[0] = x << 16, xyz[1] = y << 16, xyz[2] = 0;
			xyz[3] = (x + 16) << 16, xyz[4] = y << 16, xyz[5] = 0;
			xyz[6] = (x + 16) << 16, xyz[7] = (y + 16) << 16, xyz[8] = 0;
			xyz[9] = x << 16, xyz[10] = (y + 16) << 16, xyz[11] = 0;
			seglQuad(xyz, uv, texture, 255, 255, 0);
		}
		seglSwap();
	}
	perf_end(r);
	perf_rate(r, (float)frames * QUADS, 1e6f);
	perf_cpo(r, (float)frames * QUADS);
	frames_check(r, frames, "400 textured quads");
}

#define LAYERS 16
void perf_gpu_fill(struct perf_run *r)
{
	static float v[LAYERS * 18];
	int frames = 20, f, k;

	for (k = 0; k < LAYERS; k++) {
		float *t = v + 18 * k, w = (float)gw, h = (float)gh, z = -0.5f + 0.05f * (float)k;

		t[0] = 0, t[1] = 0, t[2] = z, t[3] = w, t[4] = 0, t[5] = z, t[6] = w, t[7] = h, t[8] = z;
		t[9] = 0, t[10] = 0, t[11] = z, t[12] = w, t[13] = h, t[14] = z, t[15] = 0, t[16] = h, t[17] = z;
	}
	glGetError();
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnableClientState(GL_VERTEX_ARRAY);
	perf_begin(r);
	for (f = 0; f < frames; f++) {
		clear(f);
		glColor4f(0.3f, 0.7f, 0.9f, 0.25f);
		glVertexPointer(3, GL_FLOAT, 0, v);
		glDrawArrays(GL_TRIANGLES, 0, 6 * LAYERS);
		seglSwap();
	}
	perf_end(r);
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisable(GL_BLEND);
	perf_rate(r, (float)frames * LAYERS * (float)gw * (float)gh, 1e6f);
	frames_check(r, frames, "16 blended layers");
}

/* the machine's copy between two places of GPU memory (0x80000000 to 0x81e00000) */
void perf_gpu_memcpy(struct perf_run *r)
{
	static uint32_t *a, *b;
	uint32_t bytes = 256 * 1024, i, rep;

	if (!a) a = seglMemory(bytes), b = seglMemory(bytes);
	if (!a || !b) {
		perf_check(r, 0, "no GPU memory left");
		return;
	}
	for (i = 0; i < bytes / 4; i++) a[i] = i * 2654435761u;
	perf_begin(r);
	for (rep = 0; rep < 8; rep++) memcpy(b, a, bytes);
	perf_end(r);
	perf_rate(r, 8.0f * bytes, 1e6f);
	perf_check(r, memcmp(a, b, bytes) == 0, "256 KB as the source, 8 times");
}
