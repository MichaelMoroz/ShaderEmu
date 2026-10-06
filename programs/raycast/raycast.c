// A Wolfenstein-style raycaster for the bare-metal machine: textured walls on a grid map,
// walked with the keyboard. 160x100 in the 8-bit indexed display mode, integer only.
// Nothing in the game depends on time: a frame is drawn when a key changes the view.

#include "../common/platform.h"
#include "sintab.h"

#define W 160
#define H 100
#define TEX 64
#define MAP 24

typedef int32_t fx;   // 16.16
#define FX(x) ((fx)((x) * 65536.0))
#define ONE 65536

static inline fx mul(fx a, fx b) { return (fx)(((int64_t)a * b) >> 16); }
static inline fx fsin(int a) { return sintab[a & 255]; }
static inline fx fcos(int a) { return sintab[(a + 64) & 255]; }

// Wall types 1..6 pick a texture; '.' is open floor.
static const char map[MAP][MAP + 1] = {
    "111111111111222222222222",
    "1..........1...........2",
    "1..........1...........2",
    "1....33....1....66.....2",
    "1....33................2",
    "1..........1....66.....2",
    "1..........1...........2",
    "1111..111111...........2",
    "4.........5....2222..222",
    "4.........5....2.......3",
    "4...5.....5....2.......3",
    "4...5..........2..6.6..3",
    "4...5555..5....2.......3",
    "4.........5....2..6.6..3",
    "4.........5............3",
    "44444..4445....2.......3",
    "3.........3....2.......3",
    "3..4...4..3....233..3333",
    "3.........3............1",
    "3..4...4.......1.......1",
    "3.........3....1...5...1",
    "3..4...4..3....1.......1",
    "3.........3....1.......1",
    "333333333333333311111111",
};

static inline int wall(int x, int y) {
    char c = map[y][x];
    return c == '.' ? 0 : c - '0';
}

// A texel is (hue << 4) | brightness. Textures are stored by column, since walls are drawn so.
static uint8_t textures[6][TEX][TEX];
// shade[d][texel]: the texel darkened by d brightness steps (distance, and which side was hit).
static uint8_t shade[16][256];
static uint8_t ceil_row[H], floor_row[H];

static uint32_t rng = 0x2545f491;
static uint32_t noise(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static const uint8_t hue_rgb[8][3] = {
    {200, 200, 205},   // 0 grey stone
    {215, 95, 70},     // 1 brick
    {190, 135, 75},    // 2 wood
    {90, 130, 230},    // 3 blue tile
    {110, 190, 100},   // 4 moss
    {150, 200, 215},   // 5 steel
    {95, 100, 125},    // 6 ceiling
    {150, 140, 125},   // 7 floor
};

static void game_palette(void) {
    for (int h = 0; h < 16; h++)
        for (int l = 0; l < 16; l++) {
            const uint8_t* c = hue_rgb[h & 7];
            uint32_t r = c[0] * (l + 1) / 16, g = c[1] * (l + 1) / 16, b = c[2] * (l + 1) / 16;
            DISP_PALETTE[h * 16 + l] = (r << 16) | (g << 8) | b;
        }
}

static uint8_t texel(int hue, int light) {
    if (light < 1) light = 1;
    if (light > 15) light = 15;
    return (uint8_t)((hue << 4) | light);
}

static void make_textures(void) {
    for (int x = 0; x < TEX; x++)
        for (int y = 0; y < TEX; y++) {
            int n = (int)(noise() & 3);
            // brick: courses of 16 rows, every other course shifted half a brick
            int bx = (x + ((y >> 4) & 1) * 16) & 31;
            textures[0][x][y] = (y & 15) == 0 || bx == 0 ? texel(0, 5 + n) : texel(1, 10 + n);
            // stone blocks with a dark joint
            int edge = (x & 31) < 2 || (y & 31) < 2;
            textures[1][x][y] = edge ? texel(0, 4) : texel(0, 9 + n + (((x >> 5) ^ (y >> 5)) & 1) * 2);
            // wooden planks with vertical grain
            textures[2][x][y] = (x & 15) == 0 ? texel(2, 4) : texel(2, 9 + ((x * 7 + (y >> 3)) & 3) + (n & 1));
            // blue tiles with a light border
            int tb = (x & 15) < 1 || (y & 15) < 1;
            textures[3][x][y] = tb ? texel(0, 12) : texel(3, 9 + (((x >> 4) + (y >> 4)) & 1) * 3 + (n & 1));
            // moss: noisy green over stone
            textures[4][x][y] = (noise() & 7) < 2 ? texel(0, 7 + n) : texel(4, 7 + n * 2);
            // steel plate with rivets
            int rx = (x & 31) - 4, ry = (y & 31) - 4;
            int rivet = rx * rx + ry * ry < 6;
            textures[5][x][y] = rivet ? texel(5, 15) : ((x & 31) == 0 || (y & 31) == 0 ? texel(5, 5) : texel(5, 10 + (n & 1)));
        }
    for (int d = 0; d < 16; d++)
        for (int t = 0; t < 256; t++) {
            int l = (t & 15) - d;
            shade[d][t] = (uint8_t)((t & 0xf0) | (l < 1 ? 1 : l));
        }
    for (int y = 0; y < H; y++) {
        int away = y < H / 2 ? H / 2 - y : y - H / 2 + 1;   // rows from the horizon
        int l = 2 + away * 12 / (H / 2);
        ceil_row[y] = texel(6, l);
        floor_row[y] = texel(7, l);
    }
}

typedef struct {
    fx x, y;
    int angle;   // 0..255
} player;

typedef struct {
    int start, end;          // rows of the wall slice, clipped to the screen
    const uint8_t* column;   // texture column
    const uint8_t* lut;      // shade table for this slice
    fx pos, step;            // texture row at `start`, and its increment per screen row
} slice;

// Casts the ray for screen column x (DDA over the grid) and describes its wall slice.
static void cast(const player* p, int x, slice* s) {
    fx dir_x = fcos(p->angle), dir_y = fsin(p->angle);
    fx cam = ((2 * x - W + 1) << 16) / W;
    fx ray_x = dir_x + mul(mul(-dir_y, FX(0.66)), cam);
    fx ray_y = dir_y + mul(mul(dir_x, FX(0.66)), cam);
    int map_x = p->x >> 16, map_y = p->y >> 16;
    fx ax = ray_x < 0 ? -ray_x : ray_x, ay = ray_y < 0 ? -ray_y : ray_y;
    // distance along the ray between grid lines; a ray nearly parallel to them never reaches one
    int64_t delta_x = ax < 16 ? ((int64_t)1 << 40) : (int64_t)(0xffffffffu / (uint32_t)ax);
    int64_t delta_y = ay < 16 ? ((int64_t)1 << 40) : (int64_t)(0xffffffffu / (uint32_t)ay);
    fx frac_x = p->x & 0xffff, frac_y = p->y & 0xffff;
    int step_x = ray_x < 0 ? -1 : 1, step_y = ray_y < 0 ? -1 : 1;
    int64_t side_x = ((ray_x < 0 ? frac_x : ONE - frac_x) * delta_x) >> 16;
    int64_t side_y = ((ray_y < 0 ? frac_y : ONE - frac_y) * delta_y) >> 16;
    int side = 0, type = 0;
    for (int i = 0; i < 2 * MAP && !type; i++) {
        if (side_x < side_y) {
            side_x += delta_x;
            map_x += step_x;
            side = 0;
        } else {
            side_y += delta_y;
            map_y += step_y;
            side = 1;
        }
        type = wall(map_x, map_y);
    }
    if (!type) type = 1;
    fx perp = (fx)(side == 0 ? side_x - delta_x : side_y - delta_y);
    if (perp < FX(0.05)) perp = FX(0.05);
    int line = (int)(((uint32_t)(W * 65536 / 1.32)) / (uint32_t)perp);   // 0.66 plane: 66 degree view
    fx hit = side == 0 ? p->y + mul(perp, ray_y) : p->x + mul(perp, ray_x);
    int tex_x = ((hit & 0xffff) * TEX) >> 16;
    if ((side == 0 && ray_x > 0) || (side == 1 && ray_y < 0)) tex_x = TEX - 1 - tex_x;
    int dark = (perp >> 16) + side * 2;   // one step per unit of distance; y-sides darker
    s->column = textures[type - 1][tex_x];
    s->lut = shade[dark > 12 ? 12 : dark];
    s->step = (fx)(((uint32_t)TEX << 16) / (uint32_t)line);
    int top = H / 2 - line / 2;
    s->start = top < 0 ? 0 : top;
    s->end = top + line > H ? H : top + line;
    s->pos = (s->start - top) * s->step;
}

// Draws the view. Four columns are rendered together so each screen word is stored once.
static void draw(const player* p) {
    volatile uint32_t* screen = (volatile uint32_t*)DISP_PIXELS;
    for (int x = 0; x < W; x += 4) {
        slice s[4];
        for (int k = 0; k < 4; k++) cast(p, x + k, &s[k]);
        volatile uint32_t* out = screen + x / 4;
        for (int y = 0; y < H; y++) {
            uint32_t word = 0;
            for (int k = 0; k < 4; k++) {
                uint32_t c;
                if (y < s[k].start) {
                    c = ceil_row[y];
                } else if (y >= s[k].end) {
                    c = floor_row[y];
                } else {
                    c = s[k].lut[s[k].column[(s[k].pos >> 16) & (TEX - 1)]];
                    s[k].pos += s[k].step;
                }
                word |= c << (8 * k);
            }
            out[y * (W / 4)] = word;
        }
    }
}

// Moves if the target is clear of walls, each axis on its own so the player slides along them.
static void walk(player* p, fx dx, fx dy) {
    const fx r = FX(0.2);
    fx nx = p->x + dx, ny = p->y + dy;
    if (!wall((nx + (dx > 0 ? r : -r)) >> 16, p->y >> 16)) p->x = nx;
    if (!wall(p->x >> 16, (ny + (dy > 0 ? r : -r)) >> 16)) p->y = ny;
}

// Applies one key; returns whether the view changed.
static int act(player* p, char key) {
    const fx speed = FX(0.2);
    fx dx = mul(fcos(p->angle), speed), dy = mul(fsin(p->angle), speed);
    switch (key) {
        case 'w': walk(p, dx, dy); return 1;
        case 's': walk(p, -dx, -dy); return 1;
        case 'q': walk(p, dy, -dx); return 1;
        case 'e': walk(p, -dy, dx); return 1;
        case 'a': p->angle = (p->angle - 4) & 255; return 1;
        case 'd': p->angle = (p->angle + 4) & 255; return 1;
    }
    return 0;
}

static void put_number(uint32_t v) {
    char buf[12];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) uart_putc(buf[--n]);
}

static int key_ready(void) { return UART_LSR & UART_LSR_DATA; }

// Title screen: a plasma drawn once, animated by rotating the palette until a key is pressed.
static void title(void) {
    volatile uint8_t* screen = (volatile uint8_t*)DISP_PIXELS;
    for (int i = 0; i < 256; i++) DISP_PALETTE[i] = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            fx v = fsin(x * 3) + fsin(y * 5 + 40) + fsin((x + y) * 2) + fsin((x * 2 - y * 3) + 90);
            screen[y * W + x] = (uint8_t)(128 + (v >> 11));
        }
    uint32_t phase = 0, next = CLINT_MTIME;
    while (!key_ready()) {
        if ((int32_t)(CLINT_MTIME - next) < 0) continue;
        next += CLINT_HZ / 50;
        phase += 2;
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t a = i + phase;
            uint32_t r = (uint32_t)(fsin((int)a) + ONE) >> 9, g = (uint32_t)(fsin((int)a + 85) + ONE) >> 9,
                     b = (uint32_t)(fsin((int)a + 170) + ONE) >> 9;
            DISP_PALETTE[i] = ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
        }
    }
    (void)uart_getc();
}

static int open_ahead(const player* p, int angle, fx dist) {
    return !wall((p->x + mul(fcos(angle), dist)) >> 16, (p->y + mul(fsin(angle), dist)) >> 16);
}

// Repeats one key for `count` frames; stops early (returning 0) when a key is pressed.
static int autopilot(player* p, char key, int count) {
    while (count--) {
        if (key_ready()) return 0;
        act(p, key);
        draw(p);
    }
    return 1;
}

// Walks the map on its own until a key is pressed, cell by cell: straight to the first wall,
// then keeping a wall on the right hand, which leads through every doorway and never ends.
static void wander(player* p) {
    int following = 0;
    p->angle = (p->angle + 32) & 0xc0;   // face along the grid
    for (;;) {
        p->x = (p->x & ~0xffff) | 0x8000;   // cell centre, so step rounding cannot build up
        p->y = (p->y & ~0xffff) | 0x8000;
        if (following && open_ahead(p, p->angle + 64, ONE)) {
            if (!autopilot(p, 'd', 16) || !autopilot(p, 'w', 5)) return;
        } else if (open_ahead(p, p->angle, ONE)) {
            if (!autopilot(p, 'w', 5)) return;
        } else {
            following = 1;
            if (!autopilot(p, 'a', 16)) return;
        }
    }
}

// A fixed walk through the map, one key per frame, for measuring and for comparing renders.
static const char demo_path[] =
    "wwwwwwwwwwddddddddddddddddwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwaaaaaaaaaaaaaaaawwwwwwwwwww"
    "wwwwddddddddddddddddwwwwwwwwwwaaaaaaaaaaaaaaaawwwwwwwwwwwwwwwwwwwwwwwwwwwwwwaaaaaaaaaaaa"
    "aaaawwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwddddddddddddddddwwwwwwwwwwwwwwwwwwwwwww"
    "wwwwwwwwwwwwwwwwwddddddddddddddddwwwwwwwwwwwwwwwwwwwwwwwwwdddddddddddddddddddddddddddddd"
    "dd";

int main(void) {
    uart_puts("\r\nraycast: w/s walk, a/d turn, q/e sidestep, arrows too, x walks on its own until a key, b runs the benchmark walk\r\n");
    make_textures();
    DISP_WIDTH = W;
    DISP_HEIGHT = H;
    DISP_MODE = DISP_MODE_INDEXED;
    uart_puts("raycast: press a key to start\r\n");
    title();
    game_palette();

    player p = {FX(2.5), FX(2.5), 0};
    int escape = 0;   // progress through an arrow key's ESC [ X sequence
    draw(&p);
    uart_puts("raycast: ready\r\n");
    for (;;) {
        // Take every key that has arrived before drawing, so held keys never queue up frames.
        int changed = 0, demo = 0;
        do {
            char c = uart_getc();
            if (escape == 1) {
                escape = c == '[' ? 2 : 0;
            } else if (escape == 2) {
                escape = 0;
                changed |= act(&p, c == 'A' ? 'w' : c == 'B' ? 's' : c == 'C' ? 'd' : c == 'D' ? 'a' : 0);
            } else if (c == 27) {
                escape = 1;
            } else if (c == 'b') {
                demo = 1;
            } else if (c == 'x') {
                demo = 2;
            } else {
                changed |= act(&p, c);
            }
        } while (key_ready());
        if (demo == 2) {
            wander(&p);
        } else if (demo) {
            for (const char* k = demo_path; *k; k++) {
                act(&p, *k);
                draw(&p);
            }
            uart_puts("raycast: demo done, ");
            put_number(sizeof demo_path - 1);
            uart_puts(" frames\r\n");
        } else if (changed) {
            draw(&p);
        }
    }
}
