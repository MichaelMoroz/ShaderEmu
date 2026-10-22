/* ClassiCube's graphics on the ShaderEmu machine's GPU device (docs/machine/gpu.md, docs/games/classicube.md). */
#include "Core.h"
#ifdef CC_BUILD_SHADEREMU
#define CC_SCRATCH_VBS_ARE_DYNAMIC
#include "_GraphicsBase.h"
#include "Errors.h"
#include "Window.h"
#include "World.h"
#include "MapRenderer.h"
#include "Game.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <pthread.h>
#include "mcwx.h"

/* from Window_ShaderEmu.c: GPU memory from 0x87000000, and where the window's pixels are */
uint8_t* SE_Gpu(void);
int SE_Surface(uint32_t* address, int* width, int* height, uint32_t* row);
uint32_t SE_Milliseconds(void);
int SE_Flat(void);

/*########################################################################################################################*
*------------------------------------------------------GPU memory---------------------------------------------------------*
*#########################################################################################################################*/
#ifdef __gpuvm__
#define GPU_PHYS 0x01000000u /* gpu_vm: the device window moved down by 0x86000000 */
#else
#define GPU_PHYS 0x87000000u
#endif
#define GPU_END  0x00b00000u
/* Two sets: a frame is built in one while the other is still shown (docs/world/volume.md). */
#define SETS_AT 0x00700000u
#define MAX_COMMANDS 2048
#define MAX_BLOCKS 512
/* The first blocks of a set stay a chunk mesh's own from frame to frame (its slot), and the
   last of them is the camera's with no origin; the rest are handed out anew every frame. */
#define KEPT_BLOCKS 384
#define PLAIN_SLOT  (KEPT_BLOCKS - 1)
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
#ifdef __gpuvm__
#define next_frame() __asm__ volatile("wait")
#else
#define next_frame() __asm__ volatile(".word 0x0100000f")
#endif

enum { CMD_CLEAR = 1, CMD_DRAW = 3 };
enum { VERTEX_CLIP = 1, VERTEX_MODELVIEW = 0x100, VERTEX_QUADS = 0x200, VERTEX_COMPACT = 0x400, VERTEX_FLOAT = 0x800,
       VERTEX_TAGGED = 0x1000, VERTEX_TABLE = 0x2000, VERTEX_PACKED = 0x4000 };
enum { FRAGMENT_COLOUR, FRAGMENT_TEXTURE, FRAGMENT_KEYED = 0x100 };
#define BASE_VERTEX (VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_QUADS | VERTEX_COMPACT | VERTEX_FLOAT)
/* a texel that is a hole where the alpha test is on: wholly transparent black */
#define KEY_TEXEL 0xFF000000u

/* What a draw's command has of the state, worked out when the state changes and not at every
   draw: one for buffers of compact vertices, one for a chunk's packed ones (DeriveState). */
struct SEState { uint32_t serial, skip, fragment, address, width, height, pass_bit, spare; };
struct SEVb;

/* What every draw reads and writes, in one place: one address in a register serves it all. */
static struct {
	uint32_t* set;                /* the frame's set, where its commands are */
	uint32_t si;                  /* which of the two it is */
	struct SESeg* rec;            /* the walk being noted (Gfx_SE_SegmentBegin), or NULL */
	uint32_t commands, mesh_vertices, passes_used;
	uint32_t frame, matrix_epoch, state_serial;
	uint32_t* last_draw;          /* the command a draw may go on from (Emit) */
	const struct SEVb* last_vb;
	uint32_t last_serial, last_block, last_end;
	struct SEVb* vb;
	struct SEState state[2];
} draw = { NULL, 0, NULL, 0, 0, 0, 1, 1, 1 };
#define commands      draw.commands
#define mesh_vertices draw.mesh_vertices
#define passes_used   draw.passes_used
#define frame_number  draw.frame
#define matrix_epoch  draw.matrix_epoch
#define state_serial  draw.state_serial
#define last_draw     draw.last_draw
#define last_vb       draw.last_vb
#define se_vb         draw.vb

static uint8_t* gpu;
static uint32_t set_at = SETS_AT, blocks = KEPT_BLOCKS, arena_top;
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

static void Settle(void);

static void Heap_FreeLater(struct Span* s) {
	if (!s) return;
	if (pending_count[0] == MAX_PENDING) { Settle(); Heap_Free(s); return; }
	pending[0][pending_count[0]++] = s;
}

static void Heap_Age(void) {
	int i;
	for (i = 0; i < pending_count[1]; i++) Heap_Free(pending[1][i]);
	Mem_Copy(pending[1], pending[0], pending_count[0] * sizeof(struct Span*));
	pending_count[1] = pending_count[0];
	pending_count[0] = 0;
}

/* The list submitted last is not waited for (Gfx_EndFrame): the game's next frame runs while
   it is drawn. What it reads must stay until then: the frame before's buffers and textures.
   Whoever writes such memory in place settles first; a frame's own set is never read by it. */
static cc_bool unsettled;

static cc_bool Drawing(void) {
	if (unsettled && ((volatile uint32_t*)gpu)[REG_SUBMIT / 4] == 0) unsettled = false;
	return unsettled;
}

/* (the first core with nothing to do until the next pass says so: docs/machine/multicore.md, "A core
   that rests"; the frame's worker ends its pass) */
static uint32_t stat_job, stat_job_wait, stat_join;   /* the frame's worker's instructions, and waits */
uint32_t cycles(void);

static void Settle(void) {
	uint32_t began;
	if (!Drawing()) return;
	if (!mcw_on_worker()) { while (Drawing()) mcw_rest(); return; }
	began = cycles();
	while (Drawing()) mc_end_pass();
	stat_job_wait += cycles() - began;
}

/* The rest of a frame's list made by a worker (Game.c): a call, joined when core 0 next needs
   the list or what it is made of, and then submitted. */
static mcw_handle frame_job;
void Gfx_EndFrame(void);

/* The game's other threads (HTTP's) wait for the heap while a job is out: only the game's
   own thread joins it (Gfx_SE_Own). */
static pthread_mutex_t se_heap = PTHREAD_MUTEX_INITIALIZER;
static pthread_t se_main;
static cc_bool se_main_known;

float SE_SeatSheet(void);
static float sheet_at;	/* how far before a sitter's camera the 2D screen is drawn, in blocks; 0: flat */
/* a seated player's menu: its sheet stays before the camera it opened under, whose inverse this is */
int SE_SeatMenuView(struct Matrix* view);
void SE_RigidInverse(struct Matrix* out, const struct Matrix* m);
static cc_bool sheet_fixed;
static struct Matrix sheet_back;

void Gfx_SE_Post(mcw_fn job) {
	pthread_mutex_lock(&se_heap);
	frame_job = mcw_call(job, 0, 0);
}
/* (by the job: its instructions) */
void Gfx_SE_JobCycles(uint32_t n) { stat_job += n; }

void Gfx_SE_Settle(void) {
	mcw_handle h = frame_job;
	if (!h) return;
	uint32_t began = cycles();
	frame_job = 0;
	mcw_join(h);
	pthread_mutex_unlock(&se_heap);
	stat_join += cycles() - began;
	Gfx_EndFrame();
}

static cc_bool OnMainThread(void) { return !se_main_known || pthread_equal(pthread_self(), se_main); }

/* The game's thread, before it changes what the list is made of: textures, buffers, the heap, the map */
void Gfx_SE_Own(void) {
	if (frame_job && !mcw_on_worker() && OnMainThread()) Gfx_SE_Settle();
}

/* Around the C library's heap (Mem_* in _PlatformBase.h): one core at a time in it */
void Gfx_SE_HeapEnter(void) {
	if (mcw_on_worker()) return;
	if (OnMainThread()) Gfx_SE_Own();
	else pthread_mutex_lock(&se_heap);
}
void Gfx_SE_HeapLeave(void) {
	if (!mcw_on_worker() && !OnMainThread()) pthread_mutex_unlock(&se_heap);
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
static cc_bool mesh_sums, texture_sums, swap_waits, hold_late, segs_off, check_kept;
/* CLASSICUBE_CHECK_KEPT=1: the outline and the held block are made again when kept, and compared */
unsigned se_kept_checked[4];
int SE_CheckKept(void) { return check_kept; }
/* Goes up when a buffer or a texture is made again or let go: a walk noted before is stale. */
static uint32_t kept_epoch = 1;
static int view_x, view_y, view_w, view_h;
static struct Matrix se_view, se_proj;
static struct SETexture* se_texture;
static cc_bool se_depthOnly, se_alphaTest, se_alphaBlend, se_depthTest = true;
static uint32_t* chunk_table;   /* the colours every chunk's vertices are tagged with */
static uint32_t chunk_colours;

uint32_t cycles(void) {
	uint32_t v;
#ifdef __gpuvm__
	__asm__ volatile("mfsr %0, cycle" : "=r"(v));
#else
	__asm__ volatile("rdcycle %0" : "=r"(v));
#endif
	return v;
}

void Gfx_Create(void) {
	const char* s;
	int spare = 0, total;
	gpu = SE_Gpu();
	if (!gpu) Process_Abort("this machine has no GPU");
	draw.set = (uint32_t*)(gpu + set_at);
	se_main = pthread_self(); se_main_known = true;

	Heap_Add(HEAP_AT, GPU_END);
	if (((volatile uint32_t*)gpu)[0] == 4) { Heap_Add(SPARE_AT, SPARE_END); spare = SPARE_END - SPARE_AT; }
	chunk_table = (uint32_t*)(gpu + Heap_Alloc(1024)->at);
	arena_top   = set_at + ARENA_IN;

	if ((s = getenv("CLASSICUBE_HOLD")))     hold_frame = atoi(s);
	if ((s = getenv("CLASSICUBE_STATS_MS"))) stats_ms   = atoi(s);
	mesh_sums = getenv("CLASSICUBE_MESH_SUM") != NULL;
	texture_sums = getenv("CLASSICUBE_TEXTURE_SUM") != NULL;
	/* CLASSICUBE_SWAP=wait: every list waited for, as it was. CLASSICUBE_HOLD_LATE=1: the held
	   frame stays while the next is made ready, and the game stops before that one is shown. */
	swap_waits = getenv("CLASSICUBE_SWAP") != NULL;
	/* CLASSICUBE_WALK=every: the chunks' commands are made anew every frame, as they were */
	segs_off   = getenv("CLASSICUBE_WALK") != NULL;
	hold_late  = getenv("CLASSICUBE_HOLD_LATE") != NULL;
	check_kept = getenv("CLASSICUBE_CHECK_KEPT") != NULL;

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

/* An epoch names what the matrices and the viewport are. The camera's (Gfx_LoadMVP) keeps its
   number while they stay what they were, over frames too: a block written for it in a set is
   then still right. Any other gets a number never used before. */
static struct Matrix main_view, main_proj;
static uint32_t main_epoch, epoch_counter = 1;
static cc_bool main_valid;

void Gfx_SetViewport(int x, int y, int w, int h) {
	Gfx_SE_Own();
	if (x == view_x && y == view_y && w == view_w && h == view_h) return;
	view_x = x; view_y = y; view_w = w; view_h = h;
	main_valid   = false;
	matrix_epoch = ++epoch_counter;
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
	const int width = tex->width, count = part->width, rows = part->height, fours = count & ~3;
	uint32_t* restrict dst = (uint32_t*)(gpu + tex->span->at) + y * width + x;
	const BitmapCol* restrict src = part->scan0;
	int xx, yy;

	/* (four at a time: the loop's own instructions were a third of a texel's) */
	for (yy = 0; yy < rows; yy++, src += rowWidth, dst += width) {
		for (xx = 0; xx < fours; xx += 4) {
			BitmapCol p = src[xx], q = src[xx + 1], r = src[xx + 2], s = src[xx + 3];
			dst[xx] = Texel(p); dst[xx + 1] = Texel(q); dst[xx + 2] = Texel(r); dst[xx + 3] = Texel(s);
		}
		for (; xx < count; xx++) dst[xx] = Texel(src[xx]);
	}
	stat_texels += count * rows;

	if (tex->wide) {
		/* the same texels again, four times across */
		const uint32_t* restrict from = (uint32_t*)(gpu + tex->span->at) + y * width + x;
		uint32_t* restrict a = (uint32_t*)(gpu + tex->wide->at) + y * width * WIDE + x;
		for (yy = 0; yy < rows; yy++, from += width, a += width * WIDE) {
			uint32_t *restrict b = a + width, *restrict c = b + width, *restrict d = c + width;
			for (xx = 0; xx < fours; xx += 4) {
				uint32_t p = from[xx], q = from[xx + 1], r = from[xx + 2], s = from[xx + 3];
				a[xx] = p; a[xx + 1] = q; a[xx + 2] = r; a[xx + 3] = s;
				b[xx] = p; b[xx + 1] = q; b[xx + 2] = r; b[xx + 3] = s;
				c[xx] = p; c[xx + 1] = q; c[xx + 2] = r; c[xx + 3] = s;
				d[xx] = p; d[xx + 1] = q; d[xx + 2] = r; d[xx + 3] = s;
			}
			for (; xx < count; xx++) {
				uint32_t t = from[xx];
				a[xx] = t; b[xx] = t; c[xx] = t; d[xx] = t;
			}
		}
	}
	stat_texel_cycles += cycles() - began;
}

static void MakeWide(struct SETexture* tex) {
	uint32_t *src, *dst;
	int x, y;
	tex->wide = Heap_Alloc(tex->width * tex->height * 4 * WIDE);
	if (!tex->wide) return;

	src = (uint32_t*)(gpu + tex->span->at);
	dst = (uint32_t*)(gpu + tex->wide->at);
	for (y = 0; y < tex->height; y++, src += tex->width, dst += tex->width * WIDE) {
		uint32_t *b = dst + tex->width, *c = b + tex->width, *d = c + tex->width;
		for (x = 0; x < tex->width; x++) {
			uint32_t t = src[x];
			dst[x] = t; b[x] = t; c[x] = t; d[x] = t;
		}
	}
}

/* CLASSICUBE_TEXTURE_SUM: a line for every texture made, to compare two builds' pictures by. */
static void TextureSum(const struct SETexture* tex) {
	const uint32_t* at = (const uint32_t*)(gpu + tex->span->at);
	uint32_t sum = 0;
	int i;
	for (i = 0; i < tex->width * tex->height; i++) sum = sum * 31 + at[i];
	printf("cctex: %d x %d, sum %08x\n", tex->width, tex->height, sum);
	fflush(stdout);
}

GfxResourceID Gfx_AllocTexture(struct Bitmap* bmp, int rowWidth, cc_uint8 flags, cc_bool mipmaps) {
	struct SETexture* tex = (struct SETexture*)Mem_TryAllocCleared(1, sizeof(struct SETexture));
	if (!tex) return NULL;

	tex->span = Heap_Alloc(bmp->width * bmp->height * 4);
	if (!tex->span) { Mem_Free(tex); return NULL; }
	tex->width  = bmp->width;
	tex->height = bmp->height;
	CopyTexels(tex, 0, 0, bmp, rowWidth);
	if (texture_sums) TextureSum(tex);
	return tex;
}

/* Texels that change while the list being drawn may read them are kept until the frame ends:
   its own list is the first that is to show them, and by then the one before is drawn. */
#define LATER_MOST  32
#define LATER_WORDS 8192
static struct { struct SETexture* tex; int x, y; struct Bitmap part; } later[LATER_MOST];
static BitmapCol later_words[LATER_WORDS];
static int later_count, later_used;

static void Later_Flush(void) {
	int i;
	for (i = 0; i < later_count; i++) {
		if (later[i].tex) CopyTexels(later[i].tex, later[i].x, later[i].y, &later[i].part, later[i].part.width);
	}
	later_count = later_used = 0;
}

void Gfx_UpdateTexture(GfxResourceID texId, int x, int y, struct Bitmap* part, int rowWidth, cc_bool mipmaps) {
	const int count = part->width, rows = part->height;
	Gfx_SE_Own();
	int xx, yy;

	if (Drawing() && later_count < LATER_MOST && later_used + count * rows <= LATER_WORDS) {
		BitmapCol* to = later_words + later_used;
		const BitmapCol* from = part->scan0;
		later[later_count].tex = (struct SETexture*)texId;
		later[later_count].x = x; later[later_count].y = y;
		later[later_count].part.scan0 = to;
		later[later_count].part.width = count; later[later_count].part.height = rows;
		for (yy = 0; yy < rows; yy++, from += rowWidth, to += count) {
			for (xx = 0; xx < count; xx++) to[xx] = from[xx];
		}
		later_used += count * rows;
		later_count++;
		return;
	}
	Settle();
	Later_Flush();
	CopyTexels((struct SETexture*)texId, x, y, part, rowWidth);
}

void Gfx_BindTexture(GfxResourceID texId) {
	if (se_texture == (struct SETexture*)texId) return;
	se_texture = (struct SETexture*)texId;
	state_serial++;
}

void Gfx_DeleteTexture(GfxResourceID* texId) {
	struct SETexture* tex = (struct SETexture*)(*texId);
	Gfx_SE_Own();
	if (!tex) return;
	if (se_texture == tex) se_texture = NULL;
	state_serial++;
	kept_epoch++;
	{ int i; for (i = 0; i < later_count; i++) if (later[i].tex == tex) later[i].tex = NULL; }

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

static void SetAlphaBlend(cc_bool enabled) { se_alphaBlend = enabled; state_serial++; }
static void SetAlphaTest(cc_bool enabled)  { se_alphaTest  = enabled; state_serial++; }
static void SetDepthTest(cc_bool enabled)  { se_depthTest  = enabled; state_serial++; }
static void SetDepthWrite(cc_bool enabled) { }
static void SetColorWrite(cc_bool r, cc_bool g, cc_bool b, cc_bool a) { }

/* (water's depth is laid down first elsewhere: here a blended pass tests depth and writes none) */
void Gfx_DepthOnlyRendering(cc_bool depthOnly) { se_depthOnly = depthOnly; state_serial++; }

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

static CC_INLINE cc_bool SameMatrix(const struct Matrix* a, const struct Matrix* b) {
	const uint32_t *p = (const uint32_t*)a, *q = (const uint32_t*)b;
	int i;
	for (i = 0; i < 16; i++) if (p[i] != q[i]) return false;
	return true;
}

static void MatricesLoaded(cc_bool camera) {
	static int width, height;
	if (Window_Main.Width != width || Window_Main.Height != height) {
		width = Window_Main.Width; height = Window_Main.Height; main_valid = false;
	}
	if (main_valid && SameMatrix(&se_view, &main_view) && SameMatrix(&se_proj, &main_proj)) { matrix_epoch = main_epoch; return; }
	matrix_epoch = ++epoch_counter;
	if (!camera) return;
	main_view  = se_view; main_proj = se_proj;
	main_epoch = matrix_epoch; main_valid = true;
}

/* From a holodeck's seat the 2D screen is a sheet sheet_at ahead, 22 degrees up and down at the
   game's aspect (a desktop's view is 60 high), drawn in the camera's space: the room shows no flat draws. */
static struct Matrix sheet_ortho;
static void SheetMatrix(MatrixType type, const struct Matrix* matrix) {
	const struct Matrix* o = &sheet_ortho;
	float d = sheet_at, sy = 0.4f * d, sx = sy * main_proj.row2.y / main_proj.row1.x;
	struct Matrix to = Matrix_Identity;
	if (type == MATRIX_PROJ) { sheet_ortho = *matrix; se_proj = main_proj; return; }
	to.row1.x = o->row1.x * sx; to.row2.y = o->row2.y * sy; to.row3.z = 0.0f;
	to.row4.x = o->row4.x * sx; to.row4.y = o->row4.y * sy; to.row4.z = -d;
	if (!sheet_fixed) { Matrix_Mul(&se_view, matrix, &to); return; }
	{	/* a menu's sheet: before the camera it opened under, into the world, then this camera's */
		struct Matrix a, b;
		Matrix_Mul(&a, matrix, &to);
		Matrix_Mul(&b, &a, &sheet_back);
		Matrix_Mul(&se_view, &b, &main_view);
	}
}

void Gfx_LoadMatrix(MatrixType type, const struct Matrix* matrix) {
	if (gfx_rendering2D && sheet_at > 0.0f && main_valid) SheetMatrix(type, matrix);
	else if (type == MATRIX_VIEW) se_view = *matrix;
	else if (type == MATRIX_PROJ) se_proj = *matrix;
	MatricesLoaded(false);
}

void Gfx_LoadMVP(const struct Matrix* view, const struct Matrix* proj, struct Matrix* mvp) {
	se_view = *view;
	se_proj = *proj;
	MatricesLoaded(true);
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

/* A block of vectors for a draw: the projection, then the modelview's rows. */
static void PlainRows(uint32_t* to) {
	const struct Matrix* m = &se_view;
	float* u = (float*)to;
	int k;
	if (proj_epoch != matrix_epoch) ProjectionRows();

	/* (word by word here: Mem_Copy is a call, and this is every draw's) */
	for (k = 0; k < 16; k++) to[k] = ((const uint32_t*)proj_rows)[k];
	u[16] = m->row1.x; u[17] = m->row2.x; u[18] = m->row3.x; u[19] = m->row4.x;
	u[20] = m->row1.y; u[21] = m->row2.y; u[22] = m->row3.y; u[23] = m->row4.y;
	u[24] = m->row1.z; u[25] = m->row2.z; u[26] = m->row3.z; u[27] = m->row4.z;
}

/* The block of a buffer with no origin: the camera's is a slot of the set, any other the frame's. */
static uint32_t PlainBlock(void) {
	static uint32_t plain, plain_epoch, kept[2];
	if (matrix_epoch == main_epoch) {
		if (kept[draw.si] != matrix_epoch) {
			PlainRows((uint32_t*)(gpu + set_at + BLOCKS_IN) + 32 * PLAIN_SLOT);
			kept[draw.si] = matrix_epoch;
		}
		return GPU_PHYS + set_at + BLOCKS_IN + 128 * PLAIN_SLOT;
	}
	if (plain_epoch != matrix_epoch) {
		if (blocks == MAX_BLOCKS) return 0;
		PlainRows((uint32_t*)(gpu + set_at + BLOCKS_IN) + 32 * blocks);
		plain = GPU_PHYS + set_at + BLOCKS_IN + 128 * blocks++;
		plain_epoch = matrix_epoch;
	}
	return plain;
}

/* The same for a chunk (vertices in eighths from `origin`): all but three of its words are
   every chunk's, and are made once for the matrices. */
static uint32_t chunk_rows[28], chunk_rows_epoch;
static cc_bool block_of_frame;   /* a chunk had no slot and got a block of the frame's */

static CC_NOINLINE void ChunkRows(void) {
	const struct Matrix* m = &se_view;
	float* u = (float*)chunk_rows;
	int k;
	if (proj_epoch != matrix_epoch) ProjectionRows();
	for (k = 0; k < 16; k++) chunk_rows[k] = ((const uint32_t*)proj_rows)[k];
	u[16] = m->row1.x * 0.125f; u[17] = m->row2.x * 0.125f; u[18] = m->row3.x * 0.125f;
	u[20] = m->row1.y * 0.125f; u[21] = m->row2.y * 0.125f; u[22] = m->row3.y * 0.125f;
	u[24] = m->row1.z * 0.125f; u[25] = m->row2.z * 0.125f; u[26] = m->row3.z * 0.125f;
	chunk_rows_epoch = matrix_epoch;
}

/* `slot`: the mesh's own block of the set, from 1; 0 when it has none. */
static uint32_t ChunkBlock(const float* origin, uint32_t slot) {
	const struct Matrix* m = &se_view;
	const uint32_t* from   = chunk_rows;
	const float x = origin[0], y = origin[1], z = origin[2];
	uint32_t* to;
	uint32_t index = slot - 1;
	if (!slot) {
		if (blocks == MAX_BLOCKS) return 0;
		index = blocks++;
		block_of_frame = true;
	}
	if (chunk_rows_epoch != matrix_epoch) ChunkRows();

	to = (uint32_t*)(gpu + set_at + BLOCKS_IN) + 32 * index;
	to[0]  = from[0];  to[1]  = from[1];  to[2]  = from[2];  to[3]  = from[3];
	to[4]  = from[4];  to[5]  = from[5];  to[6]  = from[6];  to[7]  = from[7];
	to[8]  = from[8];  to[9]  = from[9];  to[10] = from[10]; to[11] = from[11];
	to[12] = from[12]; to[13] = from[13]; to[14] = from[14]; to[15] = from[15];
	to[16] = from[16]; to[17] = from[17]; to[18] = from[18];
	to[20] = from[20]; to[21] = from[21]; to[22] = from[22];
	to[24] = from[24]; to[25] = from[25]; to[26] = from[26];
	((float*)to)[19] = m->row4.x + x * m->row1.x + y * m->row2.x + z * m->row3.x;
	((float*)to)[23] = m->row4.y + x * m->row1.y + y * m->row2.y + z * m->row3.y;
	((float*)to)[27] = m->row4.z + x * m->row1.z + y * m->row2.z + z * m->row3.z;
	return GPU_PHYS + set_at + BLOCKS_IN + 128 * index;
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
	uint32_t packed, word;       /* 1 for a chunk's vertices; the command's vertex word */
	float origin[3];
	uint32_t slot;               /* a packed mesh's own block in both sets, from 1 (0: none) */
	uint32_t block[2], block_epoch[2]; /* its matrix block in each set, and the matrix_epoch that is of */
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
	uint32_t span_frame;          /* the frame its span was last drawn from */
	uint32_t* shadow;             /* a dynamic buffer's vertices as its mesh was last made of them */
	int shadow_count, shadow_fmt;
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

/* CLASSICUBE_MESH_SUM: a line for every chunk's mesh, to compare two builds by. The second sum
   has each tag's colour for the tag, which is the same whatever order the chunks were made in. */
static void MeshSum(const struct SEMesh* mesh, const uint32_t* at, const uint32_t* coords, int count) {
	uint32_t raw = 0, sum = 0;
	int i;
	for (i = 0; i < count; i++) {
		raw = (raw * 31 + at[i]) * 31 + coords[i];
		sum = ((sum * 31 + (at[i] & 0xFFFFFF)) * 31 + chunk_table[at[i] >> 24]) * 31 + coords[i];
	}
	printf("ccmesh: at %d %d %d, %d vertices, sums %08x %08x\n", (int)mesh->origin[0], (int)mesh->origin[1],
		(int)mesh->origin[2], count, raw, sum);
	fflush(stdout);
}

/* Slots of the sets' kept blocks that no mesh has. */
static uint16_t slot_free[KEPT_BLOCKS];
static uint32_t slot_free_count, slots_used;

static uint32_t Slot_Take(void) {
	if (slot_free_count) return slot_free[--slot_free_count];
	return slots_used < PLAIN_SLOT ? ++slots_used : 0;
}

/* A mesh made: what its commands have of it, and no draw goes on from the one before it. */
static CC_INLINE void MeshMade(struct SEVb* vb, uint32_t flags) {
	vb->mesh.flags  = flags;
	vb->mesh.packed = (flags & VERTEX_PACKED) != 0;
	vb->mesh.word   = BASE_VERTEX | flags;
	if (vb->mesh.packed && !vb->mesh.slot) vb->mesh.slot = Slot_Take();
	if (!vb->dynamic) kept_epoch++;
	if (last_vb == vb) last_draw = NULL;
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

	mesh->block_epoch[0] = mesh->block_epoch[1] = 0;
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
		MeshMade(vb, VERTEX_TAGGED | VERTEX_TABLE | VERTEX_PACKED);
		mesh->colour   = GPU_PHYS + (uint32_t)((uint8_t*)chunk_table - gpu);
		if (mesh_sums) MeshSum(mesh, to, coords, count);
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

	if (!textured && !tagged) {
		/* one colour and no texture (the block outline): the vertices as they are */
		const uint32_t* restrict p = (const uint32_t*)src;
		for (i = 0; i < count; i++, p += 4, to += 4) { to[0] = p[0]; to[1] = p[1]; to[2] = p[2]; to[3] = 0; }
	}
	for (q = textured || tagged ? 0 : count; q < count; q += 4) {
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
		MeshMade(vb, VERTEX_TAGGED | VERTEX_TABLE);
		mesh->colour = GPU_PHYS + size + mesh->vertices;
	} else {
		MeshMade(vb, 0);
		mesh->colour = DeviceColour(((const uint32_t*)src)[3]);
	}
	return true;
}

static void* SpanAlloc(struct SEVb* vb, uint32_t size) {
	if (vb->span && vb->span_size >= size && vb->drawn_frame != frame_number) {
		/* (written in place: the list being drawn may read it as it was) */
		if (vb->span_frame + 1 == frame_number) Settle();
		return gpu + vb->span->at;
	}
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
	Gfx_SE_Own();
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
	Gfx_SE_Own();
	if (!data) return;
	if (se_vb == data) se_vb = NULL;
	if (last_vb == data) last_draw = NULL;
	if (data->mesh.slot) slot_free[slot_free_count++] = (uint16_t)data->mesh.slot;
	kept_epoch++;

	Heap_FreeLater(data->span);
	if (data->dynamic) Mem_Free(data->staging);
	Mem_Free(data->shadow);
	Mem_Free(data);
	*vb = NULL;
}

void* Gfx_LockVb(GfxResourceID vb, VertexFormat fmt, int count) {
	struct SEVb* data = (struct SEVb*)vb;
	Gfx_SE_Own();
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

/* The same for a chunk's vertices packed by the builder (struct SEVertex, MapRenderer.h): their
   bytes count from `corner` and are made to count from `least`, the mesh's origin, and each
   quad's colour becomes its tag. What BuildMeshIn makes of the same quads as floats. */
cc_bool Gfx_SE_UnlockPacked(GfxResourceID vb, const struct SEVertex* v, int count, const int* corner, const int* least) {
	struct SEVb* data   = (struct SEVb*)vb;
	struct SEMesh* mesh = &data->mesh;
	const uint32_t began = cycles(), words = (count * 4 + 15) & ~15u;
	const uint32_t moved = (uint32_t)(least[0] - corner[0]) << 3 | (uint32_t)(least[1] - corner[1]) << 11 |
		(uint32_t)(least[2] - corner[2]) << 19;
	uint32_t *restrict to, *restrict coords, prev, tag = 0;
	uint8_t* mem;
	int i;

	data->fmt = VERTEX_FORMAT_TEXTURED; data->count = count;
	data->staging = NULL; data->where = 0;
	mesh->block_epoch[0] = mesh->block_epoch[1] = 0;
	if (count < 4 || !(mem = (uint8_t*)SpanAlloc(data, 2 * words))) return false;
	to = (uint32_t*)mem; coords = (uint32_t*)(mem + words);
	prev = ~v->colour;

	for (i = 0; i < count; i += 4, v += 4) {
		if (v->colour != prev) { prev = v->colour; tag = (uint32_t)ChunkTagOf(DeviceColour(prev)) << 24; }
		to[i]     = (v[0].at - moved) | tag; coords[i]     = v[0].coords;
		to[i + 1] = (v[1].at - moved) | tag; coords[i + 1] = v[1].coords;
		to[i + 2] = (v[2].at - moved) | tag; coords[i + 2] = v[2].coords;
		to[i + 3] = (v[3].at - moved) | tag; coords[i + 3] = v[3].coords;
	}
	mesh->origin[0] = (float)least[0]; mesh->origin[1] = (float)least[1]; mesh->origin[2] = (float)least[2];
	mesh->vertices = (uint32_t)(mem - gpu);
	mesh->coords   = mesh->vertices + words;
	MeshMade(data, VERTEX_TAGGED | VERTEX_TABLE | VERTEX_PACKED);
	mesh->colour   = GPU_PHYS + (uint32_t)((uint8_t*)chunk_table - gpu);
	data->where    = 1;
	if (mesh_sums) MeshSum(mesh, to, coords, count);
	stat_vertices += count;
	stat_vertex_cycles += cycles() - began;
	return true;
}

void Gfx_BindDynamicVb(GfxResourceID vb) { se_vb = (struct SEVb*)vb; }

void* Gfx_LockDynamicVb(GfxResourceID vb, VertexFormat fmt, int count) {
	struct SEVb* data = (struct SEVb*)vb;
	Gfx_SE_Own();
	data->fmt = fmt; data->count = count;
	return data->staging;
}

/* Whether a buffer of 16 vertices or more was filled as it was when its mesh was made; a copy
   is kept for the next time when it was not. */
static cc_bool SameFill(struct SEVb* vb) {
	const uint32_t* now = (const uint32_t*)vb->staging;
	int words = vb->count * strideSizes[vb->fmt] / 4, i;

	if (vb->count < 16) return false;
	if (vb->where && vb->shadow && vb->shadow_count == vb->count && vb->shadow_fmt == vb->fmt) {
		for (i = 0; i < words && vb->shadow[i] == now[i]; i++) { }
		if (i == words) return true;
	}
	if (!vb->shadow) vb->shadow = (uint32_t*)Mem_TryAlloc(vb->max, SIZEOF_VERTEX_TEXTURED);
	if (!vb->shadow || vb->count > vb->max) { vb->shadow_count = 0; return false; }
	for (i = 0; i < words; i++) vb->shadow[i] = now[i];
	vb->shadow_count = vb->count; vb->shadow_fmt = vb->fmt;
	return false;
}

/* A buffer filled again before it was drawn a second time lives in the frame; one that is
   drawn frame after frame as it is gets memory of its own. */
void Gfx_UnlockDynamicVb(GfxResourceID vb) {
	struct SEVb* data = (struct SEVb*)vb;
	se_vb = data;
	if (SameFill(data)) return;
	/* (not into its span while the list being drawn reads that: the frame has room, and no wait) */
	if (data->where == 1 && data->drawn_frame != frame_number && !(data->span_frame + 1 == frame_number && Drawing()) &&
		BuildMesh(data, SpanAlloc)) return;
	data->where       = BuildMesh(data, ArenaAlloc) ? 2 : 0;
	data->where_frame = frame_number;
}

void Gfx_DeleteDynamicVb(GfxResourceID* vb) { Gfx_DeleteVb(vb); }


/*########################################################################################################################*
*--------------------------------------------------------Rendering--------------------------------------------------------*
*#########################################################################################################################*/

void Gfx_SetVertexFormat(VertexFormat fmt) {
	if (fmt == gfx_format) return;
	gfx_format = fmt;
	gfx_stride = strideSizes[fmt];
	state_serial++;
}

void Gfx_DrawVb_Lines(int verticesCount) { }

static CC_NOINLINE void DeriveState(int packed) {
	struct SEState* s = &draw.state[packed];
	struct SETexture* tex = gfx_format == VERTEX_FORMAT_TEXTURED ? se_texture : NULL;
	uint32_t pass = se_depthTest ? (se_alphaBlend ? 1 : 0) : 5, address;

	s->serial   = state_serial;
	s->skip     = se_depthOnly;
	s->fragment = pass << 16;
	s->pass_bit = (1u << pass) & 0xfe;
	s->address  = GPU_PHYS;
	s->width    = s->height = 0;
	if (!tex) return;

	s->fragment |= FRAGMENT_TEXTURE;
	if (pass == 0 && se_alphaTest) s->fragment |= FRAGMENT_KEYED;
	if (packed && !tex->wide) MakeWide(tex);
	address = packed ? (tex->wide ? tex->wide->at : 0) : tex->span->at;
	if (!address) s->skip = true;
	s->address = GPU_PHYS + address;
	s->width   = (uint32_t)tex->width * (packed ? WIDE : 1);
	s->height  = (uint32_t)tex->height;
}

/* A walk of the map's chunks is noted as it is made: the buffers it drew, where its commands
   are in the set and what they come to. While the map's renderer says the walk would be the
   same (`serial`) and nothing it drew was made again, the commands stay as they are in the
   set and only the blocks of a camera that moved are written (docs/games/classicube.md). */
#define SEG_BUFFERS 512
struct SESeg { uint32_t serial, kept, frame, first, first_mesh, count, mesh, passes, buffers, dropped; cc_bool valid, bad; };
static struct SESeg segs[2][2];
static struct SEVb* seg_buffers[2][2][SEG_BUFFERS];
static uint32_t stat_kept, stat_walked;

static CC_NOINLINE void Seg_Note(struct SEVb* vb) {
	struct SESeg* g = draw.rec;
	struct SEVb** list = seg_buffers[draw.si][g - segs[draw.si]];
	if (g->buffers && list[g->buffers - 1] == vb) return;
	if (g->buffers == SEG_BUFFERS || vb->dynamic || vb->where != 1 || (vb->mesh.packed && !vb->mesh.slot)) { g->bad = true; return; }
	list[g->buffers++] = vb;
}

cc_bool Gfx_SE_SegmentBegin(int id, uint32_t serial) {
	struct SESeg* g = &segs[draw.si][id];
	const uint32_t si = draw.si, frame = frame_number, epoch = matrix_epoch;
	struct SEVb** list = seg_buffers[si][id];
	uint32_t i;

	last_draw = NULL;
	if (segs_off || se_depthOnly || epoch != main_epoch) return false;
	if (g->valid && g->serial == serial && g->kept == kept_epoch && g->first == commands && g->first_mesh == mesh_vertices) {
		for (i = 0; i < g->buffers; i++) {
			struct SEVb* vb = list[i];
			struct SEMesh* mesh = &vb->mesh;
			vb->drawn_frame = frame; vb->span_frame = frame;
			if (mesh->block_epoch[si] == epoch) continue;
			mesh->block[si]       = mesh->packed ? ChunkBlock(mesh->origin, mesh->slot) : PlainBlock();
			mesh->block_epoch[si] = epoch;
		}
		commands      += g->count;
		mesh_vertices += g->mesh;
		passes_used   |= g->passes;
		g->frame = frame;
		stat_kept++;
		return true;
	}
	g->valid   = false; g->bad = false;
	g->serial  = serial; g->kept = kept_epoch;
	g->first   = commands; g->first_mesh = mesh_vertices;
	g->passes  = passes_used; g->buffers = 0; g->dropped = stat_dropped;
	passes_used = 0;
	draw.rec    = g;
	stat_walked++;
	return false;
}

void Gfx_SE_SegmentEnd(int id) {
	struct SESeg* g = draw.rec;
	uint32_t before;
	if (!g) return;
	draw.rec  = NULL;
	last_draw = NULL;
	before    = g->passes;
	g->passes = passes_used;
	passes_used |= before;
	g->count  = commands - g->first;
	g->mesh   = mesh_vertices - g->first_mesh;
	g->frame  = frame_number;
	g->valid  = !g->bad && g->kept == kept_epoch && g->dropped == stat_dropped;
}

/* The draw's command, when nothing is left to make for it (Ready). It calls nothing. */
static inline __attribute__((always_inline)) void Emit(struct SEVb* vb, int verticesCount, int startVertex) {
	const struct SEMesh* mesh = &vb->mesh;
	const uint32_t packed     = mesh->packed, block = mesh->block[draw.si];
	const struct SEState* s   = &draw.state[packed];
	const uint32_t mesh_count = (uint32_t)(verticesCount >> 2) * 6, at = mesh_vertices, n = commands;
	uint32_t* c = last_draw;

	if (s->skip) return;
	vb->drawn_frame = frame_number;
	if (vb->where == 1) vb->span_frame = frame_number;
	if (at + mesh_count > MAX_MESH || !block) { stat_dropped++; return; }
	mesh_vertices = at + mesh_count;

	if (c && last_vb == vb && draw.last_end == (uint32_t)startVertex && draw.last_serial == state_serial && draw.last_block == block) {
		/* the vertices after the last draw's, in the same state: more of that command */
		c[2] += mesh_count;
		draw.last_end = startVertex + verticesCount;
		return;
	}
	if (n == MAX_COMMANDS) { mesh_vertices = at; stat_dropped++; return; }

	if (draw.rec) Seg_Note(vb);
	c = draw.set + 16 * n;
	commands = n + 1;
	c[0]  = CMD_DRAW;
	c[1]  = GPU_PHYS + mesh->vertices + ((uint32_t)startVertex << (packed ? 2 : 4));
	c[2]  = mesh_count;
	c[3]  = at;
	c[4]  = mesh->word;
	c[5]  = s->fragment;
	c[6]  = block;
	c[7]  = s->address;
	c[8]  = s->width;
	c[9]  = s->height;
	c[10] = KEY_TEXEL;
	c[11] = mesh->colour;
	c[12] = packed ? GPU_PHYS + mesh->coords + (uint32_t)startVertex * 4 : 0;
	passes_used |= s->pass_bit;

	last_draw = c; last_vb = vb; draw.last_end = startVertex + verticesCount;
	draw.last_serial = state_serial; draw.last_block = block;
}

/* What a draw needs made first: the buffer's mesh when it is a frame old, the state's words,
   the mesh's matrix block. False when there is nothing to draw. */
static cc_bool Ready(struct SEVb* vb) {
	struct SEMesh* mesh = &vb->mesh;
	const uint32_t si   = draw.si;

	if (se_depthOnly) return false;
	if (vb->where == 2 && vb->where_frame != frame_number) {
		/* drawn again as it was a frame ago */
		vb->where = vb->staging && BuildMesh(vb, SpanAlloc) ? 1 : 0;
	}
	if (!vb->where) return false;
	if (draw.state[mesh->packed].serial != state_serial) DeriveState(mesh->packed);

	if (mesh->block_epoch[si] != matrix_epoch) {
		mesh->block[si]       = mesh->packed ? ChunkBlock(mesh->origin, mesh->slot) : PlainBlock();
		mesh->block_epoch[si] = matrix_epoch;
	}
	return true;
}

static CC_NOINLINE void DrawUnready(int verticesCount, int startVertex) {
	struct SEVb* vb = se_vb;
	if (Ready(vb)) Emit(vb, verticesCount, startVertex);
}

/* (the common draw, a chunk's with its block made, saves no register and calls nothing) */
static void DrawQuads(int verticesCount, int startVertex) {
	struct SEVb* vb = se_vb;
	if (!vb || verticesCount < 4) return;
	if (vb->where != 1 || vb->mesh.block_epoch[draw.si] != matrix_epoch || draw.state[vb->mesh.packed].serial != state_serial) {
		DrawUnready(verticesCount, startVertex);
		return;
	}
	Emit(vb, verticesCount, startVertex);
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
	c = draw.set + 16 * commands++;
	c[0] = CMD_CLEAR;
	c[1] = clear_colour;
	c[3] = mesh_vertices;
	mesh_vertices += 3;
	last_draw = NULL;
}

/* A seated player's menu pointer: a dot on the sheet, from where core 0 had it when the frame began */
int SE_SeatCursor(int* x, int* y);
static int cursor_on, cursor_x, cursor_y;
static void SheetCursor(float delta) {
	if (!cursor_on) return;
	Gfx_Draw2DFlat(cursor_x - 4, cursor_y - 4, 9, 9, PackedCol_Make(0, 0, 0, 255));
	Gfx_Draw2DFlat(cursor_x - 2, cursor_y - 2, 5, 5, PackedCol_Make(255, 255, 255, 255));
}

void Gfx_BeginFrame(void) {
	struct Matrix opened;
	sheet_at  = SE_SeatSheet();
	cursor_on = sheet_at > 0.0f && SE_SeatCursor(&cursor_x, &cursor_y);
	sheet_fixed = sheet_at > 0.0f && SE_SeatMenuView(&opened);
	if (sheet_fixed) SE_RigidInverse(&sheet_back, &opened);
	Game.Draw2DHooks[2] = SheetCursor;
}

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
	printf("ccstat: a frame: %u texels in %u instructions, %u vertices in %u; chunk walks %u made, %u kept\n",
		stat_texels / n, stat_texel_cycles / n, stat_vertices / n, stat_vertex_cycles / n, stat_walked, stat_kept);
	stat_walked = stat_kept = 0;
	if (stat_job) printf("ccstat: the frame's worker %u instructions a frame, %u of them waiting for the list shown; core 0 %u waiting for it\n",
		stat_job / n, stat_job_wait / n, stat_join / n);
	stat_job = stat_job_wait = stat_join = 0;
	if (check_kept) printf("cccheck: the outline kept %u times, %u of them wrong; the held block kept %u times, %u wrong\n",
		se_kept_checked[0], se_kept_checked[1], se_kept_checked[2], se_kept_checked[3]);
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
	if (!said_steady && n >= 40 && World.Loaded && !MapRenderer_SE_Busy()) { said_steady = true; printf("classicube: steady\n"); }
	fflush(stdout);
	since_ms = now; since_cycles = cycles(); since_frames = frame_number;
	stat_commands = stat_mesh = stat_dropped = 0;
	stat_texels = stat_texel_cycles = stat_vertices = stat_vertex_cycles = 0;
}

void Gfx_SE_FrameLater(void) {
	if (later_count) { Settle(); Later_Flush(); }
}

void Gfx_EndFrame(void) {
	volatile uint32_t* regs = (volatile uint32_t*)gpu;
	uint32_t address, row;
	int width, height;

	if (later_count) { Settle(); Later_Flush(); }
	Stats();
	if (hold_late && hold_frame && frame_number > hold_frame) {
		Settle();
		frame_number = hold_frame;
		hold_late    = false;
	} else if (commands && SE_Surface(&address, &width, &height, &row)) {
		for (;;) {
			while (__atomic_exchange_n(&regs[REG_LOCK / 4], 1, __ATOMIC_ACQUIRE)) sched_yield();
			if (regs[REG_SUBMIT / 4] == 0) break;
			__atomic_store_n(&regs[REG_LOCK / 4], 0, __ATOMIC_RELEASE);
			mcw_rest();
		}
		regs[REG_INTO / 4]     = address;
		regs[REG_INTO / 4 + 1] = width;
		regs[REG_INTO / 4 + 2] = height;
		regs[REG_INTO / 4 + 3] = row;
		regs[REG_SUBMIT / 4 + 1] = GPU_PHYS + set_at;
		regs[REG_SUBMIT / 4 + 2] = commands;
		regs[REG_SUBMIT / 4]     = 1 | 4 | passes_used << 8;
		__atomic_store_n(&regs[REG_LOCK / 4], 0, __ATOMIC_RELEASE);
		/* (not waited for: Settle) */
		unsettled = true;
		if (swap_waits) Settle();

		regs[REG_VOLUME / 4 + 1] = commands;
		regs[REG_VOLUME / 4 + 3] = width | height << 16 | 2u << 28 | (SE_Flat() ? 1u << 31 : 0);	/* (y is up: docs/world/holodeck.md) */
		regs[REG_VOLUME / 4]     = GPU_PHYS + set_at;
		regs[REG_VOLUME / 4 + 2]++;
		/* (a walk this frame did not make is not in this set any more) */
		segs[draw.si][0].valid &= segs[draw.si][0].frame == frame_number;
		segs[draw.si][1].valid &= segs[draw.si][1].frame == frame_number;
		set_at = set_at == SETS_AT ? SETS_AT + SET_SIZE : SETS_AT;
		draw.set = (uint32_t*)(gpu + set_at);
		draw.si ^= 1;
	} else {
		segs[draw.si][0].valid = segs[draw.si][1].valid = false;
	}
	if (hold_frame && frame_number >= hold_frame && !hold_late) {
		Settle();
		printf("classicube: holding frame %u\n", frame_number);
		fflush(stdout);
		for (;;) next_frame();
	}

	Heap_Age();
	frame_number++;
	/* (a block of the frame's under the camera's epoch would be taken for a kept one) */
	if (block_of_frame) { block_of_frame = false; main_valid = false; matrix_epoch = ++epoch_counter; }
	commands = mesh_vertices = passes_used = 0;
	blocks = KEPT_BLOCKS;
	arena_top = set_at + ARENA_IN;
	last_draw = NULL;
}
#endif
