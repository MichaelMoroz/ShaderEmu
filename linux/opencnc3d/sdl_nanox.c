// SPDX-License-Identifier: GPL-3.0-or-later
// ShaderEmu: the part of SDL2 OpenCNC3D's program calls, over Nano-X and the machine's GPU
// (docs/games/opencnc3d.md, "SDL"). The declarations are SDL2's own headers (2.30.9, zlib licence,
// fetched by linux/opencnc3d/build.sh); every function here is ours. One window, whose GL is
// Half-Life's shim (gl_ocnc.c); events from the window system turned into SDL's; time from the
// clock; threads run to their end when they are made (the machine runs a program on one core);
// sound through the card's stream voice (sound_ocnc.c). Only what the program calls is here: the
// list is in the doc, and a call outside it fails at the link.
#include <SDL.h>
#include <nano-X.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

int ocnc_gl_open(unsigned window);           // gl_ocnc.c
void ocnc_gl_swap(void);
void ocnc_gl_size(int* w, int* h);
void* ocnc_gl_proc(const char* name);

static char err[256];
const char* SDL_GetError(void) { return err; }
static int fail(const char* what) {
    snprintf(err, sizeof err, "%s", what);
    return -1;
}

// ---- start, hints, the environment, memory ----
static int opened;
int SDL_Init(Uint32 flags) {
    (void)flags;
    if (opened) return 0;
    if (GrOpen() < 0) return fail("no window system (nano-X)");
    opened = 1;
    return 0;
}
int SDL_InitSubSystem(Uint32 flags) { return SDL_Init(flags); }
void SDL_Quit(void) {
    if (opened) GrClose();
    opened = 0;
}
SDL_bool SDL_SetHint(const char* name, const char* value) { (void)name, (void)value; return SDL_TRUE; }
char* SDL_getenv(const char* name) { return getenv(name); }
void SDL_free(void* p) { free(p); }
void* SDL_memset(void* d, int c, size_t n) { return memset(d, c, n); }
void SDL_GetVersion(SDL_version* v) { v->major = 2, v->minor = 30, v->patch = 9; }
const char* SDL_GetPlatform(void) { return "Linux"; }
const char* SDL_GetCurrentVideoDriver(void) { return "nanox"; }
int SDL_ShowSimpleMessageBox(Uint32 flags, const char* title, const char* message, SDL_Window* window) {
    (void)flags, (void)window;
    fprintf(stderr, "[%s] %s\n", title ? title : "", message ? message : "");
    return 0;
}
// the folder a program keeps its things in: the one the host keeps (/persistence), else /tmp
char* SDL_GetPrefPath(const char* org, const char* app) {
    (void)org;
    const char* base = getenv("OPENCNC3D_HOME");
    char path[512];
    if (!base) base = access("/persistence", W_OK) == 0 ? "/persistence" : "/tmp";
    snprintf(path, sizeof path, "%s/%s/", base, app ? app : "opencnc3d");
    mkdir(path, 0755);
    return strdup(path);
}
char* SDL_GetBasePath(void) {
    char path[512];
    if (!getcwd(path, sizeof path - 2)) strcpy(path, ".");
    strcat(path, "/");
    return strdup(path);
}

// ---- time: the machine's clock, never a wait in place ----
static Uint64 now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (Uint64)t.tv_sec * 1000000u + (Uint64)(t.tv_nsec / 1000);
}
static Uint64 t0;
Uint32 SDL_GetTicks(void) {
    if (!t0) t0 = now_us();
    return (Uint32)((now_us() - t0) / 1000);
}
// OPENCNC3D_CLOCK=instret: the performance counter is the instructions core 0 has retired, at
// "1e9 a second", so what the program prints in milliseconds (its FRAMEPROF) is millions of them
static int clock_instret = -1;
static int instret_clock(void) {
    if (clock_instret < 0) clock_instret = getenv("OPENCNC3D_CLOCK") && !strcmp(getenv("OPENCNC3D_CLOCK"), "instret");
    return clock_instret;
}
static Uint64 instret(void) {
    unsigned lo, hi, again;
    do {
        __asm__ volatile("rdinstreth %0" : "=r"(hi));
        __asm__ volatile("rdinstret %0" : "=r"(lo));
        __asm__ volatile("rdinstreth %0" : "=r"(again));
    } while (hi != again);
    return ((Uint64)hi << 32) | lo;
}
Uint64 SDL_GetPerformanceCounter(void) { return instret_clock() ? instret() : now_us(); }
Uint64 SDL_GetPerformanceFrequency(void) { return instret_clock() ? 1000000000 : 1000000; }
void SDL_Delay(Uint32 ms) { usleep(ms * 1000); }

// ---- threads, mutexes, atomics: one core, so a thread runs to its end when it is made ----
struct SDL_Thread { int status; };
SDL_Thread* SDL_CreateThread(SDL_ThreadFunction fn, const char* name, void* data) {
    SDL_Thread* t = calloc(1, sizeof *t);
    (void)name;
    if (t) t->status = fn(data);
    return t;
}
void SDL_WaitThread(SDL_Thread* t, int* status) {
    if (!t) return;
    if (status) *status = t->status;
    free(t);
}
void SDL_DetachThread(SDL_Thread* t) { free(t); }
struct SDL_mutex { int locked; };
SDL_mutex* SDL_CreateMutex(void) { return calloc(1, sizeof(SDL_mutex)); }
int SDL_LockMutex(SDL_mutex* m) { if (m) m->locked++; return 0; }
int SDL_UnlockMutex(SDL_mutex* m) { if (m) m->locked--; return 0; }
int SDL_AtomicSet(SDL_atomic_t* a, int v) { int old = a->value; a->value = v; return old; }
int SDL_AtomicGet(SDL_atomic_t* a) { return a->value; }

// ---- the window and its GL ----
struct SDL_Window { GR_WINDOW_ID id; int w, h, x, y; Uint32 flags; struct { char name[32]; void* data; } data[8]; };
static struct SDL_Window the_window;
static SDL_Window* window;
static int screen_w = 800, screen_h = 600;

static void screen_size(void) {
    GR_SCREEN_INFO info;
    GrGetScreenInfo(&info);
    screen_w = info.cols, screen_h = info.rows;
}
SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, Uint32 flags) {
    if (SDL_Init(0) < 0) return NULL;
    if (window) { fail("one window only"); return NULL; }
    screen_size();
    // as a program on the desktop: the caption above and the bar below (docs/system/nanox.md)
    if (w > screen_w) w = screen_w;
    if (h > screen_h - 55) h = screen_h - 55;
    const char* size = getenv("OPENCNC3D_WINDOW");
    if (size) sscanf(size, "%dx%d", &w, &h);
    window = &the_window;
    window->w = w, window->h = h, window->flags = flags | SDL_WINDOW_SHOWN;
    window->flags &= ~(Uint32)SDL_WINDOW_HIDDEN;
    window->id = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, (char*)(title ? title : "OpenCNC3D"), GR_ROOT_WINDOW_ID,
                               (x & 0xFFFF0000) == SDL_WINDOWPOS_CENTERED_MASK ? -1 : x,
                               (y & 0xFFFF0000) == SDL_WINDOWPOS_CENTERED_MASK ? -1 : y, w, h, 0);
    GrSelectEvents(window->id, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN |
                   GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_POSITION | GR_EVENT_MASK_MOUSE_ENTER |
                   GR_EVENT_MASK_MOUSE_EXIT | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE |
                   GR_EVENT_MASK_FOCUS_IN | GR_EVENT_MASK_FOCUS_OUT);
    GrMapWindow(window->id);
    GrSetFocus(window->id);
    return window;
}
void SDL_DestroyWindow(SDL_Window* w) {
    if (w && w == window) GrDestroyWindow(w->id), window = NULL;
}
Uint32 SDL_GetWindowFlags(SDL_Window* w) { return w ? w->flags | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS : 0; }
void SDL_GetWindowSize(SDL_Window* w, int* x, int* y) {
    if (x) *x = w ? w->w : 0;
    if (y) *y = w ? w->h : 0;
}
void SDL_SetWindowSize(SDL_Window* w, int x, int y) { (void)w, (void)x, (void)y; }   // the window keeps its size
void SDL_GetWindowPosition(SDL_Window* w, int* x, int* y) {
    if (x) *x = w ? w->x : 0;
    if (y) *y = w ? w->y : 0;
}
void SDL_SetWindowPosition(SDL_Window* w, int x, int y) { (void)w, (void)x, (void)y; }
void SDL_SetWindowResizable(SDL_Window* w, SDL_bool on) { (void)w, (void)on; }
void SDL_SetWindowBordered(SDL_Window* w, SDL_bool on) { (void)w, (void)on; }
int SDL_SetWindowFullscreen(SDL_Window* w, Uint32 flags) { (void)w, (void)flags; return 0; }   // windowed only
int SDL_SetWindowDisplayMode(SDL_Window* w, const SDL_DisplayMode* m) { (void)w, (void)m; return 0; }
int SDL_GetWindowDisplayMode(SDL_Window* w, SDL_DisplayMode* m) {
    memset(m, 0, sizeof *m);
    m->format = SDL_PIXELFORMAT_RGB888, m->w = w ? w->w : screen_w, m->h = w ? w->h : screen_h, m->refresh_rate = 60;
    return 0;
}
void SDL_RaiseWindow(SDL_Window* w) { if (w) GrRaiseWindow(w->id); }
int SDL_SetWindowInputFocus(SDL_Window* w) { if (w) GrSetFocus(w->id); return 0; }
void SDL_SetWindowMouseGrab(SDL_Window* w, SDL_bool on) { (void)w, (void)on; }
int SDL_GetWindowDisplayIndex(SDL_Window* w) { (void)w; return 0; }
SDL_Window* SDL_GetWindowFromID(Uint32 id) { (void)id; return window; }
SDL_Window* SDL_GetKeyboardFocus(void) { return window; }
void* SDL_SetWindowData(SDL_Window* w, const char* name, void* data) {
    for (int i = 0; i < 8; i++)
        if (!w->data[i].name[0] || !strcmp(w->data[i].name, name)) {
            void* old = w->data[i].data;
            strncpy(w->data[i].name, name, 31), w->data[i].data = data;
            return old;
        }
    return NULL;
}
void* SDL_GetWindowData(SDL_Window* w, const char* name) {
    for (int i = 0; w && i < 8; i++)
        if (!strcmp(w->data[i].name, name)) return w->data[i].data;
    return NULL;
}

// displays: the one screen
int SDL_GetNumVideoDisplays(void) { return 1; }
static void mode_of_screen(SDL_DisplayMode* m) {
    screen_size();
    memset(m, 0, sizeof *m);
    m->format = SDL_PIXELFORMAT_RGB888, m->w = screen_w, m->h = screen_h, m->refresh_rate = 60;
}
int SDL_GetDesktopDisplayMode(int i, SDL_DisplayMode* m) { (void)i; mode_of_screen(m); return 0; }
int SDL_GetCurrentDisplayMode(int i, SDL_DisplayMode* m) { (void)i; mode_of_screen(m); return 0; }
int SDL_GetNumDisplayModes(int i) { (void)i; return 1; }
int SDL_GetDisplayMode(int i, int k, SDL_DisplayMode* m) { (void)i, (void)k; mode_of_screen(m); return 0; }
SDL_DisplayMode* SDL_GetClosestDisplayMode(int i, const SDL_DisplayMode* want, SDL_DisplayMode* got) {
    (void)i, (void)want;
    mode_of_screen(got);
    return got;
}
int SDL_GetDisplayBounds(int i, SDL_Rect* r) {
    (void)i;
    screen_size();
    r->x = r->y = 0, r->w = screen_w, r->h = screen_h;
    return 0;
}
int SDL_GetDisplayUsableBounds(int i, SDL_Rect* r) {
    SDL_GetDisplayBounds(i, r);
    r->h -= 55;   // the caption and the bar
    return 0;
}

// GL: one context, the window's
static int gl_open, swap_interval;
int SDL_GL_SetAttribute(SDL_GLattr a, int v) { (void)a, (void)v; return 0; }
int SDL_GL_GetAttribute(SDL_GLattr a, int* v) {
    *v = a == SDL_GL_DOUBLEBUFFER ? 1 : a == SDL_GL_DEPTH_SIZE ? 24 : a <= SDL_GL_ALPHA_SIZE ? 8 : 0;
    return 0;
}
SDL_GLContext SDL_GL_CreateContext(SDL_Window* w) {
    if (!w) { fail("no window"); return NULL; }
    if (!gl_open) {
        if (ocnc_gl_open(w->id) < 0) { fail("this machine has no GPU"); return NULL; }
        gl_open = 1;
        ocnc_gl_size(&w->w, &w->h);
    }
    return (SDL_GLContext)w;
}
void SDL_GL_DeleteContext(SDL_GLContext c) { (void)c; }
int SDL_GL_MakeCurrent(SDL_Window* w, SDL_GLContext c) { (void)w, (void)c; return 0; }
SDL_GLContext SDL_GL_GetCurrentContext(void) { return gl_open ? (SDL_GLContext)window : NULL; }
SDL_Window* SDL_GL_GetCurrentWindow(void) { return window; }
void SDL_GL_GetDrawableSize(SDL_Window* w, int* x, int* y) { SDL_GetWindowSize(w, x, y); }
int SDL_GL_SetSwapInterval(int i) { swap_interval = i; return 0; }
int SDL_GL_GetSwapInterval(void) { return swap_interval; }
void SDL_GL_SwapWindow(SDL_Window* w) { (void)w; if (gl_open) ocnc_gl_swap(); }
void* SDL_GL_GetProcAddress(const char* name) { return ocnc_gl_proc(name); }

// ---- input: Nano-X's events as SDL's ----
#define QUEUE 256
static SDL_Event queue[QUEUE];
static int q_head, q_count;
static Uint8 keys[SDL_NUM_SCANCODES];
static SDL_Keymod mods;
static int mouse_x, mouse_y, mouse_buttons, text_input;

static void push(const SDL_Event* e) {
    if (q_count == QUEUE) return;
    queue[(q_head + q_count++) % QUEUE] = *e;
}
int SDL_PushEvent(SDL_Event* e) { push(e); return 1; }

static SDL_Keycode key_of(int ch) {
    switch (ch) {
        case MWKEY_LEFT: return SDLK_LEFT; case MWKEY_RIGHT: return SDLK_RIGHT;
        case MWKEY_UP: return SDLK_UP; case MWKEY_DOWN: return SDLK_DOWN;
        case MWKEY_INSERT: return SDLK_INSERT; case MWKEY_DELETE: return SDLK_DELETE;
        case MWKEY_HOME: return SDLK_HOME; case MWKEY_END: return SDLK_END;
        case MWKEY_PAGEUP: return SDLK_PAGEUP; case MWKEY_PAGEDOWN: return SDLK_PAGEDOWN;
        case MWKEY_LSHIFT: return SDLK_LSHIFT; case MWKEY_RSHIFT: return SDLK_RSHIFT;
        case MWKEY_LCTRL: return SDLK_LCTRL; case MWKEY_RCTRL: return SDLK_RCTRL;
        case MWKEY_LALT: return SDLK_LALT; case MWKEY_RALT: return SDLK_RALT;
        case MWKEY_LMETA: return SDLK_LGUI; case MWKEY_RMETA: return SDLK_RGUI;
        case MWKEY_ENTER: case MWKEY_KP_ENTER: return SDLK_RETURN;
        case MWKEY_PAUSE: return SDLK_PAUSE; case MWKEY_PRINT: return SDLK_PRINTSCREEN;
    }
    if (ch >= MWKEY_F1 && ch <= MWKEY_F11) return SDLK_F1 + (ch - MWKEY_F1);
    if (ch == MWKEY_F12) return SDLK_F12;
    if (ch >= MWKEY_KP0 && ch <= MWKEY_KP9) return ch == MWKEY_KP0 ? SDLK_KP_0 : SDLK_KP_1 + (ch - MWKEY_KP1);
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 'a';
    if (ch < 128) return ch;
    return SDLK_UNKNOWN;
}
static SDL_Keymod mod_of(SDL_Keycode k) {
    switch (k) {
        case SDLK_LSHIFT: return KMOD_LSHIFT; case SDLK_RSHIFT: return KMOD_RSHIFT;
        case SDLK_LCTRL: return KMOD_LCTRL; case SDLK_RCTRL: return KMOD_RCTRL;
        case SDLK_LALT: return KMOD_LALT; case SDLK_RALT: return KMOD_RALT;
        case SDLK_LGUI: return KMOD_LGUI; case SDLK_RGUI: return KMOD_RGUI;
        default: return KMOD_NONE;
    }
}
static int button_of(int mw) { return mw & GR_BUTTON_L ? SDL_BUTTON_LEFT : mw & GR_BUTTON_R ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE; }
static int buttons_mask(int mw) {
    return (mw & GR_BUTTON_L ? SDL_BUTTON_LMASK : 0) | (mw & GR_BUTTON_R ? SDL_BUTTON_RMASK : 0) | (mw & GR_BUTTON_M ? SDL_BUTTON_MMASK : 0);
}

static void translate(GR_EVENT* g) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.common.timestamp = SDL_GetTicks();
    switch (g->type) {
        case GR_EVENT_TYPE_KEY_DOWN:
        case GR_EVENT_TYPE_KEY_UP: {
            int down = g->type == GR_EVENT_TYPE_KEY_DOWN;
            SDL_Keycode k = key_of(g->keystroke.ch);
            if (k == SDLK_UNKNOWN) return;
            SDL_Scancode s = SDL_GetScancodeFromKey(k);
            SDL_Keymod m = mod_of(k);
            if (m) mods = down ? (SDL_Keymod)(mods | m) : (SDL_Keymod)(mods & ~m);
            keys[s] = (Uint8)down;
            e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
            e.key.windowID = 1, e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
            e.key.keysym.sym = k, e.key.keysym.scancode = s, e.key.keysym.mod = (Uint16)mods;
            push(&e);
            int ch = g->keystroke.ch;
            if (down && text_input && ch >= 32 && ch < 127 && !(mods & KMOD_CTRL)) {
                memset(&e, 0, sizeof e);
                e.type = SDL_TEXTINPUT, e.text.windowID = 1, e.text.text[0] = (char)ch;
                push(&e);
            }
            return;
        }
        case GR_EVENT_TYPE_BUTTON_DOWN:
        case GR_EVENT_TYPE_BUTTON_UP: {
            int down = g->type == GR_EVENT_TYPE_BUTTON_DOWN;
            int changed = g->button.changebuttons;
            mouse_x = g->button.x, mouse_y = g->button.y;
            mouse_buttons = buttons_mask(g->button.buttons);
            if (changed & (MWBUTTON_SCROLLUP | MWBUTTON_SCROLLDN)) {
                if (!down) return;
                e.type = SDL_MOUSEWHEEL, e.wheel.windowID = 1, e.wheel.y = changed & MWBUTTON_SCROLLUP ? 1 : -1;
                push(&e);
                return;
            }
            e.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            e.button.windowID = 1, e.button.button = (Uint8)button_of(changed), e.button.state = down ? SDL_PRESSED : SDL_RELEASED;
            e.button.clicks = 1, e.button.x = mouse_x, e.button.y = mouse_y;
            push(&e);
            return;
        }
        case GR_EVENT_TYPE_MOUSE_POSITION:
        case GR_EVENT_TYPE_MOUSE_MOTION: {
            int x = g->mouse.x, y = g->mouse.y;
            if (x == mouse_x && y == mouse_y) return;
            e.type = SDL_MOUSEMOTION, e.motion.windowID = 1, e.motion.x = x, e.motion.y = y;
            e.motion.xrel = x - mouse_x, e.motion.yrel = y - mouse_y, e.motion.state = (Uint32)mouse_buttons;
            mouse_x = x, mouse_y = y;
            push(&e);
            return;
        }
        case GR_EVENT_TYPE_CLOSE_REQ:
            e.type = SDL_QUIT;
            push(&e);
            return;
        case GR_EVENT_TYPE_MOUSE_ENTER:
        case GR_EVENT_TYPE_MOUSE_EXIT:
        case GR_EVENT_TYPE_FOCUS_IN:
        case GR_EVENT_TYPE_FOCUS_OUT:
            e.type = SDL_WINDOWEVENT, e.window.windowID = 1;
            e.window.event = g->type == GR_EVENT_TYPE_MOUSE_ENTER ? SDL_WINDOWEVENT_ENTER : g->type == GR_EVENT_TYPE_MOUSE_EXIT
                ? SDL_WINDOWEVENT_LEAVE : g->type == GR_EVENT_TYPE_FOCUS_IN ? SDL_WINDOWEVENT_FOCUS_GAINED : SDL_WINDOWEVENT_FOCUS_LOST;
            push(&e);
            return;
        case GR_EVENT_TYPE_UPDATE:
            if (g->update.utype == GR_UPDATE_SIZE && window) {
                window->w = g->update.width, window->h = g->update.height;
                e.type = SDL_WINDOWEVENT, e.window.windowID = 1, e.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
                e.window.data1 = window->w, e.window.data2 = window->h;
                push(&e);
            }
            return;
    }
}
void SDL_PumpEvents(void) {
    if (!opened) return;
    GR_EVENT g;
    while (GrPeekEvent(&g)) {
        GrGetNextEvent(&g);
        translate(&g);
    }
}
int SDL_PollEvent(SDL_Event* e) {
    if (!q_count) SDL_PumpEvents();
    if (!q_count) return 0;
    if (e) *e = queue[q_head];
    if (e) q_head = (q_head + 1) % QUEUE, q_count--;
    return 1;
}
int SDL_PeepEvents(SDL_Event* out, int n, SDL_eventaction action, Uint32 lo, Uint32 hi) {
    int found = 0;
    if (action == SDL_ADDEVENT) {
        for (int i = 0; i < n; i++) push(&out[i]);
        return n;
    }
    for (int i = 0; i < q_count && found < n; i++) {
        SDL_Event* e = &queue[(q_head + i) % QUEUE];
        if (e->type < lo || e->type > hi) continue;
        if (out) out[found] = *e;
        found++;
        if (action == SDL_GETEVENT) {
            for (int k = i; k + 1 < q_count; k++) queue[(q_head + k) % QUEUE] = queue[(q_head + k + 1) % QUEUE];
            q_count--, i--;
        }
    }
    return found;
}
void SDL_FlushEvent(Uint32 type) {
    SDL_Event e;
    while (SDL_PeepEvents(&e, 1, SDL_GETEVENT, type, type) > 0) {}
}
const Uint8* SDL_GetKeyboardState(int* n) {
    if (n) *n = SDL_NUM_SCANCODES;
    return keys;
}
SDL_Keymod SDL_GetModState(void) { return mods; }
void SDL_StartTextInput(void) { text_input = 1; }
void SDL_StopTextInput(void) { text_input = 0; }
Uint32 SDL_GetMouseState(int* x, int* y) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    return (Uint32)mouse_buttons;
}
Uint32 SDL_GetGlobalMouseState(int* x, int* y) {
    if (x) *x = mouse_x + (window ? window->x : 0);
    if (y) *y = mouse_y + (window ? window->y : 0);
    return (Uint32)mouse_buttons;
}
int SDL_ShowCursor(int on) { (void)on; return 1; }
int SDL_SetRelativeMouseMode(SDL_bool on) { (void)on; return -1; }
void SDL_WarpMouseInWindow(SDL_Window* w, int x, int y) { (void)w; mouse_x = x, mouse_y = y; }
int SDL_WarpMouseGlobal(int x, int y) { mouse_x = x, mouse_y = y; return 0; }

// key names and scancodes: SDL's own tables for the keys a program names
static const struct { SDL_Keycode k; SDL_Scancode s; const char* name; } named[] = {
    {SDLK_RETURN, SDL_SCANCODE_RETURN, "Return"}, {SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, "Escape"},
    {SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE, "Backspace"}, {SDLK_TAB, SDL_SCANCODE_TAB, "Tab"},
    {SDLK_SPACE, SDL_SCANCODE_SPACE, "Space"}, {SDLK_DELETE, SDL_SCANCODE_DELETE, "Delete"},
    {SDLK_INSERT, SDL_SCANCODE_INSERT, "Insert"}, {SDLK_HOME, SDL_SCANCODE_HOME, "Home"},
    {SDLK_END, SDL_SCANCODE_END, "End"}, {SDLK_PAGEUP, SDL_SCANCODE_PAGEUP, "PageUp"},
    {SDLK_PAGEDOWN, SDL_SCANCODE_PAGEDOWN, "PageDown"}, {SDLK_LEFT, SDL_SCANCODE_LEFT, "Left"},
    {SDLK_RIGHT, SDL_SCANCODE_RIGHT, "Right"}, {SDLK_UP, SDL_SCANCODE_UP, "Up"}, {SDLK_DOWN, SDL_SCANCODE_DOWN, "Down"},
    {SDLK_LSHIFT, SDL_SCANCODE_LSHIFT, "Left Shift"}, {SDLK_RSHIFT, SDL_SCANCODE_RSHIFT, "Right Shift"},
    {SDLK_LCTRL, SDL_SCANCODE_LCTRL, "Left Ctrl"}, {SDLK_RCTRL, SDL_SCANCODE_RCTRL, "Right Ctrl"},
    {SDLK_LALT, SDL_SCANCODE_LALT, "Left Alt"}, {SDLK_RALT, SDL_SCANCODE_RALT, "Right Alt"},
    {SDLK_LGUI, SDL_SCANCODE_LGUI, "Left GUI"}, {SDLK_RGUI, SDL_SCANCODE_RGUI, "Right GUI"},
    {SDLK_PAUSE, SDL_SCANCODE_PAUSE, "Pause"}, {SDLK_PRINTSCREEN, SDL_SCANCODE_PRINTSCREEN, "PrintScreen"},
    {SDLK_MINUS, SDL_SCANCODE_MINUS, "-"}, {SDLK_EQUALS, SDL_SCANCODE_EQUALS, "="}, {SDLK_COMMA, SDL_SCANCODE_COMMA, ","},
    {SDLK_PERIOD, SDL_SCANCODE_PERIOD, "."}, {SDLK_SLASH, SDL_SCANCODE_SLASH, "/"}, {SDLK_SEMICOLON, SDL_SCANCODE_SEMICOLON, ";"},
    {SDLK_QUOTE, SDL_SCANCODE_APOSTROPHE, "'"}, {SDLK_LEFTBRACKET, SDL_SCANCODE_LEFTBRACKET, "["},
    {SDLK_RIGHTBRACKET, SDL_SCANCODE_RIGHTBRACKET, "]"}, {SDLK_BACKSLASH, SDL_SCANCODE_BACKSLASH, "\\"},
    {SDLK_BACKQUOTE, SDL_SCANCODE_GRAVE, "`"},
};
SDL_Scancode SDL_GetScancodeFromKey(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return (SDL_Scancode)(SDL_SCANCODE_A + (k - 'a'));
    if (k >= '1' && k <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (k - '1'));
    if (k == '0') return SDL_SCANCODE_0;
    if (k >= SDLK_F1 && k <= SDLK_F12) return (SDL_Scancode)(SDL_SCANCODE_F1 + (k - SDLK_F1));
    if (k >= SDLK_KP_1 && k <= SDLK_KP_9) return (SDL_Scancode)(SDL_SCANCODE_KP_1 + (k - SDLK_KP_1));
    if (k == SDLK_KP_0) return SDL_SCANCODE_KP_0;
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++)
        if (named[i].k == k) return named[i].s;
    return SDL_SCANCODE_UNKNOWN;
}
const char* SDL_GetKeyName(SDL_Keycode k) {
    static char one[2][8];
    static int which;
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++)
        if (named[i].k == k) return named[i].name;
    which ^= 1;
    if (k >= SDLK_F1 && k <= SDLK_F12) { snprintf(one[which], 8, "F%d", (int)(k - SDLK_F1 + 1)); return one[which]; }
    if (k > 32 && k < 127) { one[which][0] = (char)(k >= 'a' && k <= 'z' ? k - 32 : k), one[which][1] = 0; return one[which]; }
    return "";
}
SDL_Keycode SDL_GetKeyFromName(const char* name) {
    if (!name || !*name) return SDLK_UNKNOWN;
    for (unsigned i = 0; i < sizeof named / sizeof named[0]; i++)
        if (!strcasecmp(named[i].name, name)) return named[i].k;
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') return SDLK_F1 + atoi(name + 1) - 1;
    if (!name[1]) return name[0] >= 'A' && name[0] <= 'Z' ? name[0] + 32 : name[0];
    return SDLK_UNKNOWN;
}

// ---- files and sounds read from them ----
static Sint64 rw_size(SDL_RWops* c) { return ((FILE*)c->hidden.unknown.data1) ? (fseek(((FILE*)c->hidden.unknown.data1), 0, SEEK_END), ftell(((FILE*)c->hidden.unknown.data1))) : -1; }
static Sint64 rw_seek(SDL_RWops* c, Sint64 o, int w) { fseek(((FILE*)c->hidden.unknown.data1), (long)o, w); return ftell(((FILE*)c->hidden.unknown.data1)); }
static size_t rw_read(SDL_RWops* c, void* p, size_t s, size_t n) { return fread(p, s, n, ((FILE*)c->hidden.unknown.data1)); }
static size_t rw_write(SDL_RWops* c, const void* p, size_t s, size_t n) { return fwrite(p, s, n, ((FILE*)c->hidden.unknown.data1)); }
static int rw_close(SDL_RWops* c) { if (((FILE*)c->hidden.unknown.data1)) fclose(((FILE*)c->hidden.unknown.data1)); free(c); return 0; }
SDL_RWops* SDL_RWFromFile(const char* file, const char* mode) {
    FILE* f = fopen(file, mode);
    if (!f) { fail("cannot open"); return NULL; }
    SDL_RWops* c = calloc(1, sizeof *c);
    c->size = rw_size, c->seek = rw_seek, c->read = rw_read, c->write = rw_write, c->close = rw_close;
    c->type = SDL_RWOPS_UNKNOWN, c->hidden.unknown.data1 = f;
    return c;
}
// a RIFF WAVE of PCM, 8 or 16 bits, read whole
SDL_AudioSpec* SDL_LoadWAV_RW(SDL_RWops* src, int freesrc, SDL_AudioSpec* spec, Uint8** buf, Uint32* len) {
    SDL_AudioSpec* got = NULL;
    Uint8 head[12];
    *buf = NULL, *len = 0;
    if (!src || src->read(src, head, 1, 12) != 12 || memcmp(head, "RIFF", 4) || memcmp(head + 8, "WAVE", 4)) goto done;
    for (;;) {
        Uint8 ch[8];
        if (src->read(src, ch, 1, 8) != 8) break;
        Uint32 n = ch[4] | ch[5] << 8 | ch[6] << 16 | (Uint32)ch[7] << 24;
        if (!memcmp(ch, "fmt ", 4)) {
            Uint8 f[40];
            Uint32 take = n < sizeof f ? n : sizeof f;
            if (src->read(src, f, 1, take) != take) break;
            if (n > take) src->seek(src, n - take, RW_SEEK_CUR);
            memset(spec, 0, sizeof *spec);
            spec->channels = f[2];
            spec->freq = f[4] | f[5] << 8 | f[6] << 16 | f[7] << 24;
            spec->format = f[14] == 8 ? AUDIO_U8 : AUDIO_S16LSB;
            spec->samples = 4096;
        } else if (!memcmp(ch, "data", 4)) {
            *buf = malloc(n ? n : 1);
            *len = (Uint32)src->read(src, *buf, 1, n);
            got = spec;
            break;
        } else {
            src->seek(src, n + (n & 1), RW_SEEK_CUR);
        }
    }
done:
    if (freesrc && src) src->close(src);
    if (!got) fail("not a WAVE file");
    return got;
}
void SDL_FreeWAV(Uint8* buf) { free(buf); }

// ---- sound: not yet (the program goes on silent when a device does not open, as on a machine with none) ----
SDL_AudioDeviceID SDL_OpenAudioDevice(const char* device, int iscapture, const SDL_AudioSpec* want, SDL_AudioSpec* have,
                                      int changes) {
    (void)device, (void)iscapture, (void)want, (void)have, (void)changes;
    fail("no sound device yet");
    return 0;
}
void SDL_CloseAudioDevice(SDL_AudioDeviceID d) { (void)d; }
void SDL_PauseAudioDevice(SDL_AudioDeviceID d, int on) { (void)d, (void)on; }
void SDL_LockAudioDevice(SDL_AudioDeviceID d) { (void)d; }
void SDL_UnlockAudioDevice(SDL_AudioDeviceID d) { (void)d; }
