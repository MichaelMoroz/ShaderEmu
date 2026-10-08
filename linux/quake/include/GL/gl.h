// The OpenGL 1.1 that GLQuake calls, for linux/quake/qgl.c: floats and glBegin over the
// fixed-point library (programs/linux/gles.c), which is what draws. Every call Quake makes
// is renamed to qgl..., so the two libraries' names never meet. docs/quake.md.
#ifndef SHADEREMU_QUAKE_GL_H
#define SHADEREMU_QUAKE_GL_H

#include <GLES/segl.h>

typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;

#define GL_POLYGON 0x0009
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_GEQUAL 0x0206
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CULL_FACE 0x0B44
#define GL_FOG 0x0B60
#define GL_FOG_END 0x0B64
#define GL_FOG_MODE 0x0B65
#define GL_FOG_COLOR 0x0B66
#define GL_MODELVIEW_MATRIX 0x0BA6
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#define GL_FASTEST 0x1101
#define GL_NICEST 0x1102
#define GL_ALPHA 0x1906
#define GL_LUMINANCE 0x1909
#define GL_FILL 0x1B02
#define GL_FLAT 0x1D00
#define GL_SMOOTH 0x1D01
#define GL_REPLACE 0x1E01
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03
#define GL_MODULATE 0x2100
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_TEXTURE_ENV 0x2300
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_NEAREST_MIPMAP_NEAREST 0x2700
#define GL_LINEAR_MIPMAP_NEAREST 0x2701
#define GL_NEAREST_MIPMAP_LINEAR 0x2702
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_REPEAT 0x2901
#define GL_INTENSITY 0x8049
#define GL_RGBA4 0x8056

void qglBegin(GLenum mode);
void qglEnd(void);
void qglVertex2f(GLfloat x, GLfloat y);
void qglVertex3f(GLfloat x, GLfloat y, GLfloat z);
void qglVertex3fv(const GLfloat* v);
void qglTexCoord2f(GLfloat s, GLfloat t);
void qglColor3f(GLfloat r, GLfloat g, GLfloat b);
void qglColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void qglColor4fv(const GLfloat* v);
void qglColor3ubv(const GLubyte* v);

void qglEnable(GLenum what);
void qglDisable(GLenum what);
void qglBlendFunc(GLenum source, GLenum destination);
void qglClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void qglClear(GLbitfield mask);
void qglViewport(GLint x, GLint y, GLsizei width, GLsizei height);

void qglMatrixMode(GLenum mode);
void qglLoadIdentity(void);
void qglPushMatrix(void);
void qglPopMatrix(void);
void qglRotatef(GLfloat degrees, GLfloat x, GLfloat y, GLfloat z);
void qglTranslatef(GLfloat x, GLfloat y, GLfloat z);
void qglScalef(GLfloat x, GLfloat y, GLfloat z);
void qglFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void qglOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void qglGetFloatv(GLenum what, GLfloat* out);
void qglLoadMatrixf(const GLfloat* m);

void qglBindTexture(GLenum target, GLuint name);
void qglTexImage2D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border, GLenum format,
                   GLenum type, const GLvoid* pixels);
void qglTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type,
                      const GLvoid* pixels);
// For a level kept in GPU memory (world_shaderemu.c): a texture that stays there, a block of
// that memory for vertices, and both given up.
int qglStay(GLuint name);
void* qglBlock(unsigned int bytes);
void qglRelease(void);
// A surface's light (8.8 a texel, width x height of them) into light map `name` at x, y.
void qglLight(GLuint name, int x, int y, int width, int height, const unsigned int* light);
// Palette indices as they are (no copy to RGBA, no rescaling); with `holes`, index 255 is not drawn.
void qglTexImage8(const GLubyte* pixels, GLsizei width, GLsizei height, int holes);
const GLubyte* qglGetString(GLenum what);

// The window the frames go to, and the end of a frame.
int qglOpen(unsigned int nano_x_window, const unsigned char* palette);
void qglSwap(void);
// The machine's control words (docs/gpu.md), for the clock and the input counters.
const volatile unsigned int* qglControl(int offset);
// Bytes of textures kept, how many of them are in GPU memory and how many that holds, textures
// that have had to leave it so far, and the vertices the last frame gave the GPU.
void qglMemory(unsigned int* kept, unsigned int* in_gpu, unsigned int* gpu_size, unsigned int* evicted, unsigned int* vertices);

// What this GPU has one way of doing, or does not do.
static inline void qglNothing(void) {}
#define glDepthRange(a, b) qglNothing()
#define glDepthFunc(a) qglNothing()
#define glDepthMask(a) qglNothing()
#define glCullFace(a) qglNothing()
#define glPolygonMode(a, b) qglNothing()
#define glShadeModel(a) qglNothing()
#define glHint(a, b) qglNothing()
#define glTexParameterf(a, b, c) qglNothing()
#define glTexEnvf(a, b, c) qglNothing()
#define glAlphaFunc(a, b) qglNothing()
#define glDrawBuffer(a) qglNothing()
#define glReadBuffer(a) qglNothing()
#define glReadPixels(x, y, w, h, f, t, p) qglNothing()
#define glFogi(a, b) qglNothing()
#define glFogf(a, b) qglNothing()
#define glFogfv(a, b) qglNothing()

#ifndef QGL_ITSELF
#define glBegin qglBegin
#define glEnd qglEnd
#define glVertex2f qglVertex2f
#define glVertex3f qglVertex3f
#define glVertex3fv qglVertex3fv
#define glTexCoord2f qglTexCoord2f
#define glColor3f qglColor3f
#define glColor4f qglColor4f
#define glColor4fv qglColor4fv
#define glColor3ubv qglColor3ubv
#define glEnable qglEnable
#define glDisable qglDisable
#define glBlendFunc qglBlendFunc
#define glClearColor qglClearColor
#define glClear qglClear
#define glViewport qglViewport
#define glMatrixMode qglMatrixMode
#define glLoadIdentity qglLoadIdentity
#define glPushMatrix qglPushMatrix
#define glPopMatrix qglPopMatrix
#define glRotatef qglRotatef
#define glTranslatef qglTranslatef
#define glScalef qglScalef
#define glFrustum qglFrustum
#define glOrtho qglOrtho
#define glGetFloatv qglGetFloatv
#define glLoadMatrixf qglLoadMatrixf
#define glBindTexture qglBindTexture
#define glTexImage2D qglTexImage2D
#define glTexSubImage2D qglTexSubImage2D
#define glGetString qglGetString
#define glFinish qglNothing
#define glFlush qglNothing
#endif

#endif
