// A small OpenGL 1.x driver for the machine's GPU device (docs/gpu.md), with the GLX and Xlib
// calls a program needs to open its window. Display lists become vertex buffers in GPU memory;
// calling one becomes a lit draw command with the current matrices as uniforms; swapping
// buffers submits the frame's command list.
//
// Linux has no /dev/mem here, so GPU memory (physical 0x87000000, kept out of the kernel's RAM
// by the device tree) is reached as an MTD device the phram driver makes for that range.
// Everything inside works in 16.16 fixed point, which is also what the GPU reads; floats are
// converted at the API boundary, because the CPU has no FPU.

#include <GL/glx.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- GPU memory ----

#define GPU_PHYS 0x87000000u
#define GPU_SIZE 0x00b00000u
#define FRAME_AT 0x00100000u    // this frame's uniform blocks, then its command list
#define VERTEX_AT 0x00200000u   // vertex buffers of display lists, to the end
#define REG_DISPLAY 0x00        // mode, width, height
#define REG_SUBMIT 0x10         // submit, list address, command count
#define REG_FRAMES 0x1c

enum { CMD_CLEAR = 1, CMD_RECT = 2, CMD_DRAW = 3 };
enum { VERTEX_CLIP = 1, VERTEX_LIT = 2 };

static int gpu_fd = -1;
static uint32_t vertex_top = VERTEX_AT;

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
    if (pwrite(gpu_fd, data, bytes, offset) != (long)bytes) die("write to GPU memory failed");
}

// ---- fixed point ----

typedef int32_t fx;
#define ONE 65536
static inline fx mul(fx a, fx b) { return (fx)(((int64_t)a * b) >> 16); }
static inline fx from_float(float f) { return (fx)(f * 65536.0f); }

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

void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
    mat t = identity;
    t.m[0][3] = from_float(x);
    t.m[1][3] = from_float(y);
    t.m[2][3] = from_float(z);
    apply(&t);
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
    mat clip = multiply(&projection[projection_top], mv);
    memcpy(u, clip.m, sizeof clip.m);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) u[4 + i][j] = mv->m[i][j];
        u[4 + i][3] = 0;
        u[7][i] = light_direction[i];
        u[8][i] = colour[i];              // light 0's diffuse is white
        u[9][i] = mul(colour[i], 13107);  // the default scene ambient, 0.2
    }
    u[7][3] = u[8][3] = u[9][3] = 0;
    uint32_t* c = command(CMD_DRAW, count);
    c[1] = GPU_PHYS + address;
    c[2] = count;
    c[4] = lit ? VERTEX_LIT : VERTEX_CLIP;
    c[6] = GPU_PHYS + FRAME_AT + uniform_count * sizeof uniforms[0];
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

// ---- GLX and Xlib: one window, which is the display ----

struct x_display { int width, height; };
static struct x_display the_display = {1280, 720};
static Visual the_visual;
static XVisualInfo the_visual_info = {&the_visual, 0, 0, 24};

Display* XOpenDisplay(const char* name) {
    (void)name;
    gpu_open();
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
    uint32_t mode[3] = {3, width, height};
    gpu_write(REG_DISPLAY + 4, &mode[1], 8);   // size first, mode last
    gpu_write(REG_DISPLAY, &mode[0], 4);
    return 2;
}
int XDestroyWindow(Display* dpy, Window w) { (void)dpy; (void)w; return 0; }
int XMapWindow(Display* dpy, Window w) { (void)dpy; (void)w; return 0; }
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
int XPending(Display* dpy) { (void)dpy; return 0; }   // no events ever arrive
int XNextEvent(Display* dpy, XEvent* event) { (void)dpy; event->type = 0; return 0; }
KeySym XLookupKeysym(XKeyEvent* event, int index) { (void)event; (void)index; return 0; }
int XLookupString(XKeyEvent* event, char* buffer, int bytes, KeySym* keysym, XComposeStatus* status) {
    (void)event; (void)buffer; (void)bytes; (void)keysym; (void)status;
    return 0;
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

// Sends the frame: its uniform blocks and command list in one write, then the submit word, and
// waits for the GPU to count it.
void glXSwapBuffers(Display* dpy, GLXDrawable drawable) {
    (void)dpy; (void)drawable;
    static uint32_t frame[(sizeof uniforms + sizeof commands) / 4];
    uint32_t uniform_bytes = uniform_count * sizeof uniforms[0], command_bytes = command_count * sizeof commands[0];
    memcpy(frame, uniforms, uniform_bytes);
    memcpy((char*)frame + uniform_bytes, commands, command_bytes);
    gpu_write(FRAME_AT, frame, uniform_bytes + command_bytes);
    uint32_t before, now, submit[3] = {1, GPU_PHYS + FRAME_AT + uniform_bytes, command_count};
    pread(gpu_fd, &before, 4, REG_FRAMES);
    gpu_write(REG_SUBMIT, submit, sizeof submit);
    do {
        sched_yield();   // the list is drawn between the emulator's frames
        pread(gpu_fd, &now, 4, REG_FRAMES);
    } while (now == before);
    command_count = uniform_count = vertex_slots = frame_vertices = 0;
}
