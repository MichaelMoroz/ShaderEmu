/*
 * q3_tess: Quake III's client filling its tess arrays for models every frame (ioq3 renderergl1, copied):
 * LerpMeshVertexes_scalar between two frames of an MD3 surface (short positions, lat/long normals through
 * tr.sinTable) and VectorArrayNormalize with Q_rsqrt, RB_CalcDeformVertexes (deformVertexes wave), then the
 * stages' RB_CalcDiffuseColor_scalar, RB_CalcEnvironmentTexCoords (VectorNormalizeFast) and RB_CalcFogTexCoords
 * (the fog without a surface). A unit: 6 surfaces of 1,000 vertices, two frames apart, as a frame's players and
 * weapons are.
 */
#include <math.h>
#include <string.h>
#include "wl.h"

#define FUNCTABLE_SIZE 1024
#define FUNCTABLE_MASK (FUNCTABLE_SIZE - 1)
#define MD3_XYZ_SCALE (1.0f / 64)
#define VERTS 1000
#define FRAMES 4
#define SURFACES 6
#define DotProduct(a, b) ((a)[0] * (b)[0] + (a)[1] * (b)[1] + (a)[2] * (b)[2])

typedef float vec4_t[4];
static float sinTable[FUNCTABLE_SIZE];
static short xyzNormals[FRAMES][VERTS][4];

struct tess { vec4_t xyz[VERTS], normal[VERTS]; unsigned char colors[VERTS][4]; float st[VERTS][2], fogst[VERTS][2]; };

static float Q_rsqrt(float number)
{
    union { float f; int i; } t;
    float x2 = number * 0.5F, y;
    const float threehalfs = 1.5F;
    t.f = number;
    t.i = 0x5f3759df - (t.i >> 1);
    y = t.f;
    y = y * (threehalfs - (x2 * y * y));
    return y;
}

static void VectorArrayNormalize(vec4_t *normals, unsigned int count)
{
    while (count--) {
        float *v = *normals;
        float len = Q_rsqrt(DotProduct(v, v));
        v[0] *= len, v[1] *= len, v[2] *= len;
        normals++;
    }
}

static void LerpMeshVertexes_scalar(struct tess *tess, int frame, int oldframe, float backlerp)
{
    short *oldXyz = xyzNormals[oldframe][0], *newXyz = xyzNormals[frame][0], *oldNormals = oldXyz + 3, *newNormals = newXyz + 3;
    float *outXyz = tess->xyz[0], *outNormal = tess->normal[0];
    float newXyzScale = MD3_XYZ_SCALE * (1.0f - backlerp), newNormalScale = 1.0f - backlerp;
    float oldXyzScale = MD3_XYZ_SCALE * backlerp, oldNormalScale = backlerp;
    unsigned lat, lng;
    for (int vertNum = 0; vertNum < VERTS; vertNum++, oldXyz += 4, newXyz += 4, oldNormals += 4, newNormals += 4, outXyz += 4, outNormal += 4) {
        float uncompressedOldNormal[3], uncompressedNewNormal[3];
        outXyz[0] = oldXyz[0] * oldXyzScale + newXyz[0] * newXyzScale;
        outXyz[1] = oldXyz[1] * oldXyzScale + newXyz[1] * newXyzScale;
        outXyz[2] = oldXyz[2] * oldXyzScale + newXyz[2] * newXyzScale;
        lat = (newNormals[0] >> 8) & 0xff, lng = (newNormals[0] & 0xff);
        lat *= 4, lng *= 4;
        uncompressedNewNormal[0] = sinTable[(lat + (FUNCTABLE_SIZE / 4)) & FUNCTABLE_MASK] * sinTable[lng];
        uncompressedNewNormal[1] = sinTable[lat] * sinTable[lng];
        uncompressedNewNormal[2] = sinTable[(lng + (FUNCTABLE_SIZE / 4)) & FUNCTABLE_MASK];
        lat = (oldNormals[0] >> 8) & 0xff, lng = (oldNormals[0] & 0xff);
        lat *= 4, lng *= 4;
        uncompressedOldNormal[0] = sinTable[(lat + (FUNCTABLE_SIZE / 4)) & FUNCTABLE_MASK] * sinTable[lng];
        uncompressedOldNormal[1] = sinTable[lat] * sinTable[lng];
        uncompressedOldNormal[2] = sinTable[(lng + (FUNCTABLE_SIZE / 4)) & FUNCTABLE_MASK];
        outNormal[0] = uncompressedOldNormal[0] * oldNormalScale + uncompressedNewNormal[0] * newNormalScale;
        outNormal[1] = uncompressedOldNormal[1] * oldNormalScale + uncompressedNewNormal[1] * newNormalScale;
        outNormal[2] = uncompressedOldNormal[2] * oldNormalScale + uncompressedNewNormal[2] * newNormalScale;
    }
    VectorArrayNormalize(tess->normal, VERTS);
}

/* RB_CalcDeformVertexes with a sine wave: each vertex pushed along its normal by the wave at its place */
static void RB_CalcDeformVertexes(struct tess *tess, float time)
{
    float *xyz = tess->xyz[0], *normal = tess->normal[0];
    const float base = 0, amplitude = 2, phase = 0.1f, frequency = 1.2f, deformSpread = 1.0f / 64;
    for (int i = 0; i < VERTS; i++, xyz += 4, normal += 4) {
        float off = (xyz[0] + xyz[1] + xyz[2]) * deformSpread;
        float scale = base + sinTable[((int)(((phase + off) + time * frequency) * FUNCTABLE_SIZE)) & FUNCTABLE_MASK] * amplitude;
        xyz[0] += normal[0] * scale, xyz[1] += normal[1] * scale, xyz[2] += normal[2] * scale;
    }
}

static void RB_CalcDiffuseColor_scalar(struct tess *tess, const float *ambientLight, const float *directedLight, const float *lightDir, int ambientLightInt)
{
    float *normal = tess->normal[0];
    unsigned char *colors = tess->colors[0];
    for (int i = 0; i < VERTS; i++, normal += 4) {
        float incoming = DotProduct(normal, lightDir);
        int j;
        if (incoming <= 0) {
            memcpy(&colors[i * 4], &ambientLightInt, 4);
            continue;
        }
        j = (int)(ambientLight[0] + incoming * directedLight[0]);
        if (j > 255) j = 255;
        colors[i * 4 + 0] = (unsigned char)j;
        j = (int)(ambientLight[1] + incoming * directedLight[1]);
        if (j > 255) j = 255;
        colors[i * 4 + 1] = (unsigned char)j;
        j = (int)(ambientLight[2] + incoming * directedLight[2]);
        if (j > 255) j = 255;
        colors[i * 4 + 2] = (unsigned char)j;
        colors[i * 4 + 3] = 255;
    }
}

static void RB_CalcEnvironmentTexCoords(struct tess *tess, const float *viewOrigin)
{
    float *v = tess->xyz[0], *normal = tess->normal[0], *st = tess->st[0], viewer[3], reflected[3], d;
    for (int i = 0; i < VERTS; i++, v += 4, normal += 4, st += 2) {
        viewer[0] = viewOrigin[0] - v[0], viewer[1] = viewOrigin[1] - v[1], viewer[2] = viewOrigin[2] - v[2];
        float ilength = Q_rsqrt(DotProduct(viewer, viewer));   /* VectorNormalizeFast */
        viewer[0] *= ilength, viewer[1] *= ilength, viewer[2] *= ilength;
        d = DotProduct(normal, viewer);
        reflected[0] = normal[0] * 2 * d - viewer[0];
        reflected[1] = normal[1] * 2 * d - viewer[1];
        reflected[2] = normal[2] * 2 * d - viewer[2];
        st[0] = 0.5f + reflected[1] * 0.5f;
        st[1] = 0.5f - reflected[2] * 0.5f;
    }
}

static void RB_CalcFogTexCoords(struct tess *tess, const float *fogDistanceVector)
{
    float *v = tess->xyz[0], *st = tess->fogst[0];
    for (int i = 0; i < VERTS; i++, v += 4) {
        float s = DotProduct(v, fogDistanceVector) + fogDistanceVector[3] + 1.0f / 512, t = 31.0f / 32;
        st[0] = s, st[1] = t;
        st += 2;
    }
}

static void setup(void)
{
    uint32_t seed = 0x7e55;
    for (int i = 0; i < FUNCTABLE_SIZE; i++) sinTable[i] = sinf(i * (6.28318531f / FUNCTABLE_SIZE));
    for (int f = 0; f < FRAMES; f++)
        for (int v = 0; v < VERTS; v++) {
            float a = v * 0.37f + f * 0.05f, h = (v % 50) * 2.0f - 50;
            xyzNormals[f][v][0] = (short)(cosf(a) * (900 + (v % 7) * 30) + f * 20);
            xyzNormals[f][v][1] = (short)(sinf(a) * (900 + (v % 5) * 30));
            xyzNormals[f][v][2] = (short)(h * 64);
            xyzNormals[f][v][3] = (short)(((wl_rand(&seed) & 0xff) << 8) | (wl_rand(&seed) & 0xff));
        }
}

static void *inst(void) { return wl_alloc(sizeof(struct tess)); }

static uint32_t run(void *ctx)
{
    struct tess *tess = ctx;
    uint32_t sum = 0;
    for (int s = 0; s < SURFACES; s++) {
        float ambient[3] = {40 + s * 5.f, 38, 36}, directed[3] = {180, 170, 150}, dir[3] = {0.57f, 0.57f, 0.59f};
        float view[3] = {200 + s * 30.f, -150, 60}, fog[4] = {0.001f, 0.002f, -0.0005f, 0.1f};
        int ambientInt = 0xff000000 | (36 << 16) | (38 << 8) | (40 + s * 5);
        LerpMeshVertexes_scalar(tess, (s + 1) % FRAMES, s % FRAMES, 0.25f + s * 0.1f);
        if (s & 1) RB_CalcDeformVertexes(tess, s * 0.3f);
        RB_CalcDiffuseColor_scalar(tess, ambient, directed, dir, ambientInt);
        if (s % 3 == 0) RB_CalcEnvironmentTexCoords(tess, view);
        if (s == 5) RB_CalcFogTexCoords(tess, fog);
        for (int i = 0; i < VERTS; i += 9)
            sum = sum * 31 + (uint32_t)(int)(tess->xyz[i][0] * 16) + tess->colors[i][0] + (uint32_t)(int)(tess->st[i][0] * 256);
    }
    return sum;
}

const wl_kernel wl_q3_tess = {"q3_tess", "Quake III renderer: LerpMeshVertexes, deform, diffuse light, environment and fog coords", setup, inst, run};
