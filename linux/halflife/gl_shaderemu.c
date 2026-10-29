// The OpenGL 1.1 that Xash3D's renderer (ref/gl) asks for, on the machine's GPU (docs/games/halflife.md):
// glBegin and client arrays become compact vertices of linux/quake3/gpu_shaderemu.c's draws.
// Textures are only in GPU memory: words, or the game's paletted ones as layers of tiles;
// a primitive whose vertices differ in colour is tagged vertices with a table of their colours.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include "gpu_shaderemu.h"
#include "mcw.h"

// gles.c, which gpu_shaderemu.c is built on: its matrices and frame
typedef int GLint_;
void glMatrixMode(unsigned mode);
void glLoadIdentity(void);
void glLoadMatrixf(const float* m);
void glMultMatrixf(const float* m);
void glPushMatrix(void);
void glPopMatrix(void);
void glFrustumf(float l, float r, float b, float t, float n, float f);
void glOrthof(float l, float r, float b, float t, float n, float f);
void glTranslatef(float x, float y, float z);
void glScalef(float x, float y, float z);
void glRotatef(float degrees, float x, float y, float z);
void glViewport(int x, int y, int width, int height);
void glClearColor(float r, float g, float b, float a);
void glClear(unsigned mask);
int seglInit(unsigned int nano_x_window);
void seglSettle(void);
void* seglMemory(unsigned int bytes);
unsigned int seglMemoryLeft(void);
void* seglMemorySpare(unsigned int* bytes);
void seglGetMatrix(unsigned mode, void* matrix);
void seglSize(int* width, int* height);
void seglWorldUp(int axis);

enum {
    GL_POINTS = 0, GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUADS,
    GL_QUAD_STRIP, GL_POLYGON,
    GL_ZERO = 0, GL_ONE = 1, GL_SRC_COLOR = 0x0300, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
    GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
    GL_CULL_FACE = 0x0B44, GL_DEPTH_TEST = 0x0B71, GL_POLYGON_OFFSET_FILL = 0x8037, GL_ALPHA_TEST = 0x0BC0, GL_BLEND = 0x0BE2, GL_TEXTURE_2D = 0x0DE1,
    GL_COLOR_BUFFER_BIT = 0x4000, GL_MODELVIEW = 0x1700, GL_PROJECTION = 0x1701,
    GL_MODELVIEW_MATRIX = 0x0BA6, GL_PROJECTION_MATRIX = 0x0BA7, GL_VIEWPORT = 0x0BA2, GL_MAX_TEXTURE_SIZE = 0x0D33,
    GL_VENDOR = 0x1F00, GL_RENDERER, GL_VERSION, GL_EXTENSIONS,
    GL_TEXTURE_ENV_MODE = 0x2200, GL_MODULATE = 0x2100, GL_REPLACE = 0x1E01, GL_DECAL = 0x2101,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_NEAREST = 0x2600, GL_LINEAR = 0x2601,
    GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803, GL_CLAMP = 0x2900, GL_CLAMP_TO_EDGE = 0x812F,
    GL_UNSIGNED_BYTE = 0x1401, GL_UNSIGNED_SHORT = 0x1403, GL_UNSIGNED_INT = 0x1405, GL_FLOAT = 0x1406,
    GL_VERTEX_ARRAY = 0x8074, GL_COLOR_ARRAY = 0x8076, GL_TEXTURE_COORD_ARRAY = 0x8078,
    GL_RGBA = 0x1908, GL_RGB = 0x1907, GL_LUMINANCE = 0x1909, GL_ALPHA = 0x1906, GL_LUMINANCE_ALPHA = 0x190A, GL_BGRA = 0x80E1,
};

#define MAX_NAMES 8192
#define MAX_PRIMITIVE 4096
#define KEY_WORD 0xff000000u   // what a texel the alpha test would drop becomes: drawn keyed on it
#define NOWHERE 0xffffffffu

static inline uint32_t bits(float f) {
    union { float f; uint32_t u; } c = {f};
    return c.u;
}
static inline uint32_t level(float f) { return f <= 0 ? 0 : f >= 1 ? 255 : (uint32_t)(f * 255.0f + 0.5f); }
static inline float floor_of(float f) {
    float whole = (float)(int)f;
    return whole > f ? whole - 1 : whole;
}

// ---- textures ----

// A texture is only in GPU memory: words, or (hlglTexIndexed) a layer of tiles, the texture's
// bytes as tiles and a palette textures with the same one share (fragment mode 5).
enum { NO_PIXELS, WORDS, TILES };
typedef struct { uint32_t at, bytes; int kind, width, height, holes, smooth, clamped, key, atlas, palette, opaque;
    int spillable, drawn; uint8_t* spilled; uint32_t spilled_from; } texture;   // atlas, palette: + 1; spilled: its bytes in RAM
typedef struct { uint32_t at, size; int owner; } block;   // of the pool, in order; owner -1: free
typedef struct { uint32_t at, sum; int refs; } palette;
#define MAX_PALETTES 2048
#define PALETTE_OWNER(p) (MAX_NAMES + (p))
static texture textures[MAX_NAMES];
static palette palettes[MAX_PALETTES];
static block blocks[2 * MAX_NAMES + MAX_PALETTES + 8];
static int block_count;
static uint8_t* pool;
static uint32_t pool_size, pool_used, frame = 1, refused, tile_bytes;
// the light maps' atlas (hlglLightAtlas): their blocks one under another, three bytes a texel
static uint32_t atlas_at = NOWHERE, atlas_block;
static int atlas_blocks;
#define KEPT_OWNER (MAX_NAMES - 2)
#define ATLAS_OWNER (MAX_NAMES - 3)
static int bound, log_uploads, log_frames, log_picsum, log_sum;
static uint32_t picsum, picsums;	// HLGL_PICSUM=1: the frame's 2D quads summed, state and vertices
static uint32_t listsum;	// HLGL_SUM=1: the frames' whole lists (gpuListSum) summed, one after another
#define GAP_OWNER (MAX_NAMES - 1)   // what lies between the pool's two parts, which is not ours
#define GPU_TILES 5

static void block_remove(int i) {
    block_count--;
    memmove(&blocks[i], &blocks[i + 1], (size_t)(block_count - i) * sizeof(block));
}
static void pool_give(int owner) {
    for (int i = 0; i < block_count; i++) {
        if (blocks[i].owner != owner) continue;
        blocks[i].owner = -1;
        pool_used -= blocks[i].size;
        if (i + 1 < block_count && blocks[i + 1].owner < 0) blocks[i].size += blocks[i + 1].size, block_remove(i + 1);
        if (i > 0 && blocks[i - 1].owner < 0) blocks[i - 1].size += blocks[i].size, block_remove(i);
        break;
    }
    if (owner < MAX_NAMES) textures[owner].at = NOWHERE;
}
static uint32_t pool_take(uint32_t size, int owner) {
    size = (size + 15) & ~15u;
    for (int i = block_count - 1; size >= 65536 && i >= 0; i--) {   // (large ones from the top: fewer holes among the small)
        if (blocks[i].owner >= 0 || blocks[i].size < size) continue;
        if (blocks[i].size > size) {
            memmove(&blocks[i + 1], &blocks[i], (size_t)(block_count - i) * sizeof(block));
            block_count++;
            blocks[i].size -= size;
            i++, blocks[i].at = blocks[i - 1].at + blocks[i - 1].size, blocks[i].size = size;
        }
        blocks[i].owner = owner;
        pool_used += size;
        return blocks[i].at;
    }
    for (int i = 0; size < 65536 && i < block_count; i++) {
        if (blocks[i].owner >= 0 || blocks[i].size < size) continue;
        if (blocks[i].size > size) {
            memmove(&blocks[i + 1], &blocks[i], (size_t)(block_count - i) * sizeof(block));
            block_count++;
            blocks[i + 1].at += size, blocks[i + 1].size -= size;
            blocks[i].size = size;
        }
        blocks[i].owner = owner;
        pool_used += size;
        return blocks[i].at;
    }
    return NOWHERE;
}
// A palette of 256 words in GPU memory, the one already there when another texture has it: its
// number + 1, or 0 when there is no room.
static int palette_top;   // no palette at or past it has refs
static int palette_take(const uint32_t* colours) {
    uint32_t sum = 2166136261u;
    for (int i = 0; i < 256; i++) sum = (sum ^ colours[i]) * 16777619u;
    int free_one = -1;
    for (int p = 0; p < palette_top; p++) {
        if (!palettes[p].refs) {
            if (free_one < 0) free_one = p;
            continue;
        }
        if (palettes[p].sum == sum && !memcmp(pool + palettes[p].at, colours, 1024)) return palettes[p].refs++, p + 1;
    }
    if (free_one < 0 && palette_top < MAX_PALETTES) free_one = palette_top;
    if (free_one < 0 || (palettes[free_one].at = pool_take(1024, PALETTE_OWNER(free_one))) == NOWHERE) return 0;
    if (free_one == palette_top) palette_top++;
    seglSettle();
    memcpy(pool + palettes[free_one].at, colours, 1024);
    palettes[free_one].sum = sum, palettes[free_one].refs = 1;
    return free_one + 1;
}
static void palette_give(int p) {
    if (p > 0 && --palettes[p - 1].refs == 0) seglSettle(), pool_give(PALETTE_OWNER(p - 1));
}
static void texture_free(int name) {
    texture* t = &textures[name];
    if (t->at != NOWHERE) pool_give(name);
    free(t->spilled);
    palette_give(t->palette);
    if (t->kind == TILES) tile_bytes -= t->bytes;
    memset(t, 0, sizeof *t);
    t->at = NOWHERE;
}
// ---- the menu's pictures leave GPU memory for RAM when there is no room and they were not drawn lately ----
static int swaps;
void hlglSpillable(unsigned name) {
    if (name && name < GAP_OWNER) textures[name].spillable = 1;
}
static uint32_t pool_take_spilling(uint32_t size, int owner) {
    uint32_t at = pool_take(size, owner);
    for (int i = 1; at == NOWHERE && i < GAP_OWNER; i++) {
        texture* t = &textures[i];
        if (!t->spillable || t->at == NOWHERE || i == owner || swaps - t->drawn < 2) continue;
        uint8_t* copy = malloc(t->bytes);
        if (!copy) break;
        seglSettle();
        memcpy(copy, pool + t->at, t->bytes);
        t->spilled = copy, t->spilled_from = gpuAddress(pool + t->at);
        pool_give(i);
        if (log_uploads) fprintf(stderr, "hlgl: %d %dx%d to RAM\n", i, t->width, t->height);
        at = pool_take(size, owner);
    }
    return at;
}
static void unspill(int name) {
    texture* t = &textures[name];
    uint32_t at = pool_take_spilling(t->bytes, name);
    if (at == NOWHERE) return;
    seglSettle();
    t->at = at;
    memcpy(pool + at, t->spilled, t->bytes);
    if (t->kind == TILES) {   // (a layer's head points at its own cells and tiles)
        uint32_t* head = (uint32_t*)(pool + at), moved = gpuAddress(pool + at) - t->spilled_from;
        head[0] += moved, head[1] += moved;
    }
    free(t->spilled), t->spilled = 0;
}

// The bound texture's room in GPU memory, as `kind` of `bytes`: NULL when there is none.
static uint8_t* texture_room(int kind, int width, int height, uint32_t bytes) {
    texture* t = &textures[bound];
    if (t->kind != kind || t->width != width || t->height != height || t->bytes != bytes || t->at == NOWHERE) {
        texture_free(bound);
        if ((t->at = pool_take_spilling(bytes, bound)) == NOWHERE) {
            if (!refused++) fprintf(stderr, "hlgl: GPU memory full (%u of %u bytes): textures are left out\n", pool_used, pool_size);
            return 0;
        }
        t->kind = kind, t->width = width, t->height = height, t->bytes = bytes;
        if (kind == TILES) tile_bytes += bytes;
    }
    seglSettle();   // (the room may be what the list being drawn still reads)
    return pool + t->at;
}

// A texel as the GPU reads it: 0xTTRRGGBB, T transparency; one the alpha test drops is the key.
static inline uint32_t word_of(uint32_t r, uint32_t g, uint32_t b, uint32_t a, int* holes) {
    if (a < 128 && holes) *holes = 1;
    return a == 0 ? KEY_WORD : (255 - a) << 24 | r << 16 | g << 8 | b;
}
#define OPAQUE_RGBA 0x10000   // (a format of ours: RGBA bytes of a texture made without alpha, the fourth not read)
static void words_from(uint32_t* to, const uint8_t* p, int n, unsigned format, int* holes) {
    for (int i = 0; i < n; i++) {
        switch (format) {
            case OPAQUE_RGBA: to[i] = word_of(p[0], p[1], p[2], 255, holes), p += 4; break;
            case GL_RGB: to[i] = word_of(p[0], p[1], p[2], 255, holes), p += 3; break;
            case GL_BGRA: to[i] = word_of(p[2], p[1], p[0], p[3], holes), p += 4; break;
            case GL_LUMINANCE: to[i] = word_of(p[0], p[0], p[0], 255, holes), p += 1; break;
            case GL_ALPHA: to[i] = word_of(255, 255, 255, p[0], holes), p += 1; break;
            case GL_LUMINANCE_ALPHA: to[i] = word_of(p[0], p[0], p[0], p[1], holes), p += 2; break;
            default: to[i] = word_of(p[0], p[1], p[2], p[3], holes), p += 4; break;
        }
    }
}

// words_from in shares of 4-word multiples, on workers and this core, for a large picture.
#define WORD_JOBS 8
static struct { uint32_t* to; const uint8_t* from; unsigned format; int step, per; int holes[WORD_JOBS][4] __attribute__((aligned(16))); } words_job;
unsigned hlglWait(int k);
static uint32_t words_share(uint32_t j, uint32_t n) {
    uint32_t first = j * words_job.per, end = first + words_job.per < n ? first + words_job.per : n;
    int holes = 0;
    if (first < end) words_from(words_job.to + first, words_job.from + first * words_job.step, (int)(end - first), words_job.format, &holes);
    words_job.holes[j][0] = holes;
    return 0;
}
int hlglWorker(void);
void hlglPost(int k, uint32_t (*fn)(uint32_t, uint32_t), uint32_t a0, uint32_t a1);
static int studio_workers(void);
static void words_from_shared(uint32_t* to, const uint8_t* p, int n, unsigned format, int* holes) {
    int jobs = studio_workers() < WORD_JOBS - 1 ? studio_workers() + 1 : WORD_JOBS, k[WORD_JOBS];
    if (n < 16384 || jobs < 2) {
        words_from(to, p, n, format, holes);
        return;
    }
    mcw_touch(to, (unsigned)n * 4);
    words_job.to = to, words_job.from = p, words_job.format = format;
    words_job.step = format == GL_RGB ? 3 : format == GL_LUMINANCE || format == GL_ALPHA ? 1 : format == GL_LUMINANCE_ALPHA ? 2 : 4;
    words_job.per = ((n + jobs - 1) / jobs + 3) & ~3;
    for (int j = 0; j < jobs; j++) k[j] = j < jobs - 1 ? hlglWorker() : 0, hlglPost(k[j], words_share, j, n);
    for (int j = 0; j < jobs; j++) {
        hlglWait(k[j]);
        if (words_job.holes[j][0] && holes) *holes = 1;
    }
}

static void s_glGenTextures(int n, unsigned* out) {
    static unsigned next = 1;
    while (n-- > 0) *out++ = next < MAX_NAMES - 2 ? next++ : 0;
}
static void s_glDeleteTextures(int n, const unsigned* which) {
    for (int i = 0; i < n; i++)
        if (which[i] && which[i] < GAP_OWNER) texture_free((int)which[i]);
}
static void s_glBindTexture(unsigned target, unsigned name) {
    (void)target;
    bound = name < GAP_OWNER ? (int)name : 0;
}
static void s_glTexImage2D(unsigned target, int lvl, int internal, int width, int height, int border, unsigned format,
                           unsigned type, const void* pixels) {
    (void)target, (void)border;
    if (lvl != 0 || !bound || type != GL_UNSIGNED_BYTE || width <= 0 || height <= 0) return;
    texture* t = &textures[bound];
    if (t->kind == TILES) return;   // (ref/gl's copy in words of what hlglTexIndexed took)
    uint32_t* to = (uint32_t*)texture_room(WORDS, width, height, (uint32_t)width * height * 4);
    if (!to) return;
    t->holes = 0;
    t->opaque = internal == 1 || internal == 3 || internal == GL_RGB || internal == GL_LUMINANCE ||
        (internal >= 0x803F && internal <= 0x8042) || (internal >= 0x804F && internal <= 0x8054);   // GL_LUMINANCE4..16, GL_RGB4..16
    if (t->opaque && format == GL_RGBA) format = OPAQUE_RGBA;
    if (pixels) words_from_shared(to, pixels, width * height, format, &t->holes);    else memset(to, 0, t->bytes);
    if (log_uploads) fprintf(stderr, "hlgl: words %d %dx%d holes %d internal %x format %x\n", bound, width, height, t->holes, internal, format);
}
// The worker that writes the light maps' atlas now (hlglLightBlock), waited for before this core does.
static int atlas_writer;
unsigned hlglWait(int k);
void hlglAtlasWriter(int k) { atlas_writer = k; }
// A light map's block of RGBA bytes written into the atlas, on any one core: whether it was.
int hlglLightBlock(unsigned name, int x, int y, int width, int height, const uint8_t* rgba) {
    const texture* t = name < GAP_OWNER ? &textures[name] : 0;
    if (!t || !t->atlas || atlas_at == NOWHERE || t->at != NOWHERE || x < 0 || y < 0 || x + width > t->width || y + height > t->height) return 0;
    for (int row = 0; row < height; row++, rgba += width * 4) {
        uint8_t* rgb = pool + atlas_at + (((uint32_t)(t->atlas - 1) * atlas_block + (uint32_t)(y + row)) * atlas_block + (uint32_t)x) * 3;
        for (int i = 0; i < width; i++, rgb += 3) rgb[0] = rgba[i * 4], rgb[1] = rgba[i * 4 + 1], rgb[2] = rgba[i * 4 + 2];
    }
    return 1;
}

static void s_glTexSubImage2D(unsigned target, int lvl, int x, int y, int width, int height, unsigned format, unsigned type,
                              const void* pixels) {
    (void)target;
    texture* t = &textures[bound];
    if (t->atlas && atlas_writer) hlglWait(atlas_writer), atlas_writer = 0;
    if (lvl != 0 || t->kind != WORDS || type != GL_UNSIGNED_BYTE || x < 0 || y < 0 || x + width > t->width || y + height > t->height) return;
    int step = format == GL_RGB ? 3 : format == GL_LUMINANCE || format == GL_ALPHA ? 1 : format == GL_LUMINANCE_ALPHA ? 2 : 4;
    if (t->opaque && format == GL_RGBA) format = OPAQUE_RGBA;
    if (t->at == NOWHERE && (!t->atlas || atlas_at == NOWHERE || width > 1024)) return;
    seglSettle();
    for (int row = 0; row < height; row++) {
        uint32_t row_words[1024];   // (a light map in the atlas only)
        uint32_t* words = t->at == NOWHERE ? row_words : (uint32_t*)(pool + t->at) + (y + row) * t->width + x;
        words_from(words, (const uint8_t*)pixels + (size_t)row * width * step, width, format, &t->holes);
        if (!t->atlas || atlas_at == NOWHERE) continue;
        uint8_t* rgb = pool + atlas_at + (((uint32_t)(t->atlas - 1) * atlas_block + (uint32_t)(y + row)) * atlas_block + (uint32_t)x) * 3;
        for (int i = 0; i < width; i++, rgb += 3) rgb[0] = (uint8_t)(words[i] >> 16), rgb[1] = (uint8_t)(words[i] >> 8), rgb[2] = (uint8_t)words[i];
    }
}
static void s_glTexParameteri(unsigned target, unsigned name, int value) {
    (void)target;
    if (name == GL_TEXTURE_MAG_FILTER) textures[bound].smooth = value != GL_NEAREST;
    else if (name == GL_TEXTURE_WRAP_S || name == GL_TEXTURE_WRAP_T) textures[bound].clamped = value == GL_CLAMP || value == GL_CLAMP_TO_EDGE;
}
static void s_glTexParameterf(unsigned target, unsigned name, float value) { s_glTexParameteri(target, name, (int)value); }
static void s_glTexParameterfv(unsigned target, unsigned name, const float* v) { s_glTexParameteri(target, name, (int)v[0]); }
static unsigned char s_glIsTexture(unsigned name) { return name && name < GAP_OWNER && textures[name].kind; }

static int tile_side(int n) {   // the largest side of at most 255 that n is whole tiles of
    for (int d = n < 255 ? n : 255; d > 1; d--)
        if (n % d == 0) return d;
    return 1;
}
// The tiles of an indexed texture copied from its rows: on a worker when hlglTexLater (a film's frame,
// whose bytes stay until the frame is shown), done before the frame is shown
typedef struct { uint8_t* tile; const uint8_t* bytes; int width, tw, th, across, down; } tiles_t;
int hlglTexLater;
static void tiles_copy(const tiles_t* j) {
    uint8_t* tile = j->tile;
    for (int cy = 0; cy < j->down; cy++)
        for (int cx = 0; cx < j->across; cx++)
            for (int y = 0; y < j->th; y++, tile += j->tw) memcpy(tile, j->bytes + (size_t)(cy * j->th + y) * j->width + cx * j->tw, (size_t)j->tw);
}
static uint32_t tiles_job(uint32_t a0, uint32_t a1) {
    (void)a1;
    tiles_copy((const tiles_t*)a0);
    return 0;
}
int hlglWorker(void);
void hlglPost(int k, uint32_t (*fn)(uint32_t, uint32_t), uint32_t a0, uint32_t a1);
void* hlglJobMemory(unsigned bytes);
unsigned hlglWait(int k);

// Texture `name` as its bytes and palette (`entry` bytes an entry, red first; a fourth is alpha when `alpha`):
// whether it was taken. A palette with translucent entries, or more than one clear one, is not.
int hlglTexIndexed(unsigned name, int width, int height, const uint8_t* bytes, const uint8_t* palette, int entry, int alpha) {
    int key = -1, tw = tile_side(width), th = tile_side(height), across = width / tw, down = height / th;
    if (!name || name >= GAP_OWNER || width <= 0 || height <= 0 || across * down > 1024) return 0;
    for (int i = 0; entry == 4 && alpha && i < 256; i++) {
        if (palette[i * 4 + 3] == 255) continue;
        if (palette[i * 4 + 3] != 0 || key >= 0) return 0;
        key = i;
    }
    uint32_t colours[256];
    for (int i = 0; i < 256; i++) colours[i] = (uint32_t)palette[i * entry] << 16 | palette[i * entry + 1] << 8 | palette[i * entry + 2];
    uint32_t cells_at = 16, tiles_at = (cells_at + (uint32_t)across * down * 2 + 15) & ~15u;
    int was = bound;
    bound = (int)name;
    uint8_t* at = texture_room(TILES, width, height, tiles_at + (uint32_t)width * height);
    texture* t = &textures[name];
    bound = was;
    if (!at) return 0;
    palette_give(t->palette);
    if (!(t->palette = palette_take(colours))) {
        texture_free((int)name);
        return 0;
    }
    uint32_t* head = (uint32_t*)at;
    head[0] = gpuAddress(at + cells_at), head[1] = gpuAddress(at + tiles_at), head[2] = gpuAddress(pool + palettes[t->palette - 1].at);
    head[3] = (uint32_t)tw | (uint32_t)th << 8 | (uint32_t)across << 16;
    uint16_t* cells = (uint16_t*)(at + cells_at);
    for (int i = 0; i < across * down; i++) cells[i] = (uint16_t)i;
    static tiles_t* later;
    static int later_k;
    tiles_t now = {at + tiles_at, bytes, width, tw, th, across, down};
    int k = hlglTexLater && (later || (later = hlglJobMemory(sizeof *later))) ? hlglWorker() : 0;
    if (later_k > 0) hlglWait(later_k), later_k = 0;	// (its tiles_t is the one about to be written)
    if (k > 0) *later = now, hlglPost(later_k = k, tiles_job, (uint32_t)later, 0);
    else tiles_copy(&now);
    t->key = key, t->holes = key >= 0;
    if (log_uploads) fprintf(stderr, "hlgl: tiles %u %dx%d key %d\n", name, width, height, key);
    return 1;
}

// ---- state ----

static int texturing, blending, alpha_test, depth_test = 0, replace, offsetting;
static float offset_pull;	// glPolygonOffset as a share of the eye's distance its vertices are moved towards it
static unsigned blend_src = GL_ONE, blend_dst = GL_ZERO;
static float colour[4] = {1, 1, 1, 1};
static uint32_t colour_now;
static int colour_stale = 1;	// colour_now is colour's word (colour_of)

static int* flag(unsigned what) {
    switch (what) {
        case GL_TEXTURE_2D: return &texturing;
        case GL_BLEND: return &blending;
        case GL_ALPHA_TEST: return &alpha_test;
        case GL_DEPTH_TEST: return &depth_test;
        case GL_POLYGON_OFFSET_FILL: return &offsetting;
    }
    return 0;
}
// The GPU's depth has no bias: a surface offset towards the eye (a decal, -1 and -2) is drawn nearer
// along the eye's rays, which leaves its pixels where they were.
static void s_glPolygonOffset(float factor, float units) { offset_pull = -(factor + units) * 4e-4f; }
static void s_glEnable(unsigned what) {
    int* f = flag(what);
    if (f) *f = 1;
}
static void s_glDisable(unsigned what) {
    int* f = flag(what);
    if (f) *f = 0;
}
static unsigned char s_glIsEnabled(unsigned what) {
    int* f = flag(what);
    return f ? (unsigned char)*f : 0;
}
static void s_glBlendFunc(unsigned s, unsigned d) { blend_src = s, blend_dst = d; }
static void s_glTexEnvi(unsigned target, unsigned name, int value) {
    (void)target;
    if (name == GL_TEXTURE_ENV_MODE) replace = value == GL_REPLACE;
}
static void s_glTexEnvf(unsigned target, unsigned name, float value) { s_glTexEnvi(target, name, (int)value); }
static void s_glTexEnvfv(unsigned target, unsigned name, const float* v) { s_glTexEnvi(target, name, (int)v[0]); }

static void s_glColor4f(float r, float g, float b, float a) { colour[0] = r, colour[1] = g, colour[2] = b, colour[3] = a, colour_stale = 1; }
static void s_glColor3f(float r, float g, float b) { s_glColor4f(r, g, b, 1); }
static void s_glColor4fv(const float* v) { s_glColor4f(v[0], v[1], v[2], v[3]); }
static void s_glColor3fv(const float* v) { s_glColor4f(v[0], v[1], v[2], 1); }
static void s_glColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    s_glColor4f(r * (1.0f / 255), g * (1.0f / 255), b * (1.0f / 255), a * (1.0f / 255));
}
static void s_glColor3ub(unsigned char r, unsigned char g, unsigned char b) { s_glColor4ub(r, g, b, 255); }
static void s_glColor4ubv(const unsigned char* v) { s_glColor4ub(v[0], v[1], v[2], v[3]); }

// Which of the GPU's passes the state is: blending by alpha, added, or multiplying, with the
// depth test or without; what has no depth test goes into the blended pass to keep its order.
static unsigned pass_of(void) {
    unsigned kind = 0;
    if (blending) {
        if (blend_dst == GL_ONE) kind = 2;
        else if ((blend_src == GL_ZERO && blend_dst == GL_SRC_COLOR) || (blend_src == GL_DST_COLOR && blend_dst == GL_ZERO) ||
                 (blend_src == GL_DST_COLOR && blend_dst == GL_SRC_COLOR))
            kind = 3;
        else if (blend_src == GL_ONE && blend_dst == GL_ZERO) kind = 0;
        else kind = 1;
    }
    if (!depth_test) kind = kind ? kind + 4 : 5;
    return kind;
}

// The draw's state: texture, how it is coloured and in which pass. 0 when it is not to be drawn.
static void texture_state(gpu_state* s, const texture* t, unsigned pass, int alpha) {
    s->texture = gpuAddress(pool + t->at), s->width = (unsigned)t->width, s->height = (unsigned)t->height;
    s->fragment = (t->kind == TILES ? GPU_TILES : GPU_WORDS | (t->smooth ? GPU_SMOOTH : 0)) | GPU_PASS(pass) | (t->clamped ? GPU_CLAMPED : 0);
    // texels the alpha test drops are the key; blended ones are drawn by their own alpha
    if (t->holes && (alpha || pass == 0 || t->kind == TILES)) s->fragment |= GPU_KEYED, s->key = t->kind == TILES ? (unsigned)t->key : KEY_WORD;
}
static int draw_state(gpu_state* s) {
    memset(s, 0, sizeof *s);
    unsigned pass = pass_of();
    s->fragment = GPU_COLOUR | GPU_PASS(pass);
    if (texturing && bound && textures[bound].spilled) unspill(bound);
    if (texturing && bound && textures[bound].at != NOWHERE) {
        textures[bound].drawn = swaps;
        texture_state(s, &textures[bound], pass, alpha_test);
    } else if (texturing && bound) {
        return 0;   // (no pixels yet, or no room in GPU memory)
    }
    return 1;
}
static inline uint32_t colour_word(const float* c) {
    return (255 - level(c[3])) << 24 | level(c[0]) << 16 | level(c[1]) << 8 | level(c[2]);
}
static inline uint32_t colour_of(void) {
    if (colour_stale) colour_now = colour_word(colour), colour_stale = 0;
    return colour_now;
}

// ---- drawing ----

typedef struct { float x, y, z, s, t; uint32_t colour; } vertex;
static vertex prim[MAX_PRIMITIVE] __attribute__((aligned(16)));
static int prim_mode = -1, prim_count;
static float coord_s, coord_t;
static uint32_t drawn_vertices;

// Triangles of the primitive, as indices of its vertices, three each: how many.
static int triangles_of(int mode, int n, uint16_t* out, int room) {
    int t = 0;
    switch (mode) {
        case GL_TRIANGLES:
            for (int i = 0; i + 2 < n && t + 3 <= room; i += 3) out[t++] = i, out[t++] = i + 1, out[t++] = i + 2;
            break;
        case GL_TRIANGLE_STRIP:
        case GL_QUAD_STRIP:
            for (int i = 0; i + 2 < n && t + 3 <= room; i++) {
                if (i & 1) out[t++] = i + 1, out[t++] = i, out[t++] = i + 2;
                else out[t++] = i, out[t++] = i + 1, out[t++] = i + 2;
            }
            break;
        case GL_QUADS:
            for (int i = 0; i + 3 < n && t + 6 <= room; i += 4)
                out[t++] = i, out[t++] = i + 1, out[t++] = i + 2, out[t++] = i, out[t++] = i + 2, out[t++] = i + 3;
            break;
        case GL_TRIANGLE_FAN:
        case GL_POLYGON:
            for (int i = 1; i + 1 < n && t + 3 <= room; i++) out[t++] = 0, out[t++] = i, out[t++] = i + 1;
            break;
    }
    return t;
}

static uint16_t order[MAX_PRIMITIVE * 3] __attribute__((aligned(16)));   // the drawing's (texels of its own)

// The vertices moved towards the eye by offset_pull of their distance (the modelview's inverse).
static const vertex* pulled(const vertex* v, int n) {
    static vertex out[MAX_PRIMITIVE] __attribute__((aligned(16)));   // the drawing's
    float m[16], e[3];
    seglGetMatrix(GL_MODELVIEW, m);	// (columns)
    float a = m[0], b = m[4], c = m[8], d = m[1], f = m[5], g = m[9], h = m[2], i = m[6], j = m[10];
    float c0 = f * j - g * i, c1 = g * h - d * j, c2 = d * i - f * h, det = a * c0 + b * c1 + c * c2;
    if (det == 0 || n > MAX_PRIMITIVE) return v;
    float tx = -m[12] / det, ty = -m[13] / det, tz = -m[14] / det;	// the eye: the inverse times -translation
    e[0] = c0 * tx + (c * i - b * j) * ty + (b * g - c * f) * tz;
    e[1] = c1 * tx + (a * j - c * h) * ty + (c * d - a * g) * tz;
    e[2] = c2 * tx + (b * h - a * i) * ty + (a * f - b * d) * tz;
    float keep = 1 - offset_pull;
    for (int k = 0; k < n; k++) {
        out[k] = v[k];
        out[k].x = e[0] + (v[k].x - e[0]) * keep, out[k].y = e[1] + (v[k].y - e[1]) * keep, out[k].z = e[2] + (v[k].z - e[2]) * keep;
    }
    return out;
}

// The primitive drawn: one colour for all of it as compact vertices, or each vertex its own
// as tagged ones with a table of 256 colours a draw (texture coordinates then within 4 repeats).
static void draw_vertices(int mode, const vertex* v, int n) {
    if (n < 3 || mode < GL_TRIANGLES) return;
    gpu_state s;
    if (!draw_state(&s)) return;
    if (offsetting && offset_pull != 0) v = pulled(v, n);
    int textured = (s.fragment & 0xff) != GPU_COLOUR;
    int count = triangles_of(mode, n, order, MAX_PRIMITIVE * 3);
    // whole repeats off all coordinates, which changes nothing a repeating texture shows
    float low_s = 0, low_t = 0, high_s = 0, high_t = 0;
    if (textured) {
        low_s = high_s = v[0].s, low_t = high_t = v[0].t;
        for (int i = 1; i < n; i++) {
            if (v[i].s < low_s) low_s = v[i].s;
            if (v[i].t < low_t) low_t = v[i].t;
            if (v[i].s > high_s) high_s = v[i].s;
            if (v[i].t > high_t) high_t = v[i].t;
        }
        if (!(s.fragment & GPU_CLAMPED)) low_s = floor_of(low_s), low_t = floor_of(low_t);
        else low_s = low_t = 0;
    }
    uint32_t first = v[0].colour;
    int one = 1;
    for (int i = 1; i < n && one; i++) one = v[i].colour == first;
    if (replace && textured) one = 1, first = 0x00ffffff;
    if (one || high_s - low_s >= 4 || high_t - low_t >= 4) {
        s.colour = first;
        uint32_t* to = gpuDrawRoom(&s, count, count);
        if (!to) return;
        for (int i = 0; i < count; i++, to += 4) {
            const vertex* p = &v[order[i]];
            to[0] = bits(p->x), to[1] = bits(p->y), to[2] = bits(p->z);
            to[3] = ((uint32_t)(int32_t)((p->s - low_s) * 1024.0f) & 0xffff) | (uint32_t)(int32_t)((p->t - low_t) * 1024.0f) << 16;
        }
    } else {
        s.vertex = GPU_TAGGED | GPU_TABLE;
        for (int done = 0; done < count;) {
            int k = count - done > 255 ? 255 : count - done;
            uint32_t* table = gpuFrameMemory(256 * 4);
            if (!table) return;
            s.colour = gpuAddress(table);
            uint32_t* to = gpuDrawRoom(&s, k, k);
            if (!to) return;
            for (int i = 0; i < k; i++, to += 4) {
                const vertex* p = &v[order[done + i]];
                table[i] = p->colour;
                to[0] = bits(p->x), to[1] = bits(p->y), to[2] = bits(p->z);
                to[3] = ((uint32_t)((p->s - low_s) * 1024.0f) & 0xfff) | ((uint32_t)((p->t - low_t) * 1024.0f) & 0xfff) << 12 | (uint32_t)i << 24;
            }
            done += k;
        }
    }
    drawn_vertices += (uint32_t)count;
}

// ---- the level kept in GPU memory (gl_kept.c) ----

// GPU memory that stays until asked for again: the level's vertices. NULL when there is no room.
void* hlglKeptMemory(unsigned bytes) {
    seglSettle();
    if (textures[KEPT_OWNER].kind) pool_give(KEPT_OWNER), textures[KEPT_OWNER].kind = NO_PIXELS;
    if (!bytes || pool_take_spilling(bytes, KEPT_OWNER) == NOWHERE) return 0;
    textures[KEPT_OWNER].kind = WORDS;
    for (int i = 0; i < block_count; i++)
        if (blocks[i].owner == KEPT_OWNER) return pool + blocks[i].at;
    return 0;
}

// The light maps (textures of words, `block` a side) as one atlas the lit draws read, which their
// later changes are written to as well: whether it was made.
int hlglLightAtlas(int count, int block, const int* names) {
    seglSettle();
    if (atlas_at != NOWHERE) pool_give(ATLAS_OWNER), atlas_at = NOWHERE;
    for (int i = 1; i < GAP_OWNER; i++) textures[i].atlas = 0;
    size_t bytes = (size_t)count * block * block * 3;
    uint8_t* rgb = count > 0 && block > 0 ? malloc(bytes) : 0;
    if (!rgb) return 0;
    // the blocks out of their words first, which leave GPU memory: the atlas is the light maps now
    memset(rgb, 255, bytes);
    for (int b = 0; b < count; b++) {
        texture* t = names[b] > 0 && names[b] < GAP_OWNER ? &textures[names[b]] : 0;
        if (!t || t->kind != WORDS || (t->at == NOWHERE && !t->spilled) || t->width != block || t->height != block) continue;
        t->atlas = b + 1;
        const uint32_t* words = (const uint32_t*)(t->spilled ? t->spilled : pool + t->at);
        uint8_t* to = rgb + (size_t)b * block * block * 3;
        for (int i = 0; i < block * block; i++) to[i * 3] = (uint8_t)(words[i] >> 16), to[i * 3 + 1] = (uint8_t)(words[i] >> 8), to[i * 3 + 2] = (uint8_t)words[i];
        if (t->spilled) free(t->spilled), t->spilled = 0;
        else pool_give(names[b]);
    }
    if ((atlas_at = pool_take_spilling((uint32_t)bytes, ATLAS_OWNER)) == NOWHERE) {
        uint32_t largest = 0;
        for (int i = 0; i < block_count; i++)
            if (blocks[i].owner < 0 && blocks[i].size > largest) largest = blocks[i].size;
        fprintf(stderr, "hlgl: no room for the light maps' atlas: %u of %u bytes used, the largest room %u\n", pool_used, pool_size, largest);
        free(rgb);
        return 0;
    }
    atlas_block = (uint32_t)block, atlas_blocks = count;
    memcpy(pool + atlas_at, rgb, bytes);
    free(rgb);
    return 1;
}

// Runs of kept compact vertices drawn with the state as it is, lit by the atlas when `light` (a
// word a vertex: where in the atlas, 16 bits each way) is given.
void hlglDrawKept(const void* vertices, const uint32_t* light, const uint32_t* firsts, const uint32_t* counts, int runs) {
    gpu_state s;
    if (!draw_state(&s)) return;
    s.colour = replace ? 0x00ffffff : colour_of();
    int lit = light && atlas_at != NOWHERE;
    if (lit) s.fragment |= GPU_LIT, s.more[1] = gpuAddress(pool + atlas_at), s.more[2] = atlas_block | (atlas_block * atlas_blocks) << 16;
    for (int r = 0; r < runs; r++) {
        if (lit) s.more[0] = gpuAddress(light + firsts[r]);
        if (!gpuDrawAt(&s, gpuAddress((const uint8_t*)vertices + firsts[r] * 16), (int)counts[r])) return;
        drawn_vertices += counts[r];
    }
}

// Room for `draws` draws of `vertices` kept vertices in all, with the state as it is but for the
// texture, for hlglKeptPut to write on any one core before the frame is shown: 0 when there is none.
typedef struct __attribute__((aligned(16))) { gpu_state s; uint32_t *c, block, mesh_at, pass; int alpha, lit, untextured; } kept_room_t;   // (texels of its own: a worker writes it)
_Static_assert(sizeof(kept_room_t) <= 96, "a kept room is 24 words");
int hlglKeptRoom(void* room, int draws, unsigned vertices, int lit) {
    kept_room_t* r = room;
#define kept_room ( *r )
    gpu_state* s = &kept_room.s;
    memset(s, 0, sizeof *s);
    kept_room.pass = pass_of(), kept_room.alpha = alpha_test, kept_room.untextured = !texturing;
    s->fragment = GPU_COLOUR | GPU_PASS(kept_room.pass);
    s->colour = replace ? 0x00ffffff : colour_of();
    kept_room.lit = lit && atlas_at != NOWHERE;
    if (kept_room.lit) s->more[1] = gpuAddress(pool + atlas_at), s->more[2] = atlas_block | (atlas_block * atlas_blocks) << 16;
    kept_room.c = gpuDrawsRoom(draws, vertices, 1u << kept_room.pass, &kept_room.block, &kept_room.mesh_at);
    if (kept_room.c) drawn_vertices += vertices;
    return kept_room.c != 0;
}
#undef kept_room
// hlglDrawKept's runs, with texture `name`, into the room's next draws, read with matrices `block`
// (0: the room's).
void hlglKeptPut(void* room, uint32_t block, int name, const void* vertices, const uint32_t* light, const uint32_t* firsts, const uint32_t* counts, int runs) {
    kept_room_t* r = room;
#define kept_room ( *r )
    gpu_state s = kept_room.s;
    if (!block) block = kept_room.block;
    if (!kept_room.untextured && name > 0 && name < GAP_OWNER && textures[name].at != NOWHERE) texture_state(&s, &textures[name], kept_room.pass, kept_room.alpha);
    int lit = light && kept_room.lit;
    if (lit) s.fragment |= GPU_LIT;
    for (int i = 0; i < runs; i++, kept_room.c += 16) {
        if (lit) s.more[0] = gpuAddress(light + firsts[i]);
        gpuDrawPut(kept_room.c, &s, block, gpuAddress((const uint8_t*)vertices + firsts[i] * 16), kept_room.mesh_at, (int)counts[i]);
        kept_room.mesh_at += counts[i];
    }
}
#undef kept_room

// ---- worker cores (gl_studio_job.c) ----

static int workers, next_worker, posted[65], swaps;
// Core 0's time waiting, by where, in the clock's units (HL_RENDER_CALLS=1 prints it with the render timeline). DEBUG
unsigned se_wait_sum[8], se_ring_wait[66], se_ring_fn[66];
// The rings core 0 waited for at the swap most, since asked last: "k:time:fn" for the five longest. DEBUG
void hlglRingWaitsSay(char* out, int size) {
    int n = 0;
    out[0] = 0;
    for (int top = 0; top < 5; top++) {
        int best = 0;
        for (int k = 1; k < 66; k++) if (se_ring_wait[k] > se_ring_wait[best]) best = k;
        if (!best || !se_ring_wait[best]) break;
        n += snprintf(out + n, size - n, " %d:%u:%x", best, se_ring_wait[best], se_ring_fn[best]);
        se_ring_wait[best] = 0;
    }
    memset(se_ring_wait, 0, sizeof se_ring_wait);
}
static inline uint32_t se_wait_now(void) { uint32_t v; __asm__ volatile("rdtime %0" : "=r"(v)); return v; }
#define SE_WAITING(i, stmt) do { uint32_t t0_ = se_wait_now(); stmt; se_wait_sum[i] += se_wait_now() - t0_; } while (0)
static unsigned dbg_waited[65], dbg_swapwait[65], dbg_fn[65];	// DEBUG
unsigned se_settle_passes;
static uint32_t shade_table, shade_key, shade_swap;

// The workers from the last: the server's (sv_shaderemu.c), the world's, the things' light's, the
// brush entities', the glows', the world's light maps' and four for the things' bones (gl_kept.c); the studio
// models' points take the rest.
#define ROLES 16
static int studio_workers(void) { return workers > ROLES ? workers - ROLES : workers > 3 ? workers - 3 : workers > 1 ? workers - 1 : workers; }
int hlglServerWorker(void) { return workers > 1 ? workers : 0; }
int hlglWorldWorker(void) { return workers > 2 ? workers - 1 : 0; }
int hlglLightWorker(void) { return workers > 3 ? workers - 2 : 0; }
int hlglEntsWorker(void) { return workers > ROLES ? workers - 3 : 0; }
int hlglGlowWorker(void) { return workers > ROLES ? workers - 4 : 0; }
int hlglRelightWorker(void) { return workers > ROLES ? workers - 5 : 0; }
int hlglEntsHelper(void) { return workers > ROLES ? workers - 6 : 0; }
int hlglServerHelper(void) { return workers > ROLES ? workers - 7 : 0; }
int hlglBonesWorker(int i) { return workers > ROLES ? workers - ROLES + 1 + i : 0; }
int hlglRenderWorker(void) { return workers > ROLES ? workers - 8 : 0; }
int hlglServerPacker(int i) { return workers > ROLES ? workers - 11 + i : 0; }

// ---- rings: while the frame's drawing is on its worker (rnd_shaderemu.c), every other worker runs
// a loop that takes jobs from its ring, posted by that worker or by this core while it is not out ----
int SE_RenderWho(void), SE_RenderOut(void);
uint32_t SE_RenderOnCore0(uint32_t (*fn)(void*), void* args);
typedef uint32_t (*ring_fn)(uint32_t, uint32_t);
static struct __attribute__((aligned(16))) { volatile uint32_t seq, fn, a0, a1; } ring_job[65];   // the poster's
static struct __attribute__((aligned(16))) { volatile uint32_t done, result, unused[2]; } ring_done[65];   // its worker's
static struct __attribute__((aligned(16))) { volatile uint32_t stop, unused[3]; } ring_stop;
static struct __attribute__((aligned(16))) { int on, unused[3]; } rings;   // this core's
static struct __attribute__((aligned(16))) { uint8_t posted[68]; int next, inline_posts; } rp;   // the poster's
static unsigned ring_loop(unsigned k, unsigned unused) {
    for (;;) {
        uint32_t seq = ring_job[k].seq;
        if (seq != ring_done[k].done) {
            ring_done[k].result = ((ring_fn)ring_job[k].fn)(ring_job[k].a0, ring_job[k].a1);
            ring_done[k].done = seq;
        } else if (ring_stop.stop) return 0;
        else mc_end_pass();
    }
}
#define CORE0_WORKERS 2	// the last studio workers: this core's own while the drawing is out
static int core0_worker(int k) { return k > studio_workers() - CORE0_WORKERS && k <= studio_workers(); }
static int ring_worker(int k) { return k != hlglServerWorker() && k != hlglServerHelper() && k != hlglRenderWorker() && !core0_worker(k) && (k < hlglServerPacker(0) || k > hlglServerPacker(2)); }
static struct __attribute__((aligned(16))) { int next, unused[3]; } core0_turn;   // this core's, apart from the drawing's statics
#define core0_next core0_turn.next
// Whether this core may not post now: the drawing's worker is out and posts.
static int ring_closed(void) { return !SE_RenderWho() && SE_RenderOut(); }
static struct __attribute__((aligned(16))) { unsigned waits[66]; } rw;   // the drawing's worker's: DEBUG
static int ring_poll(int k) {
    if (k <= 0 || !rp.posted[k]) return 1;
    if (ring_done[k].done == ring_job[k].seq) { rp.posted[k] = 0; return 1; }
    if (SE_RenderWho()) { rw.waits[k]++; mc_end_pass(); }
    else { if (posted[k] == 3) mcw_done(k); SE_WAITING(0, mcw_rest_for(&k, 1)); }
    return 0;
}
// The passes the drawing's worker waited for ring jobs since asked last, and for whose most. DEBUG
unsigned hlglRingWaits(int* most) {
    unsigned all = 0, top = 0;
    for (int k = 1; k < 66; k++) {
        all += rw.waits[k];
        if (rw.waits[k] > top) top = rw.waits[k], *most = k;
        rw.waits[k] = 0;
    }
    return all;
}
static void ring_post(int k, ring_fn fn, uint32_t a0, uint32_t a1, int late) {
    while (!ring_poll(k)) ;
    ring_job[k].fn = (uint32_t)fn, ring_job[k].a0 = a0, ring_job[k].a1 = a1;
    ring_job[k].seq = ring_job[k].seq + 1;
    rp.posted[k] = late ? 2 : 1;
}
void hlglRingsStart(void) {
    if (rings.on || !hlglRenderWorker()) return;
    ring_stop.stop = 0;
    for (int k = 1; k <= workers; k++) {
        if (!ring_worker(k)) continue;
        if (posted[k]) mcw_wait(k);
        ring_done[k].done = ring_job[k].seq;
        rp.posted[k] = 0;
        mcw_post(k, ring_loop, (uint32_t)k, 0), posted[k] = 3;
    }
    rings.on = 1;
}
// Every job posted through the rings done: the drawing's worker's at its frame's end.
void hlglRenderJobsWait(void) {
    for (int k = 1; rings.on && k <= workers; k++) while (!ring_poll(k)) ;
}
void hlglRingsStop(void) {
    if (!rings.on) return;
    hlglRenderJobsWait();
    ring_stop.stop = 1;
    for (int k = 1; k <= workers; k++)
        if (posted[k] == 3) mcw_wait(k), posted[k] = 0;
    rings.on = 0;
}
// The rings' workers served by this core, should one have stopped at a page or a call.
void hlglRingsServe(void) {
    for (int k = 1; rings.on && k <= workers; k++) if (posted[k] == 3) mcw_done(k);
}
int hlglRingsOn(void) { return rings.on; }
// A line to stderr by a write of its own: the drawing's worker prints too, and stdio's state is core 0's.
void hlglSay(const char* fmt, ...) {
    char line[512];
    va_list v;
    va_start(v, fmt);
    int n = vsnprintf(line, sizeof line, fmt, v);
    va_end(v);
    if (n > 0) write(2, line, n < (int)sizeof line ? n : (int)sizeof line - 1);
}

// A worker that has nothing to do now, waited for if need be; 0 when there are none.
int hlglWorker(void) {
    if (!workers) return 0;
    if (rings.on) {
        if (ring_closed()) {
            int k = studio_workers() - core0_next++ % CORE0_WORKERS;
            if (posted[k]) { SE_WAITING(1, dbg_waited[k] += mcw_wait(k)); posted[k] = 0; }
            return k;
        }
        int n = studio_workers() - CORE0_WORKERS, k = rp.next % n + 1;
        for (int i = 0; i < n; i++) {   // the next ring that is free, if any is
            int f = (rp.next + i) % n + 1;
            if (!rp.posted[f] || ring_done[f].done == ring_job[f].seq) { k = f, rp.next += i; break; }
        }
        rp.next++;
        while (!ring_poll(k)) ;
        return k;
    }
    int k = next_worker % studio_workers() + 1;
    next_worker++;
    if (posted[k]) { SE_WAITING(1, dbg_waited[k] += mcw_wait(k)); posted[k] = 0; }
    return k;
}
// fn(a0, a1) on worker k, done before the frame is shown; here and now for k 0.
void hlglPost(int k, uint32_t (*fn)(uint32_t, uint32_t), uint32_t a0, uint32_t a1) {
    if (k <= 0) fn(a0, a1);
    else if (rings.on && ring_worker(k)) {
        if (ring_closed()) rp.inline_posts++, fn(a0, a1);
        else ring_post(k, fn, a0, a1, 0);
    } else mcw_post(k, fn, a0, a1), posted[k] = 1, dbg_fn[k] = (unsigned)fn;
}
// The same, but not waited for when the frame is shown: only by hlglWait.
void hlglPostLate(int k, uint32_t (*fn)(uint32_t, uint32_t), uint32_t a0, uint32_t a1) {
    if (rings.on && (k <= 0 || ring_worker(k))) {
        if (k <= 0 || ring_closed()) fn(a0, a1);
        else ring_post(k, fn, a0, a1, 1);
        return;
    }
    hlglPost(k, fn, a0, a1);
    if (k > 0) posted[k] = 2;
}
// Worker k's job done (1), or a pass ended waiting for it (0).
int hlglPoll(int k) {
    if (rings.on && ring_worker(k)) return ring_poll(k);
    if (k <= 0 || !posted[k]) return 1;
    if (mcw_done(k)) { posted[k] = 0; return 1; }
    SE_WAITING(2, mcw_rest_for(&k, 1));
    return 0;
}
unsigned hlglWait(int k) {
    unsigned w = 0;
    if (rings.on && k > 0 && ring_worker(k)) {
        while (!ring_poll(k)) w++;
        return w;
    }
    if (k > 0 && posted[k]) { SE_WAITING(2, w = mcw_wait(k)); dbg_waited[k] += w, posted[k] = 0; }
    return w;
}
static void jobs_wait(void) {
    uint32_t t0 = se_wait_now();
    for (int k = 1; rings.on && !ring_closed() && k <= workers; k++) if (rp.posted[k] == 1) { uint32_t t1 = se_wait_now(); while (!ring_poll(k)) ; se_ring_wait[k] += se_wait_now() - t1, se_ring_fn[k] = ring_job[k].fn; }
    for (int k = 1; k <= workers; k++)
        if (posted[k] == 1) { unsigned w = mcw_wait(k); dbg_swapwait[k] += w, posted[k] = 0; if (w > 1 && (swaps & 255) == 7) fprintf(stderr, "swapwait %d %u fn %x\n", k, w, dbg_fn[k]); }
    se_wait_sum[3] += se_wait_now() - t0;
}
void hlglJobsWait(void) { jobs_wait(); }
// Every job out done, the late ones too.
void hlglJobsWaitAll(void) {
    jobs_wait();
    for (int k = 1; k <= workers; k++)
        if (posted[k] == 1 || posted[k] == 2) mcw_wait(k), posted[k] = 0;
}
static uint32_t job_memory_0(void* bytes) { return (uint32_t)hlglJobMemory(*(unsigned*)bytes); }
void* hlglJobMemory(unsigned bytes) {
    if (SE_RenderWho()) return (void*)SE_RenderOnCore0(job_memory_0, &bytes);
    void* p = workers ? mcw_alloc(bytes) : malloc(bytes);
    if (p && workers) mcw_touch(p, bytes);
    return p;
}

// Room for `count` tagged vertices drawn with the state as it is, their tags 256 shades of rgb
// (0 to 1) at alpha: 0 when they are not drawn. A table not yet made is left for the caller
// to fill with hlglShades (*fill, its key), on any core before the frame is shown.
uint32_t* hlglShadedRoom(int count, const float* rgb, float alpha, uint32_t** fill, uint32_t* key_out) {
    gpu_state s;
    *fill = 0;
    if (count <= 0 || !draw_state(&s)) return 0;
    uint32_t key = (255 - level(alpha)) << 24 | level(rgb[0]) << 16 | level(rgb[1]) << 8 | level(rgb[2]);
    if (!shade_table || shade_swap != swaps || shade_key != key) {
        uint32_t* table = gpuFrameMemory(256 * 4);
        if (!table) return 0;
        *fill = table, *key_out = key;
        shade_table = gpuAddress(table), shade_key = key, shade_swap = swaps;
    }
    s.vertex = GPU_TAGGED | GPU_TABLE;
    s.colour = shade_table;
    uint32_t* to = gpuDrawRoom(&s, count, count);
    if (to) drawn_vertices += (uint32_t)count;
    return to;
}
void hlglShades(uint32_t* table, uint32_t key) {
    uint32_t r = key >> 16 & 255, g = key >> 8 & 255, b = key & 255;
    for (uint32_t i = 0; i < 256; i++) table[i] = (key & 0xff000000u) | (r * i + 127) / 255 << 16 | (g * i + 127) / 255 << 8 | (b * i + 127) / 255;
}

// The state hlglShadedRoom would draw with now but for the texture, for hlglShadedPut on any core;
// its table not yet made is left to the caller (*fill, its key). 0 when such draws are not made.
typedef struct { gpu_state s; uint32_t pass; int alpha, untextured; } shaded_template;
int hlglShadedTemplate(void* to, const float* rgb, float alpha, uint32_t** fill, uint32_t* key_out) {
    shaded_template* t = to;
    *fill = 0;
    memset(t, 0, sizeof *t);
    t->pass = pass_of(), t->alpha = alpha_test, t->untextured = !texturing;
    t->s.fragment = GPU_COLOUR | GPU_PASS(t->pass);
    uint32_t key = (255 - level(alpha)) << 24 | level(rgb[0]) << 16 | level(rgb[1]) << 8 | level(rgb[2]);
    if (!shade_table || shade_swap != swaps || shade_key != key) {
        uint32_t* table = gpuFrameMemory(256 * 4);
        if (!table) return 0;
        *fill = table, *key_out = key;
        shade_table = gpuAddress(table), shade_key = key, shade_swap = swaps;
    }
    t->s.vertex = GPU_TAGGED | GPU_TABLE;
    t->s.colour = shade_table;
    return 1;
}
unsigned hlglTemplatePass(const void* t) { return ((const shaded_template*)t)->pass; }
// Room for `draws` commands and `vertices` tagged vertices with the matrices as they are, `passes`
// their passes (a bit each): 0 when there is none.
int hlglShadedRoomFor(int draws, unsigned vertices, unsigned passes, uint32_t** commands, uint32_t** room, uint32_t* block, uint32_t* first_mesh) {
    *room = gpuFrameMemory(vertices * 16);
    if (!*room || !(*commands = gpuDrawsRoom(draws, vertices, passes, block, first_mesh))) return 0;
    drawn_vertices += vertices;
    return 1;
}
// One of those draws: `count` vertices at `at`, with the template and texture `name`.
void hlglShadedPut(uint32_t* c, const void* template, unsigned name, uint32_t block, const void* at, uint32_t first_mesh, int count) {
    const shaded_template* t = template;
    gpu_state s = t->s;
    if (!t->untextured && name > 0 && name < GAP_OWNER && textures[name].at != NOWHERE) {
        uint32_t vertex = s.vertex, colour = s.colour;
        texture_state(&s, &textures[name], t->pass, t->alpha);
        s.vertex = vertex, s.colour = colour;
    }
    gpuDrawPut(c, &s, block, gpuAddress(at), first_mesh, count);
}

// The quads hlglStretchPic makes, kept to be drawn again as they were (a menu's text, mainui.patch)
typedef struct { gpu_state s; uint32_t v[24]; } hlgl_quad;
static hlgl_quad* capture;
static int captured, capture_most;
static uint32_t capture_from;
unsigned hlglQuadBytes(void) { return sizeof(hlgl_quad); }
int hlglCapturing(void) { return capture != 0; }
void hlglCaptureTo(void* to, int most) { capture = to, capture_most = most, captured = 0, capture_from = drawn_vertices; }
// How many were kept since hlglCaptureTo; -1 when they did not fit or anything else was drawn.
int hlglCaptured(void) {
    int n = captured;
    if (!capture) return -1;
    capture = 0;
    return n > capture_most || drawn_vertices - capture_from != 6u * (uint32_t)n ? -1 : n;
}
void hlglDrawQuads(const void* quads, int count) {
    const hlgl_quad* q = quads;
    for (int i = 0; i < count; i++, q++) {
        uint32_t* to = gpuDrawRoom(&q->s, 6, 6);
        if (!to) continue;
        memcpy(to, q->v, sizeof q->v);
        drawn_vertices += 6;
        if (log_picsum) {
            for (int k = 0; k < 24; k++) picsum = (picsum ^ q->v[k]) * 16777619u;
            for (unsigned k = 0; k < sizeof q->s / 4; k++) picsum = (picsum ^ ((const uint32_t*)&q->s)[k]) * 16777619u;
        }
    }
}

// R_DrawStretchPic's quad with the state as it is: what draw_vertices makes of it from GL_QUADS.
void hlglStretchPic(float x, float y, float w, float h, float s1, float t1, float s2, float t2) {
    static const uint8_t corners[6] = {0, 1, 2, 0, 2, 3};
    const vertex v[4] = {{x, y, 0, s1, t1, 0}, {x + w, y, 0, s2, t1, 0}, {x + w, y + h, 0, s2, t2, 0}, {x, y + h, 0, s1, t2, 0}};
    gpu_state s;
    if (!draw_state(&s)) return;
    int textured = (s.fragment & 0xff) != GPU_COLOUR;
    float low_s = 0, low_t = 0;
    if (textured && !(s.fragment & GPU_CLAMPED)) low_s = floor_of(s1 < s2 ? s1 : s2), low_t = floor_of(t1 < t2 ? t1 : t2);
    s.colour = replace && textured ? 0x00ffffff : colour_of();
    uint32_t* to = gpuDrawRoom(&s, 6, 6);
    if (!to) return;
    for (int i = 0; i < 6; i++, to += 4) {
        const vertex* p = &v[corners[i]];
        to[0] = bits(p->x), to[1] = bits(p->y), to[2] = bits(p->z);
        to[3] = ((uint32_t)(int32_t)((p->s - low_s) * 1024.0f) & 0xffff) | (uint32_t)(int32_t)((p->t - low_t) * 1024.0f) << 16;
        if (log_picsum) for (int k = 0; k < 4; k++) picsum = (picsum ^ to[k]) * 16777619u;
    }
    if (log_picsum) for (unsigned k = 0; k < sizeof s / 4; k++) picsum = (picsum ^ ((const uint32_t*)&s)[k]) * 16777619u;
    if (capture) {
        if (captured < capture_most) capture[captured].s = s, memcpy(capture[captured].v, to - 24, sizeof capture[0].v);
        captured++;
    }
    drawn_vertices += 6;
}

static void s_glBegin(unsigned mode) {
    prim_mode = (int)mode;
    prim_count = 0;
}
static void s_glEnd(void) {
    draw_vertices(prim_mode, prim, prim_count);
    prim_mode = -1;
}
static void s_glTexCoord2f(float s, float t) { coord_s = s, coord_t = t; }
static void s_glTexCoord2fv(const float* v) { coord_s = v[0], coord_t = v[1]; }
static void s_glTexCoord1f(float s) { coord_s = s; }
static void s_glTexCoord3f(float s, float t, float r) { (void)r, coord_s = s, coord_t = t; }
static void s_glTexCoord4f(float s, float t, float r, float q) { (void)r, (void)q, coord_s = s, coord_t = t; }
static void s_glTexCoord1fv(const float* v) { coord_s = v[0]; }
static void s_glTexCoord3fv(const float* v) { coord_s = v[0], coord_t = v[1]; }
static void s_glTexCoord4fv(const float* v) { coord_s = v[0], coord_t = v[1]; }
static void s_glVertex3f(float x, float y, float z) {
    if (prim_count == MAX_PRIMITIVE) return;
    vertex* p = &prim[prim_count++];
    p->x = x, p->y = y, p->z = z, p->s = coord_s, p->t = coord_t, p->colour = colour_of();
}
static void s_glVertex3fv(const float* v) { s_glVertex3f(v[0], v[1], v[2]); }
static void s_glVertex2f(float x, float y) { s_glVertex3f(x, y, 0); }

// client arrays
typedef struct { const uint8_t* p; int size, stride, on; unsigned type; } array;
static array va_pos, va_coord, va_colour;
static array* array_of(unsigned which) {
    return which == GL_VERTEX_ARRAY ? &va_pos : which == GL_TEXTURE_COORD_ARRAY ? &va_coord : which == GL_COLOR_ARRAY ? &va_colour : 0;
}
static void s_glEnableClientState(unsigned which) {
    array* a = array_of(which);
    if (a) a->on = 1;
}
static void s_glDisableClientState(unsigned which) {
    array* a = array_of(which);
    if (a) a->on = 0;
}
static void set_array(array* a, int size, unsigned type, int stride, const void* p) {
    a->p = p, a->size = size, a->type = type;
    a->stride = stride ? stride : size * (type == GL_UNSIGNED_BYTE ? 1 : 4);
}
static void s_glVertexPointer(int size, unsigned type, int stride, const void* p) { set_array(&va_pos, size, type, stride, p); }
static void s_glTexCoordPointer(int size, unsigned type, int stride, const void* p) { set_array(&va_coord, size, type, stride, p); }
static void s_glColorPointer(int size, unsigned type, int stride, const void* p) { set_array(&va_colour, size, type, stride, p); }

static void array_vertex(vertex* out, int i) {
    const float* p = (const float*)(va_pos.p + (size_t)i * va_pos.stride);
    out->x = p[0], out->y = p[1], out->z = va_pos.size > 2 ? p[2] : 0;
    if (va_coord.on) {
        const float* t = (const float*)(va_coord.p + (size_t)i * va_coord.stride);
        out->s = t[0], out->t = t[1];
    } else {
        out->s = coord_s, out->t = coord_t;
    }
    if (va_colour.on && va_colour.type == GL_UNSIGNED_BYTE) {
        const uint8_t* c = va_colour.p + (size_t)i * va_colour.stride;
        out->colour = (255u - (va_colour.size > 3 ? c[3] : 255u)) << 24 | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
    } else if (va_colour.on) {
        const float* c = (const float*)(va_colour.p + (size_t)i * va_colour.stride);
        float rgba[4] = {c[0], c[1], c[2], va_colour.size > 3 ? c[3] : 1};
        out->colour = colour_word(rgba);
    } else {
        out->colour = colour_of();
    }
}
static void s_glDrawArrays(unsigned mode, int first, int count) {
    if (!va_pos.on || va_pos.type != GL_FLOAT || count > MAX_PRIMITIVE) return;
    for (int i = 0; i < count; i++) array_vertex(&prim[i], first + i);
    draw_vertices((int)mode, prim, count);
}
static void s_glDrawElements(unsigned mode, int count, unsigned type, const void* indices) {
    if (!va_pos.on || va_pos.type != GL_FLOAT || count > MAX_PRIMITIVE) return;
    for (int i = 0; i < count; i++) {
        int k = type == GL_UNSIGNED_SHORT ? ((const uint16_t*)indices)[i] : type == GL_UNSIGNED_INT ? (int)((const uint32_t*)indices)[i] :
                ((const uint8_t*)indices)[i];
        array_vertex(&prim[i], k);
    }
    draw_vertices((int)mode, prim, count);
}
static void s_glDrawRangeElements(unsigned mode, unsigned lo, unsigned hi, int count, unsigned type, const void* indices) {
    (void)lo, (void)hi;
    s_glDrawElements(mode, count, type, indices);
}
static void s_glArrayElement(int i) {
    if (prim_count < MAX_PRIMITIVE && va_pos.on) array_vertex(&prim[prim_count++], i);
}

// ---- matrices, the frame and what is asked of it ----

static void s_glMatrixMode(unsigned mode) { glMatrixMode(mode); }
static void s_glLoadIdentity(void) { glLoadIdentity(); }
static void s_glLoadMatrixf(const float* m) { glLoadMatrixf(m); }
static void s_glMultMatrixf(const float* m) { glMultMatrixf(m); }
static void s_glLoadMatrixd(const double* m) {
    float f[16];
    for (int i = 0; i < 16; i++) f[i] = (float)m[i];
    glLoadMatrixf(f);
}
static void s_glMultMatrixd(const double* m) {
    float f[16];
    for (int i = 0; i < 16; i++) f[i] = (float)m[i];
    glMultMatrixf(f);
}
static void s_glPushMatrix(void) { glPushMatrix(); }
static void s_glPopMatrix(void) { glPopMatrix(); }
static void s_glFrustum(double l, double r, double b, double t, double n, double f) { glFrustumf((float)l, (float)r, (float)b, (float)t, (float)n, (float)f); }
static void s_glOrtho(double l, double r, double b, double t, double n, double f) { glOrthof((float)l, (float)r, (float)b, (float)t, (float)n, (float)f); }
static void s_glTranslatef(float x, float y, float z) { glTranslatef(x, y, z); }
static void s_glScalef(float x, float y, float z) { glScalef(x, y, z); }
static void s_glRotatef(float d, float x, float y, float z) { glRotatef(d, x, y, z); }
static void s_glTranslated(double x, double y, double z) { glTranslatef((float)x, (float)y, (float)z); }
static void s_glScaled(double x, double y, double z) { glScalef((float)x, (float)y, (float)z); }
static void s_glRotated(double d, double x, double y, double z) { glRotatef((float)d, (float)x, (float)y, (float)z); }
static int viewport[4];
static void s_glViewport(int x, int y, int w, int h) {
    viewport[0] = x, viewport[1] = y, viewport[2] = w, viewport[3] = h;
    glViewport(x, y, w, h);
}
static void s_glClearColor(float r, float g, float b, float a) { glClearColor(r, g, b, a); }
static void s_glClear(unsigned mask) { glClear(mask & GL_COLOR_BUFFER_BIT); }

static void s_glGetFloatv(unsigned what, float* out) {
    if (what == GL_MODELVIEW_MATRIX || what == GL_PROJECTION_MATRIX) {
        float rows[16];
        seglGetMatrix(what == GL_MODELVIEW_MATRIX ? GL_MODELVIEW : GL_PROJECTION, rows);
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++) out[j * 4 + i] = rows[i * 4 + j];
    } else {
        out[0] = 0;
    }
}
static void s_glGetIntegerv(unsigned what, int* out) {
    switch (what) {
        case GL_MAX_TEXTURE_SIZE: out[0] = 1024; break;
        case GL_VIEWPORT: memcpy(out, viewport, sizeof viewport); break;
        default: out[0] = 0;
    }
}
static void s_glGetDoublev(unsigned what, double* out) {
    float f[16] = {0};
    s_glGetFloatv(what, f);
    for (int i = 0; i < 16; i++) out[i] = f[i];
}
static void s_glGetBooleanv(unsigned what, unsigned char* out) { out[0] = s_glIsEnabled(what); }
static unsigned s_glGetError(void) { return 0; }
static const unsigned char* s_glGetString(unsigned what) {
    return (const unsigned char*)(what == GL_VENDOR ? "ShaderEmu" : what == GL_RENDERER ? "the machine's GPU" :
                                  what == GL_VERSION ? "1.1 shaderemu" : what == GL_EXTENSIONS ? "GL_ARB_texture_non_power_of_two" : "");
}

static void s_nothing(void) {}

// ---- the window ----

int hlglOpen(unsigned int nano_x_window) {
    if (seglInit(nano_x_window) < 0) return -1;
    seglWorldUp(3);   // z
    log_uploads = getenv("HLGL_LOG") != 0, log_frames = getenv("HLGL_FRAMES") != 0, log_picsum = getenv("HLGL_PICSUM") != 0, log_sum = getenv("HLGL_SUM") ? atoi(getenv("HLGL_SUM")) : 0;
    if (log_frames) fprintf(stderr, "hlgl: hlglOpen at %p\n", (void*)hlglOpen);   // (a flat program's place, for a profile)
    for (int i = 0; i < MAX_NAMES; i++) textures[i].at = NOWHERE;
    // the pool: a block of RAM, which the GPU reads as its own memory (HL_POOL_MB, 28 by default);
    // else the display's spare memory when there is some, then what seglMemory has left, with
    // what lies between them as a block that never leaves
    unsigned int spare_size = 0, main_size = (getenv("HL_POOL_MB") ? (unsigned)atoi(getenv("HL_POOL_MB")) : 28) << 20;
    uint8_t* spare = 0;
    uint8_t* main_part = main_size ? aligned_alloc(4096, main_size) : 0;
    if (!main_part) {
        main_size = seglMemoryLeft() & ~15u;
        spare = seglMemorySpare(&spare_size);
        main_part = seglMemory(main_size);
    }
    if (!main_part) return -1;
    if (log_frames) fprintf(stderr, "hlgl: pool of %u KB at %p\n", (main_size + spare_size) >> 10, (void*)main_part);
    pool = spare ? spare : main_part;
    pool_size = main_size + spare_size;
    block_count = 0;
    if (spare) {
        blocks[0].at = 0, blocks[0].size = spare_size, blocks[0].owner = -1;
        blocks[1].at = spare_size, blocks[1].size = (uint32_t)(main_part - spare) - spare_size, blocks[1].owner = GAP_OWNER;
        textures[GAP_OWNER].at = spare_size;
        block_count = 2;
    }
    blocks[block_count].at = (uint32_t)(main_part - pool), blocks[block_count].size = main_size, blocks[block_count].owner = -1;
    block_count++;
    // every page written once here, so that a worker core's first store to one does not stop it
    for (uint8_t* at = main_part; at < main_part + main_size; at += 4096) __asm__ volatile("amoor.w zero, zero, (%0)" : : "r"(at) : "memory");
    for (unsigned at = 0; spare && at < spare_size; at += 4096) __asm__ volatile("amoor.w zero, zero, (%0)" : : "r"(spare + at) : "memory");

    // workers for the studio models: the program's text read and the frames' sets written here first
    const char* want = getenv("HLGL_WORKERS");
    int asked = want ? atoi(want) : 46;
    // the roles on the first cores, which have the larger write caches (mcw_fast): the drawing's worker stores a
    // frame's draws and is the frame's bound, the world's, the light maps' and the entities' next (HL_ROLES=last:
    // the studio models' first, as before)
    if (asked > ROLES && !(getenv("HL_ROLES") && !strcmp(getenv("HL_ROLES"), "last"))) {
        static const unsigned char from_last[ROLES] = { 8, 1, 5, 3, 0, 4, 6, 2, 7, 9, 10, 11, 12, 13, 14, 15 };
        unsigned char order[ROLES];
        for (int i = 0; i < ROLES; i++) order[i] = (unsigned char)(asked - from_last[i]);
        mcw_prefer(order, ROLES);
    }
    workers = mcw_open(asked);
    if (workers > 64) workers = 64;
    if (workers > 0) {
        extern char _start[], etext[];  // (a flat program's __executable_start is 0)
        mcw_touch_code(_start, (unsigned)(etext - _start));
        gpuTouchSets(mcw_touch);
    } else workers = 0;
    return 0;
}

// The window was resized: the GPU draws at its new size, which *width and *height say.
void seglWindowChanged(void);
void hlglWindowChanged(int* width, int* height) {
    jobs_wait();
    seglSettle();
    seglWindowChanged();
    seglSize(width, height);
}

void hlglSwap(void) {
    jobs_wait();
    if (log_sum) {
        uint32_t one = gpuListSum(0);
        listsum = (listsum ^ one) * 16777619u;
        if ((swaps + 1) % 32 == 0) fprintf(stderr, "hlgl: sum %d %08x\n", swaps + 1, (unsigned)listsum);
    }
    gpuSwap(0);
    swaps++;
    if (log_picsum && ++picsums % 64 == 0) fprintf(stderr, "hlgl: picsum %08x\n", (unsigned)picsum);
    picsum = 0;
    if (log_frames && ++frame % 64 == 0) fprintf(stderr, "hlgl: frame %u\n", frame);
    if (log_frames && frame % 256 == 0) {	// DEBUG
        extern struct se_asks_t { unsigned fn[64], n, unused[3]; } se_asks;   // the server's worker's
        static unsigned asks_seen;
        fprintf(stderr, "hlgl waits (passes a frame): server %.1f, asks %.1f (last %x %x);", se_settle_passes / 256.0, (se_asks.n - asks_seen) / 256.0, se_asks.fn[(se_asks.n - 1) & 63], se_asks.fn[(se_asks.n - 2) & 63]);
        asks_seen = se_asks.n;
        for (int k = 1; k <= workers; k++) fprintf(stderr, " %d:%.1f/%.1f", k, dbg_waited[k] / 256.0, dbg_swapwait[k] / 256.0), dbg_waited[k] = dbg_swapwait[k] = 0;
        fprintf(stderr, "\n");
        se_settle_passes = 0;
    }
}

// The list so far summed, for HLGL_SUM=2 (rnd_shaderemu.c prints it with the time it draws).
unsigned hlglRenderSum(void) { return log_sum == 2 ? gpuListSum(0) : 0; }

void hlglStats(unsigned* tiles, unsigned* in_gpu, unsigned* gpu_size, unsigned* left_out, unsigned* vertices) {
    *tiles = tile_bytes, *in_gpu = pool_used, *gpu_size = pool_size, *left_out = refused, *vertices = drawn_vertices;
    drawn_vertices = 0;
}

#define F(name) {#name, (void*)s_##name}
#define NOP(name) {#name, (void*)s_nothing}
static const struct { const char* name; void* fn; } table[] = {
    F(glClearColor), F(glClear), NOP(glAlphaFunc), F(glBlendFunc), NOP(glCullFace), NOP(glDrawBuffer), NOP(glReadBuffer),
    NOP(glAccum), F(glEnable), F(glDisable), F(glEnableClientState), F(glDisableClientState), F(glGetBooleanv),
    F(glGetDoublev), F(glGetFloatv), F(glGetIntegerv), F(glGetError), F(glGetString), NOP(glFinish), NOP(glFlush),
    NOP(glClearDepth), NOP(glDepthFunc), NOP(glDepthMask), NOP(glDepthRange), NOP(glFrontFace), F(glDrawElements),
    F(glDrawArrays), NOP(glColorMask), NOP(glIndexPointer), F(glVertexPointer), NOP(glNormalPointer), F(glColorPointer),
    F(glTexCoordPointer), F(glArrayElement), F(glColor3f), F(glColor3fv), F(glColor4f), F(glColor4fv), F(glColor3ub),
    F(glColor4ub), F(glColor4ubv), F(glTexCoord1f), F(glTexCoord2f), F(glTexCoord3f), F(glTexCoord4f), F(glTexCoord1fv),
    F(glTexCoord2fv), F(glTexCoord3fv), F(glTexCoord4fv), NOP(glTexGenf), NOP(glTexGenfv), NOP(glTexGeni), F(glVertex2f),
    F(glVertex3f), F(glVertex3fv), NOP(glNormal3f), NOP(glNormal3fv), F(glBegin), F(glEnd), NOP(glLineWidth),
    NOP(glPointSize), F(glMatrixMode), F(glOrtho), NOP(glRasterPos2f), F(glFrustum), F(glViewport), F(glPushMatrix),
    F(glPopMatrix), NOP(glPushAttrib), NOP(glPopAttrib), F(glLoadIdentity), F(glLoadMatrixd), F(glLoadMatrixf),
    F(glMultMatrixd), F(glMultMatrixf), F(glRotated), F(glRotatef), F(glScaled), F(glScalef), F(glTranslated),
    F(glTranslatef), NOP(glReadPixels), NOP(glDrawPixels), NOP(glStencilFunc), NOP(glStencilMask), NOP(glStencilOp),
    NOP(glClearStencil), F(glIsEnabled), NOP(glIsList), F(glIsTexture), F(glTexEnvf), F(glTexEnvfv), F(glTexEnvi),
    F(glTexParameterf), F(glTexParameterfv), F(glTexParameteri), NOP(glHint), NOP(glPixelStoref), NOP(glPixelStorei),
    F(glGenTextures), F(glDeleteTextures), F(glBindTexture), NOP(glTexImage1D), F(glTexImage2D), NOP(glTexSubImage1D),
    F(glTexSubImage2D), NOP(glCopyTexImage1D), NOP(glCopyTexImage2D), NOP(glCopyTexSubImage1D), NOP(glCopyTexSubImage2D),
    NOP(glScissor), NOP(glGetTexImage), NOP(glGetTexEnviv), F(glPolygonOffset), NOP(glPolygonMode),
    NOP(glPolygonStipple), NOP(glClipPlane), NOP(glGetClipPlane), NOP(glShadeModel), NOP(glGetTexLevelParameteriv),
    NOP(glGetTexLevelParameterfv), NOP(glFogfv), NOP(glFogf), NOP(glFogi), F(glDrawRangeElements),
};

void* hlglProcAddress(const char* name) {
    for (unsigned i = 0; i < sizeof table / sizeof table[0]; i++)
        if (!strcmp(table[i].name, name)) return table[i].fn;
    return 0;
}
