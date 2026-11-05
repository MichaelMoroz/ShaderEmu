/*
 * The machine's GPU as Quake III Arena's renderer draws with it (gpu_shaderemu.c): beside the
 * library's own calls (GLES/segl.h), draws whose command the renderer says word for word.
 */
#ifndef SHADEREMU_QUAKE3_GPU_H
#define SHADEREMU_QUAKE3_GPU_H

/* more of a draw's vertex word (docs/machine/gpu.md) */
#define GPU_QUADS	0x200		/* four corners a quad, drawn as six vertices */
#define GPU_TAGGED	0x1000		/* the fourth word is u | v << 12 | tag << 24 */
#define GPU_TABLE	0x2000		/* a vertex's colour is the table's word for its tag */
#define GPU_PACKED	0x4000		/* a vertex is a word, its coordinates a word in a second buffer */
#define GPU_FACING	0x20000		/* colour: a table of 256 normals; more[1]: the light's direction, its colour, the ambient colour */
#define GPU_MOVED	0x10000		/* more[3]: two rows of three floats the texture coordinates are moved by */
#define GPU_ARRAYS	0x100000	/* a renderer's arrays by index: more[0..2] positions, coordinates, colours, more[3] flags (gpu.h) */
/* a draw's fragment word: how it is coloured, what else, and in which pass */
#define GPU_COLOUR	0
#define GPU_WORDS	1		/* a texture of 0xTTRRGGBB words */
#define GPU_BYTES	4		/* a texture of three bytes a texel */
#define GPU_KEYED	0x100
#define GPU_LAID	0x200
#define GPU_SMOOTH	0x400
#define GPU_LIT		0x800		/* times a light map: more[0] the vertices' places in it, more[1] it, more[2] its size */
#define GPU_CLAMPED	0x1000		/* the texture does not repeat: its edge goes on */
#define GPU_PASS(n)	((unsigned)(n) << 16)	/* 0 opaque, 1 blended, 2 added, 3 multiplying; 4 to 7 the same without depth */

typedef struct {
	unsigned	texture, width, height;	/* where the GPU reads it: GPU memory or the ROM */
	unsigned	fragment, vertex, key;
	unsigned	colour;			/* 0xTTRRGGBB, T transparency; or a table's address */
	unsigned	more[4];		/* the command's words 12 to 15 */
} gpu_state;

void gpuWindowOrigin (int *x, int *y);	/* the window's first pixel on the display */
unsigned gpuAddress (const void *memory);	/* GPU memory as a command names it */
void gpuProjection (const float *columns);	/* the matrices draws are made with from now on */
void gpuModelview (const float *columns);
void gpuModelviewRows (const float *rows);	/* the same from its three rows of four, when nothing else changed since the draw before */
float *gpuModelviewBlock (void);		/* those rows where the GPU reads them, to be filled; NULL: gpuModelviewRows */

/* What seglMemory() has given out so far, and all of it since then given back. */
unsigned gpuMemoryMark (void);
void gpuMemoryRelease (unsigned mark);

/*
 * A draw of `mesh` vertices that are where they are: at an address of GPU memory, or (packed
 * vertices) of the ROM. NULL when the frame is full.
 */
unsigned *gpuDrawAt (const gpu_state *s, unsigned vertices, int mesh);
/*
 * Room in the frame for `stored` compact vertices of a draw of `mesh` (as many, or six to
 * four corners with GPU_QUADS): more of the draw before when nothing differs. NULL: no room.
 */
unsigned *gpuDrawRoom (const gpu_state *s, int stored, int mesh);
unsigned *gpuDrawsRoom (int count, unsigned mesh, unsigned passes, unsigned *block, unsigned *first_mesh);	/* draws for any core to write */
unsigned gpuMatricesBlock (void);
void gpuMatricesChanged (void);
void gpuDrawPut (unsigned *c, const gpu_state *s, unsigned block, unsigned vertices, unsigned first_mesh, int mesh);
void *gpuFrameMemory (unsigned bytes);		/* memory of this frame's own, for a draw to point at */
int gpuFrameTrim (void *at, unsigned bytes);	/* the last such memory cut to `bytes` (0: not the last, or not so large) */
int gpuFrameHolds (const void *p, unsigned bytes);	/* those bytes are this frame's memory */
/* A list built in two parts at once (SE_HALVES: Jedi Outcast's drawing on two workers): the first's split of what is left,
 * the second's beginning and end of its part, and its commands moved after the first's; each takes 256 bytes. */
int gpuHalfSplit (void *half);
void gpuHalfBegin (const void *half);
void gpuHalfEnd (void *half);
void gpuHalfMerge (const void *half);

/* Commands that stay at the head of every frame's list: so many, of so many vertices in all. */
int gpuKeep (int commands, int mesh);
/* One of them in one of the two sets frames alternate between: written when the level loads. */
unsigned *gpuKept (int set, int index, const gpu_state *s, unsigned vertices, int first_mesh, int mesh);
unsigned *gpuKeptCommand (int index);		/* in the set of the frame being made: its 16 words */
void gpuSwap (int wait);			/* shows the frame; waits until it is drawn, or does not */
void gpuEnd (unsigned *ended);		/* the frame ended: its set, commands and passes; the next goes to the other set */
void gpuShow (const unsigned *ended);	/* a frame so ended shown, without waiting */
int gpuIdle (void);			/* the GPU has no list waiting */
void gpuTouchSets (void (*touch) (void *, unsigned));	/* the two sets' memory, for a worker core to write */
unsigned gpuListSum (unsigned rom);			/* a sum over the list being built (QUAKE3_SUM) */

#endif
