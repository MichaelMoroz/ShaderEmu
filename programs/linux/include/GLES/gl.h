// The OpenGL that programs/linux/gles.c implements on the machine's GPU device (docs/gpu.md):
// OpenGL ES 1.x's way of working (vertex arrays, no glBegin), with numbers as floats or as
// GLfixed, plus GL_QUADS and paletted textures, which the device draws directly. What is not
// declared here does not exist. segl.h has the calls that tie it to a window.
#ifndef SHADEREMU_GLES_GL_H
#define SHADEREMU_GLES_GL_H

#include <stdint.h>

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef unsigned int GLbitfield;
typedef int GLint;
typedef int GLsizei;
typedef int32_t GLfixed;   // 16.16
typedef float GLfloat;
typedef unsigned char GLubyte;
typedef unsigned char GLboolean;
typedef void GLvoid;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_NO_ERROR 0

#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_QUADS 0x0007

#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_COLOR_BUFFER_BIT 0x4000

#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_COLOR 0x0306

#define GL_ALPHA_TEST 0x0BC0
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_TEXTURE_2D 0x0DE1

#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_FIXED 0x140C

#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701

#define GL_COLOR_INDEX 0x1900
#define GL_RGBA 0x1908
#define GL_COLOR_INDEX8_EXT 0x80E5
#define GL_SHARED_TEXTURE_PALETTE_EXT 0x81FB
#define GL_RGB 0x1907

#define GL_VERTEX_ARRAY 0x8074
#define GL_TEXTURE_COORD_ARRAY 0x8078

void glClearColorx(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha);
void glClear(GLbitfield mask);
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);

void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixx(const GLfixed* m);   // column-major, as OpenGL has it
void glMultMatrixx(const GLfixed* m);
void glPushMatrix(void);
void glPopMatrix(void);
void glFrustumx(GLfixed left, GLfixed right, GLfixed bottom, GLfixed top, GLfixed near_plane, GLfixed far_plane);
void glOrthox(GLfixed left, GLfixed right, GLfixed bottom, GLfixed top, GLfixed near_plane, GLfixed far_plane);
void glTranslatex(GLfixed x, GLfixed y, GLfixed z);
void glScalex(GLfixed x, GLfixed y, GLfixed z);
void glRotatex(GLfixed degrees, GLfixed x, GLfixed y, GLfixed z);   // about an axis of the coordinate system
// The same with floats. (In the library's fixed-point build these convert, and glRotatef is
// about an axis of the coordinate system only; in its float build the calls above convert.)
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glLoadMatrixf(const GLfloat* m);
void glMultMatrixf(const GLfloat* m);
void glFrustumf(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat near_plane, GLfloat far_plane);
void glOrthof(GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat near_plane, GLfloat far_plane);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat degrees, GLfloat x, GLfloat y, GLfloat z);
void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);

void glEnable(GLenum what);
void glDisable(GLenum what);
// Blending is one of three kinds: (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), (SRC_ALPHA or ONE, ONE)
// and (DST_COLOR, ZERO) or (ZERO, SRC_COLOR). Blended things are drawn after everything that
// is not, whatever the order of the calls; so are things drawn without the depth test.
void glBlendFunc(GLenum source, GLenum destination);
// With GL_ALPHA_TEST on, texels of a paletted texture with this index are not drawn.
void glColorKeySE(GLint index);

void glColor4x(GLfixed red, GLfixed green, GLfixed blue, GLfixed alpha);
void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);

// Vertices take the current colour: there is no colour array.
void glEnableClientState(GLenum array);
void glDisableClientState(GLenum array);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);     // 2 or 3 of GL_FLOAT or GL_FIXED
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);   // 2 of GL_FLOAT or GL_FIXED
void glDrawArrays(GLenum mode, GLint first, GLsizei count);

void glGenTextures(GLsizei n, GLuint* textures);
void glDeleteTextures(GLsizei n, const GLuint* textures);
void glBindTexture(GLenum target, GLuint texture);
// GL_COLOR_INDEX8_EXT with GL_COLOR_INDEX bytes, or GL_RGBA with GL_RGBA bytes. Level 0 only.
void glTexImage2D(GLenum target, GLint level, GLint internal, GLsizei width, GLsizei height, GLint border, GLenum format,
                  GLenum type, const GLvoid* pixels);
// The palette every paletted texture is looked up in: `count` entries of red, green, blue bytes.
void glColorTableEXT(GLenum target, GLenum internal, GLsizei count, GLenum format, GLenum type, const GLvoid* table);

void glFlush(void);
void glFinish(void);
GLenum glGetError(void);

#endif
