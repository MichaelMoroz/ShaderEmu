// The OpenGL 1.x subset our GPU driver (../../gl.c) implements. Names and values are the
// standard ones, so programs written for OpenGL compile unchanged; calls outside the subset
// do not exist and fail at link time.
#pragma once

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef unsigned char GLubyte;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;

#define GL_FALSE 0
#define GL_TRUE 1

#define GL_POINTS 0x0000
#define GL_LINES 0x0001
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_QUADS 0x0007
#define GL_QUAD_STRIP 0x0008
#define GL_POLYGON 0x0009

#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000

#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_BACK_LEFT 0x0402
#define GL_BACK_RIGHT 0x0403

#define GL_CULL_FACE 0x0B44
#define GL_LIGHTING 0x0B50
#define GL_DEPTH_TEST 0x0B71
#define GL_NORMALIZE 0x0BA1
#define GL_LIGHT0 0x4000

#define GL_AMBIENT 0x1200
#define GL_DIFFUSE 0x1201
#define GL_SPECULAR 0x1202
#define GL_POSITION 0x1203
#define GL_AMBIENT_AND_DIFFUSE 0x1602

#define GL_COMPILE 0x1300
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GL_FLAT 0x1D00
#define GL_SMOOTH 0x1D01

#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03

void glClear(GLbitfield mask);
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glDrawBuffer(GLenum mode);
void glEnable(GLenum cap);
void glDisable(GLenum cap);
void glShadeModel(GLenum mode);
void glFlush(void);
const GLubyte* glGetString(GLenum name);

void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glPushMatrix(void);
void glPopMatrix(void);
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glTranslated(GLdouble x, GLdouble y, GLdouble z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);

void glLightfv(GLenum light, GLenum pname, const GLfloat* params);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat* params);
void glColor3f(GLfloat r, GLfloat g, GLfloat b);

void glBegin(GLenum mode);
void glEnd(void);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glNormal3f(GLfloat x, GLfloat y, GLfloat z);

GLuint glGenLists(GLsizei range);
void glNewList(GLuint list, GLenum mode);
void glEndList(void);
void glCallList(GLuint list);
void glDeleteLists(GLuint list, GLsizei range);
