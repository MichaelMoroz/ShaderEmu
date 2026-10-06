// Stand-ins for the handful of Xlib calls a GLX program makes to open its window. There is no
// X server: the window is the machine's display, and no events arrive.
#pragma once

typedef unsigned long XID;
typedef XID Window;
typedef XID Colormap;
typedef XID Atom;
typedef XID KeySym;
typedef XID VisualID;
typedef int Bool;
typedef struct x_display Display;
typedef struct { VisualID visualid; } Visual;
typedef struct { int unused; } XComposeStatus;

#define None 0L
#define True 1
#define False 0
#define AllocNone 0
#define InputOutput 1
#define CWBackPixel (1L << 1)
#define CWBorderPixel (1L << 3)
#define CWOverrideRedirect (1L << 9)
#define CWEventMask (1L << 11)
#define CWColormap (1L << 13)
#define KeyPressMask (1L << 0)
#define ExposureMask (1L << 15)
#define StructureNotifyMask (1L << 17)
#define KeyPress 2
#define Expose 12
#define ConfigureNotify 22
#define USPosition (1L << 0)
#define USSize (1L << 1)
#define XValue 0x0001
#define YValue 0x0002
#define WidthValue 0x0004
#define HeightValue 0x0008
#define PropModeReplace 0

typedef struct {
    Visual* visual;
    VisualID visualid;
    int screen, depth;
} XVisualInfo;

typedef struct {
    unsigned long background_pixel, border_pixel;
    Colormap colormap;
    long event_mask;
    Bool override_redirect;
} XSetWindowAttributes;

typedef struct {
    long flags;
    int x, y, width, height;
} XSizeHints;

typedef struct { int type; int width, height; } XConfigureEvent;
typedef struct { int type; unsigned keycode; } XKeyEvent;
typedef union {
    int type;
    XConfigureEvent xconfigure;
    XKeyEvent xkey;
} XEvent;

Display* XOpenDisplay(const char* name);
int XCloseDisplay(Display* dpy);
int XDefaultScreen(Display* dpy);
Window XRootWindow(Display* dpy, int screen);
int XDisplayWidth(Display* dpy, int screen);
int XDisplayHeight(Display* dpy, int screen);
#define DefaultScreen(dpy) XDefaultScreen(dpy)
#define RootWindow(dpy, scr) XRootWindow(dpy, scr)
#define DisplayWidth(dpy, scr) XDisplayWidth(dpy, scr)
#define DisplayHeight(dpy, scr) XDisplayHeight(dpy, scr)

Colormap XCreateColormap(Display* dpy, Window w, Visual* visual, int alloc);
Window XCreateWindow(Display* dpy, Window parent, int x, int y, unsigned width, unsigned height, unsigned border,
                     int depth, unsigned cls, Visual* visual, unsigned long mask, XSetWindowAttributes* attr);
int XDestroyWindow(Display* dpy, Window w);
int XMapWindow(Display* dpy, Window w);
int XSetNormalHints(Display* dpy, Window w, XSizeHints* hints);
int XSetStandardProperties(Display* dpy, Window w, const char* name, const char* icon, XID pixmap, char** argv, int argc,
                           XSizeHints* hints);
Atom XInternAtom(Display* dpy, const char* name, Bool only_if_exists);
int XChangeProperty(Display* dpy, Window w, Atom property, Atom type, int format, int mode, const unsigned char* data, int n);
int XFree(void* data);
int XPending(Display* dpy);
int XNextEvent(Display* dpy, XEvent* event);
KeySym XLookupKeysym(XKeyEvent* event, int index);
int XLookupString(XKeyEvent* event, char* buffer, int bytes, KeySym* keysym, XComposeStatus* status);
int XParseGeometry(const char* spec, int* x, int* y, unsigned* width, unsigned* height);
