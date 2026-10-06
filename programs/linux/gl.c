// A small OpenGL 1.x driver for the machine's GPU device (docs/gpu.md), with the GLX and Xlib
// calls a program needs to open its window. Display lists become vertex buffers in GPU memory;
// calling one becomes a lit draw command with the current matrices as uniforms; swapping
// buffers submits the frame's command list.
//
// Built with GL_NANOX and run under the Nano-X server, the window is a Nano-X window and frames
// are drawn into its buffer (docs/nanox.md); otherwise the window is the whole display.
// Everything inside works in 16.16 fixed point, which is also what the GPU reads; floats are
// converted at the API boundary, because the CPU has no FPU. docs/gpu.md has the rest.

#include <GL/glx.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef GL_NANOX
#include <sched.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>
#include <X11/keysym.h>
#include <nano-X.h>
#endif

// ---- GPU memory ----

#define GPU_PHYS 0x87000000u
#define GPU_SIZE 0x00b00000u
// This frame's uniform blocks and command list, then the vertex buffers of display lists to
// the end. Under Nano-X they start higher, in the part of GPU memory its server leaves alone.
static uint32_t frame_at = 0x00100000u, vertex_top = 0x00200000u;
static uint32_t frame_flip;   // under Nano-X frames alternate between two places: 0 or FRAME_FLIP
#define FRAME_FLIP 0x00010000u
#define REG_DISPLAY 0x00        // mode, width, height
#define REG_SUBMIT 0x10         // submit, list address, command count
#define REG_FRAMES 0x1c

enum { CMD_CLEAR = 1, CMD_RECT = 2, CMD_DRAW = 3 };
enum { VERTEX_CLIP = 1, VERTEX_LIT = 2, VERTEX_MODELVIEW = 0x100 };

#define GPU_WAIT 0x4701   // _IO('G', 1): wait until the frame counter leaves the given value

static int gpu_fd = -1;
static uint8_t* gpu_map;   // the whole GPU memory, when the kernel has the driver

static void die(const char* what) {
    fprintf(stderr, "gl: %s\n", what);
    exit(1);
}

// The MTD number of the device named "gpu" in /proc/mtd, or -1.
static int find_gpu_mtd(void) {
    static char text[1024];
    int fd = open("/proc/mtd", O_RDONLY);
    if (fd < 0) return -1;
    long n = read(fd, text, sizeof text - 1);
    close(fd);
    text[n > 0 ? n : 0] = 0;
    char* at = strstr(text, "\"gpu\"");
    if (!at) return -1;
    while (at > text && at[-1] != '\n') at--;   // start of that line: "mtdN: ..."
    return atoi(at + 3);
}

static void gpu_open(void) {
    if (gpu_fd >= 0) return;
    gpu_fd = open("/dev/gpu", O_RDWR);
    if (gpu_fd >= 0) {
        void* map = mmap(0, GPU_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, gpu_fd, 0x01000000);   // the device maps from 0x86000000
        if (map == MAP_FAILED) die("cannot map /dev/gpu");
        gpu_map = map;
        return;
    }
    int n = find_gpu_mtd();
    if (n < 0) {
        const char spec[] = "gpu,0x87000000,0xb00000";
        int fd = open("/sys/module/phram/parameters/phram", O_WRONLY);
        if (fd < 0 || write(fd, spec, sizeof spec - 1) < 0) die("cannot create the GPU memory device (phram)");
        close(fd);
        n = find_gpu_mtd();
    }
    if (n < 0) die("no GPU memory device");
    char path[] = "/dev/mtd0";
    path[8] = (char)('0' + n);
    gpu_fd = open(path, O_RDWR);
    if (gpu_fd < 0) die("cannot open the GPU memory device");
}

static void gpu_write(uint32_t offset, const void* data, uint32_t bytes) {
    if (gpu_map) memcpy(gpu_map + offset, data, bytes);
    else if (pwrite(gpu_fd, data, bytes, offset) != (long)bytes) die("write to GPU memory failed");
}

static uint32_t gpu_frames(void) {
    uint32_t n = 0;
    if (gpu_map) n = *(volatile uint32_t*)(gpu_map + REG_FRAMES);
    else pread(gpu_fd, &n, 4, REG_FRAMES);
    return n;
}

// ---- fixed point ----

typedef int32_t fx;
#define ONE 65536
static inline fx mul(fx a, fx b) { return (fx)(((int64_t)a * b) >> 16); }
// A float as 16.16, from its bits: the CPU has no FPU, and the library's multiply and
// convert cost a few hundred instructions where this costs a dozen.
static inline fx from_float(float f) {
    union { float f; uint32_t u; } v = {f};
    int shift = (int)((v.u >> 23) & 0xff) - 127 - 23 + 16;
    uint32_t mantissa = (v.u & 0x7fffff) | 0x800000;
    if (shift <= -32 || (v.u & 0x7fffffff) == 0) return 0;
    if (shift >= 8) return (v.u >> 31) ? -0x7fffffff : 0x7fffffff;
    fx r = shift >= 0 ? (fx)(mantissa << shift) : (fx)(mantissa >> -shift);
    return (v.u >> 31) ? -r : r;
}

static uint32_t isqrt(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

// sin of an angle in degrees (16.16), by the series on -90..90 degrees.
static fx sin_degrees(fx degrees) {
    degrees %= 360 * ONE;
    if (degrees > 180 * ONE) degrees -= 360 * ONE;
    else if (degrees < -180 * ONE) degrees += 360 * ONE;
    if (degrees > 90 * ONE) degrees = 180 * ONE - degrees;
    else if (degrees < -90 * ONE) degrees = -180 * ONE - degrees;
    fx x = mul(degrees, 1144);   // radians: pi / 180 = 1144 / 65536
    fx x2 = mul(x, x);
    // x - x^3/6 + x^5/120 - x^7/5040
    return x - mul(mul(x, x2), 10923) + mul(mul(mul(x, x2), x2), 546) - mul(mul(mul(mul(x, x2), x2), x2), 13);
}

// ---- matrices: rows of 16.16, column vectors, as OpenGL composes them ----

typedef struct { fx m[4][4]; } mat;

static const mat identity = {{{ONE, 0, 0, 0}, {0, ONE, 0, 0}, {0, 0, ONE, 0}, {0, 0, 0, ONE}}};

static mat multiply(const mat* a, const mat* b) {
    mat r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r.m[i][j] = mul(a->m[i][0], b->m[0][j]) + mul(a->m[i][1], b->m[1][j]) + mul(a->m[i][2], b->m[2][j]) +
                        mul(a->m[i][3], b->m[3][j]);
    return r;
}

#define STACK 16
static mat modelview[STACK] = {{{{ONE, 0, 0, 0}, {0, ONE, 0, 0}, {0, 0, ONE, 0}, {0, 0, 0, ONE}}}};
static mat projection[STACK] = {{{{ONE, 0, 0, 0}, {0, ONE, 0, 0}, {0, 0, ONE, 0}, {0, 0, 0, ONE}}}};
static int modelview_top, projection_top;
static GLenum matrix_mode = GL_MODELVIEW;

static mat* current(void) {
    return matrix_mode == GL_PROJECTION ? &projection[projection_top] : &modelview[modelview_top];
}
static void apply(const mat* m) {
    mat* c = current();
    *c = multiply(c, m);
}

void glMatrixMode(GLenum mode) { matrix_mode = mode; }
void glLoadIdentity(void) { *current() = identity; }
void glPushMatrix(void) {
    int* top = matrix_mode == GL_PROJECTION ? &projection_top : &modelview_top;
    mat* stack = matrix_mode == GL_PROJECTION ? projection : modelview;
    if (*top + 1 < STACK) {
        stack[*top + 1] = stack[*top];
        ++*top;
    }
}
void glPopMatrix(void) {
    int* top = matrix_mode == GL_PROJECTION ? &projection_top : &modelview_top;
    if (*top > 0) --*top;
}

// Only the last column changes.
void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    fx tx = from_float(x), ty = from_float(y), tz = from_float(z);
    mat* c = current();
    for (int i = 0; i < 4; i++) c->m[i][3] += mul(c->m[i][0], tx) + mul(c->m[i][1], ty) + mul(c->m[i][2], tz);
}
void glTranslated(GLdouble x, GLdouble y, GLdouble z) { glTranslatef((GLfloat)x, (GLfloat)y, (GLfloat)z); }

void glScalef(GLfloat x, GLfloat y, GLfloat z) {
    mat s = identity;
    s.m[0][0] = from_float(x);
    s.m[1][1] = from_float(y);
    s.m[2][2] = from_float(z);
    apply(&s);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z) {
    fx degrees = from_float(angle);
    fx s = sin_degrees(degrees), c = sin_degrees(degrees + 90 * ONE);
    fx ax = from_float(x), ay = from_float(y), az = from_float(z);
    if ((ax != 0) + (ay != 0) + (az != 0) == 1) {
        // about a coordinate axis: two columns turn into each other
        int j = az ? 0 : ax ? 1 : 2, k = az ? 1 : ax ? 2 : 0;
        if (ax + ay + az < 0) s = -s;
        mat* m = current();
        for (int i = 0; i < 4; i++) {
            fx a = m->m[i][j], b = m->m[i][k];
            m->m[i][j] = mul(a, c) + mul(b, s);
            m->m[i][k] = mul(b, c) - mul(a, s);
        }
        return;
    }
    fx len = (fx)isqrt((uint32_t)(mul(ax, ax) + mul(ay, ay) + mul(az, az)) << 8) << 4;
    if (len == 0) return;
    fx inv = (fx)(0xffffffffu / (uint32_t)len);
    ax = mul(ax, inv);
    ay = mul(ay, inv);
    az = mul(az, inv);
    fx t = ONE - c;
    mat r = identity;
    r.m[0][0] = mul(mul(t, ax), ax) + c;
    r.m[0][1] = mul(mul(t, ax), ay) - mul(s, az);
    r.m[0][2] = mul(mul(t, ax), az) + mul(s, ay);
    r.m[1][0] = mul(mul(t, ax), ay) + mul(s, az);
    r.m[1][1] = mul(mul(t, ay), ay) + c;
    r.m[1][2] = mul(mul(t, ay), az) - mul(s, ax);
    r.m[2][0] = mul(mul(t, ax), az) - mul(s, ay);
    r.m[2][1] = mul(mul(t, ay), az) + mul(s, ax);
    r.m[2][2] = mul(mul(t, az), az) + c;
    apply(&r);
}

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
    mat p;
    memset(&p, 0, sizeof p);
    p.m[0][0] = from_float((float)(2 * n / (r - l)));
    p.m[0][2] = from_float((float)((r + l) / (r - l)));
    p.m[1][1] = from_float((float)(2 * n / (t - b)));
    p.m[1][2] = from_float((float)((t + b) / (t - b)));
    p.m[2][2] = from_float((float)(-(f + n) / (f - n)));
    p.m[2][3] = from_float((float)(-2 * f * n / (f - n)));
    p.m[3][2] = -ONE;
    apply(&p);
}

// ---- state ----

static fx light_direction[3] = {0, 0, ONE};   // towards the light, in eye space
static fx material[3] = {52428, 52428, 52428};
static fx current_normal[3] = {0, 0, ONE};
static fx current_colour[3] = {ONE, ONE, ONE};
static GLenum shade_model = GL_SMOOTH;
static int lighting;
static uint32_t clear_colour;

void glEnable(GLenum cap) {
    if (cap == GL_LIGHTING) lighting = 1;
}
void glDisable(GLenum cap) {
    if (cap == GL_LIGHTING) lighting = 0;
}
void glShadeModel(GLenum mode) { shade_model = mode; }
void glDrawBuffer(GLenum mode) { (void)mode; }
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x; (void)y; (void)w; (void)h; }
void glFlush(void) {}
void glColor3f(GLfloat r, GLfloat g, GLfloat b) {
    current_colour[0] = from_float(r);
    current_colour[1] = from_float(g);
    current_colour[2] = from_float(b);
}
void glNormal3f(GLfloat x, GLfloat y, GLfloat z) {
    current_normal[0] = from_float(x);
    current_normal[1] = from_float(y);
    current_normal[2] = from_float(z);
}
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
    (void)a;
    clear_colour = (uint32_t)(r * 255.0f) << 16 | (uint32_t)(g * 255.0f) << 8 | (uint32_t)(b * 255.0f);
}

const GLubyte* glGetString(GLenum name) {
    switch (name) {
        case GL_VENDOR: return (const GLubyte*)"ShaderEmu";
        case GL_RENDERER: return (const GLubyte*)"ShaderEmu GPU (a pixel shader's guest)";
        case GL_VERSION: return (const GLubyte*)"1.1 subset";
        default: return (const GLubyte*)"";
    }
}

// Only light 0, and only its position: a direction (w = 0), taken through the modelview
// matrix as OpenGL does.
void glLightfv(GLenum light, GLenum pname, const GLfloat* p) {
    if (light != GL_LIGHT0 || pname != GL_POSITION) return;
    const mat* mv = &modelview[modelview_top];
    fx v[3] = {from_float(p[0]), from_float(p[1]), from_float(p[2])}, d[3];
    for (int i = 0; i < 3; i++) d[i] = mul(mv->m[i][0], v[0]) + mul(mv->m[i][1], v[1]) + mul(mv->m[i][2], v[2]);
    // scaled down first so the squared length stays in range
    fx len = (fx)isqrt((uint32_t)(mul(d[0] >> 4, d[0] >> 4) + mul(d[1] >> 4, d[1] >> 4) + mul(d[2] >> 4, d[2] >> 4)) << 8) << 8;
    if (len == 0) return;
    fx inv = (fx)(0xffffffffu / (uint32_t)len);
    for (int i = 0; i < 3; i++) light_direction[i] = mul(d[i], inv);
}

// ---- vertices: immediate mode, recorded into the open display list ----

typedef struct {
    fx position[4], normal[4], uv[4], colour[4];
} vertex;

// A run of triangles with one material.
typedef struct {
    uint32_t address, count;
    fx material[3];
    int lit;
} segment;

#define MAX_LISTS 64
#define MAX_SEGMENTS 4
typedef struct {
    segment parts[MAX_SEGMENTS];
    int part_count;
} display_list;

static display_list lists[MAX_LISTS];
static GLuint next_list = 1, open_list;

#define MAX_BATCH 12288   // vertices of one display list (or of one frame's loose geometry)
static vertex batch[MAX_BATCH];
static uint32_t batch_count, segment_start;

#define MAX_POLYGON 64
static vertex polygon[MAX_POLYGON];   // the vertices since glBegin that are still needed
static int polygon_count, primitive_index;
static GLenum primitive;

static void triangle(const vertex* a, const vertex* b, const vertex* c, const vertex* provoking) {
    if (batch_count + 3 > MAX_BATCH) return;
    vertex* out = &batch[batch_count];
    out[0] = *a;
    out[1] = *b;
    out[2] = *c;
    if (shade_model == GL_FLAT) {
        for (int i = 0; i < 3; i++) {
            memcpy(out[i].normal, provoking->normal, sizeof out[i].normal);
            memcpy(out[i].colour, provoking->colour, sizeof out[i].colour);
        }
    }
    batch_count += 3;
}

void glBegin(GLenum mode) {
    primitive = mode;
    polygon_count = 0;
    primitive_index = 0;
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z) {
    vertex v;
    memset(&v, 0, sizeof v);
    v.position[0] = from_float(x);
    v.position[1] = from_float(y);
    v.position[2] = from_float(z);
    v.position[3] = ONE;
    memcpy(v.normal, current_normal, sizeof current_normal);
    memcpy(v.colour, current_colour, sizeof current_colour);
    vertex* p = polygon;
    if (polygon_count < MAX_POLYGON) p[polygon_count++] = v;
    int n = polygon_count;
    switch (primitive) {
        case GL_TRIANGLES:
            if (n == 3) triangle(&p[0], &p[1], &p[2], &p[2]), polygon_count = 0;
            break;
        case GL_QUADS:
            if (n == 4) {
                triangle(&p[0], &p[1], &p[2], &p[3]);
                triangle(&p[0], &p[2], &p[3], &p[3]);
                polygon_count = 0;
            }
            break;
        case GL_QUAD_STRIP:   // each new pair closes a quad with the pair before it
            if (n == 4) {
                triangle(&p[0], &p[1], &p[3], &p[3]);
                triangle(&p[0], &p[3], &p[2], &p[3]);
                p[0] = p[2];
                p[1] = p[3];
                polygon_count = 2;
            }
            break;
        case GL_TRIANGLE_STRIP:
            if (n == 3) {
                if (primitive_index++ & 1) triangle(&p[1], &p[0], &p[2], &p[2]);
                else triangle(&p[0], &p[1], &p[2], &p[2]);
                p[0] = p[1];
                p[1] = p[2];
                polygon_count = 2;
            }
            break;
        case GL_TRIANGLE_FAN:
        case GL_POLYGON:
            if (n == 3) {
                triangle(&p[0], &p[1], &p[2], primitive == GL_POLYGON ? &p[0] : &p[2]);
                p[1] = p[2];
                polygon_count = 2;
            }
            break;
    }
}

// ---- frame: uniform blocks and commands, sent together at the swap ----

#define MAX_COMMANDS 64
#define MAX_UNIFORMS 64
static uint32_t commands[MAX_COMMANDS][16];
static fx uniforms[MAX_UNIFORMS][10][4];
static uint32_t command_count, uniform_count, vertex_slots;
static uint32_t frame_vertices;   // loose geometry of this frame, parked above the display lists

static uint32_t* command(uint32_t op, uint32_t vertices) {
    static uint32_t spare[16];
    if (command_count == MAX_COMMANDS) return spare;
    uint32_t* c = commands[command_count++];
    memset(c, 0, sizeof commands[0]);
    c[0] = op;
    c[3] = vertex_slots;
    vertex_slots += vertices;
    return c;
}

// A draw of `count` vertices at `address` with the current matrices and the given material.
static void draw(uint32_t address, uint32_t count, const fx* colour, int lit) {
    if (uniform_count == MAX_UNIFORMS || count == 0) return;
    fx (*u)[4] = uniforms[uniform_count];
    const mat* mv = &modelview[modelview_top];
    // the GPU multiplies the two: rows 0-3 are the projection, 4-6 the modelview's first three
    memcpy(u, projection[projection_top].m, sizeof(mat));
    memcpy(u + 4, mv->m, 3 * sizeof mv->m[0]);
    for (int i = 0; i < 3; i++) {
        u[7][i] = light_direction[i];
        u[8][i] = colour[i];              // light 0's diffuse is white
        u[9][i] = mul(colour[i], 13107);  // the default scene ambient, 0.2
    }
    u[7][3] = u[8][3] = u[9][3] = 0;
    uint32_t* c = command(CMD_DRAW, count);
    c[1] = GPU_PHYS + address;
    c[2] = count;
    c[4] = (lit ? VERTEX_LIT : VERTEX_CLIP) | VERTEX_MODELVIEW;
    c[6] = GPU_PHYS + frame_at + frame_flip + uniform_count * sizeof uniforms[0];
    uniform_count++;
}

// Closes the run of triangles recorded since the last material change.
static void close_segment(display_list* list) {
    if (batch_count == segment_start || list->part_count == MAX_SEGMENTS) return;
    segment* s = &list->parts[list->part_count++];
    s->address = vertex_top + segment_start * sizeof(vertex);
    s->count = batch_count - segment_start;
    memcpy(s->material, lighting ? material : current_colour, sizeof s->material);
    s->lit = lighting;
    segment_start = batch_count;
}

void glMaterialfv(GLenum face, GLenum pname, const GLfloat* p) {
    (void)face;
    if (pname != GL_AMBIENT_AND_DIFFUSE && pname != GL_DIFFUSE) return;
    if (open_list) close_segment(&lists[open_list]);
    for (int i = 0; i < 3; i++) material[i] = from_float(p[i]);
}

void glEnd(void) {
    if (open_list) return;
    // Loose geometry: uploaded now and drawn with the matrices of this moment.
    uint32_t address = vertex_top + frame_vertices * sizeof(vertex);
    if (batch_count == 0 || address + batch_count * sizeof(vertex) > GPU_SIZE) return;
    gpu_write(address, batch, batch_count * sizeof(vertex));
    draw(address, batch_count, lighting ? material : current_colour, lighting);
    frame_vertices += batch_count;
    batch_count = 0;
}

GLuint glGenLists(GLsizei range) {
    GLuint first = next_list;
    if (first + (GLuint)range > MAX_LISTS) return 0;
    next_list += (GLuint)range;
    return first;
}

void glNewList(GLuint list, GLenum mode) {
    (void)mode;
    if (list == 0 || list >= MAX_LISTS) return;
    open_list = list;
    lists[list].part_count = 0;
    batch_count = segment_start = 0;
}

void glEndList(void) {
    if (!open_list) return;
    close_segment(&lists[open_list]);
    if (vertex_top + batch_count * sizeof(vertex) > GPU_SIZE) die("out of GPU memory for display lists");
    gpu_write(vertex_top, batch, batch_count * sizeof(vertex));
    vertex_top += batch_count * sizeof(vertex);
    batch_count = 0;
    open_list = 0;
}

void glCallList(GLuint list) {
    if (list == 0 || list >= MAX_LISTS) return;
    for (int i = 0; i < lists[list].part_count; i++) {
        const segment* s = &lists[list].parts[i];
        draw(s->address, s->count, s->material, s->lit);
    }
}

void glDeleteLists(GLuint list, GLsizei range) {
    for (GLuint i = list; i < list + (GLuint)range && i < MAX_LISTS; i++) lists[i].part_count = 0;
}

void glClear(GLbitfield mask) {
    if (mask & GL_COLOR_BUFFER_BIT) command(CMD_CLEAR, 3)[1] = clear_colour;
}

// ---- GLX and Xlib: one window, which is the display or a Nano-X window ----

struct x_display { int width, height; };
static struct x_display the_display = {1280, 720};
static Visual the_visual;
static XVisualInfo the_visual_info = {&the_visual, 0, 0, 24};

#ifdef GL_NANOX
#define REG_INTO 0x50            // where the picture is copied: address, width, height, row
#define REG_LOCK 0x60            // taken with an atomic swap by whoever writes a new list's registers
#define REG_CLOCK 0x34           // the host's clock in milliseconds
// The pause hint: this machine ends its frame there, which is when the GPU does its work.
#define next_frame() __asm__ volatile(".word 0x0100000f")

static int nano_x;               // a Nano-X server is running: be one of its clients
static GR_WINDOW_ID nano_window;
static GR_WINDOW_INFO nano_info; // where the window's pixels are; asked again when the window changes
static int nano_info_stale = 1;
static GR_EVENT nano_event;      // the event XPending found, for XNextEvent
static int nano_pending, nano_asked, nano_socket, nano_got;
static fd_set nano_set;

// One Nano-X event as the X event glxgears understands, or 0 for one it has no use for.
static int translate(const GR_EVENT* in, XEvent* out) {
    switch (in->type) {
        case GR_EVENT_TYPE_EXPOSURE:
            out->type = Expose;
            return 1;
        case GR_EVENT_TYPE_UPDATE:
            nano_info_stale = 1;   // moved, resized, mapped: the pixels may be elsewhere
            if (in->update.utype != GR_UPDATE_SIZE) return 0;
            out->type = ConfigureNotify;
            out->xconfigure.width = in->update.width;
            out->xconfigure.height = in->update.height;
            return 1;
        case GR_EVENT_TYPE_KEY_DOWN:
            out->type = KeyPress;
            out->xkey.keycode = in->keystroke.ch;
            return 1;
        case GR_EVENT_TYPE_CLOSE_REQ:
            GrClose();
            exit(0);
    }
    return 0;
}

// The time of day from the machine's clock word instead of a system call per frame. It moves
// once per emulator frame, which is as often as anything a program draws can be shown.
int gettimeofday(struct timeval* restrict tv, void* restrict tz) {
    (void)tz;
    if (nano_x && gpu_map) {
        uint32_t ms = *(volatile uint32_t*)(gpu_map + REG_CLOCK);
        tv->tv_sec = ms / 1000;
        tv->tv_usec = (ms % 1000) * 1000;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        tv->tv_sec = ts.tv_sec;
        tv->tv_usec = ts.tv_nsec / 1000;
    }
    return 0;
}

static void nano_take(GR_EVENT* event) {
    nano_event = *event;
    nano_got = 1;
}

// Whether an event has arrived. The server is asked for the next event once and answers
// when there is one, so looking is one select() on the socket and no round trip.
static int nano_poll(int wait) {
    struct timeval none = {0, 0};
    if (!nano_asked) {
        FD_ZERO(&nano_set);
        nano_socket = 0;
        GrPrepareSelect(&nano_socket, &nano_set);
        nano_asked = 1;
    }
    fd_set ready = nano_set;
    if (select(nano_socket + 1, &ready, 0, 0, wait ? 0 : &none) <= 0) return 0;
    nano_asked = nano_got = 0;
    GrServiceSelect(&ready, nano_take);
    return nano_got;
}
#endif

Display* XOpenDisplay(const char* name) {
    (void)name;
    gpu_open();
#ifdef GL_NANOX
    nano_x = gpu_map && GrOpen() >= 0;
    if (nano_x) {
        frame_at = 0x00700000u;
        vertex_top = 0x00780000u;
    }
#endif
    return &the_display;
}
int XCloseDisplay(Display* dpy) { (void)dpy; return 0; }
int XDefaultScreen(Display* dpy) { (void)dpy; return 0; }
Window XRootWindow(Display* dpy, int screen) { (void)dpy; (void)screen; return 1; }
int XDisplayWidth(Display* dpy, int screen) { (void)screen; return dpy->width; }
int XDisplayHeight(Display* dpy, int screen) { (void)screen; return dpy->height; }
Colormap XCreateColormap(Display* dpy, Window w, Visual* visual, int alloc) {
    (void)dpy; (void)w; (void)visual; (void)alloc;
    return 1;
}

// The window's size becomes the display's resolution, and the display switches to the GPU.
Window XCreateWindow(Display* dpy, Window parent, int x, int y, unsigned width, unsigned height, unsigned border, int depth,
                     unsigned cls, Visual* visual, unsigned long mask, XSetWindowAttributes* attr) {
    (void)parent; (void)x; (void)y; (void)border; (void)depth; (void)cls; (void)visual; (void)mask; (void)attr;
    if (width > 2048) width = 2048;
    if (height > 2048) height = 2048;
    dpy->width = (int)width;
    dpy->height = (int)height;
#ifdef GL_NANOX
    if (nano_x) {
        (void)x; (void)y;   // the window manager chooses the place
        nano_window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW | GR_WM_PROPS_NOBACKGROUND, "glxgears", GR_ROOT_WINDOW_ID,
                                    -1, -1, width, height, 0);
        GrSelectEvents(nano_window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_KEY_DOWN |
                                    GR_EVENT_MASK_CLOSE_REQ);
        return 2;
    }
#endif
    uint32_t mode[3] = {3, width, height};
    gpu_write(REG_DISPLAY + 4, &mode[1], 8);   // size first, mode last
    gpu_write(REG_DISPLAY, &mode[0], 4);
    return 2;
}
int XDestroyWindow(Display* dpy, Window w) { (void)dpy; (void)w; return 0; }
int XMapWindow(Display* dpy, Window w) {
    (void)dpy; (void)w;
#ifdef GL_NANOX
    if (nano_x) GrMapWindow(nano_window);
#endif
    return 0;
}
int XSetNormalHints(Display* dpy, Window w, XSizeHints* hints) { (void)dpy; (void)w; (void)hints; return 0; }
int XSetStandardProperties(Display* dpy, Window w, const char* name, const char* icon, XID pixmap, char** argv, int argc,
                           XSizeHints* hints) {
    (void)dpy; (void)w; (void)name; (void)icon; (void)pixmap; (void)argv; (void)argc; (void)hints;
    return 0;
}
Atom XInternAtom(Display* dpy, const char* name, Bool only_if_exists) { (void)dpy; (void)name; (void)only_if_exists; return 0; }
int XChangeProperty(Display* dpy, Window w, Atom property, Atom type, int format, int mode, const unsigned char* data, int n) {
    (void)dpy; (void)w; (void)property; (void)type; (void)format; (void)mode; (void)data; (void)n;
    return 0;
}
int XFree(void* data) { (void)data; return 0; }
// Without Nano-X no events ever arrive. With it, looking costs a system call, so an
// animating program only looks every few frames.
int XPending(Display* dpy) {
    (void)dpy;
#ifdef GL_NANOX
    static int skipped;
    XEvent unused;
    if (!nano_x || nano_pending) return nano_pending;
    if (++skipped < 6) return 0;
    skipped = 0;
    while (!nano_pending && nano_poll(0)) nano_pending = translate(&nano_event, &unused);
    return nano_pending;
#else
    return 0;
#endif
}
int XNextEvent(Display* dpy, XEvent* event) {
    (void)dpy;
    event->type = 0;
#ifdef GL_NANOX
    while (nano_x && !nano_pending)
        if (nano_poll(1)) nano_pending = translate(&nano_event, event);
    if (nano_x) translate(&nano_event, event);
    nano_pending = 0;
#endif
    return 0;
}
KeySym XLookupKeysym(XKeyEvent* event, int index) {
    (void)index;
#ifdef GL_NANOX
    switch (event->keycode) {
        case MWKEY_LEFT: return XK_Left;
        case MWKEY_RIGHT: return XK_Right;
        case MWKEY_UP: return XK_Up;
        case MWKEY_DOWN: return XK_Down;
    }
#else
    (void)event;
#endif
    return 0;
}
int XLookupString(XKeyEvent* event, char* buffer, int bytes, KeySym* keysym, XComposeStatus* status) {
    (void)keysym; (void)status;
    if (bytes < 1 || event->keycode > 0x7f) return 0;
    buffer[0] = (char)event->keycode;
    return 1;
}

// "WIDTHxHEIGHT+X+Y"; only the size matters here.
int XParseGeometry(const char* spec, int* x, int* y, unsigned* width, unsigned* height) {
    (void)x; (void)y;
    int w = atoi(spec);
    const char* sep = spec;
    while (*sep && *sep != 'x' && *sep != 'X') sep++;
    if (w <= 0 || !*sep) return 0;
    *width = (unsigned)w;
    *height = (unsigned)atoi(sep + 1);
    return WidthValue | HeightValue;
}

XVisualInfo* glXChooseVisual(Display* dpy, int screen, int* attribs) { (void)dpy; (void)screen; (void)attribs; return &the_visual_info; }
GLXContext glXCreateContext(Display* dpy, XVisualInfo* vis, GLXContext share, Bool direct) {
    (void)dpy; (void)vis; (void)share; (void)direct;
    return (GLXContext)&the_display;
}
void glXDestroyContext(Display* dpy, GLXContext ctx) { (void)dpy; (void)ctx; }
Bool glXMakeCurrent(Display* dpy, GLXDrawable drawable, GLXContext ctx) { (void)dpy; (void)drawable; (void)ctx; return True; }
const char* glXQueryExtensionsString(Display* dpy, int screen) { (void)dpy; (void)screen; return ""; }
void glXQueryDrawable(Display* dpy, GLXDrawable drawable, int attribute, unsigned int* value) {
    (void)dpy; (void)drawable; (void)attribute;
    *value = 0;
}
void (*glXGetProcAddressARB(const GLubyte* name))(void) { (void)name; return 0; }

// Without the driver: the list is drawn between the emulator's frames, so there is nothing to
// do until this one ends, and yielding in a loop is the best a process can do. A sleep is
// rounded up to the kernel's 50 ms tick (GLWAIT=sleep shows it: 20 frames/s against 80), and
// wfi, which would end the frame at once, is refused in user mode.
static void wait_for_frame(void) {
    static int mode = -1;
    if (mode < 0) {
        const char* m = getenv("GLWAIT");
        mode = m && strcmp(m, "sleep") == 0;
    }
    if (mode) usleep(100);
    else sched_yield();
}

// Sends the frame: its uniform blocks and command list in one write, then the submit word, and
// waits for the GPU to count it.
void glXSwapBuffers(Display* dpy, GLXDrawable drawable) {
    (void)dpy; (void)drawable;
    static uint32_t frame[(sizeof uniforms + sizeof commands) / 4];
    uint32_t uniform_bytes = uniform_count * sizeof uniforms[0], command_bytes = command_count * sizeof commands[0];
    if (gpu_map) {
        // mapped: the two parts go straight to GPU memory
        memcpy(gpu_map + frame_at + frame_flip, uniforms, uniform_bytes);
        memcpy(gpu_map + frame_at + frame_flip + uniform_bytes, commands, command_bytes);
    } else {
        memcpy(frame, uniforms, uniform_bytes);
        memcpy((char*)frame + uniform_bytes, commands, command_bytes);
        gpu_write(frame_at + frame_flip, frame, uniform_bytes + command_bytes);
    }
#ifdef GL_NANOX
    if (nano_x) {
        volatile uint32_t* regs = (volatile uint32_t*)gpu_map;
        if (nano_info_stale) {
            fd_set none;
            GrGetWindowInfo(nano_window, &nano_info);
            nano_info_stale = 0;
            // an event that arrived with the answer is waiting inside the library
            FD_ZERO(&none);
            nano_got = 0;
            GrServiceSelect(&none, nano_take);
            if (nano_got) {
                XEvent unused;
                nano_asked = 0;
                nano_pending = translate(&nano_event, &unused);
            }
        }
        if (nano_info.realized && nano_info.surface_address) {
            // Hand the list to the GPU, to be drawn into the window's part of its buffer, and
            // go on to the next frame. Only wait while the GPU still has the last one.
            for (;;) {
                while (__atomic_exchange_n(&regs[REG_LOCK / 4], 1, __ATOMIC_ACQUIRE)) sched_yield();
                if (regs[REG_SUBMIT / 4] == 0) break;
                __atomic_store_n(&regs[REG_LOCK / 4], 0, __ATOMIC_RELEASE);
                next_frame();
            }
            regs[REG_INTO / 4] = nano_info.surface_address;
            regs[REG_INTO / 4 + 1] = nano_info.width;
            regs[REG_INTO / 4 + 2] = nano_info.height;
            regs[REG_INTO / 4 + 3] = nano_info.surface_row;
            regs[REG_SUBMIT / 4 + 1] = GPU_PHYS + frame_at + frame_flip + uniform_bytes;
            regs[REG_SUBMIT / 4 + 2] = command_count;
            regs[REG_SUBMIT / 4] = 1 | 4;
            __atomic_store_n(&regs[REG_LOCK / 4], 0, __ATOMIC_RELEASE);
            frame_flip ^= FRAME_FLIP;
        } else {
            sched_yield();
        }
        command_count = uniform_count = vertex_slots = frame_vertices = 0;
        return;
    }
#endif
    uint32_t before = gpu_frames(), submit[3] = {1, GPU_PHYS + frame_at + uniform_bytes, command_count};
    gpu_write(REG_SUBMIT, submit, sizeof submit);
    do {
        if (gpu_map) ioctl(gpu_fd, GPU_WAIT, (unsigned long)before);
        else wait_for_frame();
    } while (gpu_frames() == before);
    command_count = uniform_count = vertex_slots = frame_vertices = 0;
}
