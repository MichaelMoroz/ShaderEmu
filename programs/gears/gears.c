// The classic three gears, drawn by the GPU device. The CPU builds the meshes once, and per
// frame only computes three matrices and a short command list; transforming, lighting,
// rasterising and texturing all happen on the GPU.
// Any key pauses and resumes.

#include "../common/gpu.h"
#include "sintab.h"

#define W 1280
#define H 720

typedef int32_t fx;   // 16.16
#define FX(x) ((fx)((x) * 65536.0))
#define ONE 65536
#define TURN 1024     // angle units per revolution

static inline fx mul(fx a, fx b) { return (fx)(((int64_t)a * b) >> 16); }
static inline fx fsin(int a) { return sintab[a & (TURN - 1)]; }
static inline fx fcos(int a) { return sintab[(a + TURN / 4) & (TURN - 1)]; }

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

// ---- meshes ----

#define MAX_VERTICES 1920
static gpu_vertex mesh[MAX_VERTICES];
static uint32_t mesh_count;

typedef struct { fx x, y; } point;

static void vertex(point p, fx z, fx nx, fx ny, fx nz) {
    gpu_vertex* v = &mesh[mesh_count++];
    v->position[0] = p.x; v->position[1] = p.y; v->position[2] = z; v->position[3] = ONE;
    v->normal[0] = nx; v->normal[1] = ny; v->normal[2] = nz;
    // texture coordinates: flat faces map x and y, the rim maps its run and the gear's width
    v->uv[0] = nz ? p.x >> 1 : (p.x + p.y) >> 1;
    v->uv[1] = nz ? p.y >> 1 : z;
}

// A flat face on z = const, as a fan around its first corner.
static void face(const point* p, int corners, fx z, fx nz) {
    for (int i = 1; i + 1 < corners; i++) {
        vertex(p[0], z, 0, 0, nz);
        vertex(p[i], z, 0, 0, nz);
        vertex(p[i + 1], z, 0, 0, nz);
    }
}

// The strip of rim between two outline points, from z = +half to -half. `side` is 1 for the
// outside of the gear and -1 for the bore.
static void rim(point p, point q, fx half, int side) {
    fx dx = q.x - p.x, dy = q.y - p.y;
    fx len = (fx)isqrt((uint32_t)(mul(dx, dx) + mul(dy, dy)) << 8) << 4;
    fx inv = (fx)(0xffffffffu / (uint32_t)len);
    fx nx = side * mul(dy, inv), ny = -side * mul(dx, inv);
    vertex(p, half, nx, ny, 0); vertex(q, half, nx, ny, 0); vertex(q, -half, nx, ny, 0);
    vertex(p, half, nx, ny, 0); vertex(q, -half, nx, ny, 0); vertex(p, -half, nx, ny, 0);
}

static point polar(fx r, int a) {
    point p = {mul(r, fcos(a)), mul(r, fsin(a))};
    return p;
}

// After glxgears: a disc with a bore and `teeth` trapezoidal teeth. Returns its first vertex.
static uint32_t gear(fx inner, fx outer, fx width, int teeth, fx tooth_depth) {
    uint32_t first = mesh_count;
    fx r0 = inner, r1 = outer - tooth_depth / 2, r2 = outer + tooth_depth / 2, half = width / 2;
    int da = TURN / teeth / 4;
    for (int i = 0; i < teeth; i++) {
        int a = i * TURN / teeth;
        point bore0 = polar(r0, a), bore1 = polar(r0, a + 4 * da);
        point root0 = polar(r1, a), tip0 = polar(r2, a + da), tip1 = polar(r2, a + 2 * da);
        point root1 = polar(r1, a + 3 * da), root2 = polar(r1, a + 4 * da);
        point disc[5] = {bore0, root0, root1, root2, bore1};
        point tooth[4] = {root0, tip0, tip1, root1};
        face(disc, 5, half, ONE);
        face(tooth, 4, half, ONE);
        face(disc, 5, -half, -ONE);
        face(tooth, 4, -half, -ONE);
        rim(root0, tip0, half, 1);
        rim(tip0, tip1, half, 1);
        rim(tip1, root1, half, 1);
        rim(root1, root2, half, 1);
        rim(bore0, bore1, half, -1);
    }
    return first;
}

// ---- matrices (rows of 16.16) ----

typedef fx mat[4][4];

static void identity(mat m) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) m[i][j] = i == j ? ONE : 0;
}

static void multiply(mat out, mat a, mat b) {
    mat r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i][j] = mul(a[i][0], b[0][j]) + mul(a[i][1], b[1][j]) + mul(a[i][2], b[2][j]) + mul(a[i][3], b[3][j]);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i][j] = r[i][j];
}

// Rotation by `angle` in the plane of axes i and j.
static void rotation(mat m, int i, int j, int angle) {
    identity(m);
    m[i][i] = fcos(angle); m[i][j] = -fsin(angle);
    m[j][i] = fsin(angle); m[j][j] = fcos(angle);
}

static void translation(mat m, fx x, fx y, fx z) {
    identity(m);
    m[0][3] = x; m[1][3] = y; m[2][3] = z;
}

// glFrustum(-w/h, w/h, -1, 1, 5, 60)
static void projection(mat m) {
    const double n = 5, f = 60, right = (double)W / H;
    identity(m);
    m[0][0] = FX(n / right);
    m[1][1] = FX(n);
    m[2][2] = FX(-(f + n) / (f - n)); m[2][3] = FX(-2 * f * n / (f - n));
    m[3][2] = -ONE; m[3][3] = 0;
}

// ---- GPU state ----

// What GPU_VERTEX_LIT reads for one gear: 0-3 clip matrix, 4-6 view rotation for normals,
// 7 light direction, 8 diffuse colour, 9 ambient colour.
typedef int32_t uniforms[10][4];
static GPU_ALIGNED uniforms gear_uniforms[3];

#define TEX 64
static uint32_t metal[TEX * TEX];     // brushed plate: light grey with a darker grid
#define BADGE 64
#define KEY 0x00ff00ffu
static uint32_t badge[BADGE * BADGE]; // a small ringed emblem, magenta = transparent

static uint32_t rng = 0x1234abcd;
static uint32_t noise(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static void make_images(void) {
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            uint32_t v = 205 + (noise() & 31) + (((x >> 4) + (y >> 4)) & 1) * 18;
            if ((x & 15) == 0 || (y & 15) == 0) v = 150;
            metal[y * TEX + x] = v << 16 | v << 8 | v;
        }
    for (int y = 0; y < BADGE; y++)
        for (int x = 0; x < BADGE; x++) {
            int dx = 2 * x - BADGE + 1, dy = 2 * y - BADGE + 1, d = dx * dx + dy * dy;
            badge[y * BADGE + x] = d > 63 * 63 ? KEY : d > 47 * 47 ? 0x00f0c040 : d > 25 * 25 ? 0x00303848 : 0x00f0c040;
        }
}

static void set_gear(int g, mat view, mat proj, fx x, fx y, int angle, uint32_t colour) {
    mat model, spin, mv, mvp;
    translation(model, x, y, 0);
    rotation(spin, 0, 1, angle);
    multiply(model, model, spin);
    multiply(mv, view, model);
    multiply(mvp, proj, mv);
    int32_t (*u)[4] = gear_uniforms[g];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) u[i][j] = mvp[i][j];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) u[4 + i][j] = mv[i][j];
    u[7][0] = FX(0.408); u[7][1] = FX(0.408); u[7][2] = FX(0.816);   // light from (5, 5, 10)
    for (int j = 0; j < 3; j++) {
        fx c = (fx)((colour >> (16 - 8 * j)) & 0xff) * 257;          // 0..255 to 0..1
        u[8][j] = mul(c, FX(0.85));
        u[9][j] = mul(c, FX(0.25));
    }
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

int main(void) {
    uart_puts("\r\ngears: building meshes\r\n");
    DISP_WIDTH = W;
    DISP_HEIGHT = H;
    DISP_MODE = DISP_MODE_GPU;
    make_images();
    uint32_t first[3], count[3];
    first[0] = gear(FX(1.0), FX(4.0), FX(1.0), 16, FX(0.7));
    first[1] = gear(FX(0.5), FX(2.0), FX(2.0), 8, FX(0.7));
    first[2] = gear(FX(1.3), FX(2.0), FX(0.5), 8, FX(0.7));
    count[0] = first[1] - first[0];
    count[1] = first[2] - first[1];
    count[2] = mesh_count - first[2];

    // the view of glxgears, a little closer: back 32 units, tilted 20 degrees, turned 30
    mat proj, view, tilt, turn;
    projection(proj);
    translation(view, 0, 0, FX(-32));
    rotation(tilt, 1, 2, 57);
    rotation(turn, 2, 0, 85);
    multiply(view, view, tilt);
    multiply(view, view, turn);

    uart_puts("gears: running, ");
    put_number(mesh_count / 3);
    uart_puts(" triangles; any key pauses\r\n");
    uint32_t frames = 0, mark = CLINT_MTIME;
    for (;;) {
        int angle = (int)(CLINT_MTIME / 25);   // about 70 degrees a second
        set_gear(0, view, proj, FX(-3.0), FX(-2.0), angle, 0x00cc1a00);
        set_gear(1, view, proj, FX(3.1), FX(-2.0), -2 * angle - 32, 0x0000cc33);
        set_gear(2, view, proj, FX(-3.1), FX(4.2), -2 * angle + 104, 0x003333ff);

        gpu_begin();
        gpu_clear(0x00101828);
        for (int g = 0; g < 3; g++)
            gpu_draw(&mesh[first[g]], count[g], GPU_VERTEX_LIT, GPU_FRAGMENT_TEXTURE, gear_uniforms[g], metal, TEX, TEX, 0);
        gpu_rect(0, H - 40, W, H, 0x00202c40);
        gpu_rect(0, H - 42, W, H - 40, 0x00607090);
        gpu_image(W - BADGE - 16, 16, W - 16, 16 + BADGE, badge, BADGE, BADGE, 1, KEY);
        gpu_submit();

        if ((++frames & 255) == 0) {
            uint32_t now = CLINT_MTIME;
            uart_puts("gears: frame ");
            put_number(frames);
            uart_puts(", ");
            put_number(256u * CLINT_HZ * 10 / (now - mark));
            uart_puts(" tenths of a frame per second\r\n");
            mark = now;
        }
        if (UART_LSR & UART_LSR_DATA) {
            (void)uart_getc();
            uart_puts("gears: paused at frame ");
            put_number(frames);
            uart_puts("\r\n");
            (void)uart_getc();
            uart_puts("gears: running\r\n");
            mark = CLINT_MTIME;
        }
    }
}
