/* ClassiCube's graphics on the ShaderEmu machine's GPU device (docs/gpu.md, docs/classicube.md). */
#include "Core.h"
#ifdef CC_BUILD_SHADEREMU
#define CC_SCRATCH_VBS_ARE_DYNAMIC
#include "_GraphicsBase.h"
#include "Errors.h"
#include "Window.h"
#include "World.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

/* from Window_ShaderEmu.c: GPU memory from 0x87000000, and where the window's pixels are */
uint8_t* SE_Gpu(void);
int SE_Surface(uint32_t* address, int* width, int* height, uint32_t* row);
uint32_t SE_Milliseconds(void);

/*########################################################################################################################*
*------------------------------------------------------GPU memory---------------------------------------------------------*
*#########################################################################################################################*/
#define GPU_PHYS 0x87000000u
#define GPU_END  0x00b00000u
/* Two sets: a frame is built in one while the other is still shown (docs/volume.md). */
#define SETS_AT 0x00700000u
#define MAX_COMMANDS 2048
#define MAX_BLOCKS 512
#define BLOCKS_IN (MAX_COMMANDS * 64)
#define ARENA_IN  (BLOCKS_IN + MAX_BLOCKS * 128)
#define SET_SIZE  0x00060000u
#define HEAP_AT   (SETS_AT + 2 * SET_SIZE)
/* the display's own framebuffer, which nothing uses while the desktop is layers (mode 4) */
#define SPARE_AT  0x00010000u
#define SPARE_END 0x00400000u
#define MAX_MESH 196608

#define REG_SUBMIT 0x10
#define REG_INTO 0x50
#define REG_LOCK 0x60
#define REG_VOLUME 0x300
#define next_frame() __asm__ volatile(".word 0x0100000f")

enum { CMD_CLEAR = 1, CMD_DRAW = 3 };
enum { VERTEX_CLIP = 1, VERTEX_MODELVIEW = 0x100, VERTEX_QUADS = 0x200, VERTEX_COMPACT = 0x400, VERTEX_FLOAT = 0x800,
       VERTEX_TAGGED = 0x1000, VERTEX_TABLE = 0x2000, VERTEX_PACKED = 0x4000 };
enum { FRAGMENT_COLOUR, FRAGMENT_TEXTURE, FRAGMENT_KEYED = 0x100 };
#define BASE_VERTEX (VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_QUADS | VERTEX_COMPACT | VERTEX_FLOAT)
/* a texel that is a hole where the alpha test is on: wholly transparent black */
#define KEY_TEXEL 0xFF000000u

static uint8_t* gpu;
static uint32_t set_at = SETS_AT, commands, blocks, mesh_vertices, arena_top, passes_used;
static uint32_t frame_number = 1, matrix_epoch = 1;
static uint32_t stat_commands, stat_mesh, stat_dropped;
/* texels and vertices made for the device, and the instructions that took */
static uint32_t stat_texels, stat_texel_cycles, stat_vertices, stat_vertex_cycles;

/* GPU memory handed out in spans; what is free is on a list of its own. */
struct Span { uint32_t at, size; struct Span *prev, *next, *free_prev, *free_next; int used; };
static struct Span *spans, *free_spans;
static uint32_t heap_free;

static void Free_Link(struct Span* s) {
	s->free_prev = NULL;
	s->free_next = free_spans;
	if (free_spans) free_spans->free_prev = s;
	free_spans = s;
}

static void Free_Unlink(struct Span* s) {
	if (s->free_prev) s->free_prev->free_next = s->free_next; else free_spans = s->free_next;
	if (s->free_next) s->free_next->free_prev = s->free_prev;
}

static void Heap_Add(uint32_t at, uint32_t end) {
	struct Span* s = (struct Span*)Mem_AllocCleared(1, sizeof(struct Span), "gpu span");
	struct Span* last = spans;
	s->at = at; s->size = end - at;
	while (last && last->next) last = last->next;
	s->prev = last;
	if (last) last->next = s; else spans = s;
	Free_Link(s);
	heap_free += s->size;
}

static struct Span* Heap_Alloc(uint32_t size) {
	struct Span *s, *rest;
	size = (size + 15) & ~15u;
	if (!size) size = 16;
	for (s = free_spans; s; s = s->free_next) {
		if (s->size < size) continue;
		if (s->size > size) {
			rest = (struct Span*)Mem_TryAllocCleared(1, sizeof(struct Span));
			if (!rest) return NULL;
			rest->at = s->at + size; rest->size = s->size - size;
			rest->prev = s; rest->next = s->next;
			if (s->next) s->next->prev = rest;
			s->next = rest;
			s->size = size;
			Free_Link(rest);
		}
		Free_Unlink(s);
		s->used = 1;
		heap_free -= size;
		return s;
	}
	return NULL;
}

static void Heap_Free(struct Span* s) {
	struct Span* o;
	s->used = 0;
	heap_free += s->size;
	o = s->next;
	if (o && !o->used && s->at + s->size == o->at) {
		Free_Unlink(o);
		s->size += o->size;
		s->next = o->next;
		if (o->next) o->next->prev = s;
		Mem_Free(o);
	}
	o = s->prev;
	if (o && !o->used && o->at + o->size == s->at) {
		o->size += s->size;
		o->next = s->next;
		if (s->next) s->next->prev = o;
		Mem_Free(s);
		return;
	}
	Free_Link(s);
}

/* A span given back may be in the list being drawn, or the one the volume display still shows:
   it is free two frames later. */
#define MAX_PENDING 1024
static struct Span* pending[2][MAX_PENDING];
static int pending_count[2];

static void Heap_FreeLater(struct Span* s) {
	if (!s) return;
	if (pending_count[0] == MAX_PENDING) { Heap_Free(s); return; }
	pending[0][pending_count[0]++] = s;
}

static void Heap_Age(void) {
	int i;
	for (i = 0; i < pending_count[1]; i++) Heap_Free(pending[1][i]);
	Mem_Copy(pending[1], pending[0], pending_count[0] * sizeof(struct Span*));
	pending_count[1] = pending_count[0];
	pending_count[0] = 0;
}

static void* Arena_Alloc(uint32_t size) {
	uint32_t at = arena_top;
	size = (size + 15) & ~15u;
	if (at + size > set_at + SET_SIZE) return NULL;
	arena_top += size;
	return gpu + at;
}


/*########################################################################################################################*
*---------------------------------------------------------General---------------------------------------------------------*
*#########################################################################################################################*/
static uint32_t clear_colour;
static uint32_t hold_frame, stats_ms = 5000;
static int view_x, view_y, view_w, view_h;
static struct Matrix se_view, se_proj;
static struct SETexture* se_texture;
static struct SEVb* se_vb;
static cc_bool se_depthOnly, se_alphaTest, se_alphaBlend, se_depthTest = true;
static uint32_t* chunk_table;   /* the colours every chunk's vertices are tagged with */
static uint32_t chunk_colours;

uint32_t cycles(void) {
	uint32_t v;
	__asm__ volatile("rdcycle %0" : "=r"(v));
	return v;
}

void Gfx_Create(void) {
	const char* s;
	int spare = 0, total;
	gpu = SE_Gpu();
	if (!gpu) Process_Abort("this machine has no GPU");

	Heap_Add(HEAP_AT, GPU_END);
	if (((volatile uint32_t*)gpu)[0] == 4) { Heap_Add(SPARE_AT, SPARE_END); spare = SPARE_END - SPARE_AT; }
	chunk_table = (uint32_t*)(gpu + Heap_Alloc(1024)->at);
	arena_top   = set_at + ARENA_IN;

	if ((s = getenv("CLASSICUBE_HOLD")))     hold_frame = atoi(s);
	if ((s = getenv("CLASSICUBE_STATS_MS"))) stats_ms   = atoi(s);

	Gfx.MaxTexWidth  = 1024;
	Gfx.MaxTexHeight = 1024;
	Gfx.NonPowTwoTexturesSupport = GFX_NONPOW2_FULL;
	Gfx.Created      = true;
	total = heap_free >> 10; spare >>= 10;
	Platform_Log2("GPU memory: %i KB, %i KB of it the display's", &total, &spare);
}

cc_bool Gfx_TryRestoreContext(void) { return true; }

void Gfx_Free(void) { Gfx_FreeState(); }

static void Gfx_FreeState(void) { FreeDefaultResources(); }
static void Gfx_RestoreState(void) {
	InitDefaultResources();
	gfx_format = -1;
}

cc_result Gfx_TakeScreenshot(struct Stream* output) { return ERR_NOT_SUPPORTED; }

void Gfx_GetApiInfo(cc_string* info) {
	int kb = heap_free >> 10;
	String_AppendConst(info, "-- Using the ShaderEmu GPU --\n");
	String_Format1(info, "GPU memory free: %i KB\n", &kb);
	PrintMaxTextureInfo(info);
}

void Gfx_SetVSync(cc_bool vsync) { gfx_vsync = vsync; }
cc_bool Gfx_WarnIfNecessary(void) { return false; }
cc_bool Gfx_GetUIOptions(struct MenuOptionsScreen* s) { return false; }

void Gfx_OnWindowResize(int width, int height) { Gfx_SetViewport(0, 0, width, height); }

void Gfx_SetViewport(int x, int y, int w, int h) {
	view_x = x; view_y = y; view_w = w; view_h = h;
	matrix_epoch++;
}
void Gfx_SetScissor(int x, int y, int w, int h) { }


/*########################################################################################################################*
*---------------------------------------------------------Textures--------------------------------------------------------*
*#########################################################################################################################*/
/* A texture is words of 0xTTRRGGBB (T: transparency). `wide` is the same four times across,
   for chunks: their vertices have 12 bits of u, and a run of blocks repeats it up to 16 times. */
struct SETexture { struct Span *span, *wide; int width, height; };
#define WIDE 4

static CC_INLINE uint32_t Texel(BitmapCol c) {
	return (c >> 24) == 0 ? KEY_TEXEL : c ^ 0xFF000000u;
}

uint32_t cycles(void);

static void CopyTexels(struct SETexture* tex, int x, int y, struct Bitmap* part, int rowWidth) {
	uint32_t began = cycles();
	const int width = tex->width, count = part->width, rows = part->height;
	uint32_t* restrict dst = (uint32_t*)(gpu + tex->span->at) + y * width + x;
	const BitmapCol* restrict src = part->scan0;
	int xx, yy;

	for (yy = 0; yy < rows; yy++, src += rowWidth, dst += width) {
		for (xx = 0; xx < count; xx++) dst[xx] = Texel(src[xx]);
	}
	stat_texels += count * rows;

	if (tex->wide) {
		/* the same texels again, four times across */
		const uint32_t* restrict from = (uint32_t*)(gpu + tex->span->at) + y * width + x;
		uint32_t* restrict a = (uint32_t*)(gpu + tex->wide->at) + y * width * WIDE + x;
		for (yy = 0; yy < rows; yy++, from += width, a += width * WIDE) {
			uint32_t *b = a + width, *c = b + width, *d = c + width;
			for (xx = 0; xx < count; xx++) {
				uint32_t t = from[xx];
				a[xx] = t; b[xx] = t; c[xx] = t; d[xx] = t;
			}
		}
	}
	stat_texel_cycles += cycles() - began;
}

static void MakeWide(struct SETexture* tex) {
	uint32_t *src, *dst;
	int x, y, k;
	tex->wide = Heap_Alloc(tex->width * tex->height * 4 * WIDE);
	if (!tex->wide) return;

	src = (uint32_t*)(gpu + tex->span->at);
	dst = (uint32_t*)(gpu + tex->wide->at);
	for (y = 0; y < tex->height; y++, src += tex->width) {
		for (k = 0; k < WIDE; k++)
			for (x = 0; x < tex->width; x++) *dst++ = src[x];
	}
}

GfxResourceID Gfx_AllocTexture(struct Bitmap* bmp, int rowWidth, cc_uint8 flags, cc_bool mipmaps) {
	struct SETexture* tex = (struct SETexture*)Mem_TryAllocCleared(1, sizeof(struct SETexture));
	if (!tex) return NULL;

	tex->span = Heap_Alloc(bmp->width * bmp->height * 4);
	if (!tex->span) { Mem_Free(tex); return NULL; }
	tex->width  = bmp->width;
	tex->height = bmp->height;
	CopyTexels(tex, 0, 0, bmp, rowWidth);
	return tex;
}

void Gfx_UpdateTexture(GfxResourceID texId, int x, int y, struct Bitmap* part, int rowWidth, cc_bool mipmaps) {
	CopyTexels((struct SETexture*)texId, x, y, part, rowWidth);
}

void Gfx_BindTexture(GfxResourceID texId) { se_texture = (struct SETexture*)texId; }

void Gfx_DeleteTexture(GfxResourceID* texId) {
	struct SETexture* tex = (struct SETexture*)(*texId);
	if (!tex) return;
	if (se_texture == tex) se_texture = NULL;

	Heap_FreeLater(tex->span);
	Heap_FreeLater(tex->wide);
	Mem_Free(tex);
	*texId = NULL;
}

void Gfx_EnableMipmaps(void)  { }
void Gfx_DisableMipmaps(void) { }


/*########################################################################################################################*
*-----------------------------------------------------State management----------------------------------------------------*
*#########################################################################################################################*/
void Gfx_SetFaceCulling(cc_bool enabled)   { }
void Gfx_SetAlphaArgBlend(cc_bool enabled) { }

static void SetAlphaBlend(cc_bool enabled) { se_alphaBlend = enabled; }
static void SetAlphaTest(cc_bool enabled)  { se_alphaTest  = enabled; }
static void SetDepthTest(cc_bool enabled)  { se_depthTest  = enabled; }
static void SetDepthWrite(cc_bool enabled) { }
static void SetColorWrite(cc_bool r, cc_bool g, cc_bool b, cc_bool a) { }

/* (water's depth is laid down first elsewhere: here a blended pass tests depth and writes none) */
void Gfx_DepthOnlyRendering(cc_bool depthOnly) { se_depthOnly = depthOnly; }

void Gfx_SetFog(cc_bool enabled)         { gfx_fogEnabled = enabled; }
static void SetFogColor(PackedCol color) { }
static void SetFogDensity(float value)   { }
static void SetFogEnd(float value)       { }
static void SetFogMode(FogFunc func)     { }


/*########################################################################################################################*
*---------------------------------------------------------Matrices--------------------------------------------------------*
*#########################################################################################################################*/
void Gfx_CalcOrthoMatrix(struct Matrix* matrix, float width, float height, float zNear, float zFar) {
	*matrix = Matrix_Identity;

	matrix->row1.x =  2.0f / width;
	matrix->row2.y = -2.0f / height;
	matrix->row3.z = -2.0f / (zFar - zNear);

	matrix->row4.x = -1.0f;
	matrix->row4.y =  1.0f;
	matrix->row4.z = -(zFar + zNear) / (zFar - zNear);
}

static float Cotangent(float x) { return Math_CosF(x) / Math_SinF(x); }
void Gfx_CalcPerspectiveMatrix(struct Matrix* matrix, float fov, float aspect, float zFar) {
	float zNear = 0.1f;
	float c = Cotangent(0.5f * fov);
	*matrix = Matrix_Identity;

	matrix->row1.x =  c / aspect;
	matrix->row2.y =  c;
	matrix->row3.z = -(zFar + zNear) / (zFar - zNear);
	matrix->row3.w = -1.0f;
	matrix->row4.z = -(2.0f * zFar * zNear) / (zFar - zNear);
	matrix->row4.w =  0.0f;
}

void Gfx_LoadMatrix(MatrixType type, const struct Matrix* matrix) {
	if (type == MATRIX_VIEW) se_view = *matrix;
	if (type == MATRIX_PROJ) se_proj = *matrix;
	matrix_epoch++;
}

void Gfx_LoadMVP(const struct Matrix* view, const struct Matrix* proj, struct Matrix* mvp) {
	se_view = *view;
	se_proj = *proj;
	matrix_epoch++;
	Matrix_Mul(mvp, view, proj);
}

void Gfx_EnableTextureOffset(float x, float y) { }
void Gfx_DisableTextureOffset(void) { }

/* The projection's rows 0, 1 and 3 with the viewport folded in, as the device takes them. */
static float proj_rows[16];
static uint32_t proj_epoch;

static void ProjectionRows(void) {
	const struct Matrix* p = &se_proj;
	int w = Window_Main.Width ? Window_Main.Width : 1, h = Window_Main.Height ? Window_Main.Height : 1;
	if (!view_w || !view_h) { view_x = view_y = 0; view_w = w; view_h = h; }
	float sx = (float)view_w / w, ox = (float)(2 * view_x + view_w) / w - 1.0f;
	float sy = (float)view_h / h, oy = (float)(2 * (h - view_y - view_h) + view_h) / h - 1.0f;
	float r0[4] = { p->row1.x, p->row2.x, p->row3.x, p->row4.x };
	float r1[4] = { p->row1.y, p->row2.y, p->row3.y, p->row4.y };
	float r3[4] = { p->row1.w, p->row2.w, p->row3.w, p->row4.w };
	int j;

	for (j = 0; j < 4; j++) {
		proj_rows[j]      = sx * r0[j] + ox * r3[j];
		proj_rows[4 + j]  = sy * r1[j] + oy * r3[j];
		proj_rows[12 + j] = r3[j];
	}
	proj_rows[8] = p->row1.z; proj_rows[9] = p->row2.z; proj_rows[10] = p->row3.z; proj_rows[11] = p->row4.z;
	proj_epoch = matrix_epoch;
}

/* A block of vectors for a draw: the projection, then the modelview's rows, for vertices that
   are `scale` times themselves plus `origin` (a chunk's are bytes from its corner). */
static uint32_t MatrixBlock(const float* origin, float scale) {
	const struct Matrix* m = &se_view;
	float* u;
	if (blocks == MAX_BLOCKS) return 0;
	if (proj_epoch != matrix_epoch) ProjectionRows();

	u = (float*)(gpu + set_at + BLOCKS_IN) + 32 * blocks;
	Mem_Copy(u, proj_rows, sizeof(proj_rows));
	u[16] = m->row1.x * scale; u[17] = m->row2.x * scale; u[18] = m->row3.x * scale;
	u[20] = m->row1.y * scale; u[21] = m->row2.y * scale; u[22] = m->row3.y * scale;
	u[24] = m->row1.z * scale; u[25] = m->row2.z * scale; u[26] = m->row3.z * scale;
	if (origin) {
		float x = origin[0], y = origin[1], z = origin[2];
		u[19] = m->row4.x + x * m->row1.x + y * m->row2.x + z * m->row3.x;
		u[23] = m->row4.y + x * m->row1.y + y * m->row2.y + z * m->row3.y;
		u[27] = m->row4.z + x * m->row1.z + y * m->row2.z + z * m->row3.z;
	} else {
		u[19] = m->row4.x; u[23] = m->row4.y; u[27] = m->row4.z;
	}
	return GPU_PHYS + set_at + BLOCKS_IN + 128 * blocks++;
}


/*########################################################################################################################*
*----------------------------------------------------------Buffers--------------------------------------------------------*
*#########################################################################################################################*/
/* A buffer's vertices as the device reads them, one of three ways:
   - compact: a texel a vertex (x, y, z, u | v << 16), all of one colour
   - tagged:  the same with u | v << 12 | tag << 24, the tag naming a colour in a table
   - packed:  a word a vertex (x, y, z bytes in eighths of a block from `origin`, a tag), u | v << 12 beside: a chunk */
struct SEMesh {
	uint32_t vertices, coords;   /* addresses in GPU memory */
	uint32_t flags, colour;      /* more of the vertex word; the one colour, or the table's address */
	float origin[3];
	uint32_t block, block_epoch, block_frame;
};

struct SEVb {
	int fmt, max, count;
	cc_bool dynamic;
	void* staging;                /* the vertices as the game writes them (a dynamic buffer keeps its own) */
	struct SEMesh mesh;
	struct Span* span;            /* GPU memory of its own, when it has any */
	uint32_t span_size;
	int where;                    /* 0 nowhere, 1 its span, 2 the frame's arena */
	uint32_t where_frame, drawn_frame;
};

static void* shared_staging;
static int shared_staging_size;

#define MAX_COLOURS 256
static uint32_t scan_colours[MAX_COLOURS];
static int scan_count;

static CC_INLINE uint32_t DeviceColour(PackedCol c) { return c ^ 0xFF000000u; }

static CC_INLINE int TagOf(uint32_t colour) {
	int i;
	for (i = scan_count - 1; i >= 0; i--) if (scan_colours[i] == colour) return i;
	if (scan_count == MAX_COLOURS) return MAX_COLOURS - 1;
	scan_colours[scan_count] = colour;
	return scan_count++;
}

/* a chunk's colours go into one table that every chunk shares */
static CC_INLINE int ChunkTagOf(uint32_t colour) {
	static uint32_t last_colour; static int last_tag = -1;
	uint32_t i;
	if (last_tag >= 0 && colour == last_colour) return last_tag;
	for (i = 0; i < chunk_colours; i++) if (chunk_table[i] == colour) break;
	if (i == chunk_colours) {
		if (chunk_colours == MAX_COLOURS) return MAX_COLOURS - 1;
		chunk_table[chunk_colours++] = colour;
	}
	last_colour = colour; last_tag = (int)i;
	return (int)i;
}

static CC_INLINE int RoundI(float f) { return (int)(f + 0.5f); }
static CC_INLINE float FloorF(float f) { int i = (int)f; return (float)(f < i ? i - 1 : i); }

/* Converts a buffer's vertices into `mesh`, in memory from `alloc`. False when there is none. */
typedef void* (*MeshAlloc)(struct SEVb* vb, uint32_t size);

static cc_bool BuildMeshIn(struct SEVb* vb, MeshAlloc alloc);

static cc_bool BuildMesh(struct SEVb* vb, MeshAlloc alloc) {
	uint32_t began = cycles();
	cc_bool made = BuildMeshIn(vb, alloc);
	stat_vertices += vb->count;
	stat_vertex_cycles += cycles() - began;
	return made;
}

static cc_bool BuildMeshIn(struct SEVb* vb, MeshAlloc alloc) {
	struct SEMesh* mesh = &vb->mesh;
	const int count = vb->count & ~3, stride = strideSizes[vb->fmt];
	const cc_bool textured = vb->fmt == VERTEX_FORMAT_TEXTURED;
	const uint8_t* restrict src = (const uint8_t*)vb->staging;
	cc_bool packed = false, tagged;
	float lo[3], hi[3];
	uint32_t prev, *restrict to, size, limit;
	int i, q, k, tag = 0;
	uint8_t* mem;

	mesh->block_epoch = 0;
	if (!count) return false;

	/* what kind: how many colours and, of a buffer that stays, how large a box */
	prev = ~((const uint32_t*)src)[3];
	scan_count = 0;
	if (textured && !vb->dynamic) {
		lo[0] = lo[1] = lo[2] = 1e30f; hi[0] = hi[1] = hi[2] = -1e30f;
		for (i = 0; i < count; i++) {
			const float* p = (const float*)(src + i * stride);
			uint32_t c = ((const uint32_t*)p)[3];
			if (c != prev) { TagOf(DeviceColour(c)); prev = c; }
			if (p[0] < lo[0]) lo[0] = p[0];
			if (p[0] > hi[0]) hi[0] = p[0];
			if (p[1] < lo[1]) lo[1] = p[1];
			if (p[1] > hi[1]) hi[1] = p[1];
			if (p[2] < lo[2]) lo[2] = p[2];
			if (p[2] > hi[2]) hi[2] = p[2];
		}
		packed = hi[0] - lo[0] <= 31.0f && hi[1] - lo[1] <= 31.0f && hi[2] - lo[2] <= 31.0f;
	} else {
		for (i = 0; i < count; i++) {
			uint32_t c = ((const uint32_t*)(src + i * stride))[3];
			if (c != prev) { TagOf(DeviceColour(c)); prev = c; }
		}
	}

	if (packed) {
		const uint32_t words = (count * 4 + 15) & ~15u;
		const float ox = FloorF(lo[0]), oy = FloorF(lo[1]), oz = FloorF(lo[2]);
		uint32_t* restrict coords;
		mem = (uint8_t*)alloc(vb, 2 * words);
		if (!mem) return false;
		to = (uint32_t*)mem; coords = (uint32_t*)(mem + words);
		prev = ~((const uint32_t*)src)[3];

		for (i = 0; i < count; i++) {
			const struct VertexTextured* v = (const struct VertexTextured*)(src + i * stride);
			int x = RoundI((v->x - ox) * 8.0f), y = RoundI((v->y - oy) * 8.0f), z = RoundI((v->z - oz) * 8.0f);
			int s = RoundI(v->U * (1024.0f / WIDE)), t = RoundI(v->V * 1024.0f);
			if (v->Col != prev) { prev = v->Col; tag = ChunkTagOf(DeviceColour(prev)); }
			if (s < 0) s = 0;
			if (s > 4095) s = 4095;
			if (t < 0) t = 0;
			if (t > 4095) t = 4095;
			to[i]     = (uint32_t)x | (uint32_t)y << 8 | (uint32_t)z << 16 | (uint32_t)tag << 24;
			coords[i] = (uint32_t)s | (uint32_t)t << 12;
		}
		mesh->origin[0] = ox; mesh->origin[1] = oy; mesh->origin[2] = oz;
		mesh->vertices = (uint32_t)(mem - gpu);
		mesh->coords   = mesh->vertices + words;
		mesh->flags    = VERTEX_TAGGED | VERTEX_TABLE | VERTEX_PACKED;
		mesh->colour   = GPU_PHYS + (uint32_t)((uint8_t*)chunk_table - gpu);
		return true;
	}

	/* one colour is the command's; several are tags, and their table after the vertices */
	tagged = scan_count > 1;
	size   = count * 16;
	mem    = (uint8_t*)alloc(vb, size + (tagged ? scan_count * 4 : 0));
	if (!mem) return false;
	to    = (uint32_t*)mem;
	limit = tagged ? 4095 : 32767;
	prev  = ~((const uint32_t*)src)[3];

	for (q = 0; q < count; q += 4) {
		float u0 = 0, v0 = 0;
		if (textured) {
			/* a quad's coordinates start in the texture's first repeat */
			const struct VertexTextured* v = (const struct VertexTextured*)(src + q * stride);
			u0 = min(min(v[0].U, v[1].U), min(v[2].U, v[3].U)); v0 = min(min(v[0].V, v[1].V), min(v[2].V, v[3].V));
			u0 = u0 >= 0.0f && u0 < 1.0f ? 0.0f : FloorF(u0);
			v0 = v0 >= 0.0f && v0 < 1.0f ? 0.0f : FloorF(v0);
		}
		for (k = 0; k < 4; k++, to += 4) {
			const uint32_t* p = (const uint32_t*)(src + (q + k) * stride);
			uint32_t fourth = 0;
			to[0] = p[0]; to[1] = p[1]; to[2] = p[2];
			if (tagged) {
				if (p[3] != prev) { prev = p[3]; tag = TagOf(DeviceColour(prev)); }
				fourth = (uint32_t)tag << 24;
			}
			if (textured) {
				const struct VertexTextured* v = (const struct VertexTextured*)p;
				uint32_t s = (uint32_t)RoundI((v->U - u0) * 1024.0f), t = (uint32_t)RoundI((v->V - v0) * 1024.0f);
				if (s > limit) s = limit;
				if (t > limit) t = limit;
				fourth |= s | t << (tagged ? 12 : 16);
			}
			to[3] = fourth;
		}
	}
	mesh->vertices = (uint32_t)(mem - gpu);
	if (tagged) {
		uint32_t* table = (uint32_t*)(mem + size);
		for (i = 0; i < scan_count; i++) table[i] = scan_colours[i];
		mesh->flags  = VERTEX_TAGGED | VERTEX_TABLE;
		mesh->colour = GPU_PHYS + size + mesh->vertices;
	} else {
		mesh->flags  = 0;
		mesh->colour = DeviceColour(((const uint32_t*)src)[3]);
	}
	return true;
}

static void* SpanAlloc(struct SEVb* vb, uint32_t size) {
	if (vb->span && vb->span_size >= size && vb->drawn_frame != frame_number) return gpu + vb->span->at;
	Heap_FreeLater(vb->span);
	vb->span = Heap_Alloc(size);
	vb->span_size = size;
	return vb->span ? gpu + vb->span->at : NULL;
}
static void* ArenaAlloc(struct SEVb* vb, uint32_t size) { return Arena_Alloc(size); }

GfxResourceID Gfx_CreateIb2(int count, Gfx_FillIBFunc fillFunc, void* obj) { return (void*)1; }
void Gfx_BindIb(GfxResourceID ib)    { }
void Gfx_DeleteIb(GfxResourceID* ib) { }

static GfxResourceID Gfx_AllocStaticVb(VertexFormat fmt, int count) {
	struct SEVb* vb;
	/* (a chunk takes 8 bytes a vertex; with less than that left the game is to draw less far) */
	if (heap_free < (uint32_t)count * 8 + 65536) return NULL;
	vb = (struct SEVb*)Mem_TryAllocCleared(1, sizeof(struct SEVb));
	if (!vb) return NULL;
	vb->fmt = fmt; vb->max = count;
	return vb;
}

static GfxResourceID Gfx_AllocDynamicVb(VertexFormat fmt, int maxVertices) {
	struct SEVb* vb = (struct SEVb*)Mem_TryAllocCleared(1, sizeof(struct SEVb));
	if (!vb) return NULL;
	vb->fmt = fmt; vb->max = maxVertices; vb->dynamic = true;
	vb->staging = Mem_TryAlloc(maxVertices, strideSizes[fmt]);
	if (!vb->staging) { Mem_Free(vb); return NULL; }
	return vb;
}

void Gfx_BindVb(GfxResourceID vb) { se_vb = (struct SEVb*)vb; }

void Gfx_DeleteVb(GfxResourceID* vb) {
	struct SEVb* data = (struct SEVb*)(*vb);
	if (!data) return;
	if (se_vb == data) se_vb = NULL;

	Heap_FreeLater(data->span);
	if (data->dynamic) Mem_Free(data->staging);
	Mem_Free(data);
	*vb = NULL;
}

void* Gfx_LockVb(GfxResourceID vb, VertexFormat fmt, int count) {
	struct SEVb* data = (struct SEVb*)vb;
	int size = count * strideSizes[fmt];
	data->fmt = fmt; data->count = count;

	if (size > shared_staging_size) {
		Mem_Free(shared_staging);
		shared_staging = Mem_Alloc(size, 1, "vertices");
		shared_staging_size = size;
	}
	data->staging = shared_staging;
	return data->staging;
}

void Gfx_UnlockVb(GfxResourceID vb) {
	struct SEVb* data = (struct SEVb*)vb;
	data->where   = BuildMesh(data, SpanAlloc) ? 1 : 0;
	data->staging = NULL;
}

/* Gfx_LockVb and Gfx_UnlockVb for vertices that are somewhere already: a chunk's, which a worker
   core made in memory of its own (MapRenderer.c). */
cc_bool Gfx_SE_UnlockFrom(GfxResourceID vb, void* vertices, VertexFormat fmt, int count) {
	struct SEVb* data = (struct SEVb*)vb;
	data->fmt = fmt; data->count = count;
	data->staging = vertices;
	data->where   = BuildMesh(data, SpanAlloc) ? 1 : 0;
	data->staging = NULL;
	return data->where;
}

void Gfx_BindDynamicVb(GfxResourceID vb) { se_vb = (struct SEVb*)vb; }

void* Gfx_LockDynamicVb(GfxResourceID vb, VertexFormat fmt, int count) {
	struct SEVb* data = (struct SEVb*)vb;
	data->fmt = fmt; data->count = count;
	return data->staging;
}

/* A buffer filled again before it was drawn a second time lives in the frame; one that is
   drawn frame after frame as it is gets memory of its own. */
void Gfx_UnlockDynamicVb(GfxResourceID vb) {
	struct SEVb* data = (struct SEVb*)vb;
	se_vb = data;
	if (data->where == 1 && data->drawn_frame != frame_number && BuildMesh(data, SpanAlloc)) return;
	data->where       = BuildMesh(data, ArenaAlloc) ? 2 : 0;
	data->where_frame = frame_number;
}

void Gfx_DeleteDynamicVb(GfxResourceID* vb) { Gfx_DeleteVb(vb); }


/*########################################################################################################################*
*--------------------------------------------------------Rendering--------------------------------------------------------*
*#########################################################################################################################*/
static uint32_t* last_draw;
static uint32_t last_state[6], last_end;
static const struct SEVb* last_vb;

void Gfx_SetVertexFormat(VertexFormat fmt) {
	gfx_format = fmt;
	gfx_stride = strideSizes[fmt];
}

void Gfx_DrawVb_Lines(int verticesCount) { }

static void DrawQuads(int verticesCount, int startVertex) {
	struct SEVb* vb = se_vb;
	struct SEMesh* mesh;
	struct SETexture* tex = gfx_format == VERTEX_FORMAT_TEXTURED ? se_texture : NULL;
	uint32_t pass, fragment, vertex, block, address, mesh_count, state[6], *c;
	cc_bool packed;

	if (!vb || se_depthOnly || verticesCount < 4) return;
	if (vb->where == 2 && vb->where_frame != frame_number) {
		/* drawn again as it was a frame ago */
		vb->where = vb->staging && BuildMesh(vb, SpanAlloc) ? 1 : 0;
	}
	if (!vb->where) return;
	vb->drawn_frame = frame_number;
	mesh   = &vb->mesh;
	packed = (mesh->flags & VERTEX_PACKED) != 0;

	mesh_count = (uint32_t)(verticesCount >> 2) * 6;
	if (mesh_vertices + mesh_count > MAX_MESH) { stat_dropped++; return; }

	pass     = se_depthTest ? (se_alphaBlend ? 1 : 0) : 5;
	fragment = pass << 16;
	address  = 0;
	if (tex) {
		fragment |= FRAGMENT_TEXTURE;
		if (pass == 0 && se_alphaTest) fragment |= FRAGMENT_KEYED;
		if (packed && !tex->wide) MakeWide(tex);
		address = packed ? (tex->wide ? tex->wide->at : 0) : tex->span->at;
		if (!address) return;
	}
	vertex = BASE_VERTEX | mesh->flags;

	if (packed) {
		if (mesh->block_epoch != matrix_epoch || mesh->block_frame != frame_number) {
			mesh->block = MatrixBlock(mesh->origin, 0.125f);
			mesh->block_epoch = matrix_epoch; mesh->block_frame = frame_number;
		}
		block = mesh->block;
	} else {
		static uint32_t plain, plain_epoch, plain_frame;
		if (plain_epoch != matrix_epoch || plain_frame != frame_number) {
			plain = MatrixBlock(NULL, 1.0f);
			plain_epoch = matrix_epoch; plain_frame = frame_number;
		}
		block = plain;
	}
	if (!block) { stat_dropped++; return; }

	state[0] = vertex; state[1] = fragment; state[2] = block; state[3] = address; state[4] = mesh->colour; state[5] = mesh->vertices;
	if (last_draw && last_vb == vb && last_end == (uint32_t)startVertex && state[0] == last_state[0] &&
		state[1] == last_state[1] && state[2] == last_state[2] && state[3] == last_state[3] && state[4] == last_state[4] && state[5] == last_state[5]) {
		/* the vertices after the last draw's, in the same state: more of that command */
		last_draw[2]  += mesh_count;
		mesh_vertices += mesh_count;
		last_end = startVertex + verticesCount;
		return;
	}
	if (commands == MAX_COMMANDS) { stat_dropped++; return; }

	c = (uint32_t*)(gpu + set_at) + 16 * commands++;
	c[0]  = CMD_DRAW;
	c[1]  = GPU_PHYS + mesh->vertices + (uint32_t)startVertex * (packed ? 4 : 16);
	c[2]  = mesh_count;
	c[3]  = mesh_vertices;
	c[4]  = vertex;
	c[5]  = fragment;
	c[6]  = block;
	c[7]  = GPU_PHYS + address;
	c[8]  = tex ? (uint32_t)tex->width * (packed ? WIDE : 1) : 0;
	c[9]  = tex ? (uint32_t)tex->height : 0;
	c[10] = KEY_TEXEL;
	c[11] = mesh->colour;
	c[12] = packed ? GPU_PHYS + mesh->coords + (uint32_t)startVertex * 4 : 0;
	mesh_vertices += mesh_count;
	passes_used   |= (1u << pass) & 0xfe;

	last_draw = c; last_vb = vb; last_end = startVertex + verticesCount;
	Mem_Copy(last_state, state, sizeof(state));
}

void Gfx_DrawVb_IndexedTris_Range(int verticesCount, int startVertex, DrawHints hints) {
	DrawQuads(verticesCount, startVertex);
}

void Gfx_DrawVb_IndexedTris(int verticesCount) {
	DrawQuads(verticesCount, 0);
}

void Gfx_DrawIndexedTris_T2fC4b(int verticesCount, int startVertex, DrawHints hints) {
	DrawQuads(verticesCount, startVertex);
}


/*########################################################################################################################*
*---------------------------------------------------------Frames----------------------------------------------------------*
*#########################################################################################################################*/
void Gfx_ClearColor(PackedCol color) { clear_colour = DeviceColour(color) & 0xFFFFFF; }

/* The device starts every list with a fresh depth buffer; only the colour needs a command. */
void Gfx_ClearBuffers(GfxBuffers buffers) {
	uint32_t* c;
	if (!(buffers & GFX_BUFFER_COLOR) || commands) return;
	c = (uint32_t*)(gpu + set_at) + 16 * commands++;
	c[0] = CMD_CLEAR;
	c[1] = clear_colour;
	c[3] = mesh_vertices;
	mesh_vertices += 3;
	last_draw = NULL;
}

void Gfx_BeginFrame(void) { }

/* The frame after next stays on the screen, as with CLASSICUBE_HOLD. */
void SE_Hold(void) { hold_frame = frame_number + 2; }

/* Every few seconds: frames a second and the machine's instructions a frame (ccstat: lines). */
static void Stats(void) {
	static uint32_t since_ms, since_cycles, since_frames;
	static cc_bool said_steady;
	uint32_t now = SE_Milliseconds(), n = frame_number - since_frames;

	stat_commands += commands; stat_mesh += mesh_vertices;
	if (!since_ms) { since_ms = now; since_cycles = cycles(); since_frames = frame_number; stat_commands = stat_mesh = 0; return; }
	if (!stats_ms || now - since_ms < stats_ms || !n) return;

	printf("ccstat: %u frames in %u ms, %u instructions a frame, %u commands, %u quads, %u draws dropped; GPU memory %u KB free\n",
		n, now - since_ms, (cycles() - since_cycles) / n, stat_commands / n, stat_mesh / n / 6, stat_dropped, heap_free >> 10);
	printf("ccstat: a frame: %u texels in %u instructions, %u vertices in %u\n",
		stat_texels / n, stat_texel_cycles / n, stat_vertices / n, stat_vertex_cycles / n);
#ifdef SE_COUNT
	{
		extern unsigned se_chunk[8];
		unsigned c = se_chunk[0] ? se_chunk[0] : 1;
		printf("ccstat: %u chunks, each in thousands: read %u, light %u, faces %u, count %u, offsets %u, blocks %u, vertices %u\n",
			se_chunk[0], se_chunk[1] / c / 1000, se_chunk[2] / c / 1000, se_chunk[3] / c / 1000, se_chunk[4] / c / 1000,
			se_chunk[5] / c / 1000, se_chunk[6] / c / 1000, se_chunk[7] / c / 1000);
		Mem_Set(se_chunk, 0, sizeof(se_chunk));
	}
#endif
	/* (for a test to wait for: the world is built and frames come steadily) */
	if (!said_steady && n >= 40 && World.Loaded) { said_steady = true; printf("classicube: steady\n"); }
	fflush(stdout);
	since_ms = now; since_cycles = cycles(); since_frames = frame_number;
	stat_commands = stat_mesh = stat_dropped = 0;
	stat_texels = stat_texel_cycles = stat_vertices = stat_vertex_cycles = 0;
}

void Gfx_EndFrame(void) {
	volatile uint32_t* regs = (volatile uint32_t*)gpu;
	uint32_t address, row;
	int width, height;

	Stats();
	if (commands && SE_Surface(&address, &width, &height, &row)) {
		for (;;) {
			while (__atomic_exchange_n(&regs[REG_LOCK / 4], 1, __ATOMIC_ACQUIRE)) sched_yield();
			if (regs[REG_SUBMIT / 4] == 0) break;
			__atomic_store_n(&regs[REG_LOCK / 4], 0, __ATOMIC_RELEASE);
			next_frame();
		}
		regs[REG_INTO / 4]     = address;
		regs[REG_INTO / 4 + 1] = width;
		regs[REG_INTO / 4 + 2] = height;
		regs[REG_INTO / 4 + 3] = row;
		regs[REG_SUBMIT / 4 + 1] = GPU_PHYS + set_at;
		regs[REG_SUBMIT / 4 + 2] = commands;
		regs[REG_SUBMIT / 4]     = 1 | 4 | passes_used << 8;
		__atomic_store_n(&regs[REG_LOCK / 4], 0, __ATOMIC_RELEASE);
		/* the list and the buffers are memory the game goes on to change: wait until drawn */
		while (regs[REG_SUBMIT / 4] != 0) next_frame();

		regs[REG_VOLUME / 4 + 1] = commands;
		regs[REG_VOLUME / 4]     = GPU_PHYS + set_at;
		regs[REG_VOLUME / 4 + 2]++;
		set_at = set_at == SETS_AT ? SETS_AT + SET_SIZE : SETS_AT;
	}
	if (hold_frame && frame_number >= hold_frame) {
		printf("classicube: holding frame %u\n", frame_number);
		fflush(stdout);
		for (;;) next_frame();
	}

	Heap_Age();
	frame_number++;
	matrix_epoch++;
	commands = blocks = mesh_vertices = passes_used = 0;
	arena_top = set_at + ARENA_IN;
	last_draw = NULL;
}
#endif
