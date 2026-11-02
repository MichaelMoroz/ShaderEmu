// The OpenGL 1.1 OpenJK's renderer (code/rd-vanilla) calls, on the machine's GPU (docs/games/jk2.md, "The picture"):
// Half-Life's (linux/halflife/gl_shaderemu.c, included as it is) over Quake III's draws (linux/quake3/gpu_shaderemu.c).
// build.sh renames the renderer's calls glX to jk2_glX; here each goes to that file's s_glX once the window is open
// (jk2glOpen), and before that, or with no window at all (a run with no Nano-X server: the sums), does what nullgl.c
// does, so that the game is the same with a picture or without.
#include "../halflife/gl_shaderemu.c"

// what gl_shaderemu.c asks of Half-Life's drawing on a worker, which this game does not have
int SE_RenderWho(void) { return 0; }
int SE_RenderOut(void) { return 0; }
uint32_t SE_RenderOnCore0(uint32_t (*fn)(void*), void* args) { return fn(args); }
// (se_asks, which gl_shaderemu.c names in a block of its own, is defined in sys_shaderemu.cpp)

static int on;   // the window is open and the GPU draws
int jk2glOn(void) { return on; }

int jk2glOpen(unsigned window) {
    if (on) return 0;
    if (!getenv("HLGL_WORKERS")) setenv("HLGL_WORKERS", "0", 0);   // (none yet: textures are converted by this core)
    if (!getenv("HL_POOL_MB")) setenv("HL_POOL_MB", "36", 0);   // (the demo's level and menus at r_picmip 2: 31.6 MB)
    if (hlglOpen(window) < 0) return -1;
    on = 1;
    return 0;
}
void jk2glSwap(void) {
    if (on) hlglSwap();
}
void jk2glStats(unsigned* in_gpu, unsigned* gpu_size, unsigned* left_out, unsigned* vertices) {
    unsigned tiles;
    if (on) hlglStats(&tiles, in_gpu, gpu_size, left_out, vertices);
    else *in_gpu = *gpu_size = *left_out = *vertices = 0;
}

typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef unsigned char GLubyte;

#define ON_THEN(call) do { if (on) call; } while (0)
void jk2_glAlphaFunc(GLenum f, GLclampf r) { (void)f, (void)r; }
void jk2_glArrayElement(GLint i) { ON_THEN(s_glArrayElement(i)); }
void jk2_glBegin(GLenum m) { ON_THEN(s_glBegin(m)); }
void jk2_glEnd(void) { ON_THEN(s_glEnd()); }
void jk2_glBindTexture(GLenum t, GLuint n) { ON_THEN(s_glBindTexture(t, n)); }
void jk2_glBlendFunc(GLenum s, GLenum d) { ON_THEN(s_glBlendFunc(s, d)); }
void jk2_glCallList(GLuint l) { (void)l; }
void jk2_glClear(GLbitfield m) { ON_THEN(s_glClear(m)); }
void jk2_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { ON_THEN(s_glClearColor(r, g, b, a)); }
void jk2_glClearDepth(GLclampd d) { (void)d; }
void jk2_glClearStencil(GLint s) { (void)s; }
void jk2_glClipPlane(GLenum p, const GLdouble* e) { (void)p, (void)e; }
void jk2_glColor3f(GLfloat r, GLfloat g, GLfloat b) { ON_THEN(s_glColor3f(r, g, b)); }
void jk2_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { ON_THEN(s_glColor4f(r, g, b, a)); }
void jk2_glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { ON_THEN(s_glColor4ub(r, g, b, a)); }
void jk2_glColor4ubv(const GLubyte* v) { ON_THEN(s_glColor4ubv(v)); }
void jk2_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { (void)r, (void)g, (void)b, (void)a; }
void jk2_glColorPointer(GLint size, GLenum type, GLsizei stride, const void* p) { ON_THEN(s_glColorPointer(size, type, stride, p)); }
void jk2_glCopyTexImage2D(GLenum t, GLint l, GLenum f, GLint x, GLint y, GLsizei w, GLsizei h, GLint b) {}
void jk2_glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {}
void jk2_glCullFace(GLenum m) { (void)m; }
void jk2_glDeleteLists(GLuint l, GLsizei r) { (void)l, (void)r; }
void jk2_glDeleteTextures(GLsizei n, const GLuint* t) { ON_THEN(s_glDeleteTextures(n, t)); }
void jk2_glDepthFunc(GLenum f) { (void)f; }
void jk2_glDepthMask(GLboolean f) { (void)f; }
void jk2_glDepthRange(GLclampd n, GLclampd f) { (void)n, (void)f; }
void jk2_glDisable(GLenum c) { ON_THEN(s_glDisable(c)); }
void jk2_glEnable(GLenum c) { ON_THEN(s_glEnable(c)); }
void jk2_glDisableClientState(GLenum a) { ON_THEN(s_glDisableClientState(a)); }
void jk2_glEnableClientState(GLenum a) { ON_THEN(s_glEnableClientState(a)); }
void jk2_glDrawArrays(GLenum m, GLint first, GLsizei count) { ON_THEN(s_glDrawArrays(m, first, count)); }
void jk2_glDrawBuffer(GLenum m) { (void)m; }
// (gl_shaderemu.c's primitive holds MAX_PRIMITIVE vertices: the renderer's triangles in pieces of whole triangles)
void jk2_glDrawElements(GLenum m, GLsizei count, GLenum type, const void* indices) {
    if (!on) return;
    if (m != GL_TRIANGLES || count <= MAX_PRIMITIVE) {
        s_glDrawElements(m, count, type, indices);
        return;
    }
    int size = type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 1, piece = MAX_PRIMITIVE / 3 * 3;
    for (int done = 0; done < count; done += piece)
        s_glDrawElements(m, count - done < piece ? count - done : piece, type, (const char*)indices + (size_t)done * size);
}
void jk2_glEndList(void) {}
void jk2_glNewList(GLuint l, GLenum m) { (void)l, (void)m; }
void jk2_glFinish(void) {}
static GLuint lists = 1;
GLuint jk2_glGenLists(GLsizei range) {
    GLuint first = lists;
    lists += range;
    return first;
}
GLenum jk2_glGetError(void) { return 0; }
void jk2_glGetFloatv(GLenum name, GLfloat* v) {
    if (on) s_glGetFloatv(name, v);
    else memset(v, 0, 16 * sizeof(GLfloat));
}
void jk2_glGetIntegerv(GLenum name, GLint* v) {
    switch (name) {
    case 0x84E2: *v = 1; return;              // GL_MAX_TEXTURE_UNITS_ARB: one, the renderer's plain paths
    case 0x0D05: case 0x0CF5: *v = 4; return; // GL_PACK_ALIGNMENT, GL_UNPACK_ALIGNMENT (0 made the screenshot of a save divide by it)
    // GL_MAX_TEXTURE_SIZE: 512 on the GPU (the menus' and loading screens' pictures of 1,024 were 12 of its 24 MB, and
    // the window is 640 wide); without a window as nullgl.c
    case 0x0D33: *v = on ? 512 : 2048; return;
    }
    if (on) s_glGetIntegerv(name, v);
    else if (name == 0x0BA2) v[0] = v[1] = 0, v[2] = 640, v[3] = 480;
    else *v = 0;
}
const GLubyte* jk2_glGetString(GLenum name) {
    switch (name) {
    case 0x1F00: return (const GLubyte*)"ShaderEmu";
    case 0x1F01: return (const GLubyte*)(on ? "the machine's GPU" : "null");
    case 0x1F02: return (const GLubyte*)"1.1";
    default: return (const GLubyte*)"";   // no extension: the renderer takes its plain paths
    }
}
GLboolean jk2_glIsEnabled(GLenum c) { return on ? s_glIsEnabled(c) : 0; }
void jk2_glLineWidth(GLfloat w) { (void)w; }
void jk2_glLoadIdentity(void) { ON_THEN(s_glLoadIdentity()); }
void jk2_glLoadMatrixf(const GLfloat* m) { ON_THEN(s_glLoadMatrixf(m)); }
void jk2_glMatrixMode(GLenum m) { ON_THEN(s_glMatrixMode(m)); }
void jk2_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) { ON_THEN(s_glOrtho(l, r, b, t, n, f)); }
void jk2_glPolygonMode(GLenum f, GLenum m) { (void)f, (void)m; }
void jk2_glPolygonOffset(GLfloat f, GLfloat u) { ON_THEN(s_glPolygonOffset(f, u)); }
void jk2_glPopMatrix(void) { ON_THEN(s_glPopMatrix()); }
void jk2_glPushMatrix(void) { ON_THEN(s_glPushMatrix()); }
// a black picture, for a saved game's screenshot (the rows padded to 4 bytes)
void jk2_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
    int bytes = format == 0x1907 ? 3 : 4;
    (void)x, (void)y, (void)type;
    memset(pixels, 0, (size_t)((width * bytes + 3) & ~3) * height);
}
void jk2_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x, (void)y, (void)w, (void)h; }
void jk2_glShadeModel(GLenum m) { (void)m; }
void jk2_glStencilFunc(GLenum f, GLint r, GLuint m) { (void)f, (void)r, (void)m; }
void jk2_glStencilMask(GLuint m) { (void)m; }
void jk2_glStencilOp(GLenum a, GLenum b, GLenum c) { (void)a, (void)b, (void)c; }
void jk2_glTexCoord2f(GLfloat s, GLfloat t) { ON_THEN(s_glTexCoord2f(s, t)); }
void jk2_glTexCoord2fv(const GLfloat* v) { ON_THEN(s_glTexCoord2fv(v)); }
void jk2_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* p) { ON_THEN(s_glTexCoordPointer(size, type, stride, p)); }
void jk2_glTexEnvf(GLenum t, GLenum n, GLfloat v) { ON_THEN(s_glTexEnvf(t, n, v)); }
void jk2_glTexImage2D(GLenum t, GLint l, GLint internal, GLsizei w, GLsizei h, GLint b, GLenum format, GLenum type, const void* p) {
    ON_THEN(s_glTexImage2D(t, l, internal, w, h, b, format, type, p));
}
void jk2_glTexParameterf(GLenum t, GLenum n, GLfloat v) { ON_THEN(s_glTexParameterf(t, n, v)); }
void jk2_glTexParameterfv(GLenum t, GLenum n, const GLfloat* v) { ON_THEN(s_glTexParameterfv(t, n, v)); }
void jk2_glTexParameteri(GLenum t, GLenum n, GLint v) { ON_THEN(s_glTexParameteri(t, n, v)); }
void jk2_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* p) {
    ON_THEN(s_glTexSubImage2D(t, l, x, y, w, h, format, type, p));
}
void jk2_glTranslatef(GLfloat x, GLfloat y, GLfloat z) { ON_THEN(s_glTranslatef(x, y, z)); }
void jk2_glVertex2f(GLfloat x, GLfloat y) { ON_THEN(s_glVertex2f(x, y)); }
void jk2_glVertex3f(GLfloat x, GLfloat y, GLfloat z) { ON_THEN(s_glVertex3f(x, y, z)); }
void jk2_glVertex3fv(const GLfloat* v) { ON_THEN(s_glVertex3fv(v)); }
void jk2_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* p) { ON_THEN(s_glVertexPointer(size, type, stride, p)); }
void jk2_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { ON_THEN(s_glViewport(x, y, w, h)); }
