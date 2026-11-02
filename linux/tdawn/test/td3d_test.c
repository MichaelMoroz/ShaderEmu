/*
 * The tilted view's arithmetic (linux/tdawn/td3d.h) on the host: what the game's 3D view and its
 * clicks rest on. Any C compiler:
 *
 *     clang -O2 -Wall -o td3d_test linux/tdawn/test/td3d_test.c && ./td3d_test
 *
 * It says PASS, or what failed and by how much.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../td3d.h"

static int failures, checks;

#define FIX(v) ((int32_t)((v) * 65536.0))
#define PIX(f) ((double)(f) / 65536.0)

static void
expect(int ok, const char *what, double got, double want)
{
	checks++;
	if (!ok) {
		failures++;
		if (failures < 20)
			printf("FAIL %s: %.4f, wanted %.4f\n", what, got, want);
	}
}

static void
near_(double got, double want, double within, const char *what)
{
	expect(fabs(got - want) <= within, what, got, want);
}

/* The clip matrix applied to a point of the scene's units, as the GPU would: screen pixels. */
static void
by_matrix(const int32_t m[16], double ux, double uy, double uz, int screen_w, int screen_h, double *sx, double *sy, double *zndc)
{
	double in[4] = {ux, uy, uz, 1}, out[4] = {0, 0, 0, 0};
	int i, j;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			out[i] += PIX(m[j * 4 + i]) * in[j];
	*sx = (out[0] / out[3] + 1) * 0.5 * screen_w;
	*sy = (1 - out[1] / out[3]) * 0.5 * screen_h;
	*zndc = out[2] / out[3];
}

static void
camera(int w, int h, int pitch)
{
	struct td3d_view v;
	int32_t sx, sy, k, gx, gy, box[4];
	char what[160];
	int x, y, z;

	td3d_view_make(&v, w, h, pitch);
	if (!v.on) {
		printf("FAIL pitch %d of %dx%d is not on\n", pitch, w, h);
		failures++;
		return;
	}
	/* the window's far row spans the screen's top row at scale 1 */
	td3d_to_screen(&v, 0, 0, 0, &sx, &sy, &k);
	snprintf(what, sizeof what, "%dx%d at %d: far row's left x", w, h, pitch);
	near_(PIX(sx), 0, 0.01, what);
	snprintf(what, sizeof what, "%dx%d at %d: far row's y", w, h, pitch);
	near_(PIX(sy), 0, 0.01, what);
	snprintf(what, sizeof what, "%dx%d at %d: far row's scale", w, h, pitch);
	near_(PIX(k), 1, 0.001, what);
	td3d_to_screen(&v, FIX(w), 0, 0, &sx, &sy, &k);
	snprintf(what, sizeof what, "%dx%d at %d: far row's right x", w, h, pitch);
	near_(PIX(sx), w, 0.01, what);
	/* the near row's middle is the screen's bottom middle, larger */
	td3d_to_screen(&v, FIX(w / 2.0), FIX(h), 0, &sx, &sy, &k);
	snprintf(what, sizeof what, "%dx%d at %d: near row's y", w, h, pitch);
	near_(PIX(sy), h, 0.02, what);
	snprintf(what, sizeof what, "%dx%d at %d: near row's scale over 1", w, h, pitch);
	expect(PIX(k) > 1.0, what, PIX(k), 1.0);
	/* every pixel of the screen shows a point inside the window: a click always lands in it */
	for (y = 0; y <= h; y += 3)
		for (x = 0; x <= w; x += 3) {
			int ok = td3d_to_ground(&v, FIX(x), FIX(y), 0, &gx, &gy);
			snprintf(what, sizeof what, "%dx%d at %d: screen %d,%d inside the window (gx)", w, h, pitch, x, y);
			expect(ok && PIX(gx) >= -0.02 && PIX(gx) <= w + 0.02, what, PIX(gx), w / 2.0);
			snprintf(what, sizeof what, "%dx%d at %d: screen %d,%d inside the window (gy)", w, h, pitch, x, y);
			expect(ok && PIX(gy) >= -0.02 && PIX(gy) <= h + 0.02, what, PIX(gy), h / 2.0);
		}
	/* there and back, on the ground and above it, within a 64th of a pixel */
	for (z = 0; z <= 48 && FIX(z) < v.eye / 2; z += 16)	/* (below the eye: at 35 degrees it is 27 pixels up) */
		for (y = 0; y <= h; y += 7)
			for (x = 0; x <= w; x += 11) {
				td3d_to_screen(&v, FIX(x), FIX(y), FIX(z), &sx, &sy, NULL);
				td3d_to_ground(&v, sx, sy, FIX(z), &gx, &gy);
				snprintf(what, sizeof what, "%dx%d at %d: %d,%d,%d there and back (x)", w, h, pitch, x, y, z);
				near_(PIX(gx), x, 1.0 / 64, what);
				snprintf(what, sizeof what, "%dx%d at %d: %d,%d,%d there and back (y)", w, h, pitch, x, y, z);
				near_(PIX(gy), y, 1.0 / 64, what);
			}
	/* a point above the ground is higher on the screen */
	{
		int32_t lx, ly;
		td3d_to_screen(&v, FIX(w / 2.0), FIX(h / 2.0), 0, &sx, &sy, NULL);
		td3d_to_screen(&v, FIX(w / 2.0), FIX(h / 2.0), FIX(20), &lx, &ly, NULL);
		snprintf(what, sizeof what, "%dx%d at %d: a raised point is higher", w, h, pitch);
		expect(ly < sy, what, PIX(ly), PIX(sy));
	}
	/* a standing rectangle keeps its middle where the ground point is, scaled */
	td3d_stand(&v, FIX(100), FIX(50), FIX(124), FIX(74), 0, box);
	td3d_to_screen(&v, FIX(112), FIX(62), 0, &sx, &sy, &k);
	snprintf(what, sizeof what, "%dx%d at %d: a standing rectangle's middle", w, h, pitch);
	near_(PIX(box[0] + box[2]) / 2, PIX(sx), 0.01, what);
	snprintf(what, sizeof what, "%dx%d at %d: a standing rectangle's width", w, h, pitch);
	near_(PIX(box[2] - box[0]), 24 * PIX(k), 0.01, what);
	/* the GPU's matrix agrees with the arithmetic, wherever the window is on the screen */
	{
		int tac_x = 0, tac_y = 8, screen_w = 320, screen_h = 200;
		int32_t m[16], px[16];
		double mx, my, mz, lastz = -2, qx, qy, qz;

		td3d_clip_matrix(&v, tac_x, tac_y, screen_w, screen_h, screen_w, screen_h, m);
		td3d_clip_matrix(&v, tac_x, tac_y, screen_w, screen_h, 1, 1, px);
		for (y = 0; y <= h; y += 13)
			for (x = 0; x <= w; x += 17)
				for (z = 0; z <= 40; z += 20) {
					td3d_to_screen(&v, FIX(x), FIX(y), FIX(z), &sx, &sy, NULL);
					by_matrix(m, (x + tac_x) / (double)screen_w, (y + tac_y) / (double)screen_h, z / (double)screen_h,
						  screen_w, screen_h, &mx, &my, &mz);
					snprintf(what, sizeof what, "%dx%d at %d: matrix at %d,%d,%d (x)", w, h, pitch, x, y, z);
					near_(mx, PIX(sx) + tac_x, 0.05, what);
					snprintf(what, sizeof what, "%dx%d at %d: matrix at %d,%d,%d (y)", w, h, pitch, x, y, z);
					near_(my, PIX(sy) + tac_y, 0.05, what);
					snprintf(what, sizeof what, "%dx%d at %d: matrix at %d,%d,%d depth inside -1..1", w, h, pitch, x, y, z);
					expect(mz > -1 && mz < 1, what, mz, 0);
					/* the same by the matrix for vertices in pixels (the models') */
					by_matrix(px, x + tac_x, y + tac_y, z, screen_w, screen_h, &qx, &qy, &qz);
					snprintf(what, sizeof what, "%dx%d at %d: pixels' matrix at %d,%d,%d", w, h, pitch, x, y, z);
					expect(fabs(qx - mx) < 0.05 && fabs(qy - my) < 0.05 && fabs(qz - mz) < 1e-3, what, qx, mx);
					/* a picture standing on this ground point with its lowest row there is a little nearer than it */
					{
						double rd = td3d_row_depth(&v, sy, FIX(z)), d = v.fb / (qz - v.fa);
						snprintf(what, sizeof what, "%dx%d at %d: a row's depth at %d,%d,%d", w, h, pitch, x, y, z);
						expect(rd < qz && (d < 1.5 * v.fn || rd > v.fa + v.fb / (d - 1.5)), what, rd, qz);
					}
				}
		/* nearer is less deep: the depth test can work */
		for (y = 0; y <= h; y += 8) {
			by_matrix(m, (w / 2.0 + tac_x) / screen_w, (h - y + tac_y) / (double)screen_h, 0, screen_w, screen_h, &mx, &my, &mz);
			snprintf(what, sizeof what, "%dx%d at %d: depth grows away from the eye at row %d", w, h, pitch, h - y);
			expect(mz > lastz, what, mz, lastz);
			lastz = mz;
		}
		for (x = 0; x < 16; x++) {
			snprintf(what, sizeof what, "%dx%d at %d: matrix number %d fits 16.16", w, h, pitch, x);
			expect(m[x] > -32768 * 65536 + 1 && m[x] < 32767 * 65536, what, PIX(m[x]), 0);
		}
	}
}

static void
flat(void)
{
	struct td3d_view v;
	int32_t sx, sy, k, gx, gy, m[16];

	td3d_view_make(&v, 240, 192, 86);
	expect(!v.on, "pitch 86 is the 2D view", v.on, 0);
	td3d_to_screen(&v, FIX(17), FIX(33), 0, &sx, &sy, &k);
	expect(sx == FIX(17) && sy == FIX(33) && k == TD3D_ONE, "the 2D view's screen is its ground", PIX(sx), 17);
	td3d_to_ground(&v, FIX(17), FIX(33), 0, &gx, &gy);
	expect(gx == FIX(17) && gy == FIX(33), "the 2D view's ground is its screen", PIX(gx), 17);
	td3d_clip_matrix(&v, 0, 8, 320, 200, 320, 200, m);
	expect(m[0] == 2 * TD3D_ONE && m[5] == -2 * TD3D_ONE && m[12] == -TD3D_ONE, "the 2D view's matrix is host.c's", PIX(m[0]), 2);
}

static void
heights(void)
{
	static unsigned char c[65 * 65];
	int i;

	for (i = 0; i < 65 * 65; i++)
		c[i] = 40;
	c[10 * 65 + 10] = 120; /* one corner raised: the cells round it slope */
	near_(PIX(td3d_height(c, FIX(5 * 24 + 7), FIX(3 * 24 + 1))), 10, 0.001, "flat ground's height (pixels)");
	near_(PIX(td3d_height(c, FIX(10 * 24), FIX(10 * 24))), 30, 0.001, "a raised corner's height");
	near_(PIX(td3d_height(c, FIX(10 * 24 - 12), FIX(10 * 24 - 12))), 20, 0.01, "halfway to it on the diagonal");
	near_(PIX(td3d_height(c, FIX(9 * 24 + 1), FIX(9 * 24 + 23))), 10 + 20.0 / 24, 0.02, "the lower triangle of a cell");
	near_(PIX(td3d_height(c, FIX(-3), FIX(5))), 0, 0, "off the map");
	near_(PIX(td3d_height(NULL, FIX(30), FIX(5))), 0, 0, "no heights");
}

/* A model part's matrix (td3d_model_matrix) against the same turns made in doubles, and through the
 * camera's matrix in pixels against td3d_to_screen of the same point: where a model's vertex lands. */
static void
model_matrices(void)
{
	struct td3d_view v;
	int32_t px[16], mv[16];
	int facing, turret, turns, bx, by, bz;
	char what[160];
	const int tac_x = 0, tac_y = 8, screen_w = 320, screen_h = 200;

	td3d_view_make(&v, 240, 192, 60);
	td3d_clip_matrix(&v, tac_x, tac_y, screen_w, screen_h, 1, 1, px);
	for (turns = 0; turns < 2; turns++)
		for (facing = 0; facing < 256; facing += 37)
			for (turret = 0; turret < 256; turret += 53) {
				double step = 0.0625, origin[3] = {-6.5, -9.0, 0.25}, pivot[3] = {1.0, -2.0, 5.5}, x = 130.0, y = 90.0, gz = 3.0;
				double a = facing * 3.14159265358979 / 128, b = (turns ? turret : facing) * 3.14159265358979 / 128;
				int32_t o[3] = {FIX(origin[0]), FIX(origin[1]), FIX(origin[2])}, pv[3] = {FIX(pivot[0]), FIX(pivot[1]), FIX(pivot[2])};

				td3d_model_matrix(FIX(step), o, pv, turns, FIX(cos(a)), FIX(sin(a)), FIX(cos(b)), FIX(sin(b)), FIX(x), FIX(y),
						  FIX(gz), mv);
				for (bx = 0; bx <= 255; bx += 85)
					for (by = 0; by <= 255; by += 51)
						for (bz = 0; bz <= 255; bz += 255) {
							/* in doubles: the point about the pivot, turned, then the pivot with the body */
							double lx = origin[0] + step * bx, ly = origin[1] + step * by, lz = origin[2] + step * bz;
							double wx, wy, mx, my, mz, sx, sy, sz, w;
							int32_t ex, ey;
							int i;

							if (turns) {
								wx = cos(a) * pivot[0] + sin(a) * pivot[1] + cos(b) * lx + sin(b) * ly;
								wy = sin(a) * pivot[0] - cos(a) * pivot[1] + sin(b) * lx - cos(b) * ly;
							} else {
								wx = cos(a) * (pivot[0] + lx) + sin(a) * (pivot[1] + ly);
								wy = sin(a) * (pivot[0] + lx) - cos(a) * (pivot[1] + ly);
							}
							mx = PIX(mv[0]) * bx + PIX(mv[4]) * by + PIX(mv[8]) * bz + PIX(mv[12]);
							my = PIX(mv[1]) * bx + PIX(mv[5]) * by + PIX(mv[9]) * bz + PIX(mv[13]);
							mz = PIX(mv[2]) * bx + PIX(mv[6]) * by + PIX(mv[10]) * bz + PIX(mv[14]);
							snprintf(what, sizeof what, "model turns %d facing %d turret %d byte %d,%d,%d", turns, facing, turret, bx, by, bz);
							near_(mx, x + wx, 0.02, what);
							near_(my, y + wy, 0.02, what);
							near_(mz, gz + pivot[2] + lz, 0.01, what);
							/* through the camera in pixels: where td3d_to_screen has that point */
							{
								double in[4] = {mx, my, mz, 1}, out[4] = {0, 0, 0, 0};
								for (i = 0; i < 16; i++)
									out[i % 4] += PIX(px[i]) * in[i / 4];
								w = out[3];
								sx = (out[0] / w + 1) * 0.5 * screen_w, sy = (1 - out[1] / w) * 0.5 * screen_h, sz = out[2] / w;
							}
							td3d_to_screen(&v, FIX(mx - tac_x), FIX(my - tac_y), FIX(mz), &ex, &ey, NULL);
							near_(sx, PIX(ex) + tac_x, 0.05, what);
							near_(sy, PIX(ey) + tac_y, 0.05, what);
							expect(sz > -1 && sz < 1, what, sz, 0);
						}
			}
}

int
main(void)
{
	static const int sizes[][2] = {{240, 192}, {226, 177}, {320, 200}, {1, 1}, {640, 400}};
	static const int pitches[] = {35, 40, 45, 50, 60, 70, 80, 85};
	unsigned i, j;

	for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
		for (j = 0; j < sizeof pitches / sizeof pitches[0]; j++)
			if (sizes[i][0] > 8)
				camera(sizes[i][0], sizes[i][1], pitches[j]);
	flat();
	heights();
	model_matrices();
	printf("%s: %d checks, %d failed\n", failures ? "FAIL" : "PASS", checks, failures);
	return failures != 0;
}
