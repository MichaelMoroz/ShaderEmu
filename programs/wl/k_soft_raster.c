/*
 * soft_raster: a float software renderer, as ClassiCube's SoftGPU backend draws (Graphics_SoftGPU.c, copied):
 * TransformVertex3D (position through the model-view-projection matrix), ViewportVertex3D (1/w, the viewport, u
 * and v over w) and DrawTriangle3D: the bounding box, edge functions stepped per pixel, barycentric weights, a
 * perspective-correct 1/w per pixel, the depth test against a float depth buffer, the texture's texel and
 * MultiplyColors with the vertex colour. The scene: a hilly grid of 24 x 24 quads (1,152 triangles) seen from
 * above at an angle, a 64 x 64 texture, 160 x 120 pixels. A unit: that frame drawn whole.
 */
#include <math.h>
#include <string.h>
#include "wl.h"

#define FB_W 160
#define FB_H 120
#define GRIDN 24
#define TEX 64
#define edgeFunction(ax, ay, bx, by, cx, cy) (((bx) - (ax)) * ((cy) - (ay)) - ((by) - (ay)) * ((cx) - (ax)))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

typedef struct { float x, y, z, w, u, v; uint32_t c; } Vertex;
struct VertexTextured { float x, y, z; uint32_t Col; float U, V; };
static struct VertexTextured verts[(GRIDN + 1) * (GRIDN + 1)];
static uint32_t texture[TEX * TEX];
static float mvp[4][4];   /* rows 1 to 4 as Matrix's row1..row4 */

struct frame { float depthBuffer[FB_W * FB_H]; uint32_t colorBuffer[FB_W * FB_H]; Vertex tv[(GRIDN + 1) * (GRIDN + 1)]; int vis[(GRIDN + 1) * (GRIDN + 1)]; };

static int TransformVertex3D(const struct VertexTextured *pos, Vertex *vertex)
{
    vertex->x = pos->x * mvp[0][0] + pos->y * mvp[1][0] + pos->z * mvp[2][0] + mvp[3][0];
    vertex->y = pos->x * mvp[0][1] + pos->y * mvp[1][1] + pos->z * mvp[2][1] + mvp[3][1];
    vertex->z = pos->x * mvp[0][2] + pos->y * mvp[1][2] + pos->z * mvp[2][2] + mvp[3][2];
    vertex->w = pos->x * mvp[0][3] + pos->y * mvp[1][3] + pos->z * mvp[2][3] + mvp[3][3];
    vertex->u = pos->U, vertex->v = pos->V, vertex->c = pos->Col;
    return vertex->z >= 0.0f;
}

static void ViewportVertex3D(Vertex *vertex)
{
    float invW = 1.0f / vertex->w;
    vertex->x = FB_W / 2 * (1 + vertex->x * invW);
    vertex->y = FB_H / 2 * (1 - vertex->y * invW);
    vertex->z = vertex->z * invW;
    vertex->w = invW;
    vertex->u *= invW;
    vertex->v *= invW;
}

static void DrawTriangle3D(struct frame *f, const Vertex *V0, const Vertex *V1, const Vertex *V2)
{
    int x0 = (int)V0->x, y0 = (int)V0->y, x1 = (int)V1->x, y1 = (int)V1->y, x2 = (int)V2->x, y2 = (int)V2->y;
    int minX = min(x0, min(x1, x2)), minY = min(y0, min(y1, y2)), maxX = max(x0, max(x1, x2)), maxY = max(y0, max(y1, y2));
    int area = edgeFunction(x0, y0, x1, y1, x2, y2);
    if (area <= 0) return;
    if (maxX < 0 || minX > FB_W - 1 || maxY < 0 || minY > FB_H - 1) return;
    minX = max(minX, 0), maxX = min(maxX, FB_W - 1), minY = max(minY, 0), maxY = min(maxY, FB_H - 1);
    float factor = 1.0f / area, w0 = V0->w, w1 = V1->w, w2 = V2->w;
    if (w0 <= 0 || w1 <= 0 || w2 <= 0) return;
    float z0 = V0->z, z1 = V1->z, z2 = V2->z;
    uint32_t color = V0->c;
    float u0 = V0->u * TEX, u1 = V1->u * TEX, u2 = V2->u * TEX, v0 = V0->v * TEX, v1 = V1->v * TEX, v2 = V2->v * TEX;
    int dx01 = y0 - y1, dy01 = x1 - x0, dx12 = y1 - y2, dy12 = x2 - x1, dx20 = y2 - y0, dy20 = x0 - x2;
    float bc0_start = edgeFunction(x1, y1, x2, y2, minX + 0.5f, minY + 0.5f);
    float bc1_start = edgeFunction(x2, y2, x0, y0, minX + 0.5f, minY + 0.5f);
    float bc2_start = edgeFunction(x0, y0, x1, y1, minX + 0.5f, minY + 0.5f);
    for (int y = minY; y <= maxY; y++, bc0_start += dy12, bc1_start += dy20, bc2_start += dy01) {
        float bc0 = bc0_start, bc1 = bc1_start, bc2 = bc2_start;
        for (int x = minX; x <= maxX; x++, bc0 += dx12, bc1 += dx20, bc2 += dx01) {
            float ic0 = bc0 * factor, ic1 = bc1 * factor, ic2 = bc2 * factor;
            if (ic0 < 0 || ic1 < 0 || ic2 < 0) continue;
            int db_index = y * FB_W + x;
            float w = 1 / (ic0 * w0 + ic1 * w1 + ic2 * w2);
            float z = (ic0 * z0 + ic1 * z1 + ic2 * z2) * w;
            if (z < 0 || z > f->depthBuffer[db_index]) continue;
            float u = (ic0 * u0 + ic1 * u1 + ic2 * u2) * w, v = (ic0 * v0 + ic1 * v1 + ic2 * v2) * w;
            uint32_t tColor = texture[(((int)v) & (TEX - 1)) * TEX + (((int)u) & (TEX - 1))];
            int R = (int)(((color >> 16) & 255) * ((tColor >> 16) & 255)) >> 8;   /* MultiplyColors */
            int G = (int)(((color >> 8) & 255) * ((tColor >> 8) & 255)) >> 8;
            int B = (int)((color & 255) * (tColor & 255)) >> 8;
            int A = (int)((color >> 24) * (tColor >> 24)) >> 8;
            if (A < 0x80) continue;
            f->depthBuffer[db_index] = z;
            f->colorBuffer[db_index] = 0xff000000u | (uint32_t)R << 16 | (uint32_t)G << 8 | (uint32_t)B;
        }
    }
}

static void setup(void)
{
    uint32_t seed = 0x50f7;
    for (int i = 0; i <= GRIDN; i++)
        for (int j = 0; j <= GRIDN; j++) {
            struct VertexTextured *v = &verts[i * (GRIDN + 1) + j];
            v->x = (float)j - GRIDN / 2, v->z = (float)i - GRIDN / 2;
            v->y = 1.5f * sinf(j * 0.45f) * cosf(i * 0.3f) + 0.2f * wl_randf(&seed);
            int shade = 160 + (int)(v->y * 40);
            v->Col = 0xff000000u | (uint32_t)shade << 16 | (uint32_t)shade << 8 | (uint32_t)shade;
            v->U = j * 0.5f, v->V = i * 0.5f;
        }
    for (int i = 0; i < TEX * TEX; i++) texture[i] = 0xff000000u | ((wl_rand(&seed) & 0x3f) + 120) << 16 | (((i >> 3) ^ (i >> 9)) & 1 ? 200u : 140u) << 8 | 90u;
    /* Gfx_CalcPerspectiveMatrix(fov 70 degrees, 4:3, far 100) times a view from (0, 9, 14) looking down 35 degrees */
    float c = cosf(0.6109f) / sinf(0.6109f), zNear = 0.1f, zFar = 100, pitch = 0.61f, cp = cosf(pitch), sp = sinf(pitch);
    float proj[4][4] = {{c / (4.0f / 3), 0, 0, 0}, {0, c, 0, 0}, {0, 0, zFar / (zNear - zFar), -1}, {0, 0, (zNear * zFar) / (zNear - zFar), 0}};
    float view[4][4] = {{1, 0, 0, 0}, {0, cp, sp, 0}, {0, -sp, cp, 0}, {0, 0, 0, 1}};
    float ty = -9, tz = -14;   /* the camera's place, moved into the view */
    view[3][1] = ty * cp + tz * -sp, view[3][2] = ty * sp + tz * cp;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            float s = 0;
            for (int m = 0; m < 4; m++) s += view[r][m] * proj[m][k];
            mvp[r][k] = s;
        }
}

static void *inst(void) { return wl_alloc(sizeof(struct frame)); }

static uint32_t run(void *ctx)
{
    struct frame *f = ctx;
    uint32_t sum = 0;
    for (int i = 0; i < FB_W * FB_H; i++) f->depthBuffer[i] = 100000000.0f, f->colorBuffer[i] = 0xff6080a0;   /* ClearDepthBuffer */
    for (int i = 0; i < (GRIDN + 1) * (GRIDN + 1); i++) {
        f->vis[i] = TransformVertex3D(&verts[i], &f->tv[i]);
        if (f->vis[i]) ViewportVertex3D(&f->tv[i]);
    }
    for (int i = 0; i < GRIDN; i++)
        for (int j = 0; j < GRIDN; j++) {   /* each quad as two triangles (DrawQuads) */
            int a = i * (GRIDN + 1) + j, b = a + 1, cc = a + GRIDN + 1, d = cc + 1;
            if (!(f->vis[a] && f->vis[b] && f->vis[cc] && f->vis[d])) continue;
            DrawTriangle3D(f, &f->tv[a], &f->tv[b], &f->tv[cc]);
            DrawTriangle3D(f, &f->tv[b], &f->tv[d], &f->tv[cc]);
        }
    for (int i = 0; i < FB_W * FB_H; i += 5) sum = sum * 31 + f->colorBuffer[i];
    return sum;
}

const wl_kernel wl_soft_raster = {"soft_raster", "ClassiCube SoftGPU: float transform and DrawTriangle3D with depth and texture", setup, inst, run};
