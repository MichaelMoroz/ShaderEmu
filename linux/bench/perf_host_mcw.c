/* The machine's own calls of its worker cores and lanes that linux/libmedia/test/mcw_host.c does not
 * have, for nxperf's native run (perf_host.c): the tests that make them are not run there (no workers),
 * so these are plain calls, enough to link and right if one is ever made. */
#include <stdint.h>
#include "mcwx.h"

int mcw_core(int k) { (void)k; return 0; }
int mcw_fast(int k) { (void)k; return 1; }
int mcw_lanes_here(void) { return 0; }
int mcw_field_lanes(void) { return 0; }

int mcw_lanes(mcw_lane_fn fn, void *ctx, uint32_t first, uint32_t count, void *out, uint32_t texels)
{
	uint32_t i;

	(void)out, (void)texels;
	for (i = 0; i < count; i++) fn(first + i, ctx);
	return 0;
}

void mcw_field_begin(mcw_lane_fn fn, void *ctx, uint32_t first, uint32_t count, void *out, uint32_t texels)
{
	mcw_lanes(fn, ctx, first, count, out, texels);
}

void mcw_field_wait(void) {}

void mcw_shade(uint32_t *pixels, int width, int height, mcw_pixel_fn fn, void *ctx)
{
	int x, y;

	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++) pixels[y * width + x] = fn(x, y, ctx);
}

static uint32_t counters[64];
int mcw_counters(void) { for (int k = 0; k < 64; k++) counters[k] = 0; return 64; }
void mcw_add(int counter, uint32_t value) { __atomic_fetch_add(&counters[counter & 63], value, __ATOMIC_RELAXED); }
uint32_t mcw_get(int counter) { return counters[counter & 63]; }
void mcw_set(int counter, uint32_t value) { counters[counter & 63] = value; }
void mcw_counters_settle(void) {}
