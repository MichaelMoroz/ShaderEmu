// Frames a second, written into the caption of the window a program's frames are drawn in.
// The caption is rows of the same buffer as the window's own pixels, just above them, so this
// is a few hundred stores a second and asks the window system for nothing. Include after
// nano-X.h and call fps_caption() once a frame (docs/system/nanox.md, "Frames a second").
#ifndef FPS_CAPTION_H
#define FPS_CAPTION_H
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>

#define FPS_ABOVE 23            // rows from the caption's top to the window's first (nanowm.h: 22 and a line)
#define FPS_W 96              // the widest text, "999 fps 99.9 ms"
#define FPS_H 11
#define FPS_TOP 5               // its rows in the caption

static struct {
    int fd;                     // /dev/gpu, opened here
    uint32_t address, row;      // the window's buffer as last seen
    uint32_t* caption;          // the caption's first row above the window's first pixel, or 0
    int width;                  // of the program's window under that caption
    uint32_t since, frames, started;
    int left, used;             // the text's box, and whether its pixels below are taken
    uint32_t under[FPS_H][FPS_W + 1], edge[FPS_H][2];   // the caption's pixels there, and the two left of it
} fps_state = { -1 };

// 5 x 7 each: the digits, then f, p, s, m and the point
static const unsigned char fps_font[15][7] = {
    { 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 }, { 14, 17, 1, 2, 4, 8, 31 }, { 30, 1, 1, 14, 1, 1, 30 },
    { 2, 6, 10, 18, 31, 2, 2 }, { 31, 16, 30, 1, 1, 17, 14 }, { 6, 8, 16, 30, 17, 17, 14 }, { 31, 1, 2, 4, 8, 8, 8 },
    { 14, 17, 17, 14, 17, 17, 14 }, { 14, 17, 17, 15, 1, 2, 12 }, { 6, 9, 8, 28, 8, 8, 8 }, { 0, 0, 30, 17, 30, 16, 16 },
    { 0, 0, 15, 16, 14, 1, 30 }, { 0, 0, 26, 21, 21, 21, 21 }, { 0, 0, 0, 0, 0, 24, 24 },
};
#define FPS_GLYPH_F 10
#define FPS_GLYPH_M 13
#define FPS_GLYPH_POINT 14

static uint32_t fps_mix(uint32_t a, uint32_t b) { return 0xff000000u | (((a & 0xfefefe) + (b & 0xfefefe)) >> 1); }

// Finds the caption above a window's pixels; none for a window the window manager gave no frame.
static void fps_locate(const GR_WINDOW_INFO* info) {
    GR_WINDOW_INFO own = *info, above;
    uint32_t row = info->surface_row ? info->surface_row : (uint32_t)info->width, first, bytes;
    void* memory;
    int tries;

    fps_state.address = info->surface_address;
    fps_state.row = row;
    fps_state.caption = 0;
    if (!info->surface_address) return;
    // The window the frames go to is the program's own, in the window manager's frame, or a
    // part of that at its top left corner (a player's picture above its controls).
    for (tries = 0; ; tries++) {
        if (tries == 2 || own.parent == GR_ROOT_WINDOW_ID) return;
        GrGetWindowInfo(own.parent, &above);
        if (above.parent == GR_ROOT_WINDOW_ID) break;
        if (own.x != 0 || own.y != 0) return;
        own = above;
    }
    if (own.y < FPS_ABOVE || own.width < 200) return;
    fps_state.width = own.width;
    if (fps_state.fd < 0 && (fps_state.fd = open("/dev/gpu", O_RDWR)) < 0) return;
    first = info->surface_address - 0x80000000u - FPS_ABOVE * row * 4;
    bytes = (first & 4095) + FPS_ABOVE * row * 4;
    memory = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fps_state.fd, first & ~4095u);
    if (memory != MAP_FAILED) fps_state.caption = (uint32_t*)((char*)memory + (first & 4095));
}

// A number's glyphs at `to`: its digits, with a point before the last `decimals` of them.
static int fps_number(int* to, uint32_t v, int decimals) {
    int digits[6], n = 0, k, out = 0;
    do digits[n++] = (int)(v % 10), v /= 10; while (v || n <= decimals);
    for (k = n - 1; k >= 0; k--) {
        to[out++] = digits[k];
        if (k == decimals && decimals) to[out++] = FPS_GLYPH_POINT;
    }
    return out;
}

// A frame was shown. `now` is the machine's clock in milliseconds (the word at 0x81000034).
// Once a second (a slow program: over three frames or 2.5 s) the caption gets the rate and the
// mean frame's time, drawn on the caption's own pixels in its text's colour (dark on a light
// caption, light on a dark one). Under 10 frames a second the rate has a tenth.
static void fps_caption(const GR_WINDOW_INFO* info, uint32_t now) {
    uint32_t elapsed = now - fps_state.since, rate, tenths, *to, ink, shade, sum = 0;
    int x, y, k, n = 0, glyph[20], left, width, box, changed;

    if (!fps_state.started) {   // (counted from the first frame, not from the machine's start)
        fps_state.started = 1, fps_state.since = now;
        return;
    }
    fps_state.frames++;
    if (elapsed < 1000 || (elapsed < 2500 && fps_state.frames < 3)) return;   // (a slow program's mean of a few)
    rate = (uint32_t)((uint64_t)fps_state.frames * 10000 / elapsed);   // tenths of a frame a second
    tenths = elapsed * 10 / fps_state.frames;                           // of a millisecond a frame
    fps_state.frames = 0;
    fps_state.since = now;
    if (info->surface_address != fps_state.address || (info->surface_row && info->surface_row != fps_state.row)) {
        fps_locate(info);
        fps_state.used = 0;
    }
    if (!fps_state.caption) return;
    n = rate < 100 ? fps_number(glyph, rate, 1) : fps_number(glyph, rate > 9999 ? 999 : rate / 10, 0);
    glyph[n++] = -1, glyph[n++] = FPS_GLYPH_F, glyph[n++] = FPS_GLYPH_F + 1, glyph[n++] = FPS_GLYPH_F + 2, glyph[n++] = -1;
    if (tenths < 100000) {
        n += tenths < 10000 ? fps_number(glyph + n, tenths, 1) : fps_number(glyph + n, tenths / 10, 0);
        glyph[n++] = -1, glyph[n++] = FPS_GLYPH_M, glyph[n++] = FPS_GLYPH_F + 2;
    } else {
        // ten seconds a frame and more: seconds
        n += fps_number(glyph + n, tenths / 1000 > 9999 ? 9999 : tenths / 1000, 1);
        glyph[n++] = -1, glyph[n++] = FPS_GLYPH_F + 2;
    }
    width = 0;
    for (k = 0; k < n; k++) width += glyph[k] == FPS_GLYPH_POINT ? 3 : 6;
    // a box of the widest text's room, left of the caption's three boxes (18 wide each, nanowm.h)
    box = fps_state.width - 3 * 18 - 10 - FPS_W;
    if (box < fps_state.width / 3) return;   // (the title keeps its room)
    // The box's caption pixels are taken once, and again when the two columns left of it have
    // changed (the caption painted anew: focus, title): our own text is never taken for them,
    // even where a list of the window system's put an older text back after we wrote.
    changed = !fps_state.used || box != fps_state.left;
    for (y = 0; y < FPS_H && !changed; y++)
        for (x = 0, to = fps_state.caption + (FPS_TOP + y) * fps_state.row + box - 2; x < 2; x++)
            if (to[x] != fps_state.edge[y][x]) changed = 1;
    for (y = 0; y < FPS_H; y++) {
        to = fps_state.caption + (FPS_TOP + y) * fps_state.row + box;
        if (changed) {
            for (x = 0; x < FPS_W + 1; x++) fps_state.under[y][x] = to[x];
            fps_state.edge[y][0] = to[-2], fps_state.edge[y][1] = to[-1];
        } else
            for (x = 0; x < FPS_W + 1; x++) to[x] = fps_state.under[y][x];
    }
    fps_state.left = box, fps_state.used = 1;
    left = box + FPS_W - width;
    for (y = 0; y < FPS_H; y++)
        for (x = 0; x < width; x += 4) {
            uint32_t c = fps_state.under[y][FPS_W - width + x];
            sum += ((c >> 16 & 255) * 3 + (c >> 8 & 255) * 6 + (c & 255)) / 10;
        }
    ink = sum / (FPS_H * ((width + 3) / 4)) > 140 ? 0xff101828u : 0xfff4f8ffu;
    shade = ink == 0xff101828u ? 0xffffffffu : 0xff000000u;
    // the shadow one pixel down and right, half blended, then the letters
    for (k = 0, x = 0; k < n; x += glyph[k] == FPS_GLYPH_POINT ? 3 : 6, k++)
        for (y = 0; y < 7 && glyph[k] >= 0; y++)
            for (int i = 0; i < 5; i++)
                if (fps_font[glyph[k]][y] & (16 >> i)) {
                    to = fps_state.caption + (FPS_TOP + 3 + y) * fps_state.row + left + x + i;
                    to[fps_state.row + 1] = fps_mix(to[fps_state.row + 1], shade);
                }
    for (k = 0, x = 0; k < n; x += glyph[k] == FPS_GLYPH_POINT ? 3 : 6, k++)
        for (y = 0; y < 7 && glyph[k] >= 0; y++)
            for (int i = 0; i < 5; i++)
                if (fps_font[glyph[k]][y] & (16 >> i))
                    fps_state.caption[(FPS_TOP + 3 + y) * fps_state.row + left + x + i] = ink;
}
#endif
