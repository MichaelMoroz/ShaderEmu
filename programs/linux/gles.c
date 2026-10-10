// An OpenGL in the manner of OpenGL ES 1.x (include/GLES/gl.h) on the machine's GPU device, for
// Nano-X programs: vertex arrays become compact vertices in GPU memory, one draw command per
// run of calls with the same state (docs/gpu.md). Numbers inside are 16.16 fixed point, or
// floats when built with SEGL_FLOAT; either build takes both kinds of call and array.

#include <GLES/segl.h>
#include <fcntl.h>
#include <sched.h>
#include <math.h>
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
// A program with larger frames builds this file with its own split (linux/quake/build.sh).
#ifndef SET_SIZE
#define SET_SIZE 0x00060000u
#define UNIFORMS_IN 0x00030000u    // within a set: its matrix blocks, eight vectors each
#define VERTICES_IN 0x00038000u    // and its vertices
#define MAX_COMMANDS 3072
#endif
#define TEXTURES_AT (SETS_AT + 2 * SET_SIZE)   // textures, to the end
#define MAX_BLOCKS 256
#ifndef MAX_TEXTURES
#define MAX_TEXTURES 512
#endif
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
enum { VERTEX_CLIP = 1, VERTEX_MODELVIEW = 0x100, VERTEX_QUADS = 0x200, VERTEX_COMPACT = 0x400, VERTEX_FLOAT = 0x800,
       VERTEX_TAGGED = 0x1000, VERTEX_TABLE = 0x2000, VERTEX_PACKED = 0x4000, VERTEX_POINTS = 0x8000 };

// A number as this build keeps it, and as the device is told to read it.
#ifdef SEGL_FLOAT
typedef float num;
#define NUM_ONE 1.0f
#define NUM_FORMAT VERTEX_FLOAT
#define FROM_FIXED(x) ((float)(x) * (1.0f / 65536))
#define FROM_FLOAT(f) (f)
#define TO_1024THS(n) ((uint32_t)(int32_t)((n) * 1024.0f))
#else
typedef GLfixed num;
#define NUM_ONE 65536
#define NUM_FORMAT 0
#define FROM_FIXED(x) (x)
#define FROM_FLOAT(f) ((GLfixed)((f) * 65536.0f))
#define TO_1024THS(n) ((uint32_t)((n) >> 6))
#endif
// The words of a number, for memory the device reads.
static inline uint32_t bits_of(num n) {
    union { num n; uint32_t u; } c = {n};
    return c.u;
}
enum { FRAGMENT_COLOUR, FRAGMENT_TEXTURE, FRAGMENT_INDEXED, FRAGMENT_KEYED = 0x100, FRAGMENT_LAID = 0x200, FRAGMENT_SMOOTH = 0x400 };

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
static uint32_t kept_passes;       // the passes those are in, which every frame's list then uses
static uint32_t shown_set, shown_commands;   // the list drawn last, for seglSwapAgain()

// ---- state ----

typedef struct { num r[4][4]; } matrix;   // rows: what the device takes dot products with
static matrix modelview[8], projection[4];
static int modelview_top, projection_top, matrix_mode = GL_MODELVIEW, matrices_dirty = 1;
static int view_x, view_y, view_w, view_h;

static uint32_t colour = 0x00ffffff, clear_colour;
static int texturing, depth_test, blending, alpha_test, blend_kind = 1, key_index;
typedef struct { uint32_t address, width, height, indexed, used, smooth; } texture;   // smooth: FRAGMENT_SMOOTH or 0
static texture textures[MAX_TEXTURES];
static uint32_t bound;

typedef struct { const uint8_t* pointer; int size, stride, enabled, floats; } array;
static array vertex_array, coord_array;

#ifdef SEGL_FLOAT
static inline num mul(num a, num b) { return a * b; }
static inline num quotient(num a, num b) { return a / b; }
#else
static inline GLfixed mul(GLfixed a, GLfixed b) { return (GLfixed)(((int64_t)a * b) >> 16); }
static inline GLfixed quotient(GLfixed a, GLfixed b) { return (GLfixed)(((int64_t)a << 16) / b); }
#endif

static matrix* current(void) {
    return matrix_mode == GL_PROJECTION ? &projection[projection_top] : &modelview[modelview_top];
}

static const matrix identity = {{{NUM_ONE, 0, 0, 0}, {0, NUM_ONE, 0, 0}, {0, 0, NUM_ONE, 0}, {0, 0, 0, NUM_ONE}}};

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

// One program at a time draws with the GPU (its memory for programs has fixed places): the
// kernel says whose it is. 0 when it is ours now; -1 when another program has it, which is
// then named on the standard error. (A kernel from before this answers nothing: ours.)
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#define SEGL_DRAW_CLAIM 0x80044705u   // _IOR('G', 5, a word)
static int draw_claim(int fd) {
    unsigned other = 0;
    if (ioctl(fd, SEGL_DRAW_CLAIM, &other) == 0 || errno != EBUSY) return 0;
    char path[40], name[64] = "another program";
    snprintf(path, sizeof path, "/proc/%u/comm", other);
    FILE* f = fopen(path, "r");
    if (f) {
        if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
        fclose(f);
    }
    fprintf(stderr, "the GPU is drawing for %s (process %u): one such program at a time, close it first\n", name, other);
    return -1;
}

int seglInit(unsigned int nano_x_window) {
    int fd = open("/dev/gpu", O_RDWR);
    if (fd < 0) return -1;
    // (not "no GPU", which a program may get by without: two programs' drawings in one memory)
    if (draw_claim(fd) < 0) exit(1);
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

unsigned int seglMemoryLeft(void) { return gpu ? GPU_SIZE - texture_top : 0; }

// While the desktop is layers (display mode 4, docs/display.md) nothing uses the display's own
// framebuffer: 3.9 MB from just above the layer table to the window system's command list.
#define SPARE_AT 0x00010000u
#define SPARE_END 0x00400000u
void* seglMemorySpare(unsigned int* bytes) {
    *bytes = 0;
    if (!gpu || ((volatile uint32_t*)gpu)[0] != 4) return 0;
    *bytes = SPARE_END - SPARE_AT;
    return gpu + SPARE_AT;
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
    blocks = 0;
    passes_used = kept_passes;
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
static void load_columns(const num* m, int and_multiply) {
    matrix t;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) t.r[i][j] = m[j * 4 + i];
    if (and_multiply) multiply(&t);
    else *current() = t, matrices_dirty = 1;
}
static void columns_from_fixed(const GLfixed* m, int and_multiply) {
    num c[16];
    for (int i = 0; i < 16; i++) c[i] = FROM_FIXED(m[i]);
    load_columns(c, and_multiply);
}
static void columns_from_float(const GLfloat* m, int and_multiply) {
    num c[16];
    for (int i = 0; i < 16; i++) c[i] = FROM_FLOAT(m[i]);
    load_columns(c, and_multiply);
}
void glLoadMatrixx(const GLfixed* m) { columns_from_fixed(m, 0); }
void glMultMatrixx(const GLfixed* m) { columns_from_fixed(m, 1); }
void glLoadMatrixf(const GLfloat* m) { columns_from_float(m, 0); }
void glMultMatrixf(const GLfloat* m) { columns_from_float(m, 1); }
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
static void frustum(num l, num r, num b, num t, num n, num f) {
    matrix m = {{{quotient(2 * n, r - l), 0, quotient(r + l, r - l), 0},
                 {0, quotient(2 * n, t - b), quotient(t + b, t - b), 0},
                 {0, 0, -quotient(f + n, f - n), -quotient(mul(2 * f, n), f - n)},
                 {0, 0, -NUM_ONE, 0}}};
    multiply(&m);
}
static void ortho(num l, num r, num b, num t, num n, num f) {
    matrix m = {{{quotient(2 * NUM_ONE, r - l), 0, 0, -quotient(r + l, r - l)},
                 {0, quotient(2 * NUM_ONE, t - b), 0, -quotient(t + b, t - b)},
                 {0, 0, -quotient(2 * NUM_ONE, f - n), -quotient(f + n, f - n)},
                 {0, 0, 0, NUM_ONE}}};
    multiply(&m);
}
// (what multiplying by the whole matrix would give, without the products by 0 and 1)
static void translate(num x, num y, num z) {
    matrix* c = current();
    for (int i = 0; i < 4; i++) c->r[i][3] += mul(c->r[i][0], x) + mul(c->r[i][1], y) + mul(c->r[i][2], z);
    matrices_dirty = 1;
}
static void scale(num x, num y, num z) {
    matrix* c = current();
    for (int i = 0; i < 4; i++) c->r[i][0] = mul(c->r[i][0], x), c->r[i][1] = mul(c->r[i][1], y), c->r[i][2] = mul(c->r[i][2], z);
    matrices_dirty = 1;
}
#ifdef SEGL_FLOAT
// Sine of an angle in degrees, by a polynomial on the first quarter turn: the C library's
// works in doubles inside, which this machine has no instructions for.
static float sine(float degrees) {
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
// About any axis, as OpenGL has it.
static void rotate(num degrees, num x, num y, num z) {
    if (x == 0 && y == 0 && z != 0) {
        // about z, which is most of them: two columns change, by no products with 0 and 1
        matrix* m = current();
        num sn = sine(z > 0 ? degrees : -degrees), cs = sine(degrees + 90.0f);
        for (int i = 0; i < 4; i++) {
            num a = m->r[i][0], b = m->r[i][1];
            m->r[i][0] = a * cs + b * sn, m->r[i][1] = b * cs - a * sn;
        }
        matrices_dirty = 1;
        return;
    }
    num length = sqrtf(x * x + y * y + z * z);
    if (length == 0) return;
    x /= length, y /= length, z /= length;
    num s = sine(degrees), c = sine(degrees + 90.0f), t = 1 - c;
    matrix m = {{{t * x * x + c, t * x * y - s * z, t * x * z + s * y, 0},
                 {t * x * y + s * z, t * y * y + c, t * y * z - s * x, 0},
                 {t * x * z - s * y, t * y * z + s * x, t * z * z + c, 0},
                 {0, 0, 0, 1}}};
    multiply(&m);
}
#else
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
// About an axis of the coordinate system only.
static void rotate(num degrees, num x, num y, num z) {
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
#endif
#define X(v) FROM_FIXED(v)
#define F(v) FROM_FLOAT(v)
void glFrustumx(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f) { frustum(X(l), X(r), X(b), X(t), X(n), X(f)); }
void glFrustumf(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) { frustum(F(l), F(r), F(b), F(t), F(n), F(f)); }
void glOrthox(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f) { ortho(X(l), X(r), X(b), X(t), X(n), X(f)); }
void glOrthof(GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f) { ortho(F(l), F(r), F(b), F(t), F(n), F(f)); }
void glTranslatex(GLfixed x, GLfixed y, GLfixed z) { translate(X(x), X(y), X(z)); }
void glTranslatef(GLfloat x, GLfloat y, GLfloat z) { translate(F(x), F(y), F(z)); }
void glScalex(GLfixed x, GLfixed y, GLfixed z) { scale(X(x), X(y), X(z)); }
void glScalef(GLfloat x, GLfloat y, GLfloat z) { scale(F(x), F(y), F(z)); }
void glRotatex(GLfixed degrees, GLfixed x, GLfixed y, GLfixed z) { rotate(X(degrees), X(x), X(y), X(z)); }
void glRotatef(GLfloat degrees, GLfloat x, GLfloat y, GLfloat z) { rotate(F(degrees), F(x), F(y), F(z)); }
#undef X
#undef F

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
void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    glColor4x((GLfixed)(red * 65536.0f), (GLfixed)(green * 65536.0f), (GLfixed)(blue * 65536.0f), (GLfixed)(alpha * 65536.0f));
}
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {
    glClearColorx((GLfixed)(red * 65536.0f), (GLfixed)(green * 65536.0f), (GLfixed)(blue * 65536.0f), (GLfixed)(alpha * 65536.0f));
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
    vertex_array.pointer = pointer, vertex_array.size = size, vertex_array.stride = stride ? stride : size * 4;
    vertex_array.floats = type == GL_FLOAT;
}
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer) {
    coord_array.pointer = pointer, coord_array.size = size, coord_array.stride = stride ? stride : size * 4;
    coord_array.floats = type == GL_FLOAT;
}

// ---- textures ----

void glGenTextures(GLsizei n, GLuint* out) {
    for (uint32_t t = 1; t < MAX_TEXTURES && n > 0; t++)
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
        if (which[i] < MAX_TEXTURES) textures[which[i]].used = 0;
    uint32_t top = TEXTURES_AT;
    for (uint32_t t = 1; t < MAX_TEXTURES; t++) {
        texture* x = &textures[t];
        uint32_t bytes = x->indexed ? x->width * x->height : x->width * x->height * 4;
        // (only textures in this memory: one that shows a window's own pixels, seglWindowTexture, is
        // below it, which as an unsigned offset is far above, and kept the top where it was for good.
        // Doom has one from its first screen wipe on, and ran out of texture memory after eight levels.)
        if (x->used && x->address >= TEXTURES_AT && x->address < GPU_SIZE && x->address + bytes > top)
            top = (x->address + bytes + 15) & ~15u;
    }
    if (top < texture_top) texture_top = top;
}
void glBindTexture(GLenum target, GLuint which) {
    (void)target;
    bound = which < MAX_TEXTURES ? which : 0;
}
// GL_TEXTURE_MAG_FILTER of the bound texture: GL_LINEAR (or a mipmap kind of it) has the device
// weigh the four texels round a point, GL_NEAREST take the one it is in, which is how a
// texture starts. Bits (a font's) are never weighed.
void glTexParameteri(GLenum target, GLenum name, GLint value) {
    (void)target;
    if (name == GL_TEXTURE_MAG_FILTER)
        textures[bound].smooth = value == GL_LINEAR || value == 0x2701 || value == 0x2703 ? FRAGMENT_SMOOTH : 0;
}
void glTexParameterx(GLenum target, GLenum name, GLfixed value) { glTexParameteri(target, name, (GLint)value); }

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
        num* u = (num*)(gpu + set_at + UNIFORMS_IN) + 32 * blocks++;
        const matrix* p = &projection[projection_top];
        const matrix* m = &modelview[modelview_top];
        int w = window_info.width ? window_info.width : 1, h = window_info.height ? window_info.height : 1;
        // (a window is far below 32,768 pixels: these fit a 32-bit division, which this machine has)
#ifdef SEGL_FLOAT
        num sx = (num)view_w / w, ox = (num)(2 * view_x + view_w) / w - 1;
        num sy = (num)view_h / h, oy = (num)(2 * view_y + view_h) / h - 1;
#else
        GLfixed sx = (view_w << 16) / w, ox = ((2 * view_x + view_w) << 16) / w - 65536;
        GLfixed sy = (view_h << 16) / h, oy = ((2 * view_y + view_h) << 16) / h - 65536;
#endif
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
typedef struct { const uint8_t *positions, *coords; size_t position_step, coord_step; int has_z, other, other_coords; } arrays;

// A word of an array of the other kind than this build's, as this build's number.
static inline num converted(uint32_t word) {
#ifdef SEGL_FLOAT
    return FROM_FIXED((GLfixed)word);
#else
    union { uint32_t u; GLfloat f; } c = {word};
    return FROM_FLOAT(c.f);
#endif
}

// One compact vertex (a texel): position, and the texture coordinates in 1/1024ths. An array
// of this build's kind of number is copied as it is.
static inline void put(uint32_t* restrict to, const arrays* a, int index) {
    const uint32_t* v = (const uint32_t*)(a->positions + (size_t)index * a->position_step);
    uint32_t uv = 0;
    if (a->coords) {
        const uint32_t* t = (const uint32_t*)(a->coords + (size_t)index * a->coord_step);
        union { uint32_t u; num n; } s0 = {t[0]}, s1 = {t[1]};
        if (a->other_coords) s0.n = converted(t[0]), s1.n = converted(t[1]);
        uv = (TO_1024THS(s0.n) & 0xffff) | TO_1024THS(s1.n) << 16;
    }
    if (a->other) {
        to[0] = bits_of(converted(v[0]));
        to[1] = bits_of(converted(v[1]));
        to[2] = a->has_z ? bits_of(converted(v[2])) : 0;
    } else {
        to[0] = v[0];
        to[1] = v[1];
        to[2] = a->has_z ? v[2] : 0;
    }
    to[3] = uv;
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count) {
    if (!gpu || !vertex_array.enabled || count < 3) return;
    const texture* x = &textures[texturing ? bound : 0];
    uint32_t pass = (blending ? blend_kind : 0) + (depth_test ? 0 : 4);
    uint32_t fragment = (texturing && x->address ? (x->indexed ? FRAGMENT_INDEXED : FRAGMENT_TEXTURE) | x->smooth : FRAGMENT_COLOUR) | pass << 16;
    if (alpha_test && x->indexed && texturing) fragment |= FRAGMENT_KEYED;
    // a fan goes in as quads, two of its triangles each (the last may have its fourth corner twice)
    int fan = mode == GL_TRIANGLE_FAN, quads = mode == GL_QUADS || fan;
    uint32_t stored = fan ? ((uint32_t)count - 1) / 2 * 4 : quads ? (uint32_t)count & ~3u :
                      mode == GL_TRIANGLES ? (uint32_t)count - count % 3 : ((uint32_t)count - 2) * 3;
    uint32_t mesh = quads ? stored / 4 * 6 : stored;
    uint32_t vertex = VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_COMPACT | NUM_FORMAT | (quads ? VERTEX_QUADS : 0);
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
                      (size_t)coord_array.stride, vertex_array.size > 2, vertex_array.floats != (NUM_FORMAT != 0),
                      coord_array.floats != (NUM_FORMAT != 0)};
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

static uint32_t last_ratio = 1;   // mesh vertices to one stored vertex of the draw made last

// Room in the frame for compact vertices drawn with the state in force, as glDrawArrays would.
// With `at` they are in GPU memory already and the draw is a command of its own. `flags` are
// more of the vertex word's: a `table` of colours, and as `extra` packed vertices' coordinates
// or the three words of points.
static uint32_t* compact_begin(GLenum mode, GLsizei count, uint32_t* stored_out, const uint8_t* at, uint32_t flags,
                               const void* table, const void* extra) {
    if (!gpu || count < 3) return 0;
    const texture* x = &textures[texturing ? bound : 0];
    uint32_t pass = (blending ? blend_kind : 0) + (depth_test ? 0 : 4);
    uint32_t fragment = (texturing && x->address ? (x->indexed ? FRAGMENT_INDEXED : FRAGMENT_TEXTURE) | x->smooth : FRAGMENT_COLOUR) | pass << 16;
    if (alpha_test && x->indexed && texturing) fragment |= FRAGMENT_KEYED;
    int fan = mode == GL_TRIANGLE_FAN, quads = mode == GL_QUADS || fan;
    uint32_t stored = fan ? ((uint32_t)count - 1) / 2 * 4 : quads ? (uint32_t)count & ~3u :
                      mode == GL_TRIANGLES ? (uint32_t)count - count % 3 : ((uint32_t)count - 2) * 3;
    const uint32_t* points = flags & VERTEX_POINTS ? extra : 0;
    if (points) stored = (uint32_t)count;
    uint32_t mesh = quads ? stored / 4 * 6 : points ? stored * 3 : stored;
    uint32_t vertex = VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_COMPACT | NUM_FORMAT | (quads ? VERTEX_QUADS : 0) | flags;
    if ((!at && vertex_top + stored * 16 > set_at + SET_SIZE) || mesh_vertices + mesh > MAX_MESH) return 0;
    uint32_t block = matrix_block();
    uint32_t tint = table ? GPU_PHYS + (uint32_t)((const uint8_t*)table - gpu) : colour;
    uint32_t state[6] = {vertex, fragment, block, x->address, tint, (uint32_t)key_index};
    uint32_t* to = at ? (uint32_t*)at : (uint32_t*)(gpu + vertex_top);
    int same = !at;
    for (int i = 0; i < 6; i++) same &= state[i] == last_state[i];
    if (last_draw && last_draw == (uint32_t*)(gpu + set_at) + 16 * (commands - 1) && same &&
        (!points || (last_draw[12] == points[0] && last_draw[13] == points[1] && last_draw[14] == points[2]))) {
        last_draw[2] += mesh;
        mesh_vertices += mesh;
    } else {
        uint32_t* c = command(CMD_DRAW, mesh);
        if (!c) return 0;
        if (points) c[12] = points[0], c[13] = points[1], c[14] = points[2];
        else if (flags & VERTEX_PACKED) c[12] = GPU_PHYS + (uint32_t)((const uint8_t*)extra - gpu);
        c[1] = GPU_PHYS + (uint32_t)((const uint8_t*)to - gpu);
        c[2] = mesh;
        c[4] = vertex;
        c[5] = fragment;
        c[6] = block;
        c[7] = GPU_PHYS + x->address;
        c[8] = x->width;
        c[9] = x->height;
        c[10] = key_index;
        c[11] = tint;
        last_draw = at ? 0 : c;   // (nothing joins a command whose vertices are elsewhere)
        for (int i = 0; i < 6; i++) last_state[i] = state[i];
        passes_used |= (1u << pass) & 0xfe;
    }
    if (!at) vertex_top += stored * 16;
    last_ratio = points ? 3 : 1;
    *stored_out = stored;
    return to;
}

unsigned int* seglCompactSpace(GLsizei count) {
    uint32_t stored;
    return compact_begin(GL_TRIANGLES, count - count % 3, &stored, 0, 0, 0, 0);
}

unsigned int* seglPoints(GLsizei count, const void* size_growth_from) {
    uint32_t stored;
    return compact_begin(GL_TRIANGLES, count, &stored, 0, VERTEX_POINTS, 0, size_growth_from);
}

void seglCompactTrim(GLsizei unused) {
    uint32_t mesh = (uint32_t)unused * last_ratio;
    if (!last_draw || unused <= 0 || mesh > last_draw[2]) return;
    last_draw[2] -= mesh;
    mesh_vertices -= mesh;
    vertex_top -= (uint32_t)unused * 16;
}

int seglCompactAt(const void* vertices, GLsizei count) {
    uint32_t stored;
    return compact_begin(GL_TRIANGLES, count - count % 3, &stored, vertices, 0, 0, 0) ? 0 : -1;
}

int seglPacked(const void* vertices, const void* coords, GLsizei count, const unsigned int* table) {
    uint32_t stored, flags = VERTEX_TAGGED | VERTEX_PACKED | (table ? VERTEX_TABLE : 0);
    return compact_begin(GL_TRIANGLES, count - count % 3, &stored, vertices, flags, table, coords) ? 0 : -1;
}

unsigned int* seglTagged(const void* vertices, GLsizei count, const unsigned int* table) {
    uint32_t stored;
    return compact_begin(GL_TRIANGLES, count - count % 3, &stored, vertices, VERTEX_TAGGED | (table ? VERTEX_TABLE : 0), table, 0);
}

void seglCompact(GLenum mode, const unsigned int* v, GLsizei count) {
    uint32_t stored;
    uint32_t* to = compact_begin(mode, count, &stored, 0, 0, 0, 0);
    int fan = mode == GL_TRIANGLE_FAN;
    if (!to) return;
#define COPY(to, from) ((to)[0] = (from)[0], (to)[1] = (from)[1], (to)[2] = (from)[2], (to)[3] = (from)[3])
    if (fan) {
        for (int i = 1; i + 1 < count; i += 2, to += 16) {
            int last = i + 2 < count ? i + 2 : count - 1;
            COPY(to, v), COPY(to + 4, v + 4 * i), COPY(to + 8, v + 4 * (i + 1)), COPY(to + 12, v + 4 * last);
        }
    } else if (mode == GL_TRIANGLE_STRIP) {
        for (int i = 0; i + 2 < count; i++, to += 12)
            COPY(to, v + 4 * (i + (i & 1))), COPY(to + 4, v + 4 * (i + 1 - (i & 1))), COPY(to + 8, v + 4 * (i + 2));
    } else {
        for (uint32_t i = 0; i < stored * 4; i++) to[i] = v[i];
    }
#undef COPY
}

// The matrix on top of a stack, column-major, as this build's numbers; and such a matrix back.
void seglGetMatrix(GLenum mode, void* out) {
    const matrix* m = mode == GL_PROJECTION ? &projection[projection_top] : &modelview[modelview_top];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) ((num*)out)[j * 4 + i] = m->r[i][j];
}
void seglSetMatrix(const void* m) { load_columns(m, 0); }

// The window's own pixels as a texture: the picture the GPU drew last, where the server keeps it.
int seglWindowTexture(GLuint name, GLfixed* across) {
    if (!window_info.surface_address) return -1;
    texture* x = &textures[name < MAX_TEXTURES ? name : 0];
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
    const texture* x = &textures[name < MAX_TEXTURES ? name : 0];
    uint32_t pass = (alpha < 255 ? 1u : 0u) + (depth_test ? 0 : 4);
    uint32_t fragment = (x->indexed ? FRAGMENT_INDEXED : FRAGMENT_TEXTURE) | x->smooth | (keyed ? FRAGMENT_KEYED : 0) | pass << 16;
    uint32_t vertex = VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_COMPACT | VERTEX_QUADS | NUM_FORMAT;
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
        to[0] = bits_of(FROM_FIXED(xyz[0]));
        to[1] = bits_of(FROM_FIXED(xyz[1]));
        to[2] = bits_of(FROM_FIXED(xyz[2]));
        to[3] = ((uint32_t)(uv[0] >> 6) & 0xffff) | (uint32_t)(uv[1] >> 6) << 16;
    }
    vertex_top += 64;
}

// A rectangle of a texture on top of what the list drew before it (segl.h): always the blended
// pass with no depth, so that opaque sprites and their shadows keep the order they come in.
static inline void sprite_corners(uint32_t* restrict to, const GLfixed* box, const int* texels) {
    uint32_t u0 = (uint32_t)texels[0] & 0xffff, u1 = (uint32_t)texels[2] & 0xffff;
    uint32_t v0 = (uint32_t)texels[1] << 16, v1 = (uint32_t)texels[3] << 16;
    uint32_t x0 = bits_of(FROM_FIXED(box[0])), y0 = bits_of(FROM_FIXED(box[1]));
    uint32_t x1 = bits_of(FROM_FIXED(box[2])), y1 = bits_of(FROM_FIXED(box[3]));
    to[0] = x0, to[1] = y0, to[2] = 0, to[3] = u0 | v0;
    to[4] = x1, to[5] = y0, to[6] = 0, to[7] = u1 | v0;
    to[8] = x1, to[9] = y1, to[10] = 0, to[11] = u1 | v1;
    to[12] = x0, to[13] = y1, to[14] = 0, to[15] = u0 | v1;
}

static void sprite_command(const GLfixed* box, const int* texels, GLuint name, unsigned colour, int keyed);

void seglSprite(const GLfixed* box, const int* texels, GLuint name, unsigned colour, int keyed) {
    const texture* x = &textures[name < MAX_TEXTURES ? name : 0];
    uint32_t fragment = (x->indexed == 2 ? 3u : (x->indexed ? FRAGMENT_INDEXED : FRAGMENT_TEXTURE) | x->smooth) |
                        (keyed ? FRAGMENT_KEYED : 0) | 5u << 16;
    // a quad more of the command before, which is most of them: nothing is called on this path
    if (last_draw && last_draw == (uint32_t*)(gpu + set_at) + 16 * (commands - 1) && !matrices_dirty && blocks &&
        last_state[0] == (VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_COMPACT | VERTEX_QUADS | NUM_FORMAT) && last_state[1] == fragment &&
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
    const texture* x = &textures[name < MAX_TEXTURES ? name : 0];
    uint32_t fragment = (x->indexed == 2 ? 3u : (x->indexed ? FRAGMENT_INDEXED : FRAGMENT_TEXTURE) | x->smooth) |
                        (keyed ? FRAGMENT_KEYED : 0) | 5u << 16;
    uint32_t vertex = VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_COMPACT | VERTEX_QUADS | NUM_FORMAT;
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

// Whole vertices in screen pixels (segl.h): the frame's room is what compact vertices use too.
void* seglScreenSpace(GLsizei* room) {
    uint32_t fit = gpu ? (set_at + SET_SIZE - vertex_top) / 64 : 0, mesh = MAX_MESH - mesh_vertices;
    *room = (GLsizei)((fit < mesh ? fit : mesh) / 3 * 3);
    return gpu ? gpu + vertex_top : 0;
}
void seglScreenUsed(GLsizei count, GLuint name) {
    const texture* x = &textures[name < MAX_TEXTURES ? name : 0];
    uint32_t* c = count > 0 ? command(CMD_DRAW, (uint32_t)count) : 0;
    if (!c) return;
    c[1] = GPU_PHYS + vertex_top;
    c[2] = (uint32_t)count;
    c[4] = NUM_FORMAT;   // vertex mode 0: positions are pixels
    c[5] = (x->address ? FRAGMENT_TEXTURE : FRAGMENT_COLOUR) | 5u << 16;
    c[6] = 0;
    c[7] = GPU_PHYS + x->address;
    c[8] = x->width;
    c[9] = x->height;
    c[10] = c[11] = 0;
    passes_used |= 1u << 5;
    last_draw = 0;
    vertex_top += (uint32_t)count * 64;
}

uint32_t* seglLastCommand(void) { return last_draw; }
uint32_t* seglLastVertices(void) { return (uint32_t*)(gpu + vertex_top - 64); }
uint32_t seglAddress(const void* memory) { return GPU_PHYS + (uint32_t)((const uint8_t*)memory - gpu); }

// Forgets what was drawn since the last swap.
void seglDiscard(void) {
    commands = kept;
    mesh_vertices = kept_mesh;
    blocks = 0;
    passes_used = kept_passes;
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
    kept_passes = passes_used = 0;
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
        const texture* t = &textures[name < MAX_TEXTURES ? name : 0];
        c[0] = CMD_DRAW;
        c[1] = GPU_PHYS + (uint32_t)((const uint8_t*)vertices - gpu) + (uint32_t)first_quad * 64;   // quad 0 is at `vertices`
        c[2] = (uint32_t)quads * 6;
        c[3] = (uint32_t)first_quad * 6;
        c[4] = VERTEX_CLIP | VERTEX_MODELVIEW | VERTEX_COMPACT | VERTEX_QUADS | NUM_FORMAT;
        c[5] = FRAGMENT_INDEXED | t->smooth | (keyed ? FRAGMENT_KEYED : 0);
        c[6] = GPU_PHYS + SETS_AT + set * SET_SIZE + UNIFORMS_IN;
        c[7] = GPU_PHYS + t->address;
        c[8] = t->width;
        c[9] = t->height;
        c[10] = key_index;
        c[11] = grey << 16 | grey << 8 | grey;
    }
}
void seglKeptBlend(int index, int kind, int words) {
    uint32_t fragment = (words ? FRAGMENT_TEXTURE : FRAGMENT_INDEXED) | (uint32_t)kind << 16;
    for (int set = 0; set < 2; set++) {
        uint32_t* c = kept_command(set, index);
        c[5] = (c[5] & (FRAGMENT_KEYED | FRAGMENT_LAID | FRAGMENT_SMOOTH)) | fragment;
    }
    kept_passes |= (1u << kind) & 0xfe;
    passes_used |= kept_passes;
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
    const texture* t = &textures[name < MAX_TEXTURES ? name : 0];
    for (int set = 0; set < 2; set++) {
        uint32_t* c = kept_command(set, index);
        c[5] = (c[5] & ~(uint32_t)FRAGMENT_SMOOTH) | t->smooth;
        c[7] = GPU_PHYS + t->address;
        c[8] = t->width;
        c[9] = t->height;
    }
}
void seglKeptLaid(int index, GLfixed u, GLfixed v, GLfixed du, GLfixed dv) {
    for (int set = 0; set < 2; set++) {
        uint32_t* c = kept_command(set, index);
        c[5] |= FRAGMENT_LAID;
        c[12] = bits_of(FROM_FIXED(u)), c[13] = bits_of(FROM_FIXED(v));
        c[14] = bits_of(FROM_FIXED(du)), c[15] = bits_of(FROM_FIXED(dv));
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

// A program that has a worker core call this library (linux/tdawn/host.c) compiles it with a
// section a variable, each starting on 16 bytes: two cores must not store to the same 16
// (docs/multicore.md). These are the last, so that no other file's variable follows ours in
// the same 16 bytes.
char segl_end_data[16] __attribute__((aligned(16), used)) = {1};
char segl_end_bss[16] __attribute__((aligned(16), used));
