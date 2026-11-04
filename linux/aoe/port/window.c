/* The game's picture in a Nano-X window: an 8-bit page in GPU memory drawn over the window, and the
 * window system's events as Windows' numbers. Apart from the rest of the port: it has no windows.h. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <GLES/segl.h>
#include <nano-X.h>
#include "aoe_window.h"
#include "ui_single.h"

#define ONE 65536
static GR_WINDOW_ID window;
static GLuint texture;
static uint8_t *page;
static int page_w, page_h, drawn, hidden;
static volatile const uint32_t *control_words;

unsigned aoe_win_clock(void) { return control_words[0x34 / 4]; }
/* the machine's input words (docs/machine/input.md): the pointer's place and its buttons, left 1, right 2 */
int aoe_win_buttons(void) { return control_words[0x20 / 4 + 2]; }
void aoe_win_pointer(int *x, int *y) { *x = control_words[0x20 / 4]; *y = control_words[0x20 / 4 + 1]; }

uint8_t *aoe_win_open(int w, int h, const char *title)
{
    if (window) {
        /* the game's layout changed (aoe_win_layout): the window keeps its size and the picture is stretched over it; a page of
         * each size is kept, so switching back and forth takes no more GPU memory (docs/games/aoe.md, "Two layouts") */
        /* (four: 640 and 800 wide, each at 4:3 for the menus and at the window's shape for the game screen) */
        static uint8_t *kept[4];
        static int kept_w[4], kept_h[4];
        int i;
        aoe_win_settle();
        for (i = 0; i < 4 && kept[i] && (kept_w[i] != page_w || kept_h[i] != page_h); i++)
            ;
        if (i < 4 && !kept[i]) kept[i] = page, kept_w[i] = page_w, kept_h[i] = page_h;
        for (i = 0; i < 4 && kept[i] && (kept_w[i] != w || kept_h[i] != h); i++)
            ;
        page = i < 4 && kept[i] ? kept[i] : seglMemory(w * h);
        page_w = w; page_h = h;
        memset(page, 0, w * h);
        glBindTexture(GL_TEXTURE_2D, texture);
        seglTexturePointer(page, w, h, GL_COLOR_INDEX8_EXT);
        return page;
    }
    if (GrOpen() < 0) { fprintf(stderr, "aoe: no Nano-X server (start one: nano-X -p &)\n"); exit(1); }
    if (ui_single_instance()) { GrClose(); _exit(0); }   /* a game takes all the memory one needs: the one that is up comes forward */
    /* A window like any program's, under other windows and the bar. Only on a screen with no room for its frame and the bar (the frame's
     * caption 29 rows, the bar 26) what is left of the frame is off the screen (docs/system/nanox.md, "A window wider or taller than the
     * screen has room for"), and the game's picture stays whole, above the bar. */
    GR_SCREEN_INFO screen;
    GrGetScreenInfo(&screen);
    /* Started from the desktop as a game, it opens at the maximised size it is given (SE_MAXIMISED, ui_launch.h): the window
     * manager then maximises it with no resize, and the picture is stretched over it as over any resized window. */
    int ww = w, wh = h;
    if (getenv("SE_MAXIMISED") && sscanf(getenv("SE_MAXIMISED"), "%dx%d", &ww, &wh) != 2) ww = w, wh = h;
    if (ww < 320 || wh < 200) ww = w, wh = h;
    window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW | (screen.rows < wh + (wh != h ? 55 : 58) ? GR_WM_PROPS_ONTOP : 0), (char *) title, GR_ROOT_WINDOW_ID, -1, -1, ww, wh, 0);
    GrSelectEvents(window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN |
        GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_POSITION | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_EXPOSURE);
    {
        /* the extra buttons: the left hand's stick scrolls the map (docs/machine/input.md, "Game controls") */
        GR_WM_PROPERTIES props;
        props.flags = GR_WM_FLAGS_COMMAND, props.props = GR_WMC_BUTTONS, props.background = 1;
        GrSetWMProperties(window, &props);
    }
    GrMapWindow(window);
    GrSetFocus(window);
    if (seglInit(window) < 0) { fprintf(stderr, "aoe: this machine has no GPU\n"); exit(1); }
    control_words = (volatile const uint32_t *) ((const char *) seglPalette() - 0x400);
    page_w = w; page_h = h;
    page = seglMemory(w * h);
    memset(page, 0, w * h);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    seglTexturePointer(page, w, h, GL_COLOR_INDEX8_EXT);
    return page;
}

void aoe_win_settle(void)
{
    if (drawn) { seglSettle(); drawn = 0; }
}

void aoe_win_palette(const uint8_t *rgbx)
{
    aoe_win_settle();
    unsigned *words = seglPalette();
    for (int i = 0; i < 256; i++)
        words[i] = rgbx[i * 4] << 16 | rgbx[i * 4 + 1] << 8 | rgbx[i * 4 + 2];
}

void aoe_win_not_a_frame(void) { seglNotAFrame(); }

void aoe_win_present(void)
{
    static const GLfixed whole[16] = { 2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE };
    static const GLfixed xyz[12] = { 0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0 };
    static const GLfixed uv[8] = { 0, 0, ONE, 0, ONE, ONE, 0, ONE };
    int width, height;
    if (hidden) return;   /* the bar's button took it down: nothing shows, and a list here would be for a window that is not mapped */
    seglSize(&width, &height);
    glViewport(0, 0, width, height);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixx(whole);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    seglQuad(xyz, uv, texture, 255, 255, 0);
    seglSwapEarly();
    drawn = 1;
}

/* A point of the window, in its pixels now, as a pixel of the picture: the window may have been resized or
 * maximized, and the picture is stretched over it (docs/games/aoe.md, "Windowed"). Not held to the picture: a
 * held button's words go on past the window's sides. */
void aoe_win_map(int *x, int *y)
{
    int width, height;
    seglSize(&width, &height);
    if (width <= 0 || height <= 0 || (width == page_w && height == page_h)) return;
    *x = (int) ((long long) *x * page_w / width) - (*x < 0);
    *y = (int) ((long long) *y * page_h / height) - (*y < 0);
}

void aoe_win_size(int *w, int *h) { seglSize(w, h); }

void aoe_win_origin(int *x, int *y)
{
    GR_WINDOW_INFO info;
    GR_WINDOW_ID w = window;
    *x = *y = 0;
    while (w && w != GR_ROOT_WINDOW_ID) {
        GrGetWindowInfo(w, &info);
        *x += info.x; *y += info.y;
        w = info.parent;
    }
}

void aoe_win_close(void)
{
    aoe_win_settle();
    if (window) GrClose();
    window = 0;
}

static int vk_of(GR_KEY k)
{
    if (k >= 'a' && k <= 'z') return k - 32;
    if (k >= 'A' && k <= 'Z') return k;
    if (k >= '0' && k <= '9') return k;
    switch (k) {
    case MWKEY_LEFT: return 0x25;
    case MWKEY_UP: return 0x26;
    case MWKEY_RIGHT: return 0x27;
    case MWKEY_DOWN: return 0x28;
    case MWKEY_ENTER: return 0x0d;
    case MWKEY_ESCAPE: return 0x1b;
    case MWKEY_BACKSPACE: return 0x08;
    case MWKEY_TAB: return 0x09;
    case MWKEY_DELETE: return 0x2e;
    case MWKEY_HOME: return 0x24;
    case MWKEY_END: return 0x23;
    case MWKEY_PAGEUP: return 0x21;
    case MWKEY_PAGEDOWN: return 0x22;
    case MWKEY_LSHIFT: case MWKEY_RSHIFT: return 0x10;
    case MWKEY_LCTRL: case MWKEY_RCTRL: return 0x11;
    case MWKEY_LALT: case MWKEY_RALT: return 0x12;
    case ' ': return 0x20;
    }
    if (k >= MWKEY_F1 && k <= MWKEY_F12) return 0x70 + (k - MWKEY_F1);
    return 0;
}

/* A controller's notch of the left hand's stick: its arrow key held PAD_HOLD_MS, which scrolls the map
 * (docs/machine/input.md, "Game controls"). */
#define PAD_HOLD_MS 120
static int pad_key;                 /* the arrow held, or 0 */
static long long pad_until;
static long long pad_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* The layout for the window's size (docs/games/aoe.md, "Two layouts"): 800 x 600 once the window has room for most of it, else 640 x
 * 480. A size is taken a second after the last change (a drag gives many). The width wanted when it should change, else 0. */
int aoe_win_game_height(int width);
static int sized_w, sized_h;
static long long sized_at;
int aoe_win_layout(void)
{
    static int fixed = -1;
    int want;
    if (fixed < 0) { const char *e = getenv("AOE_LAYOUT"); fixed = e && !strcmp(e, "fixed"); }
    if (fixed || !sized_at || pad_ms() - sized_at < 1000) return 0;
    sized_at = 0;
    want = sized_w >= 760 && sized_h >= 540 ? 800 : 640;
    /* (the same width with another shape: the game screen's height follows the window too) */
    return want != page_w || (page_h != want * 3 / 4 && page_h != aoe_win_game_height(want)) ? want : 0;
}

/* The game screen's height for a layout WIDTH wide in the window as it is now: the window's shape, so the picture is
 * scaled by one factor (docs/games/aoe.md, "The game screen in the window's shape"); its top band and bottom panel are
 * placed from the screen's height, the view is what lies between (scr_game.cpp's dy, aoe.patch). In whole 2s, from 9:16
 * of the width to the width. The
 * menus stay 4:3 (their pictures are 800 x 600). AOE_LAYOUT=fixed or 43: 4:3 always, the way before. */
int aoe_win_game_height(int width)
{
    static int four_three = -1;
    int ww = 0, wh = 0, h;
    if (four_three < 0) { const char *e = getenv("AOE_LAYOUT"); four_three = e && (!strcmp(e, "fixed") || !strcmp(e, "43")); }
    if (window) seglSize(&ww, &wh);
    if (four_three || width < 800 || ww <= 0 || wh <= 0) return width * 3 / 4;   /* (640's layout is 4:3: only 800's places follow the height) */
    h = (int) ((long long) width * wh / ww) & ~1;
    return h < width * 9 / 16 ? (width * 9 / 16) & ~1 : h > width ? width : h;
}

/* One event, or 0. */
int aoe_win_event(struct aoe_event *e)
{
    GR_EVENT g;
    if (!window) return 0;
    if (pad_key && pad_ms() >= pad_until) {
        e->kind = AOE_KEY_UP, e->a = pad_key, e->b = 0;
        pad_key = 0;
        return 1;
    }
    for (;;) {
        if (!GrPeekEvent(&g)) return 0;
        GrGetNextEvent(&g);
        switch (g.type) {
        case GR_EVENT_TYPE_KEY_DOWN:
        case GR_EVENT_TYPE_KEY_UP:
            e->kind = g.type == GR_EVENT_TYPE_KEY_DOWN ? AOE_KEY_DOWN : AOE_KEY_UP;
            e->a = vk_of(g.keystroke.ch);
            e->b = g.keystroke.ch < 256 ? g.keystroke.ch : 0;
            if (!e->a && !e->b) continue;
            return 1;
        case GR_EVENT_TYPE_BUTTON_DOWN:
        case GR_EVENT_TYPE_BUTTON_UP:
            if (g.type == GR_EVENT_TYPE_BUTTON_DOWN && !pad_key &&
                (g.button.changebuttons & (GR_BUTTON_SCROLL2LEFT | GR_BUTTON_SCROLL2UP | GR_BUTTON_SCROLL2RIGHT | GR_BUTTON_SCROLL2DN))) {
                GR_BUTTON b = g.button.changebuttons;
                pad_key = b & GR_BUTTON_SCROLL2LEFT ? 0x25 : b & GR_BUTTON_SCROLL2UP ? 0x26 : b & GR_BUTTON_SCROLL2RIGHT ? 0x27 : 0x28;
                pad_until = pad_ms() + PAD_HOLD_MS;
                e->kind = AOE_KEY_DOWN, e->a = pad_key, e->b = 0;
                if (getenv("AOE_INPUT_LOG")) fprintf(stderr, "aoe: pad holds key %d\n", pad_key);
                return 1;
            }
            e->kind = g.type == GR_EVENT_TYPE_BUTTON_DOWN ? AOE_BUTTON_DOWN : AOE_BUTTON_UP;
            e->a = g.button.x; e->b = g.button.y;
            aoe_win_map(&e->a, &e->b);
            e->c = g.type == GR_EVENT_TYPE_BUTTON_DOWN ? g.button.changebuttons : g.button.changebuttons;
            e->c = e->c & GR_BUTTON_L ? 1 : e->c & GR_BUTTON_R ? 2 : 0;
            if (!e->c) continue;
            return 1;
        case GR_EVENT_TYPE_MOUSE_POSITION:
            e->kind = AOE_MOVE; e->a = g.mouse.x; e->b = g.mouse.y;
            aoe_win_map(&e->a, &e->b);
            return 1;
        case GR_EVENT_TYPE_CLOSE_REQ:
            e->kind = AOE_CLOSE;
            return 1;
        case GR_EVENT_TYPE_UPDATE:
            if (g.update.utype == GR_UPDATE_UNMAP || g.update.utype == GR_UPDATE_UNMAPTEMP || g.update.utype == GR_UPDATE_DESTROY) {
                hidden = 1;
                continue;
            }
            hidden = 0;
            if (g.update.utype == GR_UPDATE_SIZE && g.update.wid == window)
                sized_w = g.update.width, sized_h = g.update.height, sized_at = pad_ms();
            /* A frame or a move gives the window other memory, and a game at its menu draws nothing more: the page
             * is still the picture, so it is shown again (docs/games/aoe.md, "A window made again"). */
            seglWindowChanged();
            aoe_win_present();
            continue;
        case GR_EVENT_TYPE_EXPOSURE:
            aoe_win_present();
            continue;
        default:
            continue;
        }
    }
}

void aoe_win_rest(void)
{
    aoe_win_settle();
    __asm__ volatile(".word 0x0100000f");   /* pause */
}
