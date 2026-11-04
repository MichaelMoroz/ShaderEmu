// The GPU's skinned draws (gpu.h's VERTEX_BONES; docs/machine/gpu.md, "A skinned model") against the same mesh skinned
// here as Jedi Outcast's RB_SurfaceGhoul and RB_CalcDiffuseColor skin and light it (docs/games/jk2.md, "Ghoul2 on the
// GPU"): a window of four panels, the vertices 0, 4, 8 and 12 bytes past a texel (the misalignments a model's file has),
// lit, unlit, lit and tinted, textured. SKINTEST=cpu: made by this core (compact vertices with a colour table, as
// gl_jk2.c's fast_triangles draws rd-vanilla's); else by the GPU from the vertices as they lie. The two pictures must be
// the same pixels: captures under WARP (rvc_harness --cpu --warp, tools/snapshot_screen.py). Built with
// JK2_SKINTEST=1 (linux/jk2/build.sh), not part of the image.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <nano-X.h>
#include "gpu_shaderemu.h"

int seglInit(unsigned int nano_x_window);
void seglSwap(void);
void glMatrixMode(unsigned mode);
void glLoadIdentity(void);
void glOrthof(float l, float r, float b, float t, float n, float f);
void glTranslatef(float x, float y, float z);
void glScalef(float x, float y, float z);
void glClearColor(float r, float g, float b, float a);
void glClear(unsigned mask);
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701
#define GPU_BONES 0x40000u

#define N 7                       // a grid of N x N vertices
#define VERTS (N * N)
#define TRIS (2 * (N - 1) * (N - 1))
typedef struct { float normal[3], at[3]; uint32_t refs, low; } vertex_t;   // mdxmVertex_t's 32 bytes
static uint8_t room[4][VERTS * 32 + 64] __attribute__((aligned(16)));
static float coords[VERTS * 2] __attribute__((aligned(16)));
static int tris[TRIS * 3] __attribute__((aligned(16)));
static float bones[4 * 12] __attribute__((aligned(16)));
static uint32_t texture[8 * 8] __attribute__((aligned(16)));
static const float light[9] = {0.48f, -0.6f, 0.64f, 180.0f, 150.0f, 120.0f, 40.0f, 50.0f, 70.0f};

static uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// the mesh: a grid in -1..1 whose vertices have 1 to 4 bones of varied weights, and normals of their own
static void make(void) {
    srand(7);
    for (int b = 0; b < 4; b++) {
        float a = 0.3f * (float)(b + 1), c = cosf(a), s = sinf(a), k = 1.0f - 0.08f * (float)b;
        float* m = &bones[b * 12];
        m[0] = c * k, m[1] = -s * k, m[2] = 0.1f * (float)b, m[3] = 0.15f * (float)b - 0.2f;
        m[4] = s * k, m[5] = c * k, m[6] = -0.05f * (float)b, m[7] = 0.1f - 0.07f * (float)b;
        m[8] = 0.05f * (float)b, m[9] = 0.1f, m[10] = k, m[11] = 0.02f * (float)b;
    }
    for (int i = 0; i < VERTS; i++) {
        int x = i % N, y = i / N;
        vertex_t v;
        float nx = (float)(rand() % 200 - 100) / 150.0f, ny = (float)(rand() % 200 - 100) / 150.0f;
        float len = sqrtf(nx * nx + ny * ny + 1.0f);
        v.normal[0] = nx / len, v.normal[1] = ny / len, v.normal[2] = 1.0f / len;
        v.at[0] = -1.0f + 2.0f * (float)x / (N - 1), v.at[1] = -1.0f + 2.0f * (float)y / (N - 1), v.at[2] = 0.0f;
        int count = 1 + (x + y) % 4, left = 1023;
        uint32_t refs = (uint32_t)(count - 1) << 30, low = 0;
        for (int j = 0; j < count; j++) {
            refs |= (uint32_t)((i + j) % 4) << (5 * j);
            if (j + 1 < count) {
                int w = left / 2 + rand() % (left / 4 + 1);
                left -= w;
                low |= (uint32_t)(w & 0xff) << (8 * j);
                refs |= (uint32_t)((w >> 8) & 3) << (12 + 2 * j + 8);
            }
        }
        v.refs = refs, v.low = low;
        for (int r = 0; r < 4; r++) memcpy(&room[r][4 * r + 32 * i], &v, 32);
        coords[2 * i] = (float)x / (N - 1) * 2.0f, coords[2 * i + 1] = (float)y / (N - 1);
    }
    int t = 0;
    for (int y = 0; y + 1 < N; y++)
        for (int x = 0; x + 1 < N; x++) {
            int a = y * N + x;
            tris[t++] = a, tris[t++] = a + 1, tris[t++] = a + N;
            tris[t++] = a + 1, tris[t++] = a + N + 1, tris[t++] = a + N;
        }
    for (int i = 0; i < 64; i++) texture[i] = ((i ^ (i >> 3)) & 1) ? 0x00f0f0f0 : 0x00a0c0ff;
}

// a vertex skinned as RB_SurfaceGhoul skins it (its operations in its order), and lit as RB_CalcDiffuseColor and
// RB_CalcDiffuseEntityColor (the channels' multipliers 1, or the entity's colour over 255)
static float weight(const vertex_t* v, int j) {
    return (1.0f / 1023.0f) * (float)(((v->low >> (8 * j)) & 0xff) | ((v->refs >> (12 + 2 * j)) & 0x300));
}
static float row(const float* m, const float* p) { return m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3]; }
static void skin(const vertex_t* v, float* x, float* n) {
    int count = (int)(v->refs >> 30) + 1;
    const float* b = &bones[12 * (v->refs & 31)];
    for (int r = 0; r < 3; r++) n[r] = b[4 * r] * v->normal[0] + b[4 * r + 1] * v->normal[1] + b[4 * r + 2] * v->normal[2];
    if (count == 1) {
        for (int r = 0; r < 3; r++) x[r] = row(b + 4 * r, v->at);
    } else if (count == 2) {
        const float* b2 = &bones[12 * ((v->refs >> 5) & 31)];
        float w = weight(v, 0);
        for (int r = 0; r < 3; r++) {
            float t1 = row(b + 4 * r, v->at), t2 = row(b2 + 4 * r, v->at);
            x[r] = w * (t1 - t2) + t2;
        }
    } else {
        float w = weight(v, 0), total = w;
        for (int r = 0; r < 3; r++) x[r] = w * row(b + 4 * r, v->at);
        int k = 1;
        for (; k < count - 1; k++) {
            const float* bk = &bones[12 * ((v->refs >> (5 * k)) & 31)];
            w = weight(v, k);
            total += w;
            for (int r = 0; r < 3; r++) x[r] += w * row(bk + 4 * r, v->at);
        }
        const float* bl = &bones[12 * ((v->refs >> (5 * k)) & 31)];
        w = 1.0f - total;
        for (int r = 0; r < 3; r++) x[r] += w * row(bl + 4 * r, v->at);
    }
}
static uint32_t lit(const float* n, const float* m, uint32_t alpha) {
    float in = n[0] * light[0] + n[1] * light[1] + n[2] * light[2];
    uint32_t out = 0;
    for (int c = 0; c < 3; c++) {
        float j = in > 0 ? light[6 + c] + in * light[3 + c] : light[6 + c];
        if (j > 255) j = 255;
        out |= (uint32_t)(int)(j * m[c]) << (8 * c);
    }
    return out | alpha << 24;
}
// 0xTTRRGGBB of red, green, blue, alpha bytes
static uint32_t word_of(uint32_t c) { return (255u - (c >> 24)) << 24 | (c & 0xff) << 16 | (c & 0xff00) | (c >> 16 & 0xff); }

static void place(int panel) {
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(80.0f + 160.0f * (float)panel, 240.0f, 0.0f);
    glScalef(55.0f, 55.0f, 0.2f);
}

static void frame(int cpu) {
    glClearColor(0.15f, 0.15f, 0.2f, 1);
    glClear(0x4100);
    for (int p = 0; p < 4; p++) {
        const vertex_t* verts = (const vertex_t*)&room[p][4 * p];
        const int lit_on = p != 1;
        const uint32_t rgba = p == 3 ? 0xff80e0ffu : 0xffffffffu;   // (the last one tinted, as lightingDiffuseEntity)
        const float m[3] = {(float)(rgba & 0xff) / 255.0f, (float)(rgba >> 8 & 0xff) / 255.0f, (float)(rgba >> 16 & 0xff) / 255.0f};
        gpu_state s;
        memset(&s, 0, sizeof s);
        s.fragment = GPU_WORDS | GPU_PASS(0), s.texture = (unsigned)(uintptr_t)texture, s.width = 8, s.height = 8;
        place(p);
        if (cpu) {
        // this core's: a compact vertex an index, its colour a table's
        uint32_t* table = gpuFrameMemory(256 * 4);
        s.vertex = GPU_TAGGED | GPU_TABLE, s.colour = gpuAddress(table);
        uint32_t* to = gpuDrawRoom(&s, TRIS * 3, TRIS * 3);
        if (!table || !to) return;
        for (int i = 0; i < TRIS * 3; i++, to += 4) {
            const vertex_t* v = &verts[tris[i]];
            float x[3], n[3];
            skin(v, x, n);
            table[i] = word_of(lit_on ? lit(n, m, 0xff) : rgba);
            to[0] = bits(x[0]), to[1] = bits(x[1]), to[2] = bits(x[2]);
            to[3] = ((uint32_t)(coords[2 * tris[i]] * 1024.0f) & 0xfff) | ((uint32_t)(coords[2 * tris[i] + 1] * 1024.0f) & 0xfff) << 12 |
                    (uint32_t)i << 24;
        }
        continue;
        }
        // the GPU's, as gl_jk2.c's jk2glSkinned makes it
        float* b = gpuFrameMemory(4 * 48 + 48);
        if (!b) return;
        memcpy(b, bones, 4 * 48);
        float* l = b + 48;
        for (int k = 0; k < 3; k++) l[4 * k] = light[3 * k], l[4 * k + 1] = light[3 * k + 1], l[4 * k + 2] = light[3 * k + 2], l[4 * k + 3] = m[k];
        s.vertex = GPU_BONES, s.colour = word_of(lit_on ? 0xff000000u | (rgba & 0x00ffffffu) : rgba);
        s.more[0] = (unsigned)(uintptr_t)verts, s.more[1] = gpuAddress(b), s.more[2] = (unsigned)(uintptr_t)coords;
        s.more[3] = lit_on ? gpuAddress(l) : 0;
        uint32_t* c = gpuDrawAt(&s, (unsigned)(uintptr_t)tris, TRIS * 3);
        if (!c) return;
        c[4] &= ~0x400u;
    }
}

int main(void) {
    make();
    if (GrOpen() < 0) { printf("skintest: no window system\n"); return 1; }
    GR_WINDOW_ID w = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, (char*)"skintest", GR_ROOT_WINDOW_ID, 20, 20, 640, 480, 0);
    GrMapWindow(w);
    if (seglInit(w) < 0) { printf("skintest: no GPU\n"); return 1; }
    for (int k = 0; k < 8; k++) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrthof(0, 640, 480, 0, -1, 1);
        frame(getenv("SKINTEST") && !strcmp(getenv("SKINTEST"), "cpu"));
        seglSwap();
    }
    printf("skintest: drawn\n");
    fflush(stdout);
    sleep(60);
    return 0;
}
