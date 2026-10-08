// A fixed-point OpenGL (include/GLES/gl.h) on the machine's GPU device, for Nano-X programs.
// Vertex arrays become compact vertices in GPU memory and one draw command per run of calls
// with the same state; blending and depth use pick the GPU pass (docs/gpu.md). Nothing is
// converted to floating point on the way: GLfixed is what the device reads.

#include <GLES/segl.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <nano-X.h>

// ---- GPU memory: the part Nano-X leaves to one program ----

#define GPU_PHYS 0x87000000u
#define GPU_SIZE 0x00b00000u
// A frame's commands, matrix blocks and vertices are a set; frames alternate between two sets,
// so the frame shown last stays whole while the next is built (the volume display draws it
// again and again from its own point of view, docs/volume.md).
#define SETS_AT 0x00700000u
#define SET_SIZE 0x00060000u
#define UNIFORMS_IN 0x00030000u    // within a set: its matrix blocks, eight vectors each
#define VERTICES_IN 0x00038000u    // and its vertices
#define TEXTURES_AT 0x007c0000u    // textures, to the end
#define MAX_COMMANDS 3072
#define MAX_BLOCKS 256
#define REG_VOLUME 0x300           // the last whole frame: list address, command count, frames so far
#define MAX_MESH 196608            // vertices the device's mesh has for one list
#define REG_PALETTE 0x400
#define REG_SUBMIT 0x10            // submit, list address, command count
#define REG_INTO 0x50              // where the picture is copied: address, width, height, row
#define REG_LOCK 0x60
#define REG_BUFFERS 0x64           // goes up when the window system gives a window another buffer
// The pause hint: this machine ends its frame there, which is when the GPU does its work.
#define next_frame() __asm__ volatile(".word 0x0100000f")

enum { CMD_CLEAR = 1, CMD_DRAW = 3 };
enum { VERTEX_CLIP = 1, VERTEX_MODELVIEW = 0x100, VERTEX_QUADS = 0x200, VERTEX_COMPACT = 0x400 };
enum { FRAGMENT_COLOUR, FRAGMENT_TEXTURE, FRAGMENT_INDEXED, FRAGMENT_KEYED = 0x100, FRAGMENT_LAID = 0x200 };

static uint8_t* gpu;   // GPU memory from GPU_PHYS
static GR_WINDOW_ID window;
static GR_WINDOW_INFO window_info;
static int swaps;
static uint32_t buffers_seen;

static uint32_t commands, blocks, mesh_vertices, vertex_top, texture_top = TEXTURES_AT, passes_used;
static uint32_t set_at = SETS_AT;   // the set the frame being built is in
static uint32_t* last_draw;   // the command the next vertices may join, with what it was made for
static uint32_t last_state[6];
static uint32_t kept, kept_mesh;   // commands at the head of every frame's list that the program wrote itself
static uint32_t shown_set, shown_commands;   // the list drawn last, for seglSwapAgain()

// ---- state ----

typedef struct { GLfixed r[4][4]; } matrix;   // rows: what the device takes dot products with
static matrix modelview[8], projection[4];
static int modelview_top, projection_top, matrix_mode = GL_MODELVIEW, matrices_dirty = 1;
static int view_x, view_y, view_w, view_h;

static uint32_t colour = 0x00ffffff, clear_colour;
static int texturing, depth_test, blending, alpha_test, blend_kind = 1, key_index;
typedef struct { uint32_t address, width, height, indexed, used; } texture;
static texture textures[512];
static uint32_t bound;

typedef struct { const uint8_t* pointer; int size, stride, enabled; } array;
static array vertex_array, coord_array;

static inline GLfixed mul(GLfixed a, GLfixed b) { return (GLfixed)(((int64_t)a * b) >> 16); }
static inline GLfixed quotient(GLfixed a, GLfixed b) { return (GLfixed)(((int64_t)a << 16) / b); }

static matrix* current(void) {
    return matrix_mode == GL_PROJECTION ? &projection[projection_top] : &modelview[modelview_top];
}

static const matrix identity = {{{65536, 0, 0, 0}, {0, 65536, 0, 0}, {0, 0, 65536, 0}, {0, 0, 0, 65536}}};

// current = current * m
static void multiply(const matrix* m) {
    matrix* c = current();
    matrix out;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out.r[i][j] = mul(c->r[i][0], m->r[0][j]) + mul(c->r[i][1], m->r[1][j]) + mul(c->r[i][2], m->r[2][j]) +
                          mul(c->r[i][3], m->r[3][j]);
    *c = out;
    matrices_dirty = 1;
}

// ---- window ----

int seglInit(unsigned int nano_x_window) {
    int fd = open("/dev/gpu", O_RDWR);
    if (fd < 0) return -1;
    void* map = mmap(0, GPU_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01000000);   // the device maps from 0x86000000
    if (map == MAP_FAILED) return -1;
    gpu = map;
    window = nano_x_window;
    GrGetWindowInfo(window, &window_info);
    modelview[0] = projection[0] = identity;
    view_w = window_info.width;
    view_h = window_info.height;
    vertex_top = set_at + VERTICES_IN;
    return 0;
}

void seglSize(int* width, int* height) {
    *width = window_info.width;
    *height = window_info.height;
}

void seglWindowChanged(void) {
    if (gpu) GrGetWindowInfo(window, &window_info);
}

void seglPicturePoint(int x, int y, int width, int height, int* px, int* py) {
    int w = window_info.width > 0 ? window_info.width : 1, h = window_info.height > 0 ? window_info.height : 1;
    x = x * width / w;
    y = y * height / h;
    *px = x < 0 ? 0 : x >= width ? width - 1 : x;
    *py = y < 0 ? 0 : y >= height ? height - 1 : y;
}

void* seglMemory(unsigned int bytes) {
    uint32_t at = texture_top;
    bytes = (bytes + 15) & ~15u;
    if (!gpu || at + bytes > GPU_SIZE) return 0;
    texture_top += bytes;
    return gpu + at;
}

unsigned int* seglPalette(void) { return (unsigned int*)(gpu + REG_PALETTE); }

void seglSwap(void) {
    volatile uint32_t* regs = (volatile uint32_t*)gpu;
    // the window has another buffer (it was resized): ask where and how large
    if (regs[REG_BUFFERS / 4] != buffers_seen || swaps++ == 0) {
        buffers_seen = regs[REG_BUFFERS / 4];
        GrGetWindowInfo(window, &window_info);
    }
    if (commands && window_info.realized && window_info.surface_address) {
        for (;;) {
            while (__atomic_exchange_n(&regs[REG_LOCK / 4], 1, __ATOMIC_ACQUIRE)) sched_yield();
            if (regs[REG_SUBMIT / 4] == 0) break;
            __atomic_store_n(&regs[REG_LOCK / 4], 0, __ATOMIC_RELEASE);
            next_frame();
        }
        regs[REG_INTO / 4] = window_info.surface_address;
        regs[REG_INTO / 4 + 1] = window_info.width;
        regs[REG_INTO / 4 + 2] = window_info.height;
        regs[REG_INTO / 4 + 3] = window_info.surface_row;
        regs[REG_SUBMIT / 4 + 1] = GPU_PHYS + set_at;
        regs[REG_SUBMIT / 4 + 2] = commands;
        regs[REG_SUBMIT / 4] = 1 | 4 | passes_used << 8;   // draw, copy into the window; the passes used
        shown_set = set_at;
        shown_commands = commands | passes_used << 16;
        __atomic_store_n(&regs[REG_LOCK / 4], 0, __ATOMIC_RELEASE);
        // the list and the textures are memory the program goes on to change: wait until drawn
        while (regs[REG_SUBMIT / 4] != 0) next_frame();
        regs[REG_VOLUME / 4 + 1] = commands;
        regs[REG_VOLUME / 4] = GPU_PHYS + set_at;
        regs[REG_VOLUME / 4 + 2]++;
        set_at = set_at == SETS_AT ? SETS_AT + SET_SIZE : SETS_AT;
    }
    commands = kept;
    mesh_vertices = kept_mesh;
    blocks = passes_used = 0;
    last_draw = 0;
    vertex_top = set_at + VERTICES_IN;
    matrices_dirty = 1;
}

// ---- frame ----

static uint32_t* command(uint32_t op, uint32_t mesh) {
    if (commands == MAX_COMMANDS || mesh_vertices + mesh > MAX_MESH) return 0;
    uint32_t* c = (uint32_t*)(gpu + set_at) + 16 * commands++;
    c[0] = op;
    c[3] = mesh_vertices;
    mesh_vertices += mesh;
    return c;
}

void glClearColorx(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha) {
    (void)alpha;
    clear_colour = (uint32_t)(red >> 8 > 255 ? 255 : red >> 8) << 16 | (uint32_t)(green >> 8 > 255 ? 255 : green >> 8) << 8 |
                   (uint32_t)(blue >> 8 > 255 ? 255 : blue >> 8);
}

// The device starts every list with a fresh depth buffer; only the colour needs a command.
void glClear(GLbitfield mask) {
    if (!gpu || !(mask & GL_COLOR_BUFFER_BIT)) return;
    uint32_t* c = command(CMD_CLEAR, 3);
    if (c) c[1] = clear_colour;
    last_draw = 0;
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    view_x = x;
    view_y = y;
    view_w = width;
    view_h = height;
    matrices_dirty = 1;
}

// ---- matrices ----

void glMatrixMode(GLenum mode) { matrix_mode = mode; }
void glLoadIdentity(void) {
    *current() = identity;
    matrices_dirty = 1;
}
static void from_columns(matrix* out, const GLfixed* m) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out->r[i][j] = m[j * 4 + i];
}
void glLoadMatrixx(const GLfixed* m) {
    from_columns(current(), m);
    matrices_dirty = 1;
}
void glMultMatrixx(const GLfixed* m) {
    matrix t;
    from_columns(&t, m);
    multiply(&t);
}
void glPushMatrix(void) {
    if (matrix_mode == GL_PROJECTION) {
        if (projection_top < 3) projection[projection_top + 1] = projection[projection_top], projection_top++;
    } else if (modelview_top < 7) {
        modelview[modelview_top + 1] = modelview[modelview_top], modelview_top++;
    }
}
void glPopMatrix(void) {
    if (matrix_mode == GL_PROJECTION) {
        if (projection_top > 0) projection_top--;
    } else if (modelview_top > 0) {
        modelview_top--;
    }
    matrices_dirty = 1;
}
void glFrustumx(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f) {
    matrix m = {{{quotient(2 * n, r - l), 0, quotient(r + l, r - l), 0},
                 {0, quotient(2 * n, t - b), quotient(t + b, t - b), 0},
                 {0, 0, -quotient(f + n, f - n), -quotient(mul(2 * f, n), f - n)},
                 {0, 0, -65536, 0}}};
    multiply(&m);
}
void glOrthox(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f) {
    matrix m = {{{quotient(2 * 65536, r - l), 0, 0, -quotient(r + l, r - l)},
                 {0, quotient(2 * 65536, t - b), 0, -quotient(t + b, t - b)},
                 {0, 0, -quotient(2 * 65536, f - n), -quotient(f + n, f - n)},
                 {0, 0, 0, 65536}}};
    multiply(&m);
}
void glTranslatex(GLfixed x, GLfixed y, GLfixed z) {
    matrix m = identity;
    m.r[0][3] = x;
    m.r[1][3] = y;
    m.r[2][3] = z;
    multiply(&m);
}
void glScalex(GLfixed x, GLfixed y, GLfixed z) {
    matrix m = identity;
    m.r[0][0] = x;
    m.r[1][1] = y;
    m.r[2][2] = z;
    multiply(&m);
}
// Sine of an angle in degrees (16.16), by a polynomial on the first quarter turn.
static GLfixed sine(GLfixed degrees) {
    int64_t d = degrees % (360 * 65536);
    if (d < 0) d += 360 * 65536;
    int negative = d >= 180 * 65536;
    if (negative) d -= 180 * 65536;
    if (d > 90 * 65536) d = 180 * 65536 - d;
    GLfixed x = (GLfixed)(d * 1144 >> 16);   // radians: pi/180 is 1144/65536
    GLfixed x2 = mul(x, x);
    GLfixed s = mul(x, 65536 - mul(x2, 10923 - mul(x2, 546 - mul(x2, 13))));   // 1/6, 1/120, 1/5040
    return negative ? -s : s;
}
void glRotatex(GLfixed degrees, GLfixed x, GLfixed y, GLfixed z) {
    GLfixed s = sine(degrees), c = sine(degrees + 90 * 65536);
    matrix m = identity;
    if (x && !y && !z) {
        if (x < 0) s = -s;
        m.r[1][1] = c, m.r[1][2] = -s, m.r[2][1] = s, m.r[2][2] = c;
    } else if (y && !x && !z) {
        if (y < 0) s = -s;
        m.r[0][0] = c, m.r[0][2] = s, m.r[2][0] = -s, m.r[2][2] = c;
    } else if (z && !x && !y) {
        if (z < 0) s = -s;
        m.r[0][0] = c, m.r[0][1] = -s, m.r[1][0] = s, m.r[1][1] = c;
    } else {
        return;
    }
    multiply(&m);
}

// ---- state ----

static int* flag(GLenum what) {
    switch (what) {
        case GL_TEXTURE_2D: return &texturing;
        case GL_DEPTH_TEST: return &depth_test;
        case GL_BLEND: return &blending;
        case GL_ALPHA_TEST: return &alpha_test;
    }
    return 0;
}
void glEnable(GLenum what) {
    int* f = flag(what);
    if (f) *f = 1;
}
void glDisable(GLenum what) {
    int* f = flag(what);
    if (f) *f = 0;
}
void glBlendFunc(GLenum source, GLenum destination) {
    if (destination == GL_ONE_MINUS_SRC_ALPHA) blend_kind = 1;
    else if (destination == GL_ONE) blend_kind = 2;
    else if (source == GL_DST_COLOR || destination == GL_SRC_COLOR) blend_kind = 3;
}
void glColorKeySE(GLint index) { key_index = index; }

static uint32_t byte_of(GLfixed v) { return v <= 0 ? 0 : v >= 65536 ? 255 : (uint32_t)v >> 8; }
void glColor4x(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha) {
    colour = (255 - byte_of(alpha)) << 24 | byte_of(red) << 16 | byte_of(green) << 8 | byte_of(blue);
}
void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha) {
    colour = (uint32_t)(255 - alpha) << 24 | (uint32_t)red << 16 | (uint32_t)green << 8 | blue;
}

static array* array_of(GLenum which) {
    return which == GL_VERTEX_ARRAY ? &vertex_array : which == GL_TEXTURE_COORD_ARRAY ? &coord_array : 0;
}
void glEnableClientState(GLenum which) {
    array* a = array_of(which);
    if (a) a->enabled = 1;
}
void glDisableClientState(GLenum which) {
    array* a = array_of(which);
    if (a) a->enabled = 0;
}
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    (void)type;
    vertex_array.pointer = pointer, vertex_array.size = size, vertex_array.stride = stride ? stride : size * 4;
}
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    (void)type;
    coord_array.pointer = pointer, coord_array.size = size, coord_array.stride = stride ? stride : size * 4;
}

// ---- textures ----

void glGenTextures(GLsizei n, GLuint* out) {
    for (uint32_t t = 1; t < 512 && n > 0; t++)
        if (!textures[t].used) {
            textures[t].used = 1;
            textures[t].address = 0;
            *out++ = t;
            n--;
        }
    while (n-- > 0) *out++ = 0;
}
// Texture memory is taken in order and given back only from the end: enough for a program
// that drops all of a level's textures together.
void glDeleteTextures(GLsizei n, const GLuint* which) {
    for (int i = 0; i < n; i++)
        if (which[i] < 512) textures[which[i]].used = 0;
    uint32_t top = TEXTURES_AT;
    for (uint32_t t = 1; t < 512; t++) {
        texture* x = &textures[t];
        uint32_t bytes = x->indexed ? x->width * x->height : x->width * x->height * 4;
        if (x->used && x->address >= TEXTURES_AT && x->address + bytes > top) top = (x->address + bytes + 15) & ~15u;
    }
    if (top < texture_top) texture_top = top;
}
void glBindTexture(GLenum target, GLuint which) {
    (void)target;
    bound = which < 512 ? which : 0;
}
void seglTexturePointer(const void* pixels, GLsizei width, GLsizei height, GLenum internal) {
    texture* x = &textures[bound];
    x->address = (uint32_t)((const uint8_t*)pixels - gpu);
    x->width = width;
    x->height = height;
    x->indexed = internal == GL_COLOR_INDEX8_EXT ? 1 : internal == SEGL_BITS ? 2 : 0;
}
void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border, GLenum format,
                  GLenum type, const GLvoid* pixels) {
    (void)target, (void)border, (void)format, (void)type;
    if (!gpu || level != 0 || !bound) return;
    int indexed = internal == GL_COLOR_INDEX8_EXT;
    uint8_t* to = seglMemory(indexed ? width * height : width * height * 4);
    if (!to) return;
    if (indexed) {
        if (pixels) memcpy(to, pixels, width * height);
    } else if (pixels) {
        const uint8_t* p = pixels;
        uint32_t* w = (uint32_t*)to;
        for (int i = 0; i < width * height; i++, p += 4)
            w[i] = (uint32_t)(255 - p[3]) << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
    seglTexturePointer(to, width, height, internal);
}
void glColorTableEXT(GLenum target, GLenum internal, GLsizei count, GLenum format, GLenum type, const GLvoid* table) {
    (void)target, (void)internal, (void)format, (void)type;
    const uint8_t* p = table;
    uint32_t* words = seglPalette();
    for (int i = 0; i < count && i < 256; i++, p += 3) words[i] = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

// ---- drawing ----

// The block of vectors a draw reads its matrices from: the projection with the viewport folded
// in (rows 0-3), then the modelview (rows 4-6), which the device applies first.
static void matrix_block_write(void) {
    if (matrices_dirty && blocks < MAX_BLOCKS) {
        GLfixed* u = (GLfixed*)(gpu + set_at + UNIFORMS_IN) + 32 * blocks++;
        const matrix* p = &projection[projection_top];
        const matrix* m = &modelview[modelview_top];
        int w = window_info.width ? window_info.width : 1, h = window_info.height ? window_info.height : 1;
        // (a window is far below 32,768 pixels: these fit a 32-bit division, which this machine has)
        GLfixed sx = (view_w << 16) / w, ox = ((2 * view_x + view_w) << 16) / w - 65536;
        GLfixed sy = (view_h << 16) / h, oy = ((2 * view_y + view_h) << 16) / h - 65536;
        for (int j = 0; j < 4; j++) {
            u[j] = mul(sx, p->r[0][j]) + mul(ox, p->r[3][j]);
            u[4 + j] = mul(sy, p->r[1][j]) + mul(oy, p->r[3][j]);
            u[8 + j] = p->r[2][j];
            u[12 + j] = p->r[3][j];
            u[16 + j] = m->r[0][j];
            u[20 + j] = m->r[1][j];
            u[24 + j] = m->r[2][j];
        }
        matrices_dirty = 0;
    }
}
static uint32_t matrix_block(void) {
    if (blocks < (kept ? 1u : 0u)) blocks = 1, matrices_dirty = 1;   // block 0 is the kept commands' (seglKeptMatrices)
    matrix_block_write();
    return GPU_PHYS + set_at + UNIFORMS_IN + 128 * (blocks ? blocks - 1 : 0);
}

// The arrays a draw reads, copied out of the state once: a store to GPU memory cannot change these.
typedef struct { const uint8_t *positions, *coords; size_t position_step, coord_step; int has_z; } arrays;

// One compact vertex (a texel): position, and the texture coordinates in 1/1024ths.
static inline void put(uint32_t* restrict to, const arrays* a, int index) {
    const GLfixed* v = (const GLfixed*)(a->positions + (size_t)index * a->position_step);
    uint32_t uv = 0;
    if (a->coords) {
        const GLfixed* t = (const GLfixed*)(a->coords + (size_t)index * a->coord_step);
        uv = ((uint32_t)(t[0] >> 6) & 0xffff) | (uint32_t)(t[1] >> 6) << 16;
    }
    to[0] = v[0];
    to[1] = v[1];
    to[2] = a->has_z ? v[2] : 0;
    to[3] = uv;
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (!gpu || !vertex_array.enabled || count < 3) return;
    const texture* x = &textures[texturing ? bound : 0];
    uint32_t pass = (blending ? blend_kind : 0) + (depth_test ? 0 : 4);
    uint32_t fragment = (texturing && x->address ? (x->indexed ? FRAGMENT_INDEXED : FRAGMENT_TEXTURE) : FRAGMENT_COLOUR) | pass << 16;
    if (alpha_test && x->indexed && texturing) fragment |= FRAGMENT_KEYED;
    // a fan goes in as quads, two of its triangles each (the last may have its fourth corner twice)
    int fan = mode == GL_TRIANGLE_FAN, quads = mode == GL_QUADS || fan;
    uint32_t stored = fan ? ((uint32_t)count - 1) / 2 * 4 : quads ? (uint32_t)count & ~3u :
                      mode == GL_TRIANGLES ? (uint32_t)count - count % 3 : ((uint32_t)count - 2) * 3;
    uint32_t mesh = quads ? stored / 4 * 6 : stored;
    uint32_t vertex = VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_COMPACT | (quads ? VERTEX_QUADS : 0);
    if (vertex_top + stored * 16 > set_at + SET_SIZE || mesh_vertices + mesh > MAX_MESH) return;
    uint32_t block = matrix_block();
    uint32_t state[6] = {vertex, fragment, block, x->address, colour, (uint32_t)key_index};
    uint32_t* to = (uint32_t*)(gpu + vertex_top);

    // more of the same joins the command before: one command, and one draw, per run of state
    int same = 1;
    for (int i = 0; i < 6; i++) same &= state[i] == last_state[i];   // (the library's memcmp goes a byte at a time)
    if (last_draw && last_draw == (uint32_t*)(gpu + set_at) + 16 * (commands - 1) && same) {
        last_draw[2] += mesh;
        mesh_vertices += mesh;
    } else {
        uint32_t* c = command(CMD_DRAW, mesh);
        if (!c) return;
        c[1] = GPU_PHYS + vertex_top;
        c[2] = mesh;
        c[4] = vertex;
        c[5] = fragment;
        c[6] = block;
        c[7] = GPU_PHYS + x->address;
        c[8] = x->width;
        c[9] = x->height;
        c[10] = key_index;
        c[11] = colour;
        last_draw = c;
        for (int i = 0; i < 6; i++) last_state[i] = state[i];
        passes_used |= (1u << pass) & 0xfe;
    }
    const arrays a = {vertex_array.pointer, coord_array.enabled ? coord_array.pointer : 0, (size_t)vertex_array.stride,
                      (size_t)coord_array.stride, vertex_array.size > 2};
    if (fan) {
        for (int i = 1; i + 1 < count; i += 2, to += 16) {
            int last = i + 2 < count ? i + 2 : count - 1;
            put(to, &a, first), put(to + 4, &a, first + i), put(to + 8, &a, first + i + 1), put(to + 12, &a, first + last);
        }
    } else if (mode == GL_TRIANGLE_STRIP) {
        for (int i = 0; i + 2 < count; i++, to += 12)
            put(to, &a, first + i + (i & 1)), put(to + 4, &a, first + i + 1 - (i & 1)), put(to + 8, &a, first + i + 2);
    } else {
        for (uint32_t i = 0; i < stored; i++, to += 4) put(to, &a, first + i);
    }
    vertex_top += stored * 16;
}

// The window's own pixels as a texture: the picture the GPU drew last, where the server keeps it.
int seglWindowTexture(GLuint name, GLfixed* across) {
    if (!window_info.surface_address) return -1;
    texture* x = &textures[name < 512 ? name : 0];
    uint32_t row = window_info.surface_row ? window_info.surface_row : (uint32_t)window_info.width;
    x->address = window_info.surface_address - GPU_PHYS;   // (below this program's own memory: the sum wraps back)
    x->width = row;
    x->height = window_info.height;
    x->indexed = 0;
    *across = (GLfixed)(((uint32_t)window_info.width << 16) / (row ? row : 1));
    return 0;
}

// One quad of a texture, with everything it needs in the call (segl.h): what
// glDrawArrays does for four corners, without the state calls before it.
void seglQuad(const GLfixed* xyz, const GLfixed* uv, GLuint name, unsigned grey, int alpha, int keyed) {
    const texture* x = &textures[name < 512 ? name : 0];
    uint32_t pass = (alpha < 255 ? 1u : 0u) + (depth_test ? 0 : 4);
    uint32_t fragment = (x->indexed ? FRAGMENT_INDEXED : FRAGMENT_TEXTURE) | (keyed ? FRAGMENT_KEYED : 0) | pass << 16;
    uint32_t vertex = VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_COMPACT | VERTEX_QUADS;
    uint32_t tint = (uint32_t)(255 - alpha) << 24 | grey << 16 | grey << 8 | grey;
    if (!gpu || vertex_top + 64 > set_at + SET_SIZE || mesh_vertices + 6 > MAX_MESH) return;
    uint32_t block = matrices_dirty || !blocks ? matrix_block() : GPU_PHYS + set_at + UNIFORMS_IN + 128 * (blocks - 1);
    uint32_t* restrict to = (uint32_t*)(gpu + vertex_top);
    if (last_draw && last_draw == (uint32_t*)(gpu + set_at) + 16 * (commands - 1) && last_state[0] == vertex &&
        last_state[1] == fragment && last_state[2] == block && last_state[3] == x->address && last_state[4] == tint &&
        last_state[5] == (uint32_t)key_index) {
        last_draw[2] += 6;
        mesh_vertices += 6;
    } else {
        uint32_t* c = command(CMD_DRAW, 6);
        if (!c) return;
        c[1] = GPU_PHYS + vertex_top;
        c[2] = 6;
        c[4] = last_state[0] = vertex;
        c[5] = last_state[1] = fragment;
        c[6] = last_state[2] = block;
        c[7] = GPU_PHYS + x->address;
        last_state[3] = x->address;
        c[8] = x->width;
        c[9] = x->height;
        c[10] = last_state[5] = key_index;
        c[11] = last_state[4] = tint;
        last_draw = c;
        passes_used |= (1u << pass) & 0xfe;
    }
    for (int i = 0; i < 4; i++, to += 4, xyz += 3, uv += 2) {
        to[0] = xyz[0];
        to[1] = xyz[1];
        to[2] = xyz[2];
        to[3] = ((uint32_t)(uv[0] >> 6) & 0xffff) | (uint32_t)(uv[1] >> 6) << 16;
    }
    vertex_top += 64;
}

// A rectangle of a texture on top of what the list drew before it (segl.h): always the blended
// pass with no depth, so that opaque sprites and their shadows keep the order they come in.
static inline void sprite_corners(uint32_t* restrict to, const GLfixed* box, const int* texels) {
    uint32_t u0 = (uint32_t)texels[0] & 0xffff, u1 = (uint32_t)texels[2] & 0xffff;
    uint32_t v0 = (uint32_t)texels[1] << 16, v1 = (uint32_t)texels[3] << 16;
    to[0] = box[0], to[1] = box[1], to[2] = 0, to[3] = u0 | v0;
    to[4] = box[2], to[5] = box[1], to[6] = 0, to[7] = u1 | v0;
    to[8] = box[2], to[9] = box[3], to[10] = 0, to[11] = u1 | v1;
    to[12] = box[0], to[13] = box[3], to[14] = 0, to[15] = u0 | v1;
}

static void sprite_command(const GLfixed* box, const int* texels, GLuint name, unsigned colour, int keyed);

void seglSprite(const GLfixed* box, const int* texels, GLuint name, unsigned colour, int keyed) {
    const texture* x = &textures[name < 512 ? name : 0];
    uint32_t fragment = (x->indexed == 2 ? 3u : x->indexed ? FRAGMENT_INDEXED : FRAGMENT_TEXTURE) |
                        (keyed ? FRAGMENT_KEYED : 0) | 5u << 16;
    // a quad more of the command before, which is most of them: nothing is called on this path
    if (last_draw && last_draw == (uint32_t*)(gpu + set_at) + 16 * (commands - 1) && !matrices_dirty && blocks &&
        last_state[0] == (VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_COMPACT | VERTEX_QUADS) && last_state[1] == fragment &&
        last_state[2] == GPU_PHYS + set_at + UNIFORMS_IN + 128 * (blocks - 1) && last_state[3] == x->address &&
        last_state[4] == colour && last_state[5] == (uint32_t)key_index &&
        vertex_top + 64 <= set_at + SET_SIZE && mesh_vertices + 6 <= MAX_MESH) {
        last_draw[2] += 6;
        mesh_vertices += 6;
        sprite_corners((uint32_t*)(gpu + vertex_top), box, texels);
        vertex_top += 64;
        return;
    }
    sprite_command(box, texels, name, colour, keyed);
}

static __attribute__((noinline)) void sprite_command(const GLfixed* box, const int* texels, GLuint name, unsigned colour,
                                                     int keyed) {
    const texture* x = &textures[name < 512 ? name : 0];
    uint32_t fragment = (x->indexed == 2 ? 3u : x->indexed ? FRAGMENT_INDEXED : FRAGMENT_TEXTURE) |
                        (keyed ? FRAGMENT_KEYED : 0) | 5u << 16;
    uint32_t vertex = VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_COMPACT | VERTEX_QUADS;
    if (!gpu || vertex_top + 64 > set_at + SET_SIZE || mesh_vertices + 6 > MAX_MESH) return;
    uint32_t block = matrices_dirty || !blocks ? matrix_block() : GPU_PHYS + set_at + UNIFORMS_IN + 128 * (blocks - 1);
    uint32_t* restrict to = (uint32_t*)(gpu + vertex_top);
    if (last_draw && last_draw == (uint32_t*)(gpu + set_at) + 16 * (commands - 1) && last_state[0] == vertex &&
        last_state[1] == fragment && last_state[2] == block && last_state[3] == x->address && last_state[4] == colour &&
        last_state[5] == (uint32_t)key_index) {
        last_draw[2] += 6;
        mesh_vertices += 6;
    } else {
        uint32_t* c = command(CMD_DRAW, 6);
        if (!c) return;
        c[1] = GPU_PHYS + vertex_top;
        c[2] = 6;
        c[4] = last_state[0] = vertex;
        c[5] = last_state[1] = fragment;
        c[6] = last_state[2] = block;
        c[7] = GPU_PHYS + x->address;
        last_state[3] = x->address;
        c[8] = x->width;
        c[9] = x->height;
        c[10] = last_state[5] = key_index;
        c[11] = last_state[4] = colour;
        last_draw = c;
        passes_used |= 1u << 5;
    }
    sprite_corners(to, box, texels);
    vertex_top += 64;
}

uint32_t* seglLastCommand(void) { return last_draw; }
uint32_t* seglLastVertices(void) { return (uint32_t*)(gpu + vertex_top - 64); }
uint32_t seglAddress(const void* memory) { return GPU_PHYS + (uint32_t)((const uint8_t*)memory - gpu); }

// Forgets what was drawn since the last swap.
void seglDiscard(void) {
    commands = kept;
    mesh_vertices = kept_mesh;
    blocks = passes_used = 0;
    last_draw = 0;
    vertex_top = set_at + VERTICES_IN;
    matrices_dirty = 1;
}

// Has the list drawn last drawn once more, as its memory is now: its textures are the
// program's memory, and so are the commands and vertices seglLast...() pointed at.
void seglSwapAgain(void) {
    volatile uint32_t* regs = (volatile uint32_t*)gpu;
    if (!gpu || !shown_commands || !window_info.realized || !window_info.surface_address) return;
    if (regs[REG_BUFFERS / 4] != buffers_seen) {
        buffers_seen = regs[REG_BUFFERS / 4];
        GrGetWindowInfo(window, &window_info);
    }
    for (;;) {
        while (__atomic_exchange_n(&regs[REG_LOCK / 4], 1, __ATOMIC_ACQUIRE)) sched_yield();
        if (regs[REG_SUBMIT / 4] == 0) break;
        __atomic_store_n(&regs[REG_LOCK / 4], 0, __ATOMIC_RELEASE);
        next_frame();
    }
    regs[REG_INTO / 4] = window_info.surface_address;
    regs[REG_INTO / 4 + 1] = window_info.width;
    regs[REG_INTO / 4 + 2] = window_info.height;
    regs[REG_INTO / 4 + 3] = window_info.surface_row;
    regs[REG_SUBMIT / 4 + 1] = GPU_PHYS + shown_set;
    regs[REG_SUBMIT / 4 + 2] = shown_commands & 0xffff;
    regs[REG_SUBMIT / 4] = 1 | 4 | (shown_commands >> 16) << 8;
    __atomic_store_n(&regs[REG_LOCK / 4], 0, __ATOMIC_RELEASE);
    while (regs[REG_SUBMIT / 4] != 0) next_frame();
}

// ---- commands that stay (segl.h) ----

// Both sets hold them: a frame is built in one while the other is still being shown.
static uint32_t* kept_command(int set, int index) { return (uint32_t*)(gpu + SETS_AT + set * SET_SIZE) + 16 * index; }

int seglKeep(int count, int quads) {
    if (!gpu || count < 0 || count > MAX_COMMANDS - 64 || (uint32_t)quads * 6 > MAX_MESH - 4096) return -1;
    kept = count;
    kept_mesh = (uint32_t)quads * 6;
    commands = kept;   // (the frame being built starts again: call this before drawing in it)
    mesh_vertices = kept_mesh;
    last_draw = 0;
    blocks = 0;
    return 0;
}
void seglKeptQuads(int index, const void* vertices, int first_quad, int quads, GLuint name, unsigned grey, int keyed) {
    for (int set = 0; set < 2; set++) {
        uint32_t* c = kept_command(set, index);
        const texture* t = &textures[name < 512 ? name : 0];
        c[0] = CMD_DRAW;
        c[1] = GPU_PHYS + (uint32_t)((const uint8_t*)vertices - gpu) + (uint32_t)first_quad * 64;   // quad 0 is at `vertices`
        c[2] = (uint32_t)quads * 6;
        c[3] = (uint32_t)first_quad * 6;
        c[4] = VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_COMPACT | VERTEX_QUADS;
        c[5] = FRAGMENT_INDEXED | (keyed ? FRAGMENT_KEYED : 0);
        c[6] = GPU_PHYS + SETS_AT + set * SET_SIZE + UNIFORMS_IN;
        c[7] = GPU_PHYS + t->address;
        c[8] = t->width;
        c[9] = t->height;
        c[10] = key_index;
        c[11] = grey << 16 | grey << 8 | grey;
    }
}
void seglKeptShown(int index, int quads) {
    for (int set = 0; set < 2; set++) kept_command(set, index)[2] = (uint32_t)quads * 6;
}
int seglSet(void) { return set_at != SETS_AT; }
void seglKeptGreyIn(int set, int index, unsigned grey) { kept_command(set, index)[11] = grey << 16 | grey << 8 | grey; }
void seglKeptGrey(int index, unsigned grey) {
    for (int set = 0; set < 2; set++) kept_command(set, index)[11] = grey << 16 | grey << 8 | grey;
}
void seglKeptTexture(int index, GLuint name) {
    const texture* t = &textures[name < 512 ? name : 0];
    for (int set = 0; set < 2; set++) {
        uint32_t* c = kept_command(set, index);
        c[7] = GPU_PHYS + t->address;
        c[8] = t->width;
        c[9] = t->height;
    }
}
void seglKeptLaid(int index, GLfixed u, GLfixed v, GLfixed du, GLfixed dv) {
    for (int set = 0; set < 2; set++) {
        uint32_t* c = kept_command(set, index);
        c[5] |= FRAGMENT_LAID;
        c[12] = u, c[13] = v, c[14] = du, c[15] = dv;
    }
}
void seglKeptMatrices(void) {
    if (!gpu || !kept) return;
    blocks = 0;
    matrices_dirty = 1;
    matrix_block_write();
}

void glFlush(void) {}
void glFinish(void) {}
GLenum glGetError(void) { return GL_NO_ERROR; }
