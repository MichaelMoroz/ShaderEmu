/* The path tracer's scene and its job for a worker core (tracer.c, docs/raytrace.md). */
#ifndef PATHTRACE_TRACER_H
#define PATHTRACE_TRACER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PT_TILE 32
enum { PT_DIFFUSE, PT_MIRROR, PT_GLASS };

/* What a picture is traced with: written between two pictures by the core that hands the
 * tiles out, only read by the ones that trace. */
struct pt_scene {
	int width, height;	/* the width a multiple of four */
	int bounces;		/* how many times a path may turn */
	int material[2];	/* the two balls: PT_DIFFUSE, PT_MIRROR or PT_GLASS */
	int owners;		/* each tile framed in its core's colour */
	float light;		/* how bright the lamp is */
	uint32_t seed;
	float *sums;		/* three floats a pixel: the light of the samples so far */
	uint32_t *pixels;	/* the picture, 0x00RRGGBB */
};
extern struct pt_scene pt;

/* One more sample of every pixel of a tile: `how` is the sample's number (from 0), with the
 * core's number in its top byte. Returns the rays cast. */
uint32_t pt_tile(uint32_t tile, uint32_t how);

#ifdef __cplusplus
}
#endif
#endif
