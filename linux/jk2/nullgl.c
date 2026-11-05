/* The OpenGL 1.1 calls OpenJK's renderer (code/rd-vanilla) makes, doing nothing: the "null" picture of the plan's
   steps 0 and 1 (docs/games/jk2.md), on the build computer and on the machine. Everything the renderer does but
   draw still happens: the level, shaders, images and Ghoul2 models are loaded, the scene is built and sorted, the
   models' bones and vertices are made. What a call answers is what the renderer needs to take its plain paths: no
   extension, one texture unit, textures up to 2048 texels. The list is what `nm -u` gives for the renderer's objects. */
#include <string.h>

typedef unsigned int GLenum;
typedef int GLint;
typedef float GLfloat;
typedef unsigned char GLubyte;
typedef unsigned int GLuint;

#define NOTHING(name) void name(void) {}
NOTHING(glAlphaFunc) NOTHING(glArrayElement) NOTHING(glBegin) NOTHING(glBindTexture) NOTHING(glBlendFunc)
NOTHING(glCallList) NOTHING(glClear) NOTHING(glClearColor) NOTHING(glClearDepth) NOTHING(glClearStencil)
NOTHING(glClipPlane) NOTHING(glColor3f) NOTHING(glColor4f) NOTHING(glColor4ub) NOTHING(glColor4ubv)
NOTHING(glColorMask) NOTHING(glColorPointer) NOTHING(glCopyTexImage2D) NOTHING(glCopyTexSubImage2D)
NOTHING(glCullFace) NOTHING(glDeleteLists) NOTHING(glDeleteTextures) NOTHING(glDepthFunc) NOTHING(glDepthMask)
NOTHING(glDepthRange) NOTHING(glDisable) NOTHING(glDisableClientState) NOTHING(glDrawArrays) NOTHING(glDrawBuffer)
NOTHING(glDrawElements) NOTHING(glEnable) NOTHING(glEnableClientState) NOTHING(glEnd) NOTHING(glEndList)
NOTHING(glFinish) NOTHING(glLineWidth) NOTHING(glLoadIdentity) NOTHING(glLoadMatrixf) NOTHING(glMatrixMode)
NOTHING(glNewList) NOTHING(glOrtho) NOTHING(glPolygonMode) NOTHING(glPolygonOffset) NOTHING(glPopMatrix)
NOTHING(glPushMatrix) NOTHING(glScissor) NOTHING(glShadeModel) NOTHING(glStencilFunc) NOTHING(glStencilMask)
NOTHING(glStencilOp) NOTHING(glTexCoord2f) NOTHING(glTexCoord2fv) NOTHING(glTexCoordPointer) NOTHING(glTexEnvf)
NOTHING(glTexImage2D) NOTHING(glTexParameterf) NOTHING(glTexParameterfv) NOTHING(glTexParameteri)
NOTHING(glTexSubImage2D) NOTHING(glTranslatef) NOTHING(glScalef) NOTHING(glVertex2f) NOTHING(glVertex3f) NOTHING(glVertex3fv)
NOTHING(glVertexPointer) NOTHING(glViewport)

static GLuint lists = 1;

GLuint glGenLists(int range)
{
    GLuint first = lists;
    lists += range;
    return first;
}

GLenum glGetError(void)
{
    return 0;
}

unsigned char glIsEnabled(GLenum cap)
{
    (void)cap;
    return 0;
}

const GLubyte *glGetString(GLenum name)
{
    switch (name) {
    case 0x1F00: return (const GLubyte *)"ShaderEmu";       /* GL_VENDOR */
    case 0x1F01: return (const GLubyte *)"null";            /* GL_RENDERER */
    case 0x1F02: return (const GLubyte *)"1.1";             /* GL_VERSION */
    default: return (const GLubyte *)"";                    /* GL_EXTENSIONS: none */
    }
}

void glGetIntegerv(GLenum name, GLint *v)
{
    switch (name) {
    case 0x0D33: *v = 2048; break;      /* GL_MAX_TEXTURE_SIZE */
    case 0x84E2: *v = 1; break;         /* GL_MAX_TEXTURE_UNITS_ARB */
    case 0x0D05: case 0x0CF5: *v = 4; break;   /* GL_PACK_ALIGNMENT, GL_UNPACK_ALIGNMENT (0 made the screenshot of a save divide by it) */
    case 0x0BA2: v[0] = v[1] = 0; v[2] = 640; v[3] = 480; break;  /* GL_VIEWPORT */
    default: *v = 0; break;
    }
}

void glGetFloatv(GLenum name, GLfloat *v)
{
    (void)name;
    memset(v, 0, 16 * sizeof(GLfloat));   /* (the largest answer is a matrix) */
}

/* a black picture, for a saved game's screenshot (the rows padded to 4 bytes) */
void glReadPixels(GLint x, GLint y, int width, int height, GLenum format, GLenum type, void *pixels)
{
    int bytes = format == 0x1907 ? 3 : 4;      /* GL_RGB, else GL_RGBA */
    (void)x; (void)y; (void)type;
    memset(pixels, 0, (size_t)((width * bytes + 3) & ~3) * height);
}

/* the drawing on a worker (gl_jk2.c, the machine's): never on the build computer */
int jk2RenderOn(void) { return 0; }
void jk2RenderSettle(void) {}
void jk2RenderSettleBack(void) {}
void jk2ServerPoll(void) {}
int jk2MixOut(void) { return 0; }
int jk2MixPartPost(unsigned (*fn)(unsigned, unsigned), unsigned a0, unsigned a1) { (void)fn, (void)a0, (void)a1; return 0; }
void jk2MixPartJoin(void) {}
int jk2VoicePost(unsigned (*fn)(unsigned, unsigned), unsigned a0, unsigned a1) { (void)fn, (void)a0, (void)a1; return 0; }
int jk2WorldPost(unsigned (*fn)(unsigned, unsigned), unsigned a0, unsigned a1) { (void)fn, (void)a0, (void)a1; return 0; }
int jk2WorldJoin(void) { return 0; }
void jk2VoiceJoin(void) {}
int jk2HandPost(unsigned (*fn)(unsigned, unsigned), unsigned a0, unsigned a1) { (void)fn, (void)a0, (void)a1; return 0; }
void jk2HandJoin(void) {}
void jk2FrontSettle(void) {}
int jk2FrontOn(void) { return 0; }
void jk2FrontGive(void (*fn)(void *), void *arg) { fn(arg); }
void jk2FrontAwait(volatile unsigned *made, unsigned seq) { (void)made, (void)seq; }
void jk2RenderGive(void (*fn)(const void *), const void *arg) { fn(arg); }
void jk2RenderPresent(void) {}
int jk2RenderSwapMark(void) { return 0; }
int jk2RenderWorkerHere(void) { return 0; }
void jk2RenderGo(void) {}
int jk2SkinPost(unsigned (*fn)(unsigned, unsigned), unsigned a0, unsigned a1) { (void)fn; (void)a0; (void)a1; return 0; }
void jk2SkinJoin(void) {}
void jk2Touch(void *memory, unsigned bytes) { (void)memory; (void)bytes; }
void jk2WaitPass(void) {}/* Ghoul2 on the GPU (gl_jk2.c's): never on the build computer, which has no window */
int jk2glOn(void) { return 0; }
int jk2glSkinned(const void *tris, int count, const void *verts, const void *coords, const float *bones, int nbones,
                 const float *light, unsigned rgba) {
    (void)tris; (void)count; (void)verts; (void)coords; (void)bones; (void)nbones; (void)light; (void)rgba;
    return 0;
}
int jk2glEnvElements(int count, const unsigned *idx, const float *normal, const float *eye) {
    (void)count; (void)idx; (void)normal; (void)eye;
    return 0;
}
void jk2glKey(const void *surface, int frame) { (void)surface; (void)frame; }
void jk2glKeysForget(void) {}
int jk2MixWorkerHere(void) { return 0; }
/* tess's arrays in the frame's GPU memory (gl_jk2.c's): none on the build computer, tr_shade.cpp's own arrays */
void *jk2glTessRoom(unsigned bytes) { (void)bytes; return 0; }
int jk2glTessTrim(void *at, unsigned bytes) { (void)at; (void)bytes; return 0; }
void jk2glRooms(const void *xyz, const void *colours, const void *tc0, const void *tc1) { (void)xyz; (void)colours; (void)tc0; (void)tc1; }
void jk2glVertexCount(int n) { (void)n; }

/* the GL's kept and tagged words, the in-play test and the joins' counts (gl_jk2.c's, sys_shaderemu.cpp's under SE_WINDOW):
   none on the build computer, so the renderer makes tess's words itself, which leaves the game as it is */
int SE_InPlay(void) { return 1; }
void hlglWordsInPlace(unsigned char *rgba, int n, int opaque, int *holes) { (void)rgba; (void)n; (void)opaque; (void)holes; }
int jk2glKeptOn(void) { return 0; }
int jk2glKeptWords(unsigned *to, const float *points, int stride, int st, const int *idx, int count, int verts) {
    (void)to; (void)points; (void)stride; (void)st; (void)idx; (void)count; (void)verts;
    return 0;
}
int jk2glTaggedDraws(const unsigned *words, unsigned *tables, const int *counts, int draws) {
    (void)words; (void)tables; (void)counts; (void)draws;
    return 0;
}
void jk2glSettle(void) {}
int jk2glKeptFaces(unsigned rgba, const unsigned *const *words, const int *counts, int faces, int floored) {
    (void)rgba; (void)words; (void)counts; (void)faces; (void)floored;
    return 0;
}
unsigned se_join_waits, se_rs_waits;
/* mcwx.h's loop over the workers: the whole range on this core */
void mcw_for_opt(unsigned n, void (*fn)(void *ctx, unsigned from, unsigned to), void *ctx, const void *o) { (void)o; if (n) fn(ctx, 0, n); }
/* the drawing on two workers (gl_jk2.c's): never on the build computer */
int jk2DrawHalfGive(void *half, void *gl) { (void)half; (void)gl; return 0; }
int jk2DrawHalfJoin(void *gl) { (void)gl; return 0; }
int jk2DrawParts(void) { return 0; }
int jk2DrawPartGive(int part, void *half, void *gl) { (void)part; (void)half; (void)gl; return 0; }
int jk2DrawPartJoin(int part, void *gl) { (void)part; (void)gl; return 0; }
void jk2glHalfInit(const void *gl) { (void)gl; }
void jk2glHalfEnd(void *gl) { (void)gl; }
