/*
 * hl_studio: Half-Life's models posed and lit every frame (Xash3D's ref/gl, the machine's engine). Copied from
 * xash3d public/xash3d_mathlib.c and public/matrixlib.c: R_StudioCalcBones (the run-length animation values
 * decoded, two frames lerped), AngleQuaternion (studio angles), QuaternionAlign, QuaternionSlerp,
 * Matrix3x4_FromOriginQuat, Matrix3x4_ConcatTransforms; then gl_studio.c's per-vertex work as the port's job
 * does it (gl_studio_job.c): each vertex moved by its bone, each normal turned and lit against the bone's light
 * vector (R_StudioLighting's ambient and shade). A model: 48 bones in a tree, 6 animation channels each, 1,200
 * vertices and normals; a unit poses and lights 4 of them at different frames.
 */
#include <math.h>
#include <string.h>
#include "wl.h"

#define BONES 48
#define VERTS 1200
#define MODELS 4
#define FRAMES 60

typedef float vec3_t[3];
typedef float vec4_t[4];
typedef float matrix3x4[3][4];
typedef union { struct { unsigned char valid, total; } num; short value; } mstudioanimvalue_t;
typedef struct { int parent; int bonecontroller[6]; float value[6], scale[6]; } mstudiobone_t;
typedef struct { unsigned short offset[6]; } mstudioanim_t;

static mstudiobone_t bones[BONES];
static mstudioanim_t *anims[BONES];   /* each followed by its values, as in a model file */
static vec3_t verts[VERTS], norms[VERTS];
static unsigned char vertbone[VERTS];
static vec3_t lightvec = {0.3f, -0.5f, -0.81f};

struct scratch { matrix3x4 bonetransform[BONES]; vec3_t blight[BONES]; vec3_t out[VERTS]; float shade[VERTS]; };

static inline void SinCos(float a, float *s, float *c) { *s = sinf(a), *c = cosf(a); }

static inline void AngleQuaternion(const vec3_t angles, vec4_t q)
{
    float sr, sp, sy, cr, cp, cy;
    SinCos(angles[2] * 0.5f, &sy, &cy);
    SinCos(angles[1] * 0.5f, &sp, &cp);
    SinCos(angles[0] * 0.5f, &sr, &cr);
    q[0] = sr * cp * cy - cr * sp * sy;
    q[1] = cr * sp * cy + sr * cp * sy;
    q[2] = cr * cp * sy - sr * sp * cy;
    q[3] = cr * cp * cy + sr * sp * sy;
}

static void QuaternionAlign(const vec4_t p, const vec4_t q, vec4_t qt)
{
    float a = 0, b = 0;
    for (int i = 0; i < 4; i++) a += (p[i] - q[i]) * (p[i] - q[i]), b += (p[i] + q[i]) * (p[i] + q[i]);
    if (a > b) for (int i = 0; i < 4; i++) qt[i] = -q[i];
    else for (int i = 0; i < 4; i++) qt[i] = q[i];
}

static void QuaternionSlerpNoAlign(const vec4_t p, const vec4_t q, float t, vec4_t qt)
{
    float cosom = p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3], sclp, sclq;
    if ((1.0f + cosom) > 0.000001f) {
        if ((1.0f - cosom) > 0.000001f) {
            float omega = acosf(cosom), sinom = sinf(omega);
            sclp = sinf((1.0f - t) * omega) / sinom;
            sclq = sinf(t * omega) / sinom;
        } else sclp = 1.0f - t, sclq = t;
        for (int i = 0; i < 4; i++) qt[i] = sclp * p[i] + sclq * q[i];
    } else {
        qt[0] = -q[1], qt[1] = q[0], qt[2] = -q[3], qt[3] = q[2];
        sclp = sinf((1.0f - t) * (0.5f * 3.14159265f));
        sclq = sinf(t * (0.5f * 3.14159265f));
        for (int i = 0; i < 3; i++) qt[i] = sclp * p[i] + sclq * qt[i];
    }
}

static void QuaternionSlerp(const vec4_t p, const vec4_t q, float t, vec4_t qt)
{
    vec4_t q2;
    QuaternionAlign(p, q, q2);
    QuaternionSlerpNoAlign(p, q2, t, qt);
}

static void Matrix3x4_FromOriginQuat(matrix3x4 out, const vec4_t q, const vec3_t origin)
{
    out[0][0] = 1.0f - 2.0f * q[1] * q[1] - 2.0f * q[2] * q[2];
    out[1][0] = 2.0f * q[0] * q[1] + 2.0f * q[3] * q[2];
    out[2][0] = 2.0f * q[0] * q[2] - 2.0f * q[3] * q[1];
    out[0][1] = 2.0f * q[0] * q[1] - 2.0f * q[3] * q[2];
    out[1][1] = 1.0f - 2.0f * q[0] * q[0] - 2.0f * q[2] * q[2];
    out[2][1] = 2.0f * q[1] * q[2] + 2.0f * q[3] * q[0];
    out[0][2] = 2.0f * q[0] * q[2] + 2.0f * q[3] * q[1];
    out[1][2] = 2.0f * q[1] * q[2] - 2.0f * q[3] * q[0];
    out[2][2] = 1.0f - 2.0f * q[0] * q[0] - 2.0f * q[1] * q[1];
    out[0][3] = origin[0], out[1][3] = origin[1], out[2][3] = origin[2];
}

static void Matrix3x4_ConcatTransforms(matrix3x4 out, const matrix3x4 in1, const matrix3x4 in2)
{
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) out[r][c] = in1[r][0] * in2[0][c] + in1[r][1] * in2[1][c] + in1[r][2] * in2[2][c];
        out[r][3] += in1[r][3];
    }
}

static void R_StudioCalcBones(int frame, float s, const mstudiobone_t *pbone, const mstudioanim_t *panim, vec3_t pos, vec4_t q)
{
    float v1[6], v2[6];
    for (int i = 0; i < 6; i++) {
        const mstudioanimvalue_t *panimvalue = (const mstudioanimvalue_t *)((const unsigned char *)panim + panim->offset[i]);
        int j = frame;
        if (panim->offset[i] == 0) { v1[i] = v2[i] = pbone->value[i]; continue; }
        if (panimvalue->num.total < panimvalue->num.valid) j = 0;
        while (panimvalue->num.total <= j) {
            j -= panimvalue->num.total;
            panimvalue += panimvalue->num.valid + 1;
            if (panimvalue->num.total < panimvalue->num.valid) j = 0;
        }
        if (panimvalue->num.valid > j) {
            v1[i] = panimvalue[j + 1].value;
            if (panimvalue->num.valid > j + 1) v2[i] = panimvalue[j + 2].value;
            else if (panimvalue->num.total > j + 1) v2[i] = v1[i];
            else v2[i] = panimvalue[panimvalue->num.valid + 2].value;
        } else {
            v1[i] = panimvalue[panimvalue->num.valid].value;
            if (panimvalue->num.total > j + 1) v2[i] = v1[i];
            else v2[i] = panimvalue[panimvalue->num.valid + 2].value;
        }
        v1[i] = pbone->value[i] + v1[i] * pbone->scale[i];
        v2[i] = pbone->value[i] + v2[i] * pbone->scale[i];
    }
    if (v1[0] != v2[0] || v1[1] != v2[1] || v1[2] != v2[2])
        for (int i = 0; i < 3; i++) pos[i] = v1[i] + s * (v2[i] - v1[i]);
    else memcpy(pos, v1, sizeof(vec3_t));
    if (v1[3] != v2[3] || v1[4] != v2[4] || v1[5] != v2[5]) {
        vec4_t q1, q2;
        AngleQuaternion(&v1[3], q1);
        AngleQuaternion(&v2[3], q2);
        QuaternionSlerp(q1, q2, s, q);
    } else AngleQuaternion(&v1[3], q);
}

static void setup(void)
{
    uint32_t seed = 0x57d10;
    for (int b = 0; b < BONES; b++) {
        mstudiobone_t *bone = &bones[b];
        bone->parent = b ? (int)(wl_rand(&seed) % (uint32_t)b) : -1;
        for (int i = 0; i < 6; i++) {
            bone->bonecontroller[i] = -1;
            bone->value[i] = i < 3 ? 8 * wl_randf(&seed) - 4 : 0.6f * wl_randf(&seed) - 0.3f;
            bone->scale[i] = i < 3 ? 0.05f : 0.0005f;
        }
        /* each channel: runs of (valid, total) with `valid` values, over FRAMES frames; a fixed channel now and then */
        mstudioanimvalue_t values[6][FRAMES * 2 + 6];
        int count[6];
        for (int i = 0; i < 6; i++) {
            int n = 0, f = 0;
            if (wl_rand(&seed) % 5 == 0) { count[i] = 0; continue; }
            while (f < FRAMES) {
                int total = 1 + (int)(wl_rand(&seed) % 8), valid = 1 + (int)(wl_rand(&seed) % (unsigned)total);
                values[i][n].num.valid = (unsigned char)valid, values[i][n].num.total = (unsigned char)total;
                n++;
                for (int k = 0; k < valid; k++) values[i][n++].value = (short)((int)(wl_rand(&seed) % 2000) - 1000);
                f += total;
            }
            values[i][n].num.valid = 1, values[i][n].num.total = 1, n++;
            values[i][n++].value = 0;
            count[i] = n;
        }
        int size = (int)sizeof(mstudioanim_t);
        for (int i = 0; i < 6; i++) size += count[i] * 2;
        mstudioanim_t *a = wl_alloc((unsigned)size);
        unsigned short at = sizeof(mstudioanim_t);
        for (int i = 0; i < 6; i++) {
            a->offset[i] = count[i] ? at : 0;
            memcpy((unsigned char *)a + at, values[i], (size_t)count[i] * 2);
            at = (unsigned short)(at + count[i] * 2);
        }
        anims[b] = a;
    }
    for (int v = 0; v < VERTS; v++) {
        float len = 0;
        for (int i = 0; i < 3; i++) verts[v][i] = 20 * wl_randf(&seed) - 10, norms[v][i] = wl_randf(&seed) - 0.5f, len += norms[v][i] * norms[v][i];
        for (int i = 0; i < 3; i++) norms[v][i] /= sqrtf(len);
        vertbone[v] = (unsigned char)(wl_rand(&seed) % BONES);
    }
}

static void *inst(void) { return wl_alloc(sizeof(struct scratch)); }

static uint32_t run(void *ctx)
{
    struct scratch *s = ctx;
    uint32_t sum = 0;
    for (int m = 0; m < MODELS; m++) {
        int frame = (m * 13 + 5) % (FRAMES - 1);
        float lerp = 0.25f + 0.17f * m;
        for (int b = 0; b < BONES; b++) {   /* R_StudioSetupBones */
            vec3_t pos;
            vec4_t q;
            matrix3x4 bonematrix;
            R_StudioCalcBones(frame, lerp, &bones[b], anims[b], pos, q);
            Matrix3x4_FromOriginQuat(bonematrix, q, pos);
            if (bones[b].parent == -1) memcpy(s->bonetransform[b], bonematrix, sizeof bonematrix);
            else Matrix3x4_ConcatTransforms(s->bonetransform[b], s->bonetransform[bones[b].parent], bonematrix);
            for (int i = 0; i < 3; i++)   /* the light in the bone's frame (Matrix3x4_VectorIRotate) */
                s->blight[b][i] = lightvec[0] * s->bonetransform[b][0][i] + lightvec[1] * s->bonetransform[b][1][i] + lightvec[2] * s->bonetransform[b][2][i];
        }
        for (int v = 0; v < VERTS; v++) {   /* the job: moved by the bone, lit by the normal */
            const float (*t)[4] = s->bonetransform[vertbone[v]];
            const float *p = verts[v], *n = norms[v], *l = s->blight[vertbone[v]];
            for (int i = 0; i < 3; i++) s->out[v][i] = p[0] * t[i][0] + p[1] * t[i][1] + p[2] * t[i][2] + t[i][3];
            float lightcos = n[0] * l[0] + n[1] * l[1] + n[2] * l[2], illum = 96;
            if (lightcos > 1.0f) lightcos = 1;
            illum += 160 * (lightcos > 0 ? lightcos : 0.25f * lightcos);
            s->shade[v] = illum > 255 ? 255 : illum < 0 ? 0 : illum;
        }
        for (int v = 0; v < VERTS; v += 37) sum = sum * 31 + wl_fold(s->out[v][0] + s->out[v][1] + s->out[v][2] + s->shade[v]);
    }
    return sum;
}

const wl_kernel wl_hl_studio = {"hl_studio", "Half-Life: R_StudioCalcBones, slerp, bone matrices, vertex job and light", setup, inst, run};
