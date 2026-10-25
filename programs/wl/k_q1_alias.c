/*
 * q1_alias: Quake's software drawing of its monsters and items (WinQuake r_alias.c, d_polyse.c, copied):
 * R_AliasTransformAndProjectFinalVerts (each vertex through the 3 x 4 alias transform, 1/z, the light normal's
 * cosine against the light into a colormap row) and D_DrawSubdiv with D_PolysetRecursiveTriangle, the affine
 * rasterizer Quake uses for models far enough away (trivial_accept & 2): every triangle split until its edges
 * are a pixel, each new point z-tested in a short z-buffer and written through the skin and the colormap.
 * The model: a closed mesh of 342 vertices and 640 triangles (a lumpy sphere, a 64 x 64 skin). A unit: 4 such
 * models at 30 to 70 pixels across in a 320 x 200 view.
 */
#include <math.h>
#include <string.h>
#include "wl.h"

#define VIEW_W 320
#define VIEW_H 200
#define RINGS 18
#define SEGS 19
#define NUMVERTS (RINGS * SEGS)
#define NUMTRIS ((RINGS - 1) * (SEGS - 1) * 2 + 28)
#define MODELS 4
#define ALIAS_ONSEAM 0x0020
#define NUMVERTEXNORMALS 162

typedef struct { unsigned char v[3], lightnormalindex; } trivertx_t;
typedef struct { int onseam, s, t; } stvert_t;
typedef struct { int facesfront, vertindex[3]; } mtriangle_t;
typedef struct { int v[6], flags; float reserved; } finalvert_t;

static trivertx_t verts[NUMVERTS];
static stvert_t stverts[NUMVERTS];
static mtriangle_t tris[NUMTRIS];
static int numtris;
static unsigned char skin[64][64], colormap[64 * 256];
static float avertexnormals[NUMVERTEXNORMALS][3];

struct view {
    finalvert_t fv[NUMVERTS];
    short zbuffer[VIEW_W * VIEW_H];
    unsigned char viewbuffer[VIEW_W * VIEW_H];
    const unsigned char *pcolormap;
    float aliastransform[3][4];
};

static void D_PolysetRecursiveTriangle(struct view *w, int *lp1, int *lp2, int *lp3)
{
    int *temp, d, new[6], z;
    short *zbuf;
    d = lp2[0] - lp1[0];
    if (d < -1 || d > 1) goto split;
    d = lp2[1] - lp1[1];
    if (d < -1 || d > 1) goto split;
    d = lp3[0] - lp2[0];
    if (d < -1 || d > 1) goto split2;
    d = lp3[1] - lp2[1];
    if (d < -1 || d > 1) goto split2;
    d = lp1[0] - lp3[0];
    if (d < -1 || d > 1) goto split3;
    d = lp1[1] - lp3[1];
    if (d < -1 || d > 1) {
    split3:
        temp = lp1, lp1 = lp3, lp3 = lp2, lp2 = temp;
        goto split;
    }
    return;   // entire tri is filled
split2:
    temp = lp1, lp1 = lp2, lp2 = lp3, lp3 = temp;
split:
    new[0] = (lp1[0] + lp2[0]) >> 1;
    new[1] = (lp1[1] + lp2[1]) >> 1;
    new[2] = (lp1[2] + lp2[2]) >> 1;
    new[3] = (lp1[3] + lp2[3]) >> 1;
    new[5] = (lp1[5] + lp2[5]) >> 1;
    if (lp2[1] > lp1[1]) goto nodraw;
    if ((lp2[1] == lp1[1]) && (lp2[0] < lp1[0])) goto nodraw;
    z = new[5] >> 16;
    zbuf = w->zbuffer + new[1] * VIEW_W + new[0];
    if (z >= *zbuf) {
        *zbuf = (short)z;
        w->viewbuffer[new[1] * VIEW_W + new[0]] = w->pcolormap[skin[(new[3] >> 16) & 63][(new[2] >> 16) & 63]];
    }
nodraw:
    D_PolysetRecursiveTriangle(w, lp3, lp1, new);
    D_PolysetRecursiveTriangle(w, lp3, new, lp2);
}

static void setup(void)
{
    uint32_t seed = 0xa11a;
    for (int i = 0; i < NUMVERTEXNORMALS; i++) {   /* (anorms.h's 162 directions, spread as a spiral here) */
        float y = 1 - 2 * (i + 0.5f) / NUMVERTEXNORMALS, r = sqrtf(1 - y * y), a = i * 2.39996f;
        avertexnormals[i][0] = r * cosf(a), avertexnormals[i][1] = y, avertexnormals[i][2] = r * sinf(a);
    }
    for (int i = 0; i < RINGS; i++)
        for (int j = 0; j < SEGS; j++) {
            float th = 3.14159f * i / (RINGS - 1), ph = 6.28318f * j / (SEGS - 1), r = 100 + 25 * wl_randf(&seed);
            float x = sinf(th) * cosf(ph), y = cosf(th), zz = sinf(th) * sinf(ph);
            trivertx_t *v = &verts[i * SEGS + j];
            v->v[0] = (unsigned char)(127 + x * r), v->v[1] = (unsigned char)(127 + y * r), v->v[2] = (unsigned char)(127 + zz * r);
            int best = 0;
            float bd = -2;
            for (int n = 0; n < NUMVERTEXNORMALS; n++) {
                float d = avertexnormals[n][0] * x + avertexnormals[n][1] * y + avertexnormals[n][2] * zz;
                if (d > bd) bd = d, best = n;
            }
            v->lightnormalindex = (unsigned char)best;
            stverts[i * SEGS + j].s = (j * 32 / (SEGS - 1)) << 16, stverts[i * SEGS + j].t = (i * 63 / (RINGS - 1)) << 16;
            stverts[i * SEGS + j].onseam = (j == 0 || j == SEGS - 1) ? ALIAS_ONSEAM : 0;
        }
    for (int i = 0; i < RINGS - 1; i++)
        for (int j = 0; j < SEGS - 1; j++) {
            int a = i * SEGS + j, b = a + 1, c = a + SEGS, d = c + 1, front = j < (SEGS - 1) / 2;
            tris[numtris++] = (mtriangle_t){front, {a, c, b}};
            tris[numtris++] = (mtriangle_t){front, {b, c, d}};
        }
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) skin[y][x] = (unsigned char)(((x >> 3) ^ (y >> 3)) * 13 + (wl_rand(&seed) & 7));
    for (int l = 0; l < 64; l++)
        for (int c = 0; c < 256; c++) colormap[l * 256 + c] = (unsigned char)((c & 0xf0) | (((c & 15) * (63 - l)) >> 6));
}

static void *inst(void) { return wl_alloc(sizeof(struct view)); }

static uint32_t run(void *ctx)
{
    struct view *w = ctx;
    uint32_t sum = 0;
    memset(w->zbuffer, 0, sizeof w->zbuffer);
    for (int m = 0; m < MODELS; m++) {
        /* R_AliasSetUpTransform: a yaw, a scale to 30-70 pixels, the place on screen (x, y scaled by 1/z later) */
        float yaw = m * 0.7f, c = cosf(yaw), s = sinf(yaw), dist = 300 + m * 60, size = (0.9f - m * 0.07f) * 0.25f;
        float plight[3] = {-0.6f, -0.5f, -0.62f};
        int ambient = 20 + m * 3, shadelight = 40;
        float xcenter = 40 + (m % 4) * 75, ycenter = 50 + (m / 4) * 90;
        w->aliastransform[0][0] = c * size * dist, w->aliastransform[0][1] = 0, w->aliastransform[0][2] = -s * size * dist;
        w->aliastransform[0][3] = -127 * (c - s) * size * dist;
        w->aliastransform[1][0] = 0, w->aliastransform[1][1] = size * dist, w->aliastransform[1][2] = 0, w->aliastransform[1][3] = -127 * size * dist;
        w->aliastransform[2][0] = s * size, w->aliastransform[2][1] = 0, w->aliastransform[2][2] = c * size, w->aliastransform[2][3] = dist - 127 * (s + c) * size;
        /* R_AliasTransformAndProjectFinalVerts (into the fixed-point form D_PolysetRecursiveTriangle takes) */
        finalvert_t *fv = w->fv;
        const trivertx_t *pverts = verts;
        const stvert_t *pstverts = stverts;
        for (int i = 0; i < NUMVERTS; i++, fv++, pverts++, pstverts++) {
            float zi = 1.0f / (pverts->v[0] * w->aliastransform[2][0] + pverts->v[1] * w->aliastransform[2][1] +
                               pverts->v[2] * w->aliastransform[2][2] + w->aliastransform[2][3]);
            fv->v[5] = (int)(zi * 65536.0f * 4096.0f);
            fv->v[0] = (int)((pverts->v[0] * w->aliastransform[0][0] + pverts->v[1] * w->aliastransform[0][1] + pverts->v[2] * w->aliastransform[0][2] +
                              w->aliastransform[0][3]) * zi + xcenter);
            fv->v[1] = (int)((pverts->v[0] * w->aliastransform[1][0] + pverts->v[1] * w->aliastransform[1][1] + pverts->v[2] * w->aliastransform[1][2] +
                              w->aliastransform[1][3]) * zi + ycenter);
            fv->v[2] = pstverts->s, fv->v[3] = pstverts->t, fv->flags = pstverts->onseam;
            const float *plightnormal = avertexnormals[pverts->lightnormalindex];
            float lightcos = plightnormal[0] * plight[0] + plightnormal[1] * plight[1] + plightnormal[2] * plight[2];
            int temp = ambient;
            if (lightcos < 0) {
                temp += (int)(shadelight * lightcos);
                if (temp < 0) temp = 0;
            }
            fv->v[4] = (63 - (temp > 63 ? 63 : temp)) << 8;
            if (fv->v[0] < 0) fv->v[0] = 0;
            if (fv->v[0] >= VIEW_W) fv->v[0] = VIEW_W - 1;
            if (fv->v[1] < 0) fv->v[1] = 0;
            if (fv->v[1] >= VIEW_H) fv->v[1] = VIEW_H - 1;
        }
        /* D_DrawSubdiv */
        for (int i = 0; i < numtris; i++) {
            finalvert_t *index0 = w->fv + tris[i].vertindex[0], *index1 = w->fv + tris[i].vertindex[1], *index2 = w->fv + tris[i].vertindex[2];
            if (((index0->v[1] - index1->v[1]) * (index0->v[0] - index2->v[0]) - (index0->v[0] - index1->v[0]) * (index0->v[1] - index2->v[1])) >= 0)
                continue;
            w->pcolormap = &colormap[index0->v[4] & 0xFF00];
            if (tris[i].facesfront) D_PolysetRecursiveTriangle(w, index0->v, index1->v, index2->v);
            else {
                int s0 = index0->v[2], s1 = index1->v[2], s2 = index2->v[2];
                if (index0->flags & ALIAS_ONSEAM) index0->v[2] += 32 << 16;
                if (index1->flags & ALIAS_ONSEAM) index1->v[2] += 32 << 16;
                if (index2->flags & ALIAS_ONSEAM) index2->v[2] += 32 << 16;
                D_PolysetRecursiveTriangle(w, index0->v, index1->v, index2->v);
                index0->v[2] = s0, index1->v[2] = s1, index2->v[2] = s2;
            }
        }
    }
    for (int i = 0; i < VIEW_W * VIEW_H; i += 7) sum = sum * 31 + w->viewbuffer[i] + (uint32_t)w->zbuffer[i];
    return sum;
}

const wl_kernel wl_q1_alias = {"q1_alias", "Quake: alias models transformed, lit and drawn by D_PolysetRecursiveTriangle", setup, inst, run};
