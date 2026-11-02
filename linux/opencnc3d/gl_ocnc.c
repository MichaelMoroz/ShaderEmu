// SPDX-License-Identifier: GPL-3.0-or-later
// ShaderEmu: the fixed-function OpenGL OpenCNC3D's renderer calls, on the machine's GPU: Half-Life's
// (linux/halflife/gl_shaderemu.c, included as it is, as linux/jk2/gl_jk2.c does) over Quake III's draws.
// build.sh renames every glX the program's objects call to ocnc_glX, so that gles.c's own names (which the
// shim is built on) never meet them; each ocnc_glX goes to the shim's s_glX. What the shim does not have
// is listed in docs/games/opencnc3d.md ("GL") and says so here once, the first time it is called.
#include "../halflife/gl_shaderemu.c"

// what gl_shaderemu.c asks of Half-Life's drawing on a worker which this program has not
int SE_RenderWho(void) { return 0; }
int SE_RenderOut(void) { return 0; }
uint32_t SE_RenderOnCore0(uint32_t (*fn)(void*), void* args) { return fn(args); }
// (se_asks, which gl_shaderemu.c names in a debugging print, is defined in brain_link.c)

typedef unsigned GLenum_;
static int unit;   // glActiveTexture: what the shim draws is unit 0's; the others' state is not kept

static void lacking(const char* name, int* said) {
    if (*said) return;
    *said = 1;
    if (getenv("OPENCNC3D_GL_LOG")) fprintf(stderr, "ocnc gl: %s is not drawn by the machine's GL\n", name);
}
#define LACK(name) do { static int said; lacking(#name, &said); } while (0)

int ocnc_gl_open(unsigned window) {
    if (!getenv("HLGL_WORKERS")) setenv("HLGL_WORKERS", "0", 0);
    if (!getenv("HL_POOL_MB")) setenv("HL_POOL_MB", "24", 0);
    if (hlglOpen(window) < 0) return -1;
    seglWorldUp(2);   // y
    return 0;
}
void ocnc_gl_swap(void) { hlglSwap(); }
void ocnc_gl_size(int* w, int* h) { seglSize(w, h); }

// ---- the calls ----
void ocnc_glActiveTexture(GLenum_ t) { unit = (int)(t - 0x84C0); }
void ocnc_glClientActiveTexture(GLenum_ t) { (void)t; }
void ocnc_glAlphaFunc(GLenum_ f, float r) { (void)f, (void)r; }   // the shim keys texels it would drop
void ocnc_glBegin(GLenum_ m) { s_glBegin(m); }
void ocnc_glEnd(void) { s_glEnd(); }
void ocnc_glBindTexture(GLenum_ t, unsigned n) { if (!unit) s_glBindTexture(t, n); }
void ocnc_glBlendFunc(GLenum_ s, GLenum_ d) { s_glBlendFunc(s, d); }
void ocnc_glClear(unsigned m) { s_glClear(m); }
void ocnc_glClearColor(float r, float g, float b, float a) { s_glClearColor(r, g, b, a); }
void ocnc_glClearDepth(double d) { (void)d; }
void ocnc_glClipPlane(GLenum_ p, const double* e) { (void)p, (void)e; LACK(glClipPlane); }
void ocnc_glColor3f(float r, float g, float b) { s_glColor3f(r, g, b); }
void ocnc_glColor3ub(unsigned char r, unsigned char g, unsigned char b) { s_glColor3ub(r, g, b); }
void ocnc_glColor3ubv(const unsigned char* v) { s_glColor3ub(v[0], v[1], v[2]); }
void ocnc_glColor4f(float r, float g, float b, float a) { s_glColor4f(r, g, b, a); }
void ocnc_glColor4fv(const float* v) { s_glColor4fv(v); }
void ocnc_glColor4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { s_glColor4ub(r, g, b, a); }
void ocnc_glColor4ubv(const unsigned char* v) { s_glColor4ubv(v); }
void ocnc_glColorMask(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { (void)r, (void)g, (void)b, (void)a; LACK(glColorMask); }
void ocnc_glColorPointer(int s, GLenum_ t, int st, const void* p) { s_glColorPointer(s, t, st, p); }
void ocnc_glCopyTexSubImage2D(GLenum_ t, int l, int x, int y, int a, int b, int w, int h) { (void)t, (void)l, (void)x, (void)y, (void)a, (void)b, (void)w, (void)h; LACK(glCopyTexSubImage2D); }
void ocnc_glCullFace(GLenum_ m) { (void)m; }
void ocnc_glDeleteTextures(int n, const unsigned* t) { s_glDeleteTextures(n, t); }
void ocnc_glDepthFunc(GLenum_ f) { (void)f; }
void ocnc_glDepthMask(unsigned char m) { (void)m; }
void ocnc_glDisable(GLenum_ w) { if (!unit) s_glDisable(w); }
void ocnc_glDisableClientState(GLenum_ w) { s_glDisableClientState(w); }
void ocnc_glDrawArrays(GLenum_ m, int f, int n) { s_glDrawArrays(m, f, n); }
void ocnc_glDrawBuffer(GLenum_ b) { (void)b; }
void ocnc_glDrawElements(GLenum_ m, int n, GLenum_ t, const void* i) { s_glDrawElements(m, n, t, i); }
void ocnc_glEnable(GLenum_ w) { if (!unit) s_glEnable(w); }
void ocnc_glEnableClientState(GLenum_ w) { s_glEnableClientState(w); }
void ocnc_glFinish(void) {}
void ocnc_glFlush(void) {}
void ocnc_glFrontFace(GLenum_ m) { (void)m; }
void ocnc_glFrustum(double l, double r, double b, double t, double n, double f) { s_glFrustum(l, r, b, t, n, f); }
void ocnc_glGenTextures(int n, unsigned* t) { s_glGenTextures(n, t); }
void ocnc_glGetBooleanv(GLenum_ w, unsigned char* o) { s_glGetBooleanv(w, o); }
unsigned ocnc_glGetError(void) { return 0; }
void ocnc_glGetFloatv(GLenum_ w, float* o) { s_glGetFloatv(w, o); }
void ocnc_glGetIntegerv(GLenum_ w, int* o) { s_glGetIntegerv(w, o); }
const unsigned char* ocnc_glGetString(GLenum_ w) { return s_glGetString(w); }
void ocnc_glGetTexEnvfv(GLenum_ t, GLenum_ n, float* o) { (void)t, (void)n; o[0] = o[1] = o[2] = o[3] = 0; }
void ocnc_glGetTexEnviv(GLenum_ t, GLenum_ n, int* o) { (void)t, o[0] = n == GL_TEXTURE_ENV_MODE ? (replace ? GL_REPLACE : GL_MODULATE) : 0; }
void ocnc_glGetTexImage(GLenum_ t, int l, GLenum_ f, GLenum_ ty, void* p) { (void)t, (void)l, (void)f, (void)ty, (void)p; LACK(glGetTexImage); }
void ocnc_glGetTexLevelParameteriv(GLenum_ t, int l, GLenum_ n, int* o) {
    (void)t, (void)l;
    const texture* x = bound ? &textures[bound] : 0;
    o[0] = !x ? 0 : n == 0x1000 ? x->width : n == 0x1001 ? x->height : 0;   // GL_TEXTURE_WIDTH, _HEIGHT
}
unsigned char ocnc_glIsEnabled(GLenum_ w) { return s_glIsEnabled(w); }
unsigned char ocnc_glIsTexture(unsigned t) { return s_glIsTexture(t); }
void ocnc_glLineWidth(float w) { (void)w; }
void ocnc_glLoadIdentity(void) { s_glLoadIdentity(); }
void ocnc_glLoadMatrixf(const float* m) { s_glLoadMatrixf(m); }
void ocnc_glMatrixMode(GLenum_ m) { if (m != 0x1702) s_glMatrixMode(m); }   // (not the texture matrix)
void ocnc_glMultMatrixf(const float* m) { s_glMultMatrixf(m); }
void ocnc_glMultiTexCoord2f(GLenum_ t, float s, float r) { if (t == 0x84C0) s_glTexCoord2f(s, r); }
void ocnc_glMultiTexCoord3f(GLenum_ t, float s, float r, float q) { if (t == 0x84C0) s_glTexCoord2f(s, r); (void)q; }
void ocnc_glNormal3f(float x, float y, float z) { (void)x, (void)y, (void)z; }
void ocnc_glNormal3fv(const float* v) { (void)v; }
void ocnc_glNormalPointer(GLenum_ t, int s, const void* p) { (void)t, (void)s, (void)p; }
void ocnc_glOrtho(double l, double r, double b, double t, double n, double f) { s_glOrtho(l, r, b, t, n, f); }
void ocnc_glPixelStorei(GLenum_ n, int v) { (void)n, (void)v; }
void ocnc_glPolygonOffset(float f, float u) { s_glPolygonOffset(f, u); }
void ocnc_glPushAttrib(unsigned m) { (void)m; LACK(glPushAttrib); }
void ocnc_glPopAttrib(void) {}
void ocnc_glPushClientAttrib(unsigned m) { (void)m; }
void ocnc_glPopClientAttrib(void) {}
void ocnc_glPopMatrix(void) { s_glPopMatrix(); }
void ocnc_glPushMatrix(void) { s_glPushMatrix(); }
void ocnc_glReadBuffer(GLenum_ b) { (void)b; }
void ocnc_glReadPixels(int x, int y, int w, int h, GLenum_ f, GLenum_ t, void* p) {
    (void)x, (void)y, (void)f, (void)t;
    LACK(glReadPixels);
    if (p) memset(p, 0, (size_t)w * h * 4);
}
void ocnc_glRotatef(float d, float x, float y, float z) { s_glRotatef(d, x, y, z); }
void ocnc_glScalef(float x, float y, float z) { s_glScalef(x, y, z); }
void ocnc_glScissor(int x, int y, int w, int h) { (void)x, (void)y, (void)w, (void)h; LACK(glScissor); }
void ocnc_glShadeModel(GLenum_ m) { (void)m; }
void ocnc_glTexCoord2f(float s, float t) { s_glTexCoord2f(s, t); }
void ocnc_glTexCoord2fv(const float* v) { s_glTexCoord2fv(v); }
void ocnc_glTexCoordPointer(int s, GLenum_ t, int st, const void* p) { s_glTexCoordPointer(s, t, st, p); }
void ocnc_glTexEnvf(GLenum_ t, GLenum_ n, float v) { if (!unit) s_glTexEnvf(t, n, v); }
void ocnc_glTexEnvfv(GLenum_ t, GLenum_ n, const float* v) { if (!unit) s_glTexEnvfv(t, n, v); }
void ocnc_glTexEnvi(GLenum_ t, GLenum_ n, int v) { if (!unit) s_glTexEnvi(t, n, v); }
void ocnc_glTexImage2D(GLenum_ t, int l, int i, int w, int h, int b, GLenum_ f, GLenum_ ty, const void* p) {
    if (!unit) s_glTexImage2D(t, l, i, w, h, b, f, ty, p);
}
void ocnc_glTexParameterf(GLenum_ t, GLenum_ n, float v) { if (!unit) s_glTexParameterf(t, n, v); }
void ocnc_glTexParameteri(GLenum_ t, GLenum_ n, int v) { if (!unit) s_glTexParameteri(t, n, v); }
void ocnc_glTexSubImage2D(GLenum_ t, int l, int x, int y, int w, int h, GLenum_ f, GLenum_ ty, const void* p) {
    if (!unit) s_glTexSubImage2D(t, l, x, y, w, h, f, ty, p);
}
void ocnc_glTranslated(double x, double y, double z) { s_glTranslated(x, y, z); }
void ocnc_glTranslatef(float x, float y, float z) { s_glTranslatef(x, y, z); }
void ocnc_glVertex2f(float x, float y) { s_glVertex2f(x, y); }
void ocnc_glVertex2i(int x, int y) { s_glVertex2f((float)x, (float)y); }
void ocnc_glVertex3f(float x, float y, float z) { s_glVertex3f(x, y, z); }
void ocnc_glVertex3fv(const float* v) { s_glVertex3fv(v); }
void ocnc_glVertexPointer(int s, GLenum_ t, int st, const void* p) { s_glVertexPointer(s, t, st, p); }
void ocnc_glViewport(int x, int y, int w, int h) { s_glViewport(x, y, w, h); }

// SDL_GL_GetProcAddress: the entry points past 1.1 a program asks for by name
void* ocnc_gl_proc(const char* name) {
    static const struct { const char* name; void* fn; } past[] = {
        {"glActiveTexture", (void*)ocnc_glActiveTexture}, {"glActiveTextureARB", (void*)ocnc_glActiveTexture},
        {"glClientActiveTexture", (void*)ocnc_glClientActiveTexture}, {"glClientActiveTextureARB", (void*)ocnc_glClientActiveTexture},
        {"glMultiTexCoord2f", (void*)ocnc_glMultiTexCoord2f}, {"glMultiTexCoord2fARB", (void*)ocnc_glMultiTexCoord2f},
        {"glMultiTexCoord3f", (void*)ocnc_glMultiTexCoord3f},
    };
    for (unsigned i = 0; i < sizeof past / sizeof past[0]; i++)
        if (!strcmp(past[i].name, name)) return past[i].fn;
    if (getenv("OPENCNC3D_GL_LOG")) fprintf(stderr, "ocnc gl: no %s\n", name);
    return 0;   // (no shaders, no framebuffers: tier 1)
}
