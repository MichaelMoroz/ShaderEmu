/*
 * The file bake.py makes of a level, its models, its pictures and its sounds
 * (/usr/share/quake3-se.dat, docs/games/quake3.md), as the renderer and the sound read it: tables,
 * and after them what they point at, by its place in the file. The GPU and the sound card read
 * textures and samples where the file lies in the ROM.
 */
#ifndef SHADEREMU_QUAKE3_DATA_H
#define SHADEREMU_QUAKE3_DATA_H

#include <stdint.h>

#define SE_MAGIC	0x33514553u	/* "SEQ3" */
#define SE_SECTIONS	32

typedef struct { char tag[4]; uint32_t at, bytes; } se_section_t;
typedef struct { uint32_t magic, version, sections, blob; se_section_t section[SE_SECTIONS]; } se_header_t;

/* TEXS: a texture's texels where the file has them, or `at` bytes into a sheet's (SHET) */
#define SE_VERSION	2
#define SE_NO_SHEET	0xffff
enum { SE_BYTES, SE_WORDS };		/* three bytes a texel; a word, 0xTTRRGGBB */
typedef struct { uint32_t at; uint16_t width, height; uint8_t format, flags; uint16_t sheet; uint32_t key; } se_texture_t;

/* SHET: a file of textures one under another, decoded to `channels` bytes a texel in GPU memory */
enum { SE_JPEG = 1, SE_PNG };
typedef struct { uint32_t at, bytes; uint16_t width, height; uint8_t channels, packing; uint16_t unused; uint32_t unused2; } se_sheet_t;

/* SHAD, in the order of their names; `number` is what stages, surfaces and runs call them by */
enum { SE_SKY = 1, SE_OFFSET = 2, SE_AUTOSPRITE = 4, SE_NODRAW = 8, SE_2D = 16 };
typedef struct { char name[64]; uint32_t flags; uint16_t first_stage; uint8_t stages, unused; int32_t number; } se_shader_t;

/* STAG */
enum { SE_OPAQUE, SE_BLEND, SE_ADD, SE_FILTER, SE_MASKED = 9 };	/* the GPU's ways of drawing over what is there (its passes) */
enum { SE_KEYED = 1, SE_LIGHTMAP = 2, SE_ENV = 4, SE_CLAMP = 8, SE_IMPLICIT = 16 };
enum { SE_RGB_IDENTITY, SE_RGB_VERTEX, SE_RGB_ENTITY, SE_RGB_ONE_MINUS_ENTITY, SE_RGB_DIFFUSE, SE_RGB_WAVE, SE_RGB_CONST };
enum { SE_A_IDENTITY, SE_A_VERTEX, SE_A_ENTITY, SE_A_ONE_MINUS_ENTITY, SE_A_WAVE, SE_A_CONST };
enum { SE_SIN = 1, SE_TRIANGLE, SE_SQUARE, SE_SAWTOOTH, SE_INVERSE_SAWTOOTH, SE_NOISE };
enum { SE_SCROLL = 1, SE_SCALE, SE_ROTATE, SE_STRETCH, SE_TURB };
typedef struct { uint32_t function; float base, amplitude, phase, frequency; } se_wave_t;
typedef struct { uint8_t kind, function; uint16_t unused; float p[4]; } se_tcmod_t;
typedef struct {
	uint16_t	texture;	/* the first of its pictures (TEXS), 0xffff for none */
	uint8_t		frames, pass;	/* how many pictures it turns through; one of SE_OPAQUE.. */
	float		fps;
	uint8_t		flags, rgb, alpha, tcmods;
	se_wave_t	rgb_wave, alpha_wave;
	uint8_t		constant[4];
	se_tcmod_t	tcmod[3];
} se_stage_t;

/* MODL, in the order of their names */
typedef struct {
	char		name[64];
	uint16_t	frames, surfaces, tags, first_surface;
	uint32_t	tags_at;	/* the tags' names, 64 bytes each, then 12 floats a tag a frame */
	uint32_t	frames_at;	/* a frame's own origin and radius: 4 floats */
	float		scale[3], translate[3];	/* a packed vertex's bytes to the model's units */
	float		mins[3], maxs[3];
} se_model_t;

/* SURF: a surface's triangles, three vertices each */
typedef struct {
	char		name[64];
	int32_t		shader;
	uint32_t	vertices;
	uint32_t	coords_at;	/* a word a vertex: u | v << 12 */
	uint32_t	vertices_at;	/* a word a vertex a frame: x | y << 8 | z << 16 | normal << 24 */
} se_surface_t;

/* SKIN, in the order of their names, and SKNE */
typedef struct { char name[64]; uint16_t first, count; uint32_t unused; } se_skin_t;
typedef struct { char surface[64]; int32_t shader; } se_skin_entry_t;

/* WHDR, WRUN, WBMD: the level */
enum { SE_RUN_LIT = 1, SE_RUN_TAGGED = 2, SE_RUN_MOVING = 4, SE_RUN_SKY = 8, SE_RUN_SPRITES = 16 };
typedef struct {
	int32_t		shader;
	uint16_t	stage;		/* of STAG */
	uint8_t		pass, flags;
	uint32_t	first, count;	/* its vertices among the level's */
	int32_t		texture, light;	/* its stage's first picture; its light map (TEXS), -1 for none */
	uint32_t	light_at;	/* a word a vertex: where in the light map */
	uint32_t	table_at;	/* 256 colours a tagged vertex has one of */
	float		whole[2];	/* whole repeats taken off every vertex's coordinates */
	uint16_t	model, unused;
} se_run_t;
typedef struct {
	uint32_t	runs, models, vertices_at, vertices;
	float		grid_origin[3], grid_size[3];
	int32_t		grid_bounds[3];
	uint32_t	grid_at, grid_points;	/* a point: ambient, directed (three bytes each), two of direction */
} se_world_t;
typedef struct { uint32_t first_run, runs; float mins[3], maxs[3]; } se_bmodel_t;

/* SNDS, in the order of their names */
typedef struct { char name[64]; uint32_t at, samples, rate; uint16_t width, channels; } se_sound_t;

/* NRML: the 256 ways a model's vertex can face (three floats each), which its tag is one of */
#define SE_NORMALS	256

#endif
