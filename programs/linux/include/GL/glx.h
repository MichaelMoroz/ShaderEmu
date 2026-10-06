// GLX as far as our driver goes: one window, which is the machine's display.
#pragma once
#include <GL/gl.h>
#include <X11/Xlib.h>

#define GLX_RGBA 4
#define GLX_DOUBLEBUFFER 5
#define GLX_STEREO 6
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_DEPTH_SIZE 12
#define GLX_SAMPLE_BUFFERS 100000
#define GLX_SAMPLES 100001
#define GLX_SWAP_INTERVAL_EXT 0x20F1

typedef struct glx_context* GLXContext;
typedef XID GLXDrawable;

XVisualInfo* glXChooseVisual(Display* dpy, int screen, int* attribs);
GLXContext glXCreateContext(Display* dpy, XVisualInfo* vis, GLXContext share, Bool direct);
void glXDestroyContext(Display* dpy, GLXContext ctx);
Bool glXMakeCurrent(Display* dpy, GLXDrawable drawable, GLXContext ctx);
void glXSwapBuffers(Display* dpy, GLXDrawable drawable);
const char* glXQueryExtensionsString(Display* dpy, int screen);
void glXQueryDrawable(Display* dpy, GLXDrawable drawable, int attribute, unsigned int* value);
void (*glXGetProcAddressARB(const GLubyte* name))(void);
