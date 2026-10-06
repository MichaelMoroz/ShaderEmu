// A small raytracer for the bare-metal machine: three spheres on a checkered floor, one light,
// shadows and reflections. Integer only (16.16 fixed point), since the CPU has no FPU.
// It asks for its settings on the console, then draws straight to the display, one word per pixel.

#include "../common/platform.h"

typedef int32_t fx;
#define FX(x) ((fx)((x) * 65536.0))
#define ONE FX(1)

typedef struct { fx x, y, z; } vec;

static inline fx mul(fx a, fx b) { return (fx)(((int64_t)a * b) >> 16); }
// 1/x for x > 0, as one hardware divide.
static inline fx recip(fx x) { return (fx)(0xffffffffu / (uint32_t)x); }

static uint32_t isqrt(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static fx fsqrt(fx x) {
    if (x <= 0) return 0;
    return x < (1 << 24) ? (fx)(isqrt((uint32_t)x << 8) << 4) : (fx)(isqrt((uint32_t)x) << 8);
}

static inline vec v3(fx x, fx y, fx z) { vec r = {x, y, z}; return r; }
static inline vec add(vec a, vec b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec sub(vec a, vec b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec scale(vec a, fx s) { return v3(mul(a.x, s), mul(a.y, s), mul(a.z, s)); }
static inline fx dot(vec a, vec b) { return mul(a.x, b.x) + mul(a.y, b.y) + mul(a.z, b.z); }
static inline vec mix(vec a, vec b, fx t) { return add(a, scale(sub(b, a), t)); }
static vec normalize(vec a) { return scale(a, recip(fsqrt(dot(a, a)))); }

typedef struct {
    vec center;
    fx radius, inv_radius;
    vec color;
    fx reflect;
} sphere;

#define SPHERES 3
static const sphere spheres[SPHERES] = {
    {{FX(0.0), FX(1.0), FX(3.2)}, FX(1.0), FX(1.0), {FX(0.90), FX(0.25), FX(0.20)}, FX(0.25)},
    {{FX(-2.1), FX(0.7), FX(4.2)}, FX(0.7), FX(1.0 / 0.7), {FX(0.25), FX(0.75), FX(0.30)}, FX(0.15)},
    {{FX(1.7), FX(0.6), FX(2.3)}, FX(0.6), FX(1.0 / 0.6), {FX(0.85), FX(0.85), FX(0.95)}, FX(0.65)},
};

#define FAR FX(40)
#define EPS FX(0.01)

// Nearest sphere along the ray, or -1. `dir` is unit length.
static int hit_sphere(vec orig, vec dir, fx* t_out) {
    int best = -1;
    fx best_t = FAR;
    for (int i = 0; i < SPHERES; i++) {
        vec oc = sub(orig, spheres[i].center);
        fx b = dot(oc, dir);
        fx c = dot(oc, oc) - mul(spheres[i].radius, spheres[i].radius);
        fx disc = mul(b, b) - c;
        if (disc <= 0) continue;
        fx t = -b - fsqrt(disc);
        if (t > EPS && t < best_t) {
            best_t = t;
            best = i;
        }
    }
    *t_out = best_t;
    return best;
}

static vec sky(vec dir) {
    fx t = (dir.y + ONE) >> 1;
    return mix(v3(FX(0.85), FX(0.90), FX(1.0)), v3(FX(0.25), FX(0.45), FX(0.90)), t);
}

static vec trace(vec orig, vec dir, vec light, int bounces, int shadows) {
    vec result = v3(0, 0, 0);
    fx weight = ONE;
    for (int bounce = 0; bounce < bounces; bounce++) {
        fx t;
        int s = hit_sphere(orig, dir, &t);
        vec normal, base;
        fx reflect;
        if (dir.y < -EPS) {                      // the floor, y = 0
            fx tp = mul(orig.y, recip(-dir.y));
            if (tp < t) {
                t = tp;
                s = SPHERES;
            }
        }
        if (s < 0) {
            result = add(result, scale(sky(dir), weight));
            break;
        }
        vec p = add(orig, scale(dir, t));
        if (s == SPHERES) {
            normal = v3(0, ONE, 0);
            int check = ((p.x >> 16) + (p.z >> 16)) & 1;
            base = check ? v3(FX(0.85), FX(0.85), FX(0.80)) : v3(FX(0.20), FX(0.22), FX(0.28));
            base = mix(base, sky(dir), mul(t, FX(1.0 / 40)));   // fade into the horizon
            reflect = FX(0.20);
        } else {
            normal = scale(sub(p, spheres[s].center), spheres[s].inv_radius);
            base = spheres[s].color;
            reflect = spheres[s].reflect;
        }
        orig = add(p, scale(normal, EPS * 2));
        fx diffuse = dot(normal, light);
        fx shadow_t;
        if (diffuse < 0 || (shadows && hit_sphere(orig, light, &shadow_t) >= 0)) diffuse = 0;
        fx lit = FX(0.25) + mul(diffuse, FX(0.75));
        result = add(result, scale(base, mul(mul(weight, ONE - reflect), lit)));
        weight = mul(weight, reflect);
        if (weight < FX(0.02)) break;
        dir = sub(dir, scale(normal, 2 * dot(dir, normal)));
    }
    return result;
}

static uint32_t to_byte(fx c) {
    if (c < 0) c = 0;
    if (c > ONE - 1) c = ONE - 1;
    return (uint32_t)(c >> 8);
}

static void put_number(int v) {
    char buf[12];
    int n = 0;
    do {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) uart_putc(buf[--n]);
}

// Prompts for a number; Enter alone keeps `def`. The result is clamped to [lo, hi].
static int ask(const char* what, int def, int lo, int hi) {
    uart_puts("  ");
    uart_puts(what);
    uart_puts(" (");
    put_number(lo);
    uart_putc('-');
    put_number(hi);
    uart_puts(") [");
    put_number(def);
    uart_puts("]: ");
    int v = 0, digits = 0;
    for (;;) {
        char c = uart_getc();
        if (c == '\r' || c == '\n') break;
        if ((c == 8 || c == 127) && digits) {
            v /= 10;
            digits--;
            uart_puts("\b \b");
        } else if (c >= '0' && c <= '9' && digits < 5) {
            v = v * 10 + (c - '0');
            digits++;
            uart_putc(c);
        }
    }
    uart_puts("\r\n");
    if (!digits) return def;
    return v < lo ? lo : v > hi ? hi : v;
}

static void render(int width, int height, int bounces, int aa, int shadows) {
    volatile uint32_t* pixels = (volatile uint32_t*)DISP_PIXELS;
    DISP_MODE = DISP_MODE_OFF;
    for (int i = 0; i < width * height; i++) pixels[i] = 0;
    DISP_WIDTH = (uint32_t)width;
    DISP_HEIGHT = (uint32_t)height;
    DISP_MODE = DISP_MODE_RGB32;

    const vec eye = v3(FX(0.0), FX(1.3), FX(-2.5));
    const vec light = normalize(v3(FX(-0.5), FX(1.0), FX(-0.6)));
    // Sub-pixel grid: aa x aa rays per pixel, in units of 1/(aa*height) of the image height.
    int sh = height * aa;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            vec sum = v3(0, 0, 0);
            for (int sy = 0; sy < aa; sy++) {
                fx v = ((sh - 1 - 2 * (y * aa + sy)) << 16) / sh;
                for (int sx = 0; sx < aa; sx++) {
                    fx u = ((2 * (x * aa + sx) - width * aa + 1) << 16) / sh;
                    vec dir = normalize(v3(mul(u, FX(0.6)), mul(v, FX(0.6)) - FX(0.12), ONE));
                    sum = add(sum, trace(eye, dir, light, bounces, shadows));
                }
            }
            if (aa > 1) sum = scale(sum, ONE / (aa * aa));
            pixels[y * width + x] = (to_byte(sum.x) << 16) | (to_byte(sum.y) << 8) | to_byte(sum.z);
        }
        if ((y & 15) == 15) uart_putc('.');
    }
}

int main(void) {
    int width = 320, height = 200, bounces = 3, aa = 1, shadows = 1;
    for (;;) {
        uart_puts("\r\nraytrace settings (Enter keeps the value in brackets)\r\n");
        width = ask("width", width, 16, 640);
        height = ask("height", height, 16, 480);
        bounces = ask("bounces", bounces, 1, 6);
        aa = ask("rays per pixel side", aa, 1, 3);
        shadows = ask("shadows, 0 or 1", shadows, 0, 1);
        uart_puts("rendering ");
        put_number(width);
        uart_putc('x');
        put_number(height);
        uart_puts(" ");
        render(width, height, bounces, aa, shadows);
        uart_puts("\r\nraytrace: done\r\n");
    }
}
