// GLQuake's OpenGL on the machine's GPU (docs/games/quake.md): glBegin and its vertices turned into
// the compact vertices programs/linux/gles.c draws (its float build: nothing is converted).
// Textures stay palette indices, a light map is a second draw that multiplies, and so is a
// model's shading.

#define QGL_ITSELF
#include <GL/gl.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TEXTURES 4096       // as gles.c is built for Quake (build.sh)
#define POSES 1021              // model poses kept in GPU memory, in the texture slots from POSE_FIRST
#define POSE_FIRST 3072
#define BLOCK_OWNER (MAX_TEXTURES - 2)   // the pool's block that is not a texture (qglBlock)
#define GAP_OWNER (MAX_TEXTURES - 3)     // and the memory between the pool's two parts, which is not ours
#define MAX_PRIMITIVE 8192      // vertices between glBegin and glEnd
#define MAX_SHADE 6144          // vertices of shading waiting for their draw
#define KEY 255                 // Quake's transparent index
#define LIGHT_BLOCK 128         // a light map is this many texels square (gl_rsurf.c)
#define REG_PALETTE 0x400

// what Quake asked for; gles.c is told before each draw
static int want_texture, want_blend, want_alpha, want_depth;
static int told = -1;   // what gles.c was last told: -1 nothing that still holds, 0 as asked, 1 with white
static GLenum blend_source = GL_SRC_ALPHA, blend_destination = GL_ONE_MINUS_SRC_ALPHA;
static uint8_t colour[4] = {255, 255, 255, 255};

// The 2D screen made on the first core while a worker draws the rest (scene_shaderemu.c): its
// rectangles go where that core is told, as runs of one texture and tint, which the worker draws
// as they are (qglRecordDraw). Anything else asked of this file meanwhile on that core spoils it.
typedef struct { GLuint name; uint32_t colour; int keyed, first, count, unused[3]; } qgl_run;
static int rec_on, rec_failed, rec_room, rec_quads, rec_runs, rec_most, rec_texture;
static uint32_t* rec_at;
static GLuint rec_bound;
static uint8_t rec_colour[4];
static uintptr_t rec_stack;
static qgl_run* rec_run;
// (on the stack of the core that records, within 32 KB below where it began: the workers' stacks
// are 64 KB apart, and the first core's far from them)
static inline int recording(void) {
    uint8_t here;
    return rec_on && rec_stack + 0x1000u - (uintptr_t)&here < 0x9000u;
}
static int rec_fail(void) { return rec_failed = 1; }

static uint32_t primitive[MAX_PRIMITIVE * 4];   // x, y, z as float bits, then the shade
static float coords[MAX_PRIMITIVE * 2];
static int mode, count, shaded;   // mode: 0 outside glBegin .. glEnd
#define PARTICLE_ROOM 256         // particles are written into the frame's vertices, room for so many at a time
static uint32_t* particle_at;
static int particle_room, particles;
static float coord_s, coord_t;
static uint32_t shade[MAX_SHADE * 4];
static int shade_count;
static uint32_t byte_bits[256];   // 0..255 as float bits: a model's vertices are bytes

static inline uint32_t bits(float f) {
    union { float f; uint32_t u; } c = {f};
    return c.u;
}
static inline uint8_t level(float f) {
    return f <= 0 ? 0 : f >= 1 ? 255 : (uint8_t)(f * 255.0f);
}
// Texture coordinates as a compact vertex holds them, in 1,024ths.
static inline float number(uint32_t u) {
    union { uint32_t u; float f; } c = {u};
    return c.f;
}
// floorf, which is a call, for numbers an int holds
static inline float floor_of(float f) {
    float whole = (float)(int)f;
    return whole > f ? whole - 1 : whole;
}
static inline uint32_t coord_bits(float s, float t) {
    return ((uint32_t)(int32_t)(s * 1024.0f) & 0xffff) | (uint32_t)(int32_t)(t * 1024.0f) << 16;
}

// ---- textures ----

// GPU memory left to a program is under 3 MB and a level's textures with its models' skins and
// light maps are more. Every texture is kept in ordinary memory, as the GPU will read it, and
// GPU memory holds the ones being drawn: the one drawn longest ago makes room for a new one.
enum { NONE, INDEXED, WORDS, POSE };
#define NOWHERE 0xffffffffu
typedef struct { uint8_t* host; uint32_t at, bytes, drawn; int width, height, kind, holes, stays, smooth; } texture;
typedef struct { uint32_t at, size; int owner; } block;   // of the pool, in order; owner -1: free
static texture textures[MAX_TEXTURES];
static struct { const void* model; int pose; } pose_of[POSES];   // whose pose a pose slot holds
#define LIGHTS 128              // tables of the light at each normal, for models' shading
static struct { int key; uint32_t drawn; } light_of[LIGHTS];
static uint32_t* lights;
static block blocks[2 * MAX_TEXTURES + 2];
static int block_count;
static uint8_t* pool;
static uint32_t pool_size, pool_used, host_bytes, evictions, frame = 1;
static GLuint bound, ramp_name = MAX_TEXTURES - 1;
static uint32_t uploads;   // goes up when a texture's pixels or kind may be others: a kept command's words are then stale

// gpu_shaderemu.c: a draw made again from its command's words, and the modelview as rows
unsigned qgpuCommands(void);
const unsigned* qgpuLastCommand(void);
void qgpuRows(const float* rows);
int qgpuReplay(const unsigned* words);
void qgpuHeadBegin(unsigned* kept);
void qgpuHeadEnd(unsigned* kept);
int qgpuHeadAgain(const unsigned* kept);

// The frame's first draws, when they are the same as in the frame this set last held (qglHead).
#define HEAD_TEXTURES 32
typedef struct { uint32_t key, frame, uploads, names, name[HEAD_TEXTURES], at[HEAD_TEXTURES]; unsigned kept[9]; } head;
static head heads[2], *head_made;
static unsigned int frame_vertices, last_vertices;

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
    textures[owner].at = NOWHERE;
}

static uint32_t pool_take(uint32_t size, int owner) {
    size = (size + 15) & ~15u;
    for (int i = 0; i < block_count; i++) {
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

// Room in the pool, made if need be by what was drawn longest ago leaving it: a model's pose
// first (it can be made again), then, unless the room is for a pose, a texture. Never what
// this frame drew, unless `any`. NOWHERE if there is none.
static uint32_t pool_room(uint32_t bytes, int owner, int any) {
    uint32_t at;
    int for_pose = owner >= POSE_FIRST && owner < POSE_FIRST + POSES;
    while ((at = pool_take(bytes, owner)) == NOWHERE) {
        int oldest = -1;
        for (int poses = 1; poses >= 0 && oldest < 0 && (poses || !for_pose); poses--)
            for (int i = 0; i < block_count; i++) {
                int o = blocks[i].owner;
                if (o < 0 || o >= GAP_OWNER) continue;
                const texture* t = &textures[o];
                if (t->stays || (t->kind == POSE) != poses || (!any && t->drawn >= frame)) continue;
                if (oldest < 0 || t->drawn < textures[oldest].drawn) oldest = o;
            }
        if (oldest < 0) return NOWHERE;
        pool_give(oldest);
        evictions++;
    }
    return at;
}

// A texture the frame's first draws are drawn with: they hold while it is where it is.
static void head_texture(int name) {
    head* h = head_made;
    if (h->names > HEAD_TEXTURES) return;
    for (uint32_t i = 0; i < h->names; i++)
        if (h->name[i] == (uint32_t)name) return;
    if (h->names < HEAD_TEXTURES) h->name[h->names] = (uint32_t)name, h->at[h->names] = textures[name].at;
    h->names++;   // (one more than there is room for: too many, and not kept)
}

// Has the texture in GPU memory for the draw that follows. 0: it has no pixels, or everything
// in GPU memory is this frame's own and nothing may go.
static int resident(int name) {
    texture* t = &textures[name];
    uint32_t at;
    if (t->kind == NONE || t->kind == POSE) return 0;
    t->drawn = frame;
    if (head_made && t->at != NOWHERE) head_texture(name);
    if (t->at != NOWHERE) return 1;
    if ((at = pool_room(t->bytes, name, 0)) == NOWHERE) return 0;
    t->at = at;
    seglSettle();   // (the room may be what the list being drawn still reads)
    memcpy(pool + at, t->host, t->bytes);
    glBindTexture(GL_TEXTURE_2D, name);
    seglTexturePointer(pool + at, t->width, t->height, t->kind == INDEXED ? GL_COLOR_INDEX8_EXT : GL_RGBA);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, t->smooth ? GL_LINEAR : GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, bound);
    if (head_made) head_texture(name);
    return 1;
}

// The game's filter for the bound texture: GL_LINEAR for the level, its light, the models and
// the pictures, GL_NEAREST for the letters. The GPU weighs four texels for the first
// (docs/machine/gpu.md). QUAKE_FILTER=nearest has every texture as it was before the GPU could.
void qglTexParameterf(GLenum target, GLenum name, GLfloat value) {
    if (recording()) {
        rec_fail();
        return;
    }
    static int never = -1;
    if (never < 0) {
        const char* how = getenv("QUAKE_FILTER");
        never = how && !strcmp(how, "nearest");
    }
    if (name != GL_TEXTURE_MAG_FILTER || bound >= POSE_FIRST) return;
    uploads++;
    textures[bound].smooth = !never && (int)value != GL_NEAREST;
    glTexParameteri(target, name, textures[bound].smooth ? GL_LINEAR : GL_NEAREST);
}

// The bound texture's own memory for pixels of this size and kind.
static uint8_t* texture_memory(int width, int height, int kind) {
    texture* t = &textures[bound];
    uint32_t bytes = (uint32_t)width * height * (kind == INDEXED ? 1 : 4);
    if (t->kind != kind || t->width != width || t->height != height) {
        if (t->at != NOWHERE) pool_give(bound);
        host_bytes -= t->kind == NONE ? 0 : t->bytes;
        t->host = realloc(t->host, bytes ? bytes : 1);
        t->kind = t->host ? kind : NONE;
        t->bytes = bytes, t->width = width, t->height = height, t->at = NOWHERE;
        host_bytes += t->kind == NONE ? 0 : bytes;
    }
    return t->kind == NONE ? 0 : t->host;
}

// A texture a kept command draws with (world_shaderemu.c): put into GPU memory now and left
// there, at the same place, until qglRelease. 0 if there is no room for it.
int qglStay(GLuint name) {
    if (name >= POSE_FIRST || !resident((int)name)) return 0;
    textures[name].stays = 1;
    return 1;
}
// GPU memory for a level's own vertices, from the textures' pool: one block at a time.
void* qglBlock(unsigned int bytes) {
    uint32_t at = pool_room(bytes, BLOCK_OWNER, 1);
    seglSettle();
    return at == NOWHERE ? 0 : pool + at;
}
// The block is given back, no texture stays, and the poses of the level's models are forgotten.
void qglRelease(void) {
    uploads++;
    pool_give(BLOCK_OWNER);
    for (int i = 0; i < MAX_TEXTURES; i++) textures[i].stays = i == GAP_OWNER;
    for (int i = 0; i < POSES; i++) {
        if (textures[POSE_FIRST + i].at != NOWHERE) pool_give(POSE_FIRST + i);
        pose_of[i].model = 0;
    }
}

void qglMemory(unsigned int* kept, unsigned int* in_gpu, unsigned int* gpu_size, unsigned int* evicted, unsigned int* vertices) {
    *kept = host_bytes;
    *in_gpu = pool_used;
    *gpu_size = pool_size;
    *evicted = evictions;
    *vertices = last_vertices;
}

static void shade_flush_all(void);
static void run_flush(void);
static int run_room, quads_open;   // (sprites and quads that wait for their draw: below)

// Before anything is set or drawn: what waits for its draw gets it. (Mostly nothing does.)
static inline void shade_flush(void) {
    if (rec_on && recording()) {
        rec_fail();
        return;
    }
    if (run_room | quads_open | shade_count) shade_flush_all();
}

// A model's pose as compact vertices with its skin's coordinates is kept in the pool like a
// texture: a pose drawn again (a thing at rest, a flame's few) is a command and no vertices.
// The pool slot of a pose, and whether its vertices are there (else it is the slot to fill).
static int pose_slot(const void* model, int pose, int* there) {
    uint32_t h = (((uint32_t)(uintptr_t)model >> 4) ^ ((uint32_t)pose * 2654435761u)) % POSES;
    int oldest = -1;
    for (int k = 0; k < 8; k++) {
        int i = (int)((h + k) % POSES);
        texture* t = &textures[POSE_FIRST + i];
        if (pose_of[i].model == model && pose_of[i].pose == pose && t->at != NOWHERE) {
            *there = 1;
            return i;
        }
        if (t->at == NOWHERE) {
            if (oldest < 0 || textures[POSE_FIRST + oldest].at != NOWHERE) oldest = i;
        } else if (t->drawn < frame && (oldest < 0 || (textures[POSE_FIRST + oldest].at != NOWHERE && t->drawn < textures[POSE_FIRST + oldest].drawn))) {
            oldest = i;
        }
    }
    *there = 0;
    return oldest;
}

void qglBindTexture(GLenum target, GLuint name) {
    if (recording()) {
        rec_bound = name < POSE_FIRST ? name : 0;
        return;
    }
    shade_flush();
    told = -1;
    bound = name < POSE_FIRST ? name : 0;
    glBindTexture(target, bound);
}

void qglTexImage8(const GLubyte* pixels, GLsizei width, GLsizei height, int holes) {
    texture* t = &textures[bound];
    uint8_t* to = bound ? texture_memory(width, height, INDEXED) : 0;
    uploads++;
    if (!to) return;
    t->holes = holes;
    if (pixels) memcpy(to, pixels, t->bytes);
    else memset(to, 0, t->bytes);
    if (t->at != NOWHERE) seglSettle(), memcpy(pool + t->at, to, t->bytes);
}

// Rows of a texture of words from the RGBA bytes Quake uploads.
static void words(uint32_t* to, const uint8_t* from, int n, GLenum format) {
    (void)format;
    for (int i = 0; i < n; i++, from += 4)
        to[i] = (uint32_t)(255 - from[3]) << 24 | (uint32_t)from[0] << 16 | (uint32_t)from[1] << 8 | from[2];
}

// A surface's light as GLQuake sums it (8.8 a texel), written into its place in a light map:
// the texture kept here and, when it is there, the one in GPU memory. GLQuake's own copy of
// the light maps and its uploads of them are not used (uploads of that format do nothing).
void qglLight(GLuint name, int x, int y, int width, int height, const unsigned int* light) {
    GLuint was = bound;
    texture* t = &textures[name];
    bound = name;
    if (t->kind != WORDS || t->width != LIGHT_BLOCK) {
        uint8_t* fresh = texture_memory(LIGHT_BLOCK, LIGHT_BLOCK, WORDS);
        if (fresh) memset(fresh, 0, t->bytes);
        t->holes = 0;
    }
    bound = was;
    if (t->kind != WORDS || x < 0 || y < 0 || x + width > t->width || y + height > t->height) return;
    // (a texture that is not in GPU memory has its second copy written over the first)
    uint8_t* there = t->at != NOWHERE ? pool + t->at : t->host;
    if (t->at != NOWHERE) seglSettle();
    for (int row = 0; row < height; row++, light += width) {
        uint32_t at = (uint32_t)((y + row) * t->width + x) * 4;
        uint32_t* to = (uint32_t*)(t->host + at);
        uint32_t* also = (uint32_t*)(there + at);
        for (int i = 0; i < width; i++) {
            uint32_t v = light[i] >> 7;
            to[i] = also[i] = (v > 255 ? 255 : v) * 0x010101u;
        }
    }
}

// The words of a light map that stays in GPU memory (qglStay), for a worker to write a
// surface's light there (world_shaderemu.c); NULL when it is not such a one.
unsigned int* qglLightWords(GLuint name) {
    const texture* t = &textures[name < MAX_TEXTURES ? name : 0];
    return t->kind == WORDS && t->width == LIGHT_BLOCK && t->stays && t->at != NOWHERE ? (unsigned int*)(pool + t->at) : 0;
}

void qglTexImage2D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border, GLenum format,
                   GLenum type, const GLvoid* pixels) {
    (void)target, (void)internal, (void)border, (void)type;
    if (level != 0 || !bound || (format != GL_RGBA && format != GL_COLOR_INDEX)) return;
    if (format == GL_COLOR_INDEX) {
        qglTexImage8(pixels, width, height, 0);
        return;
    }
    texture* t = &textures[bound];
    uint32_t* to = (uint32_t*)texture_memory(width, height, WORDS);
    uploads++;
    if (!to) return;
    t->holes = 0;
    if (pixels) words(to, pixels, width * height, format);
    else memset(to, 0, t->bytes);
    if (t->at != NOWHERE) seglSettle(), memcpy(pool + t->at, to, t->bytes);
}

void qglTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                      const GLvoid* pixels) {
    (void)target, (void)type;
    texture* t = &textures[bound];
    if (level != 0 || format != GL_RGBA || t->kind != WORDS || x < 0 || y < 0 || x + width > t->width || y + height > t->height) return;
    int step = 4;
    for (int row = 0; row < height; row++) {
        uint32_t at = (uint32_t)((y + row) * t->width + x) * 4;
        words((uint32_t*)(t->host + at), (const uint8_t*)pixels + row * width * step, width, format);
        if (t->at != NOWHERE) seglSettle(), memcpy(pool + t->at + at, t->host + at, (size_t)width * 4);
    }
}

// ---- state ----

static int* wanted(GLenum what) {
    switch (what) {
        case GL_TEXTURE_2D: return &want_texture;
        case GL_BLEND: return &want_blend;
        case GL_ALPHA_TEST: return &want_alpha;
        case GL_DEPTH_TEST: return &want_depth;
    }
    return 0;
}
void qglEnable(GLenum what) {
    int* w = wanted(what);
    if (recording()) {   // (the 2D screen's state: a texture or none; blending is the sprites' own)
        if (what == GL_TEXTURE_2D) rec_texture = 1;
        else if (what == GL_DEPTH_TEST) rec_fail();
        return;
    }
    if (w && !*w) shade_flush(), *w = 1, told = -1;
}
void qglDisable(GLenum what) {
    int* w = wanted(what);
    if (recording()) {
        if (what == GL_TEXTURE_2D) rec_texture = 0;
        return;
    }
    if (w && *w) shade_flush(), *w = 0, told = -1;
}
void qglBlendFunc(GLenum source, GLenum destination) {
    if (recording()) return;
    blend_source = source;
    blend_destination = destination;
    told = -1;
}

// What gles.c has been told, so that it is told only what changes: a draw's state was seven
// calls, most of them saying what it was set to already.
static int given_texture = -1, given_depth = -1, given_blend = -1, given_alpha = -1;
static GLenum given_source, given_destination;
static uint32_t given_colour;
static int colour_given;

static inline void give(int* given, GLenum what, int on) {
    if (*given == on) return;
    *given = on;
    (on ? glEnable : glDisable)(what);
}
static inline void give_blend(GLenum source, GLenum destination) {
    if (given_source == source && given_destination == destination) return;
    given_source = source, given_destination = destination;
    glBlendFunc(source, destination);
}
static inline void give_colour(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    uint32_t packed = r << 24 | g << 16 | b << 8 | a;
    if (given_colour == packed && colour_given) return;
    given_colour = packed, colour_given = 1;
    glColor4ub((GLubyte)r, (GLubyte)g, (GLubyte)b, (GLubyte)a);
}

// Tells gles.c what the next draw is drawn with. Everything drawn without the depth test (the
// 2D screen) goes into the one blended pass, so that it keeps the order it is drawn in.
static void state(int white) {
    const texture* t = &textures[bound];
    if (told == white && (!want_texture || (t->at != NOWHERE && t->drawn == frame))) return;
    told = white;
    int textured = want_texture && resident(bound);
    give(&given_texture, GL_TEXTURE_2D, textured);
    give(&given_depth, GL_DEPTH_TEST, want_depth);
    give(&given_blend, GL_BLEND, want_blend || !want_depth);
    if (!want_depth) give_blend(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    else if (blend_source == GL_ZERO && blend_destination == GL_ONE_MINUS_SRC_COLOR) give_blend(GL_ZERO, GL_SRC_COLOR);
    else give_blend(blend_source, blend_destination);
    give(&given_alpha, GL_ALPHA_TEST, textured && t->holes && (want_alpha || want_blend || !want_depth));
    if (white) give_colour(255, 255, 255, 255);
    else give_colour(colour[0], colour[1], colour[2], colour[3]);
}

void qglColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    if (recording()) {
        rec_colour[0] = level(r), rec_colour[1] = level(g), rec_colour[2] = level(b), rec_colour[3] = level(a);
        return;
    }
    colour[0] = level(r), colour[1] = level(g), colour[2] = level(b), colour[3] = level(a);
    if (told == 0) told = -1;
    if (mode) shaded = 1;
}
void qglColor3f(GLfloat r, GLfloat g, GLfloat b) { qglColor4f(r, g, b, 1); }
void qglColor4fv(const GLfloat* v) { qglColor4f(v[0], v[1], v[2], v[3]); }
void qglColor3ubv(const GLubyte* v) {
    if (recording()) {
        rec_colour[0] = v[0], rec_colour[1] = v[1], rec_colour[2] = v[2], rec_colour[3] = 255;
        return;
    }
    colour[0] = v[0], colour[1] = v[1], colour[2] = v[2], colour[3] = 255;
    if (told == 0) told = -1;
    if (mode) shaded = 1;
}

void qglClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { glClearColor(r, g, b, a); }
void qglClear(GLbitfield mask) {
    shade_flush();
    glClear(mask);
}
void qglViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    shade_flush();
    glViewport(x, y, width, height);
}

// ---- matrices ----

// An entity's matrix is set as it is (qglEntity) and not taken off again: the world's is put
// back when something is drawn or turned from it (qglWorldAgain).
static const float* entity_base;   // the world's matrix, while the modelview is an entity's

void qglWorldAgain(void) {
    const float* m = entity_base;
    if (!m) return;
    shade_flush();
    float rows[12] = {m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10], m[14]};
    qgpuRows(rows);
    entity_base = 0;
}

void qglMatrixMode(GLenum which) {
    shade_flush();
    glMatrixMode(which);
}
void qglLoadIdentity(void) {
    shade_flush();
    entity_base = 0;
    glLoadIdentity();
}
void qglPushMatrix(void) {
    qglWorldAgain();
    glPushMatrix();
}
void qglPopMatrix(void) {
    shade_flush();
    glPopMatrix();
}
void qglRotatef(GLfloat degrees, GLfloat x, GLfloat y, GLfloat z) {
    shade_flush();
    glRotatef(degrees, x, y, z);
}
void qglTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    shade_flush();
    glTranslatef(x, y, z);
}
void qglScalef(GLfloat x, GLfloat y, GLfloat z) {
    shade_flush();
    glScalef(x, y, z);
}
void qglFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    shade_flush();
    glFrustumf((float)l, (float)r, (float)b, (float)t, (float)n, (float)f);
}
void qglOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    shade_flush();
    glOrthof((float)l, (float)r, (float)b, (float)t, (float)n, (float)f);
}
void qglMultMatrixf(const GLfloat* m) {
    shade_flush();
    glMultMatrixf(m);
}
void qglGetFloatv(GLenum what, GLfloat* out) {
    if (what == GL_MODELVIEW_MATRIX) seglGetMatrix(GL_MODELVIEW, out);
    else if (what == GL_PROJECTION_MATRIX) seglGetMatrix(GL_PROJECTION, out);
}
void qglLoadMatrixf(const GLfloat* m) {
    shade_flush();
    entity_base = 0;
    seglSetMatrix(m);
}

// gles.c's sine of degrees, the same sums: an entity's matrix below must come out as its own.
static float turn_sine(float degrees) {
    float turns = degrees * (1.0f / 360);
    float d = (turns - (float)(int)turns) * 360.0f;
    if (d < 0) d += 360.0f;
    int negative = d >= 180.0f;
    if (negative) d -= 180.0f;
    if (d > 90.0f) d = 180.0f - d;
    float x = d * 0.017453292f, x2 = x * x;
    float s = x * (1.0f + x2 * (-0.16666667f + x2 * (0.0083333310f + x2 * (-0.00019840874f + x2 * 0.0000027525562f))));
    return negative ? -s : s;
}

// The view's modelview as GLQuake makes it: from `base` (its two turns that put z up, made by
// the library once) turned about x, y and z by so many degrees and moved by `move`: what
// glRotatef and glTranslatef make of it in gles.c, sum for sum, without the products by 0 and
// 1. `out` is what glGetFloatv gives of it.
void qglView(const float* base, float about_x, float about_y, float about_z, const float* move, float* out) {
    float xs = turn_sine(about_x), xc = turn_sine(about_x + 90.0f), xa = (1 - xc) + xc;
    float ys = turn_sine(about_y), yc = turn_sine(about_y + 90.0f), yb = (1 - yc) + yc;
    float zs = turn_sine(about_z), zc = turn_sine(about_z + 90.0f);
    shade_flush();
    for (int i = 0; i < 3; i++) {
        float a = base[i], b = base[4 + i], c = base[8 + i], d = base[12 + i], a0, b0, c0;
        b0 = b, c0 = c;
        a = a * xa, b = b0 * xc + c0 * xs, c = c0 * xc - b0 * xs;
        a0 = a, c0 = c;
        a = a0 * yc - c0 * ys, b = b * yb, c = a0 * ys + c0 * yc;
        a0 = a, b0 = b;
        a = a0 * zc + b0 * zs, b = b0 * zc - a0 * zs;
        d += a * move[0] + b * move[1] + c * move[2];
        out[i] = a, out[4 + i] = b, out[8 + i] = c, out[12 + i] = d;
    }
    out[3] = out[7] = out[11] = 0, out[15] = 1;
    entity_base = 0;
    seglSetMatrix(out);
}

// An entity's modelview from the world's (`base`, as glGetFloatv gave it): what glTranslatef
// to its place, glRotatef about z, y and x by its angles (those that are not 0) and, with
// `move`, glTranslatef and glScalef make of it in gles.c, sum for sum, without the products
// by the 0s and 1s of a turn about an axis. The fourth row, which no draw reads, is base's.
void qglEntity(const float* base, const float* place, float yaw, float pitch, float roll, const float* move, const float* scale) {
    float m[12], yaw_s = 0, yaw_c = 0, pitch_s = 0, pitch_c = 0, pitch_b = 0, roll_s = 0, roll_c = 0, roll_a = 0;
    shade_flush();
    if (yaw != 0) yaw_s = turn_sine(yaw), yaw_c = turn_sine(yaw + 90.0f);
    if (pitch != 0) pitch_s = turn_sine(pitch), pitch_c = turn_sine(pitch + 90.0f), pitch_b = (1 - pitch_c) + pitch_c;
    if (roll != 0) roll_s = turn_sine(roll), roll_c = turn_sine(roll + 90.0f), roll_a = (1 - roll_c) + roll_c;
    for (int i = 0; i < 3; i++) {
        float a = base[i], b = base[4 + i], c = base[8 + i], d = base[12 + i];
        d += a * place[0] + b * place[1] + c * place[2];
        if (yaw != 0) {
            float a0 = a, b0 = b;
            a = a0 * yaw_c + b0 * yaw_s, b = b0 * yaw_c - a0 * yaw_s;
        }
        if (pitch != 0) {
            float a0 = a, c0 = c;
            a = a0 * pitch_c - c0 * pitch_s, b = b * pitch_b, c = a0 * pitch_s + c0 * pitch_c;
        }
        if (roll != 0) {
            float b0 = b, c0 = c;
            a = a * roll_a, b = b0 * roll_c + c0 * roll_s, c = c0 * roll_c - b0 * roll_s;
        }
        if (move) {
            d += a * move[0] + b * move[1] + c * move[2];
            a = a * scale[0], b = b * scale[1], c = c * scale[2];
        }
        m[4 * i] = a, m[4 * i + 1] = b, m[4 * i + 2] = c, m[4 * i + 3] = d;
    }
    // A brush model (no move) is pulled toward the eye by 1/16384 of its distance: where it lies
    // on a world face (a lift down, a door shut) it wins the depth test instead of fighting.
    // A point keeps its place on the picture: x/w and y/w do not change.
    if (!move)
        for (int k = 0; k < 12; k++) m[k] *= 1.0f - 1.0f / 16384;
    qgpuRows(m);
    entity_base = base;
}

// ---- drawing ----

// The 2D screen's pictures and letters are rectangles of textures. A run of them with one
// texture is written where the frame's next vertices go, and is a command of the list when
// anything else is drawn or set: shade_flush(), which comes before all of that, ends it.
static uint32_t* run_at;
static int run_room, run_count, run_keyed;
static GLuint run_name;
static uint32_t run_colour;

static void run_flush(void) {
    if (run_count) seglSpritesUsed(run_count, run_name, run_colour, run_keyed);
    frame_vertices += 4 * run_count;
    run_count = run_room = 0;
}

// Polygons drawn one after another with nothing set between them (a door's faces, a pool's)
// are written straight into the frame, as the two triangles the GPU makes of a quad's corners
// (0 1 2 and 0 2 3): room is taken for many, and what is left of it given back when something
// is set or drawn. A draw costs 200 instructions before its first vertex.
#define ROOM_QUADS 64
static uint32_t* quads_at;   // where the next quad's six vertices go
static int quads_left, quads_open;

static void quads_close(void) {
    if (quads_left) seglCompactTrim(quads_left * 6);
    quads_left = quads_open = 0;
}

// A model's shading: the triangles drawn once more, multiplying what is there by a ramp of
// greys looked up with the vertex's light. Kept until the state they were drawn in changes.
static void shade_flush_all(void) {
    if (run_room) run_flush();
    if (quads_open) quads_close();
    if (!shade_count) return;
    glBindTexture(GL_TEXTURE_2D, ramp_name);
    give(&given_texture, GL_TEXTURE_2D, 1);
    give(&given_depth, GL_DEPTH_TEST, 1);
    give(&given_blend, GL_BLEND, 1);
    give_blend(GL_ZERO, GL_SRC_COLOR);
    give(&given_alpha, GL_ALPHA_TEST, 0);
    give_colour(255, 255, 255, 255);
    seglCompact(GL_TRIANGLES, shade, shade_count);
    frame_vertices += shade_count;
    shade_count = 0;
    told = -1;
    glBindTexture(GL_TEXTURE_2D, bound);
}

static inline void shade_vertex(const uint32_t* v) {
    uint32_t* to = shade + 4 * shade_count++;
    to[0] = v[0], to[1] = v[1], to[2] = v[2];
    to[3] = (v[3] * 4 + 2) | 512u << 16;   // the middle of texel v[3] of the 256 x 1 ramp
}

static void shade_add(int mode) {
    int triangles = mode == GL_TRIANGLES ? count / 3 : count - 2;
    for (int i = 0; i < triangles; i++) {
        if (shade_count + 3 > MAX_SHADE) shade_flush();
        if (mode == GL_TRIANGLES) {
            shade_vertex(primitive + 12 * i), shade_vertex(primitive + 12 * i + 4), shade_vertex(primitive + 12 * i + 8);
        } else if (mode == GL_TRIANGLE_STRIP) {
            shade_vertex(primitive + 4 * (i + (i & 1))), shade_vertex(primitive + 4 * (i + 1 - (i & 1)));
            shade_vertex(primitive + 4 * (i + 2));
        } else {
            shade_vertex(primitive), shade_vertex(primitive + 4 * (i + 1)), shade_vertex(primitive + 4 * (i + 2));
        }
    }
}

void qglBegin(GLenum what) {
    if (recording()) {
        rec_fail();
        return;
    }
    mode = what == GL_POLYGON ? GL_TRIANGLE_FAN : what;
    count = shaded = 0;
}

void qglTexCoord2f(GLfloat s, GLfloat t) {
    if (recording()) return;
    coord_s = s;
    coord_t = t;
}

void qglVertex3f(GLfloat x, GLfloat y, GLfloat z) {
    if (count == MAX_PRIMITIVE || recording()) return;
    uint32_t* v = primitive + 4 * count;
    v[0] = bits(x), v[1] = bits(y), v[2] = bits(z);
    v[3] = colour[1];
    coords[2 * count] = coord_s, coords[2 * count + 1] = coord_t;
    count++;
}
void qglVertex3fv(const GLfloat* v) { qglVertex3f(v[0], v[1], v[2]); }
void qglVertex2f(GLfloat x, GLfloat y) { qglVertex3f(x, y, 0); }

// The 2D screen's pictures and letters: a quad that is a rectangle of a texture, corners in
// order round it from the top left, is one call that most often joins the command before.
static int rectangle(void) {
    const uint32_t* v = primitive;
    const float* c = coords;
    if (v[1] != v[5] || v[4] != v[8] || v[9] != v[13] || v[0] != v[12] || (v[2] | v[6] | v[10] | v[14])) return 0;
    if (c[1] != c[3] || c[2] != c[4] || c[5] != c[7] || c[0] != c[6]) return 0;
    if (c[0] < 0 || c[1] < 0 || c[4] < 0 || c[5] < 0 || c[0] > 1 || c[1] > 1 || c[4] > 1 || c[5] > 1) return 0;
    const texture* t = &textures[bound];
    shade_flush();
    if (!resident(bound)) return 0;
    GLfixed box[4] = {(GLfixed)(number(v[0]) * 65536.0f), (GLfixed)(number(v[1]) * 65536.0f),
                      (GLfixed)(number(v[8]) * 65536.0f), (GLfixed)(number(v[9]) * 65536.0f)};
    int texels[4] = {(int)(c[0] * 1024.0f), (int)(c[1] * 1024.0f), (int)(c[4] * 1024.0f), (int)(c[5] * 1024.0f)};
    seglSprite(box, texels, bound, (255u - colour[3]) << 24 | (uint32_t)colour[0] << 16 | (uint32_t)colour[1] << 8 | colour[2],
               t->holes && t->kind == INDEXED);
    frame_vertices += 4;
    return 1;
}

// A rectangle of the bound texture, corners in pixels of the 2D screen and in 1,024ths of the
// texture: what glBegin(GL_QUADS) and four corners in order come to (rectangle() below),
// without them. 0 when it is not to be drawn so, and nothing was done.
static int rec_rect(int x0, int y0, int x1, int y1, int s0, int t0, int s1, int t1) {
    uint32_t tint = (255u - rec_colour[3]) << 24 | (uint32_t)rec_colour[0] << 16 | (uint32_t)rec_colour[1] << 8 | rec_colour[2];
    const texture* t = &textures[rec_bound];
    int keyed = t->holes && t->kind == INDEXED;
    qgl_run* r = rec_runs ? &rec_run[rec_runs - 1] : 0;
    if (!rec_texture || rec_quads == rec_room) return rec_fail();
    if (!r || r->name != rec_bound || r->colour != tint || r->keyed != keyed) {
        if (rec_runs == rec_most) return rec_fail();
        r = &rec_run[rec_runs++];
        r->name = rec_bound, r->colour = tint, r->keyed = keyed, r->first = rec_quads, r->count = 0;
    }
    uint32_t* v = rec_at + 16 * rec_quads;
    uint32_t left = bits((float)x0), top = bits((float)y0), right = bits((float)x1), bottom = bits((float)y1);
    uint32_t u0 = (uint32_t)s0 & 0xffff, u1 = (uint32_t)s1 & 0xffff, v0 = (uint32_t)t0 << 16, v1 = (uint32_t)t1 << 16;
    v[0] = left, v[1] = top, v[2] = 0, v[3] = u0 | v0;
    v[4] = right, v[5] = top, v[6] = 0, v[7] = u1 | v0;
    v[8] = right, v[9] = bottom, v[10] = 0, v[11] = u1 | v1;
    v[12] = left, v[13] = bottom, v[14] = 0, v[15] = u0 | v1;
    rec_quads++, r->count++;
    return 1;
}

int qglRect(int x0, int y0, int x1, int y1, int s0, int t0, int s1, int t1) {
    if (recording()) return rec_rect(x0, y0, x1, y1, s0, t0, s1, t1);
    uint32_t tint = (255u - colour[3]) << 24 | (uint32_t)colour[0] << 16 | (uint32_t)colour[1] << 8 | colour[2];
    if (run_count == run_room || run_name != bound || run_colour != tint) {
        const texture* t = &textures[bound];
        if (want_depth || !want_texture || mode) return 0;
        shade_flush();
        if (!resident(bound)) return 0;
        run_at = seglSpriteSpace(&run_room);
        run_name = bound, run_colour = tint, run_keyed = t->holes && t->kind == INDEXED;
        if (!run_room) return 1;   // (the frame is full: not drawn, as the list would not have taken it)
    }
    uint32_t* v = run_at;
    uint32_t left = bits((float)x0), top = bits((float)y0), right = bits((float)x1), bottom = bits((float)y1);
    uint32_t u0 = (uint32_t)s0 & 0xffff, u1 = (uint32_t)s1 & 0xffff, v0 = (uint32_t)t0 << 16, v1 = (uint32_t)t1 << 16;
    v[0] = left, v[1] = top, v[2] = 0, v[3] = u0 | v0;
    v[4] = right, v[5] = top, v[6] = 0, v[7] = u1 | v0;
    v[8] = right, v[9] = bottom, v[10] = 0, v[11] = u1 | v1;
    v[12] = left, v[13] = bottom, v[14] = 0, v[15] = u0 | v1;
    run_at += 16, run_count++;
    return 1;
}
// Letters of the bound texture, 16 x 16 of them in it, 8 pixels square and 8 apart from x, y:
// each what qglRect draws of one, and none for a space. 0 if nothing was done.
int qglText(int x, int y, const char* text, int count) {
    if (recording()) {
        for (int i = 0; i < count; i++, x += 8) {
            uint32_t c = (uint8_t)text[i];
            if (c != 32) rec_rect(x, y, x + 8, y + 8, (int)(c & 15) * 64, (int)(c >> 4) * 64, (int)(c & 15) * 64 + 64, (int)(c >> 4) * 64 + 64);
        }
        return 1;
    }
    uint32_t tint = (255u - colour[3]) << 24 | (uint32_t)colour[0] << 16 | (uint32_t)colour[1] << 8 | colour[2];
    uint32_t top = bits((float)y), bottom = bits((float)(y + 8));
    for (int i = 0; i < count; i++, x += 8) {
        uint32_t c = (uint8_t)text[i];
        if (c == 32) continue;
        if (run_count == run_room || run_name != bound || run_colour != tint) {
            // (a run begins: qglRect knows how, and says no only before anything is drawn)
            if (!qglRect(x, y, x + 8, y + 8, (int)(c & 15) * 64, (int)(c >> 4) * 64, (int)(c & 15) * 64 + 64, (int)(c >> 4) * 64 + 64))
                return 0;
            continue;
        }
        uint32_t* v = run_at;
        uint32_t left = bits((float)x), right = bits((float)(x + 8));
        uint32_t u0 = (c & 15) * 64, u1 = u0 + 64, v0 = (c >> 4) * 64 << 16, v1 = v0 + (64u << 16);
        v[0] = left, v[1] = top, v[2] = 0, v[3] = u0 | v0;
        v[4] = right, v[5] = top, v[6] = 0, v[7] = u1 | v0;
        v[8] = right, v[9] = bottom, v[10] = 0, v[11] = u1 | v1;
        v[12] = left, v[13] = bottom, v[14] = 0, v[15] = u0 | v1;
        run_at += 16, run_count++;
    }
    return 1;
}
// The same for a picture: `st` is where in its texture it is, left, top, right, bottom of 1.
int qglPicture(int x, int y, int width, int height, const float* st) {
    if (st[0] < 0 || st[1] < 0 || st[2] > 1 || st[3] > 1 || st[0] > st[2] || st[1] > st[3]) return 0;
    return qglRect(x, y, x + width, y + height, (int)(st[0] * 1024.0f), (int)(st[1] * 1024.0f), (int)(st[2] * 1024.0f),
                   (int)(st[3] * 1024.0f));
}

// The recording (scene_shaderemu.c): on the first core, into `vertices` (room for `quads`) and
// `runs` (room for `most`); its end says how many runs, -1 if it was spoilt.
void seglSpritesAt(const void* vertices, GLsizei quads, GLuint name, unsigned colour, int keyed);   // gles.c
int qglRecording(void) { return recording(); }
void qglRecordFail(void) { rec_fail(); }
void qglRecordBegin(unsigned int* vertices, int quads, void* runs, int most) {
    uint8_t here;
    rec_stack = (uintptr_t)&here;
    rec_at = vertices, rec_room = quads, rec_run = runs, rec_most = most;
    rec_quads = rec_runs = rec_failed = 0;
    rec_bound = 0, rec_texture = 1;
    rec_colour[0] = rec_colour[1] = rec_colour[2] = rec_colour[3] = 255;
    rec_on = 1;
}
int qglRecordEnd(void) {
    rec_on = 0;
    return rec_failed ? -1 : rec_runs;
}
// On the core that draws: the runs recorded, in order, where the recording left their vertices.
void qglRecordDraw(const unsigned int* vertices, const void* runs, int count) {
    const qgl_run* r = runs;
    shade_flush();
    for (int i = 0; i < count; i++, r++)
        if (resident(r->name)) {
            seglSpritesAt(vertices + 16 * r->first, r->count, r->name, r->colour, r->keyed);
            frame_vertices += 4 * (unsigned int)r->count;
        }
}
// GPU memory for the recordings' vertices: three of `bytes` (a frame's, the next one's, one being drawn).
#define STAGE_MEMORY (3 * 1024 * 64)
static uint32_t* stage_memory;
unsigned int* qglStageMemory(int which) { return stage_memory ? stage_memory + which * (STAGE_MEMORY / 3 / 4) : 0; }

void qglEnd(void) {
    int what = mode, lit;
    if (recording()) return;
    mode = 0;
    if (quads_open) quads_close();
    if (particle_room) seglCompactTrim(particle_room);
    particle_room = particles = 0;
    if (count < 3) return;
    if (what == GL_QUADS && count == 4 && !want_depth && want_texture && rectangle()) return;
    // light given a vertex at a time is the shading draw's; without a texture it is one colour
    lit = shaded && want_texture && !want_blend && want_depth && what != GL_QUADS;
    if (lit) shade_add(what);
    else shade_flush();
    // A compact vertex holds texture coordinates within 32 repeats: whole repeats are taken off
    // all of a primitive's, which changes nothing a repeating texture shows.
    float low_s = coords[0], low_t = coords[1];
    for (int i = 1; i < count; i++) {
        if (coords[2 * i] < low_s) low_s = coords[2 * i];
        if (coords[2 * i + 1] < low_t) low_t = coords[2 * i + 1];
    }
    low_s = floor_of(low_s), low_t = floor_of(low_t);
    for (int i = 0; i < count; i++) primitive[4 * i + 3] = coord_bits(coords[2 * i] - low_s, coords[2 * i + 1] - low_t);
    state(lit);
    seglCompact(what, primitive, count);
    frame_vertices += count;
}

// A particle, between glBegin(GL_TRIANGLES) and glEnd: a point, which the GPU makes a triangle
// of, its colour a palette index, which is a texel of the 16 x 16 picture of all of them.
void qglParticle(const float* o, int index) {
    // GLQuake's triangle: 1.5 units up and right of the point, larger from 20 units away on
    static const float how[3] = {1.5f, 0.004f, 20.0f};
    if (!particle_room) {
        if (!particles) shade_flush(), state(0);
        particles = 1;
        particle_at = seglPoints(PARTICLE_ROOM, how);
        if (!particle_at) return;
        particle_room = PARTICLE_ROOM;
    }
    uint32_t* v = particle_at;
    particle_at += 4, particle_room--, frame_vertices++;
    v[0] = bits(o[0]), v[1] = bits(o[1]), v[2] = bits(o[2]);
    v[3] = (uint32_t)((index & 15) * 64 + 32) | (uint32_t)((index >> 4 & 15) * 64 + 32) << 16;   // the middle of texel `index`
}

// The same for many: room for particles the game writes itself (three words of place, then
// qgl_particle_colour of its index), and how much of the room it last had was left over.
unsigned int qgl_particle_colour[256];
unsigned int* qglParticleRoom(int* room) {
    static const float how[3] = {1.5f, 0.004f, 20.0f};
    if (!particles) shade_flush(), state(0);
    particles = 1;
    uint32_t* at = seglPoints(PARTICLE_ROOM, how);
    *room = at ? PARTICLE_ROOM : 0;
    frame_vertices += (unsigned int)*room;
    return at;
}
void qglParticleRoomLeft(int room) {
    particle_room = room;
    frame_vertices -= (unsigned int)room;
}
// Particles drawn where workers wrote them in GPU memory (qglParticleMemory), 3 or more.
int qgpuPointsAt(const void* vertices, int count, const float* how);
int qglParticlesAt(const unsigned int* vertices, int count) {
    static const float how[3] = {1.5f, 0.004f, 20.0f};
    if (!particles) shade_flush(), state(0);
    particles = 1;
    if (!qgpuPointsAt(vertices, count, how)) return 0;
    frame_vertices += (unsigned int)count;
    return 1;
}
static uint32_t* particle_memory;
#define PARTICLE_MEMORY (4 * 2048 * 16)
unsigned int* qglParticleMemory(unsigned int bytes) { return bytes <= PARTICLE_MEMORY ? particle_memory : 0; }

// Room for `most` quads drawn with what is set now, 24 words each: the compact vertices of
// corners 0 1 2 and then 0 2 3. The caller writes them there and says how many it wrote
// (qglQuadsUsed) before it asks again. NULL when the frame has no room.
unsigned int* qglQuadsRoom(int most) {
    if (!quads_open || told != 0 || quads_left < most) {
        shade_flush();
        state(0);
        quads_left = most > ROOM_QUADS ? most : ROOM_QUADS;
        quads_at = seglCompactSpace(quads_left * 6);
        // (a frame nearly full may still have what was asked for)
        if (!quads_at && quads_left > most) quads_at = seglCompactSpace((quads_left = most) * 6);
        if (!quads_at) {
            quads_left = 0;
            return 0;
        }
        quads_open = 1;
    }
    return quads_at;
}
void qglQuadsUsed(int quads) {
    quads_at += 24 * quads, quads_left -= quads;
    frame_vertices += (unsigned int)quads * 6;
}

// A polygon of the level, its vertices made when the level loaded (world_shaderemu.c): a fan,
// drawn as quads of two of its triangles each, the last corner twice when one is left over.
void qglFan(const unsigned int* v, int n) {
    uint32_t* to = n < 3 ? 0 : qglQuadsRoom((n - 1) / 2);
    if (!to) return;
    for (int i = 1; i + 1 < n; i += 2, to += 24) {
        const uint32_t *b = v + 4 * i, *c = b + 4, *d = i + 2 < n ? c + 4 : c;
        to[0] = to[12] = v[0], to[1] = to[13] = v[1], to[2] = to[14] = v[2], to[3] = to[15] = v[3];
        to[4] = b[0], to[5] = b[1], to[6] = b[2], to[7] = b[3];
        to[8] = to[16] = c[0], to[9] = to[17] = c[1], to[10] = to[18] = c[2], to[11] = to[19] = c[3];
        to[20] = d[0], to[21] = d[1], to[22] = d[2], to[23] = d[3];
    }
    qglQuadsUsed((n - 1) / 2);
}

// What Quake asked a draw to be made with: a kept command's words hold while it is the same.
static inline uint32_t asked(void) {
    return (uint32_t)want_texture | (uint32_t)want_blend << 1 | (uint32_t)want_alpha << 2 | (uint32_t)want_depth << 3 |
           (blend_source & 0xfff) << 4 | (blend_destination & 0xfff) << 16;
}

// Triangles of compact vertices that are in GPU memory already (a brush model's, kept by
// world_shaderemu.c), drawn with what is set now.
// `kept` is 24 words of the caller's for the command they were last drawn with (0 at first):
// drawn again with the same texture where it was, it is copied (qgpuReplay).
void qglTrianglesAt(const unsigned int* vertices, int count, unsigned int* kept) {
    const texture* t = &textures[bound];
    uint32_t tint = (uint32_t)colour[0] << 24 | (uint32_t)colour[1] << 16 | (uint32_t)colour[2] << 8 | colour[3];
    shade_flush();
    if (kept[0] == bound + 1 && kept[1] == t->at && kept[2] == uploads && kept[3] == asked() && kept[4] == tint &&
        kept[5] == (uint32_t)count && kept[6] == (uint32_t)(uintptr_t)vertices) {
        textures[bound].drawn = frame;
        qgpuReplay(kept + 8);
        return;
    }
    kept[0] = 0;
    state(0);
    unsigned before = qgpuCommands();
    seglCompactAt(vertices, count);
    if (qgpuCommands() != before + 1 || t->at == NOWHERE || !want_texture) return;
    kept[0] = bound + 1, kept[1] = t->at, kept[2] = uploads, kept[3] = asked(), kept[4] = tint;
    kept[5] = (uint32_t)count, kept[6] = (uint32_t)(uintptr_t)vertices;
    memcpy(kept + 8, qgpuLastCommand(), 64);
}

// Quads, four corners each: many polygons of one surface in one go (a draw costs some 200
// instructions before its first vertex).
void qglQuads(const unsigned int* vertices, int n) {
    shade_flush();
    state(0);
    seglCompact(GL_QUADS, vertices, n & ~3);
    frame_vertices += n;
}

// Where `bytes` of a model's are kept in the pool (a pose's vertices, or with pose COORDS its
// texture coordinates), or NULL when they cannot be; `fresh` when they are still to be written there.
#define COORDS 0x7fffffff
static uint32_t* pose_place(const void* model, int pose, uint32_t bytes, int* fresh, int* which) {
    int there, slot = pose_slot(model, pose, &there);
    if (slot < 0) return 0;
    *which = slot;
    texture* kept = &textures[POSE_FIRST + slot];
    *fresh = !there;
    if (!there) {
        if (kept->at != NOWHERE) pool_give(POSE_FIRST + slot);
        kept->kind = POSE, kept->bytes = bytes, kept->stays = 0;
        kept->at = pool_room(kept->bytes, POSE_FIRST + slot, 0);
        pose_of[slot].model = model, pose_of[slot].pose = pose;
        if (kept->at == NOWHERE) return 0;
    }
    kept->drawn = frame;
    return (uint32_t*)(pool + kept->at);
}

// The table of the light at each normal that `key` names: a colour a normal, for a model's
// shading. `fresh` when the caller is to fill it. NULL when this frame uses every table.
unsigned int* qglLights(int key, int* fresh) {
    uint32_t h = (uint32_t)key * 2654435761u >> 25;
    int free_one = -1;
    for (int k = 0; k < LIGHTS; k++) {
        int i = (int)((h + (uint32_t)k) % LIGHTS);
        if (light_of[i].key == key && light_of[i].drawn) {
            light_of[i].drawn = frame;
            *fresh = 0;
            return lights + 256 * i;
        }
        if (free_one < 0 && light_of[i].drawn < frame) free_one = i;
        if (!light_of[i].drawn || (k >= 7 && free_one >= 0)) break;
    }
    if (free_one < 0) return 0;
    light_of[free_one].key = key, light_of[free_one].drawn = frame;
    *fresh = 1;
    seglSettle();   // (the caller fills it, and the list being drawn may read it as it was)
    return lights + 256 * free_one;
}

static void pose_write(uint32_t* p, const unsigned char* v, const unsigned int* uv, int n) {
    for (int i = 0; i < n; i++, p += 4, v += 4)
        p[0] = byte_bits[v[0]], p[1] = byte_bits[v[1]], p[2] = byte_bits[v[2]], p[3] = uv[i] | (uint32_t)v[3] << 24;
}

// The two commands a model's pose was last drawn with, kept by model, pose and skin: drawn
// again they are copied, with the frame's matrices (qgpuReplay) and the light's table as it is
// now, while the skin and the pose are where they were in GPU memory.
typedef struct {
    const void* model;
    int pose, count, coords_slot, pose_slot, shaded;
    GLuint name;
    uint32_t skin_at, coords_at, pose_at, uploads, asked, skin[16], shade[16];
} drawn_model;
#define DRAWN 256
static drawn_model drawn[DRAWN];

// A model's pose: triangles of byte vertices (x, y, z, which normal) and their texture
// coordinates, packed already. Drawn with its skin, and with `light` (qglLights) once more as
// shading: the same vertices, copies in the pool of what the model's file has, coloured by
// their normals. (Without room in the pool they are written out into the frame.)
void qglModel(const void* model, int pose, const unsigned char* v, const unsigned int* uv, const unsigned int* light, int n) {
    int fresh = 0, coords_slot = 0, pose_slot = 0, kept_words = 0;
    drawn_model* d = &drawn[(((uint32_t)(uintptr_t)model >> 4) ^ ((uint32_t)pose * 2654435761u >> 8) ^ bound * 40503u) % DRAWN];
    shade_flush();
    if (d->model == model && d->pose == pose && d->name == bound && (d->shaded || !light) && d->count == n && d->uploads == uploads &&
        d->asked == asked() && textures[bound].at == d->skin_at &&
        pose_of[d->coords_slot].model == model && pose_of[d->coords_slot].pose == COORDS && textures[POSE_FIRST + d->coords_slot].at == d->coords_at &&
        pose_of[d->pose_slot].model == model && pose_of[d->pose_slot].pose == pose && textures[POSE_FIRST + d->pose_slot].at == d->pose_at) {
        textures[bound].drawn = textures[POSE_FIRST + d->coords_slot].drawn = textures[POSE_FIRST + d->pose_slot].drawn = frame;
        if (!qgpuReplay(d->skin) && light) {
            d->shade[11] = seglAddress(light);   // (the one word of it that is the light's)
            qgpuReplay(d->shade);
        }
        return;
    }
    d->model = 0, d->shaded = 0;
    state(1);
    uint32_t* coords = pose_place(model, COORDS, (uint32_t)n * 4, &fresh, &coords_slot);
    if (coords && fresh) seglSettle(), memcpy(coords, uv, (size_t)n * 4);
    uint32_t* kept = coords ? pose_place(model, pose, (uint32_t)n * 4, &fresh, &pose_slot) : 0;
    if (kept && fresh) seglSettle(), memcpy(kept, v, (size_t)n * 4), frame_vertices += n;
    uint32_t* p = 0;
    unsigned before = qgpuCommands();
    if (kept) {
        seglPacked(kept, coords, n, 0);
        if (qgpuCommands() == before + 1) memcpy(d->skin, qgpuLastCommand(), 64), kept_words = 1;
    } else if ((p = seglTagged(0, n, 0)) != 0) pose_write(p, v, uv, n), frame_vertices += n;
    if (light) {   // (lit all over, a flame, has no shading to draw)
        give(&given_texture, GL_TEXTURE_2D, 0);
        give(&given_blend, GL_BLEND, 1);
        give_blend(GL_ZERO, GL_SRC_COLOR);
        give(&given_alpha, GL_ALPHA_TEST, 0);
        told = -1;
        before = qgpuCommands();
        if (kept) {
            seglPacked(kept, coords, n, light);
            if (qgpuCommands() == before + 1) memcpy(d->shade, qgpuLastCommand(), 64), d->shaded = 1;
            else kept_words = 0;
        } else if ((p = seglTagged(0, n, light)) != 0) pose_write(p, v, uv, n), frame_vertices += n;
    }
    if (!kept_words || textures[bound].at == NOWHERE || !want_texture) return;
    d->model = model, d->pose = pose, d->count = n, d->name = bound, d->uploads = uploads, d->asked = asked();
    d->coords_slot = coords_slot, d->pose_slot = pose_slot, d->skin_at = textures[bound].at;
    d->coords_at = textures[POSE_FIRST + coords_slot].at, d->pose_at = textures[POSE_FIRST + pose_slot].at;
}

// The frame's first draws, which `key` names (the status bar as it is now), before anything
// else of the frame is drawn. With `end` 0: 1 when this set held the same two frames ago, the
// last time it was drawn into, and they are still there: nothing is to be drawn. Else the
// caller draws them and says so with `end` 1.
int qglHead(int end, unsigned int key) {
    head* h = &heads[seglSet() & 1];
    shade_flush();
    if (end) {
        if (!head_made) return 0;
        qgpuHeadEnd(h->kept);
        h->key = key, h->uploads = uploads;
        h->frame = h->names > HEAD_TEXTURES ? 0 : frame;
        head_made = 0;
        return 0;
    }
    if (h->frame + 2 == frame && h->key == key && h->uploads == uploads) {
        uint32_t i;
        for (i = 0; i < h->names && textures[h->name[i]].at == h->at[i]; i++) continue;
        if (i == h->names && qgpuHeadAgain(h->kept)) {
            for (i = 0; i < h->names; i++) textures[h->name[i]].drawn = frame;
            h->frame = frame;
            return 1;
        }
    }
    h->frame = 0, h->names = 0;
    head_made = h;
    qgpuHeadBegin(h->kept);
    return 0;
}

// ---- the window ----

const GLubyte* qglGetString(GLenum what) {
    return (const GLubyte*)(what == GL_VENDOR ? "ShaderEmu" : what == GL_RENDERER ? "the machine's GPU device" :
                            what == GL_VERSION ? "1.1 over gles.c" : "");
}

const volatile unsigned int* qglControl(int offset) {
    return (const volatile unsigned int*)((const char*)seglPalette() - REG_PALETTE + offset);
}

int qglOpen(unsigned int nano_x_window, const unsigned char* palette) {
    if (seglInit(nano_x_window) < 0) return -1;
    seglWorldUp(3);   // z
    unsigned int* words256 = seglPalette();
    for (int i = 0; i < 256; i++, palette += 3)
        words256[i] = (unsigned int)palette[0] << 16 | (unsigned int)palette[1] << 8 | palette[2];
    glColorKeySE(KEY);
    // the ramp a model's shading is looked up in, which stays; the rest is the textures' pool
    uint32_t* ramp = seglMemory(256 * 4);
    if (!ramp) return -1;
    for (uint32_t i = 0; i < 256; i++) ramp[i] = i * 0x010101u;
    glBindTexture(GL_TEXTURE_2D, ramp_name);
    seglTexturePointer(ramp, 256, 1, GL_RGBA);
    glBindTexture(GL_TEXTURE_2D, 0);
    for (int i = 0; i < MAX_TEXTURES; i++) textures[i].at = NOWHERE;
    lights = seglMemory(LIGHTS * 256 * 4);
    if (!lights) return -1;
    particle_memory = seglMemory(PARTICLE_MEMORY);
    stage_memory = seglMemory(STAGE_MEMORY);   // (the 2D screen made on the first core)   // (the workers' particles, drawn from where they are)
    // the pool: what seglMemory has, and before it the spare memory when there is some, with
    // what lies between them as a block that never leaves
    unsigned int spare_size, main_size = seglMemoryLeft() & ~15u;
    uint8_t* spare = seglMemorySpare(&spare_size);
    uint8_t* main_part = seglMemory(main_size);
    if (!main_part) return -1;
    pool = spare ? spare : main_part;
    pool_size = main_size + spare_size;
    block_count = 0;
    if (spare) {
        blocks[0].at = 0, blocks[0].size = spare_size, blocks[0].owner = -1;
        blocks[1].at = spare_size, blocks[1].size = (uint32_t)(main_part - spare) - spare_size, blocks[1].owner = GAP_OWNER;
        textures[GAP_OWNER].at = spare_size, textures[GAP_OWNER].stays = 1;
        block_count = 2;
    }
    blocks[block_count].at = (uint32_t)(main_part - pool), blocks[block_count].size = main_size, blocks[block_count].owner = -1;
    block_count++;
    // Every page of ours written once here: the device maps them clean, and a worker that first
    // writes one stops until this core has served it (the written bit, src/mmu.h): 344 in demo1.
    for (uint8_t* at = (uint8_t*)ramp; at < main_part + main_size; at += 4096)
        __asm__ volatile("amoor.w zero, zero, (%0)" : : "r"(at) : "memory");
    for (unsigned int at = 0; spare && at < spare_size; at += 4096)
        __asm__ volatile("amoor.w zero, zero, (%0)" : : "r"(spare + at) : "memory");
    for (int i = 0; i < 256; i++) byte_bits[i] = bits((float)i);
    for (uint32_t i = 0; i < 256; i++) qgl_particle_colour[i] = ((i & 15) * 64 + 32) | ((i >> 4) * 64 + 32) << 16;
    return 0;
}

// The list is submitted and not waited for: whatever writes GPU memory it may read (the pool,
// the light tables, the palette) settles first, and the game's next frame runs meanwhile.
// QUAKE_SWAP=wait waits, as it did.
static void swap_frame(int hand) {
    static int waits = -1;
    if (waits < 0) {
        const char* how = getenv("QUAKE_SWAP");
        waits = how && !strcmp(how, "wait");
    }
    shade_flush();
    if (hand) {
        seglSwapHand();
        if (waits) seglSettle();
    } else if (waits) seglSwap();
    else seglSwapEarly();
    last_vertices = frame_vertices;
    frame_vertices = 0;
    frame++;
    told = -1;
}
void qglSwap(void) { swap_frame(0); }
// A frame drawn on a worker core: the first core submits its list (seglSwapTake).
void qglSwapHand(void) { swap_frame(1); }
