/*
 * Tiberian Dawn's tilted view (docs/plans/td3d-plan.md, docs/games/tdawn.md "In 3D"): a camera over
 * the map, its ground to the screen and back, and the matrix the GPU is given. Shared by the game
 * (gl.cpp, host.c, shaderemu.cpp) and the host's test (linux/tdawn/test/td3d_test.c).
 *
 * Everything is in the game's pixels of the tactical window: gx right and gy down from its top
 * left, gz up from the ground. The camera looks north, down at `pitch` degrees below the
 * horizontal (90: straight down, the 2D view), and is placed so that the window's top row of
 * the map spans the whole width of the screen's top row (scale 1 there) and the middle of its
 * bottom row is at the middle of the screen's bottom row. Every pixel of the screen then shows
 * a point of the ground inside the window the game has (its rectangle of cells), nearer rows
 * larger: nothing the game culls by that rectangle is missed, and every click lands in it.
 *
 *      depth = (yfar - gy) cos + (eye - gz) sin           (how far in front of the eye)
 *      up    = (yfar - gy) sin - (eye - gz) cos           (above the eye's axis)
 *      sx    = w/2 + F (gx - w/2) / depth,   sy = h/2 - F up / depth
 *
 * with F = (h/2) cos / (1 - sin), yfar = F cos + (h/2) sin, eye = F sin - (h/2) cos.
 *
 * The camera is made in floats once (when the window or the pitch changes); the ground to the
 * screen and back are integer arithmetic (16.16 numbers in 64-bit products), as the game is.
 */
#ifndef TDAWN_TD3D_H
#define TDAWN_TD3D_H

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TD3D_ONE 65536
#define TD3D_PITCH 60 /* the default, degrees (TDAWN_PITCH) */

struct td3d_view {
	int on;		/* 0: no tilt (a pitch outside 35 to 85): every function is the 2D view's */
	int w, h;	/* the tactical window, pixels */
	int pitch;	/* degrees */
	/* 16.16 */
	int32_t f, cosp, sinp, yfar, eye;
	/* for the clip matrix: depth at the window's near row, middle (16.16) */
	int32_t near_depth;
	float ff, fc, fs, fyfar, feye;	/* the same in floats, for the matrix */
};

static inline int32_t
td3d_fixed(float v)
{
	return (int32_t)(v * 65536.0f + (v < 0 ? -0.5f : 0.5f));
}

/* A camera for a window of w x h pixels at `pitch` degrees. */
static inline void
td3d_view_make(struct td3d_view *v, int w, int h, int pitch)
{
	float r = (float)pitch * 3.14159265f / 180.0f, c = cosf(r), s = sinf(r), half = 0.5f * (float)h;

	v->w = w, v->h = h, v->pitch = pitch;
	v->on = pitch >= 35 && pitch <= 85 && w > 0 && h > 0;	/* (at 30 or less the near row is behind the eye; beyond 85 the 64-bit products would not hold) */
	if (!v->on)
		return;
	v->fc = c, v->fs = s;
	v->ff = half * c / (1.0f - s);
	v->fyfar = v->ff * c + half * s;
	v->feye = v->ff * s - half * c;
	v->f = td3d_fixed(v->ff), v->cosp = td3d_fixed(c), v->sinp = td3d_fixed(s);
	v->yfar = td3d_fixed(v->fyfar), v->eye = td3d_fixed(v->feye);
	v->near_depth = td3d_fixed(v->ff - (float)h * c);
}

/* depth and up (16.16) of a ground point given in 16.16 pixels */
static inline void
td3d_eye_of(const struct td3d_view *v, int32_t gy, int32_t gz, int64_t *depth, int64_t *up)
{
	int64_t ahead = (int64_t)v->yfar - gy, below = (int64_t)v->eye - gz;

	*depth = (ahead * v->cosp + below * v->sinp) >> 16;
	*up = (ahead * v->sinp - below * v->cosp) >> 16;
}

/*
 * A point of the ground (16.16 pixels of the window) on the screen (16.16 pixels of the window),
 * and the scale there (16.16: how many pixels of the screen a pixel of the ground across is).
 * 0 if it is behind the eye or too near it to draw.
 */
static inline int
td3d_to_screen(const struct td3d_view *v, int32_t gx, int32_t gy, int32_t gz, int32_t *sx, int32_t *sy, int32_t *scale)
{
	int64_t depth, up, k;

	if (!v->on) {
		*sx = gx, *sy = gy - gz;
		if (scale)
			*scale = TD3D_ONE;
		return 1;
	}
	td3d_eye_of(v, gy, gz, &depth, &up);
	if (depth < TD3D_ONE)	/* (a pixel in front of the eye at least) */
		return 0;
	k = ((int64_t)v->f << 32) / depth;	/* F / depth, 32 bits of fraction */
	*sx = (int32_t)(((int64_t)v->w << 15) + ((((int64_t)gx - ((int64_t)v->w << 15)) * k) >> 32));
	*sy = (int32_t)(((int64_t)v->h << 15) - ((up * k) >> 32));
	if (scale)
		*scale = (int32_t)(k >> 16);
	return 1;
}

/*
 * The point of the ground at height gz that a pixel of the screen shows (both 16.16, of the
 * window): the click's ray met with the ground. 0 if the ray never meets it (above the horizon).
 */
static inline int
td3d_to_ground(const struct td3d_view *v, int32_t sx, int32_t sy, int32_t gz, int32_t *gx, int32_t *gy)
{
	int64_t rx, ry, down, t, ahead;

	if (!v->on) {
		*gx = sx, *gy = sy + gz;
		return 1;
	}
	/* the ray's way per unit ahead of the eye: rx right, ry up (32 bits of fraction) */
	rx = (((int64_t)sx - ((int64_t)v->w << 15)) << 32) / v->f;
	ry = ((((int64_t)v->h << 15) - (int64_t)sy) << 32) / v->f;
	/* in the world: (rx, ry sin + cos, ry cos - sin), 32 bits of fraction */
	down = ((ry * v->cosp) >> 16) - ((int64_t)v->sinp << 16);
	if (down >= 0)
		return 0;
	t = (((int64_t)gz - v->eye) << 32) / down;	/* how far along the ray, 16.16 */
	ahead = (t * (((ry * v->sinp) >> 16) + ((int64_t)v->cosp << 16))) >> 32;
	*gx = (int32_t)(((int64_t)v->w << 15) + ((t * rx) >> 32));
	*gy = (int32_t)((int64_t)v->yfar - ahead);
	return 1;
}

/*
 * A rectangle of the 2D view standing up in the tilted one (a sprite: docs/games/tdawn.md "In 3D"):
 * its middle is a point of the ground, put where the camera shows it, and the rectangle is
 * scaled about it by the camera's scale there. In and out in 16.16 pixels of the window.
 */
static inline int
td3d_stand(const struct td3d_view *v, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t gz, int32_t box[4])
{
	int32_t mx = (int32_t)(((int64_t)x0 + x1) >> 1), my = (int32_t)(((int64_t)y0 + y1) >> 1), sx, sy, k;

	if (!td3d_to_screen(v, mx, my, gz, &sx, &sy, &k))
		return 0;
	box[0] = sx + (int32_t)((((int64_t)x0 - mx) * k) >> 16);
	box[1] = sy + (int32_t)((((int64_t)y0 - my) * k) >> 16);
	box[2] = sx + (int32_t)((((int64_t)x1 - mx) * k) >> 16);
	box[3] = sy + (int32_t)((((int64_t)y1 - my) * k) >> 16);
	return 1;
}

/*
 * The GPU's projection for the tilted view (docs/machine/gpu.md: rows of a clip matrix, OpenGL's
 * conventions), in the units the vertices are in: a unit of x is unit_w of the screen's pixels
 * (screen_w for host.c's kept quads and sprites, which go across the whole window from 0 to 1;
 * 1 for vertices in pixels, the models'), y down by unit_h, z up in the same units as y. The
 * tactical window is at (tac_x, tac_y) of the screen. Column-major for glLoadMatrixx, as
 * OpenGL's are; scaled as a whole so that every number fits 16.16 (a clip matrix may be scaled:
 * what it draws is the same).
 */
static inline void
td3d_clip_matrix(const struct td3d_view *v, int tac_x, int tac_y, int screen_w, int screen_h, int unit_w, int unit_h,
		 int32_t out[16])
{
	float row[4][4], most = 0, scale, sw = (float)screen_w, sh = (float)screen_h, uw = (float)unit_w, uh = (float)unit_h;
	float n, far_, a, b;
	int i, j;

	if (!v->on) {
		/* glOrthox(0, 1, 1, 0, -1, 1) over the units: host.c's whole window when they are the screen's */
		static const int32_t whole[16] = {2 * TD3D_ONE, 0, 0, 0, 0, -2 * TD3D_ONE, 0, 0, 0, 0, -TD3D_ONE, 0, -TD3D_ONE, TD3D_ONE, 0, TD3D_ONE};
		for (i = 0; i < 16; i++)
			out[i] = whole[i];
		out[0] = (int32_t)((int64_t)out[0] * unit_w / screen_w), out[5] = (int32_t)((int64_t)out[5] * unit_h / screen_h);
		return;
	}
	/* depth and up as rows of (ux, uy, uz, 1): gx = ux uw - tac_x, gy = uy uh - tac_y, gz = uz uh */
	float depth[4] = {0, -uh * v->fc, -uh * v->fs, (v->fyfar + (float)tac_y) * v->fc + v->feye * v->fs};
	float up[4] = {0, -uh * v->fs, uh * v->fc, (v->fyfar + (float)tac_y) * v->fs - v->feye * v->fc};
	/* sx depth = (tac_x + w/2) depth + F (gx - w/2) */
	float cx = (float)tac_x + 0.5f * (float)v->w, cy = (float)tac_y + 0.5f * (float)v->h;
	float sxd[4], syd[4];
	for (j = 0; j < 4; j++) {
		sxd[j] = cx * depth[j];
		syd[j] = cy * depth[j] - v->ff * up[j];
	}
	sxd[0] += v->ff * uw;
	sxd[3] += -v->ff * ((float)tac_x + 0.5f * (float)v->w);
	/* near and far for the depth test: a quarter of the nearest ground's depth, four times the far row's */
	n = 0.25f * (v->ff - (float)v->h * v->fc);
	far_ = 4.0f * v->ff;
	a = (far_ + n) / (far_ - n), b = -2.0f * far_ * n / (far_ - n);
	for (j = 0; j < 4; j++) {
		row[0][j] = 2.0f / sw * sxd[j] - depth[j];
		row[1][j] = depth[j] - 2.0f / sh * syd[j];
		row[2][j] = a * depth[j] + (j == 3 ? b : 0.0f);
		row[3][j] = depth[j];
	}
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			most = fabsf(row[i][j]) > most ? fabsf(row[i][j]) : most;
	scale = most > 0 ? 8192.0f / most : 1.0f;
	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			out[j * 4 + i] = td3d_fixed(row[i][j] * scale);
}

/* Height of the ground (16.16 pixels) at a point of the map (16.16 pixels of the whole map), from a
 * map's 65 x 65 corner heights in quarters of a pixel (heights.py): the cell's two triangles. */
static inline int32_t
td3d_height(const unsigned char *corners, int32_t mx, int32_t my)
{
	int cx, cy;
	int32_t fx, fy, h00, h10, h01, h11, z;

	if (!corners || mx < 0 || my < 0)
		return 0;
	cx = mx / (24 * TD3D_ONE), cy = my / (24 * TD3D_ONE);
	if (cx > 63 || cy > 63)
		return 0;
	fx = (mx - cx * 24 * TD3D_ONE) / 24, fy = (my - cy * 24 * TD3D_ONE) / 24;	/* 0..1, 16.16 */
	h00 = corners[cy * 65 + cx], h10 = corners[cy * 65 + cx + 1];
	h01 = corners[(cy + 1) * 65 + cx], h11 = corners[(cy + 1) * 65 + cx + 1];
	/* the diagonal from the top left to the bottom right: two planes */
	if (fx >= fy)
		z = h00 * TD3D_ONE + (h10 - h00) * fx + (h11 - h10) * fy;
	else
		z = h00 * TD3D_ONE + (h11 - h01) * fx + (h01 - h00) * fy;
	return z / 4;	/* quarters of a pixel to pixels */
}

/*
 * A model part's modelview (host.c's model_now; linux/tdawn/models/pack.py): its bytes b to the screen's
 * pixels, as columns for glLoadMatrixx (16.16). A point of the part is origin + step b about its
 * pivot; a part that turns (a turret) is turned by its own facing (cb, sb: the cosine and sine of
 * it) about the pivot, which goes round with the body (ca, sa); one that does not turns with the body.
 * Facings are clockwise from north, and the model's y ahead is up the screen: (c x + s y, s x - c y).
 * It stands at (x, y) of the screen and gz above the ground (16.16 pixels).
 */
static inline int32_t
td3d_mul(int32_t a, int32_t b)
{
	return (int32_t)(((int64_t)a * b) >> 16);
}

static inline void
td3d_model_matrix(int32_t step, const int32_t origin[3], const int32_t pivot[3], int turns, int32_t ca, int32_t sa,
		  int32_t cb, int32_t sb, int32_t x, int32_t y, int32_t gz, int32_t mv[16])
{
	int32_t t0, t1, c = turns ? cb : ca, s = turns ? sb : sa;
	int i;

	if (turns) {
		t0 = td3d_mul(ca, pivot[0]) + td3d_mul(sa, pivot[1]) + td3d_mul(cb, origin[0]) + td3d_mul(sb, origin[1]);
		t1 = td3d_mul(sa, pivot[0]) - td3d_mul(ca, pivot[1]) + td3d_mul(sb, origin[0]) - td3d_mul(cb, origin[1]);
	} else {
		t0 = td3d_mul(ca, pivot[0] + origin[0]) + td3d_mul(sa, pivot[1] + origin[1]);
		t1 = td3d_mul(sa, pivot[0] + origin[0]) - td3d_mul(ca, pivot[1] + origin[1]);
	}
	for (i = 0; i < 16; i++)
		mv[i] = 0;
	mv[0] = td3d_mul(step, c), mv[1] = td3d_mul(step, s);
	mv[4] = td3d_mul(step, s), mv[5] = -td3d_mul(step, c);
	mv[10] = step;
	mv[12] = x + t0, mv[13] = y + t1, mv[14] = pivot[2] + origin[2] + gz, mv[15] = TD3D_ONE;
}

#ifdef __cplusplus
}
#endif
#endif
