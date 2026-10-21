/* A compiled program drawn on the machine (hlsl.h): its steps a job of the lanes' field, then a
 * job that copies each index's first texel into the picture; on the workers' own lanes where
 * there is no field, or as plain calls on this core where there are no workers. */
#include <string.h>
#include "hlsl.h"
#include "mcw.h"
#include "mcwx.h"

void *hlsl_internal(hlsl_program *p, uint32_t ***steps_out, uint32_t **steps_bytes, uint32_t ***pixels, int **wh, int **stage);

typedef uint32_t (*step_fn)(uint32_t, void *, uint32_t);

/* what the copy reads and writes: the steps' output, its texels an index, the picture */
static struct { uint32_t *from, stride, pad[2]; } copy_ctx __attribute__((aligned(16)));

static void copy_texel(uint32_t i, void *ctx)
{
	(void)ctx;
	const uint32_t *s = copy_ctx.from + copy_ctx.stride * i;
	uint32_t *d = (uint32_t *)((uintptr_t)copy_ctx.pad[0]) + 4 * i;

	d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = s[3];
}

static void copy_piece(void *ctx, uint32_t from, uint32_t to)
{
	uint32_t i;

	for (i = from; i < to; i++) copy_texel(i, ctx);
}

/* a worker's share, in steps on its lanes (plain calls where it has none) */
static step_fn piece_fn;
static void *piece_ctx;
static uint32_t piece_stride;

static void steps_piece(void *ctx, uint32_t from, uint32_t to)
{
	uint8_t *out = (uint8_t *)(uintptr_t)((uint32_t *)piece_ctx)[0];

	(void)ctx;
	mcw_lanes_steps(piece_fn, piece_ctx, from, to - from, out + piece_stride * from, piece_stride / 16, 0);
}

void hlsl_draw_begin(hlsl_program *p, uint32_t *pixels, int width, int height)
{
	uint32_t **out, *bytes, **pix, *ctx = hlsl_ctx(p), n = (uint32_t)(width * height) / 4, texels = hlsl_stats(p).texels, need;
	int *wh, *stage;
	step_fn fn = hlsl_step(p);

	hlsl_internal(p, &out, &bytes, &pix, &wh, &stage);
	hlsl_draw_wait(p);
	need = n * 16 * texels;
	if (texels > 1 && *bytes < need) {
		*out = mcw_alloc(need);	/* (the workers' memory: a smaller one before is not given back) */
		*bytes = need;
		if (!*out) return;
		mcw_touch(*out, need);
	}
	*pix = pixels, wh[0] = width, wh[1] = height;
	ctx[0] = (uint32_t)(uintptr_t)(texels > 1 ? *out : pixels), ctx[1] = 0, ctx[2] = (uint32_t)width / 4, ctx[3] = 16 * texels;
	if (!mcw_self() && mcw_field_lanes() > 0) {
		*stage = 1;
		mcw_field_steps(fn, ctx, 0, n, (void *)(uintptr_t)ctx[0], texels);
		return;
	}
	if (mcw_count() > 0) {
		mcw_for_opts o;

		memset(&o, 0, sizeof o);
		o.first_idle = 1;
		piece_fn = fn, piece_ctx = ctx, piece_stride = 16 * texels;
		mcw_for_opt(n, steps_piece, 0, &o);
	} else {
		uint32_t i;

		for (i = 0; i < n; i++) {
			uint32_t r = 0;

			while (fn(i, ctx, r)) r++;
		}
	}
	if (texels > 1) {
		copy_ctx.from = *out, copy_ctx.stride = 4 * texels, copy_ctx.pad[0] = (uint32_t)(uintptr_t)pixels;
		if (mcw_count() > 0) mcw_for(n, copy_piece, 0);
		else copy_piece(0, 0, n);
	}
	*stage = 0;
}

int hlsl_draw_ready(hlsl_program *p)
{
	uint32_t **out, *bytes, **pix;
	int *wh, *stage;
	uint32_t texels = hlsl_stats(p).texels;

	hlsl_internal(p, &out, &bytes, &pix, &wh, &stage);
	if (*stage == 1) {
		if (!mcw_field_ready()) return 0;
		if (texels == 1) { *stage = 0; return 1; }
		copy_ctx.from = *out, copy_ctx.stride = 4 * texels, copy_ctx.pad[0] = (uint32_t)(uintptr_t)*pix;
		*stage = 2;
		mcw_field_begin(copy_texel, &copy_ctx, 0, (uint32_t)(wh[0] * wh[1]) / 4, *pix, 1);
		return 0;
	}
	if (*stage == 2) {
		if (!mcw_field_ready()) return 0;
		*stage = 0;
	}
	return 1;
}

void hlsl_draw_wait(hlsl_program *p)
{
	while (!hlsl_draw_ready(p)) mcw_rest();
}

void hlsl_draw(hlsl_program *p, uint32_t *pixels, int width, int height)
{
	hlsl_draw_begin(p, pixels, width, height);
	hlsl_draw_wait(p);
}
