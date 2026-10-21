/*
 * hlsl.h: a fragment shader in HLSL compiled, in the machine and at run time, to RISC-V code that
 * runs on the lanes (docs/machine/lanes.md; docs/system/hlsl.md). The entry returns a pixel's colour:
 *
 *   float3 eye;                                   // a uniform: set from C with hlsl_set()
 *   float4 main(float4 pos : SV_Position) : SV_Target { return float4(pos.xy / 256.0, 0.5, 1); }
 *
 * pos.xy is the pixel's centre, from the top left. Every loop that is not unrolled may stop where
 * a lane's pass ends and go on in the next: the code keeps its own state, so a shader may run for
 * as long as it likes.
 */
#ifndef HLSL_H
#define HLSL_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hlsl_program hlsl_program;

/* The program compiled, or 0 and a message with its line in `error`. */
hlsl_program *hlsl_compile(const char *source, const char *entry, char *error, int error_bytes);
void hlsl_free(hlsl_program *p);

/* A uniform's place in the program's uniforms (floats, ints and bools, 4 bytes a component, in the
 * order declared), and how many components it has; -1 if there is none of that name. */
int hlsl_uniform(const hlsl_program *p, const char *name, int *components);
/* Sets a uniform for the pictures drawn from now on; returns 0 if there is none of that name. */
int hlsl_set(hlsl_program *p, const char *name, const float *values, int count);
int hlsl_set_int(hlsl_program *p, const char *name, const int32_t *values, int count);

/* What the compiler made: bytes of code, the longest step between two places it may stop, how
 * many texels an index keeps (1 to 6), and the places. */
struct hlsl_stats {
	uint32_t code_bytes, constants, yields, texels, longest_step, spilled, instructions;
};
struct hlsl_stats hlsl_stats(const hlsl_program *p);

/* A picture of width x height words (width a multiple of four, pixels 16-byte aligned): on the
 * field's lanes where the machine has them, else the workers' (mcw_open first), else this core. */
void hlsl_draw_begin(hlsl_program *p, uint32_t *pixels, int width, int height);
int hlsl_draw_ready(hlsl_program *p);
void hlsl_draw_wait(hlsl_program *p);
void hlsl_draw(hlsl_program *p, uint32_t *pixels, int width, int height);

/* The machine code of one step, for a lane: index is four pixels side by side (one with
 * HLSL_RAW), ctx the program's block (hlsl_ctx), round as mcw_step_fn's. */
uint32_t (*hlsl_step(const hlsl_program *p))(uint32_t index, void *ctx, uint32_t round);

/* Testing: one pixel an index, the entry's float4 stored as it is in the index's first texel. */
#define HLSL_RAW 1
hlsl_program *hlsl_compile_ex(const char *source, const char *entry, int flags, char *error, int error_bytes);
/* The block the step reads: out, first, width in indices, bytes an index; then the uniforms. */
uint32_t *hlsl_ctx(hlsl_program *p);
const uint8_t *hlsl_code(const hlsl_program *p, uint32_t *bytes);

#ifdef __cplusplus
}
#endif
#endif
