/* The game's picture in a Nano-X window: an 8-bit page in GPU memory drawn over the window, and the
 * window system's events as Windows' numbers. Apart from the rest of the port: it has no windows.h. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <GLES/segl.h>
#include <nano-X.h>
#include "aoe_window.h"

#define ONE 65536
static GR_WINDOW_ID window;
static GLuint texture;
static uint8_t *page;
static int page_w, page_h, drawn;
static volatile const uint32_t *control_words;

unsigned aoe_win_clock(void) { return control_words[0x34 / 4]; }

uint8_t *aoe_win_open(int w, int h, const char *title)
{
    if (GrOpen() < 0) { fprintf(stderr, "aoe: no Nano-X server (start one: nano-X -p &)\n"); exit(1); }
    /* A frame like any program's: what the screen has no room for of it is off the screen (docs/system/nanox.md,
     * "A window wider or taller than the screen has room for"), and the game's picture stays whole, above the bar. */
    window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW | GR_WM_PROPS_ONTOP, (char *) title, GR_ROOT_WINDOW_ID, -1, -1, w, h, 0);
    GrSelectEvents(window, GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_KEY_UP | GR_EVENT_MASK_BUTTON_DOWN |
        GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_MOUSE_POSITION | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE);
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

void aoe_win_present(void)
{
    static const GLfixed whole[16] = { 2 * ONE, 0, 0, 0, 0, -2 * ONE, 0, 0, 0, 0, -ONE, 0, -ONE, ONE, 0, ONE };
    static const GLfixed xyz[12] = { 0, 0, 0, ONE, 0, 0, ONE, ONE, 0, 0, ONE, 0 };
    static const GLfixed uv[8] = { 0, 0, ONE, 0, ONE, ONE, 0, ONE };
    int width, height;
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

/* One event, or 0. */
int aoe_win_event(struct aoe_event *e)
{
    GR_EVENT g;
    if (!window) return 0;
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
            e->kind = g.type == GR_EVENT_TYPE_BUTTON_DOWN ? AOE_BUTTON_DOWN : AOE_BUTTON_UP;
            e->a = g.button.x; e->b = g.button.y;
            e->c = g.type == GR_EVENT_TYPE_BUTTON_DOWN ? g.button.changebuttons : g.button.changebuttons;
            e->c = e->c & GR_BUTTON_L ? 1 : e->c & GR_BUTTON_R ? 2 : 0;
            if (!e->c) continue;
            return 1;
        case GR_EVENT_TYPE_MOUSE_POSITION:
            e->kind = AOE_MOVE; e->a = g.mouse.x; e->b = g.mouse.y;
            return 1;
        case GR_EVENT_TYPE_CLOSE_REQ:
            e->kind = AOE_CLOSE;
            return 1;
        case GR_EVENT_TYPE_UPDATE:
            if (g.update.utype == GR_UPDATE_SIZE) seglWindowChanged();
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
