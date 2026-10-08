// GLQuake's OpenGL on the machine's GPU (docs/quake.md): glBegin and its vertices turned into
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
static inline uint32_t coord_bits(float s, float t) {
    return ((uint32_t)(int32_t)(s * 1024.0f) & 0xffff) | (uint32_t)(int32_t)(t * 1024.0f) << 16;
}

// ---- textures ----

// GPU memory left to a program is under 3 MB and a level's textures with its models' skins and
// light maps are more. Every texture is kept in ordinary memory, as the GPU will read it, and
// GPU memory holds the ones being drawn: the one drawn longest ago makes room for a new one.
enum { NONE, INDEXED, WORDS, POSE };
#define NOWHERE 0xffffffffu
typedef struct { uint8_t* host; uint32_t at, bytes, drawn; int width, height, kind, holes, stays; } texture;
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

// Has the texture in GPU memory for the draw that follows. 0: it has no pixels, or everything
// in GPU memory is this frame's own and nothing may go.
static int resident(int name) {
    texture* t = &textures[name];
    uint32_t at;
    if (t->kind == NONE || t->kind == POSE) return 0;
    t->drawn = frame;
    if (t->at != NOWHERE) return 1;
    if ((at = pool_room(t->bytes, name, 0)) == NOWHERE) return 0;
    t->at = at;
    memcpy(pool + at, t->host, t->bytes);
    glBindTexture(GL_TEXTURE_2D, name);
    seglTexturePointer(pool + at, t->width, t->height, t->kind == INDEXED ? GL_COLOR_INDEX8_EXT : GL_RGBA);
    glBindTexture(GL_TEXTURE_2D, bound);
    return 1;
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
    return at == NOWHERE ? 0 : pool + at;
}
// The block is given back, no texture stays, and the poses of the level's models are forgotten.
void qglRelease(void) {
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

static void shade_flush(void);

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
    shade_flush();
    told = -1;
    bound = name < POSE_FIRST ? name : 0;
    glBindTexture(target, bound);
}

void qglTexImage8(const GLubyte* pixels, GLsizei width, GLsizei height, int holes) {
    texture* t = &textures[bound];
    uint8_t* to = bound ? texture_memory(width, height, INDEXED) : 0;
    if (!to) return;
    t->holes = holes;
    if (pixels) memcpy(to, pixels, t->bytes);
    else memset(to, 0, t->bytes);
    if (t->at != NOWHERE) memcpy(pool + t->at, to, t->bytes);
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
    if (!to) return;
    t->holes = 0;
    if (pixels) words(to, pixels, width * height, format);
    else memset(to, 0, t->bytes);
    if (t->at != NOWHERE) memcpy(pool + t->at, to, t->bytes);
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
        if (t->at != NOWHERE) memcpy(pool + t->at + at, t->host + at, (size_t)width * 4);
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
    if (w && !*w) shade_flush(), *w = 1, told = -1;
}
void qglDisable(GLenum what) {
    int* w = wanted(what);
    if (w && *w) shade_flush(), *w = 0, told = -1;
}
void qglBlendFunc(GLenum source, GLenum destination) {
    blend_source = source;
    blend_destination = destination;
    told = -1;
}

// Tells gles.c what the next draw is drawn with. Everything drawn without the depth test (the
// 2D screen) goes into the one blended pass, so that it keeps the order it is drawn in.
static void state(int white) {
    const texture* t = &textures[bound];
    if (told == white && (!want_texture || (t->at != NOWHERE && t->drawn == frame))) return;
    told = white;
    int textured = want_texture && resident(bound);
    (textured ? glEnable : glDisable)(GL_TEXTURE_2D);
    (want_depth ? glEnable : glDisable)(GL_DEPTH_TEST);
    (want_blend || !want_depth ? glEnable : glDisable)(GL_BLEND);
    if (!want_depth) glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    else if (blend_source == GL_ZERO && blend_destination == GL_ONE_MINUS_SRC_COLOR) glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    else glBlendFunc(blend_source, blend_destination);
    (textured && t->holes && (want_alpha || want_blend || !want_depth) ? glEnable : glDisable)(GL_ALPHA_TEST);
    if (white) glColor4ub(255, 255, 255, 255);
    else glColor4ub(colour[0], colour[1], colour[2], colour[3]);
}

void qglColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
    colour[0] = level(r), colour[1] = level(g), colour[2] = level(b), colour[3] = level(a);
    if (told == 0) told = -1;
    if (mode) shaded = 1;
}
void qglColor3f(GLfloat r, GLfloat g, GLfloat b) { qglColor4f(r, g, b, 1); }
void qglColor4fv(const GLfloat* v) { qglColor4f(v[0], v[1], v[2], v[3]); }
void qglColor3ubv(const GLubyte* v) {
    colour[0] = v[0], colour[1] = v[1], colour[2] = v[2], colour[3] = 255;
    if (told == 0) told = -1;
    if (mode) shaded = 1;
}

void qglClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { glClearColor(r, g, b, a); }
void qglClear(GLbitfield mask) { glClear(mask); }
void qglViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    shade_flush();
    glViewport(x, y, width, height);
}

// ---- matrices ----

void qglMatrixMode(GLenum which) {
    shade_flush();
    glMatrixMode(which);
}
void qglLoadIdentity(void) {
    shade_flush();
    glLoadIdentity();
}
void qglPushMatrix(void) { glPushMatrix(); }
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
}
void qglLoadMatrixf(const GLfloat* m) {
    shade_flush();
    seglSetMatrix(m);
}

// ---- drawing ----

// A model's shading: the triangles drawn once more, multiplying what is there by a ramp of
// greys looked up with the vertex's light. Kept until the state they were drawn in changes.
static void shade_flush(void) {
    if (!shade_count) return;
    glBindTexture(GL_TEXTURE_2D, ramp_name);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    glDisable(GL_ALPHA_TEST);
    glColor4ub(255, 255, 255, 255);
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
    mode = what == GL_POLYGON ? GL_TRIANGLE_FAN : what;
    count = shaded = 0;
}

void qglTexCoord2f(GLfloat s, GLfloat t) {
    coord_s = s;
    coord_t = t;
}

void qglVertex3f(GLfloat x, GLfloat y, GLfloat z) {
    if (count == MAX_PRIMITIVE) return;
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

void qglEnd(void) {
    int what = mode, lit;
    mode = 0;
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
    low_s = floorf(low_s), low_t = floorf(low_t);
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

// A polygon of the level, its vertices made when the level loaded (world_shaderemu.c).
void qglFan(const unsigned int* vertices, int n) {
    shade_flush();
    state(0);
    seglCompact(GL_TRIANGLE_FAN, vertices, n);
    frame_vertices += n;
}

// Where `bytes` of a model's are kept in the pool (a pose's vertices, or with pose COORDS its
// texture coordinates), or NULL when they cannot be; `fresh` when they are still to be written there.
#define COORDS 0x7fffffff
static uint32_t* pose_place(const void* model, int pose, uint32_t bytes, int* fresh) {
    int there, slot = pose_slot(model, pose, &there);
    if (slot < 0) return 0;
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
    return lights + 256 * free_one;
}

static void pose_write(uint32_t* p, const unsigned char* v, const unsigned int* uv, int n) {
    for (int i = 0; i < n; i++, p += 4, v += 4)
        p[0] = byte_bits[v[0]], p[1] = byte_bits[v[1]], p[2] = byte_bits[v[2]], p[3] = uv[i] | (uint32_t)v[3] << 24;
}

// A model's pose: triangles of byte vertices (x, y, z, which normal) and their texture
// coordinates, packed already. Drawn with its skin, and with `light` (qglLights) once more as
// shading: the same vertices, copies in the pool of what the model's file has, coloured by
// their normals. (Without room in the pool they are written out into the frame.)
void qglModel(const void* model, int pose, const unsigned char* v, const unsigned int* uv, const unsigned int* light, int n) {
    int fresh = 0;
    shade_flush();
    state(1);
    uint32_t* coords = pose_place(model, COORDS, (uint32_t)n * 4, &fresh);
    if (coords && fresh) memcpy(coords, uv, (size_t)n * 4);
    uint32_t* kept = coords ? pose_place(model, pose, (uint32_t)n * 4, &fresh) : 0;
    if (kept && fresh) memcpy(kept, v, (size_t)n * 4), frame_vertices += n;
    uint32_t* p = 0;
    if (kept) seglPacked(kept, coords, n, 0);
    else if ((p = seglTagged(0, n, 0)) != 0) pose_write(p, v, uv, n), frame_vertices += n;
    if (!light) return;   // lit all over (a flame): no shading to draw
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    glDisable(GL_ALPHA_TEST);
    told = -1;
    if (kept) seglPacked(kept, coords, n, light);
    else if ((p = seglTagged(0, n, light)) != 0) pose_write(p, v, uv, n), frame_vertices += n;
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
    for (int i = 0; i < 256; i++) byte_bits[i] = bits((float)i);
    return 0;
}

void qglSwap(void) {
    shade_flush();
    seglSwap();
    last_vertices = frame_vertices;
    frame_vertices = 0;
    frame++;
    told = -1;
}
