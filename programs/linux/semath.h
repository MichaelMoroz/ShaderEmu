/* semath.h: the vectors, matrices and quaternions of games and graphics on the machine, and the special functions they
 * want (docs/system/semath.md). One header, every function static inline, floats only (the F instructions; doubles are
 * compiler-rt's software here), no state. Matrices are column-major, as OpenGL and gles.c keep them: m[c * 4 + r] is
 * row r of column c, and a vector is a column multiplied on the right. Angles are in radians.
 *
 * sinf, cosf, sqrtf and the rest come from the float libm (linux/userland/libm, 14 to 58 instructions a call). Where
 * a cheaper approximation is enough, it is a function of its own here with its bound stated (sm_rsqrt, sm_atan2,
 * sm_exp2, sm_log2), checked against doubles by tools/semath_test.c on the host. */
#ifndef SEMATH_H
#define SEMATH_H

#include <math.h>
#include <stdint.h>

#define SM_PI 3.14159265358979323846f

typedef struct { float x, y; } vec2;
typedef struct { float x, y, z; } vec3;
typedef struct { float x, y, z, w; } vec4;
typedef struct { float m[16]; } mat4;   /* column-major: m[c * 4 + r] */
typedef struct { float x, y, z, w; } quat;   /* w the real part */

/* ---- numbers ---- */

static inline float sm_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float sm_lerp(float a, float b, float t) { return a + (b - a) * t; }
static inline float sm_saturate(float v) { return sm_clamp(v, 0.0f, 1.0f); }
static inline float sm_smoothstep(float e0, float e1, float v) {
    float t = sm_saturate((v - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}
static inline float sm_radians(float degrees) { return degrees * (SM_PI / 180.0f); }
static inline float sm_degrees(float radians) { return radians * (180.0f / SM_PI); }
static inline uint32_t sm_asu(float f) { union { float f; uint32_t u; } v = {f}; return v.u; }
static inline float sm_asf(uint32_t u) { union { uint32_t u; float f; } v = {u}; return v.f; }

/* The GPU's 16.16 numbers (docs/machine/gpu.md): a float to them and back. */
static inline int32_t sm_fixed(float f) { return (int32_t)(f * 65536.0f + (f < 0 ? -0.5f : 0.5f)); }
static inline float sm_from_fixed(int32_t v) { return (float)v * (1.0f / 65536.0f); }

/* 1 / sqrt(x) for x > 0: the bits' first guess (3.4% at most) and two Newton steps, within 4.8e-6 of it (relative), no
 * division and no fsqrt: enough to normalise a direction. 1 / sqrtf(x) is exact and dearer. */
static inline float sm_rsqrt(float x) {
    float h = 0.5f * x, y = sm_asf(0x5f375a86u - (sm_asu(x) >> 1));
    y = y * (1.5f - h * y * y);
    return y * (1.5f - h * y * y);
}

/* atan2(y, x) within 1.2e-5 radians: the octant by compares, then the minimax polynomial of degree 9 on [0, 1] in the
 * ratio of the smaller to the larger (one division). (0, 0) is 0. */
static inline float sm_atan2(float y, float x) {
    float ax = fabsf(x), ay = fabsf(y), big = ax > ay ? ax : ay, small = ax > ay ? ay : ax;
    if (big == 0.0f) return 0.0f;
    float t = small / big, s = t * t;
    float r = t * (0.9998660f + s * (-0.3302995f + s * (0.1801410f + s * (-0.0851330f + s * 0.0208351f))));
    if (ay > ax) r = 0.5f * SM_PI - r;
    if (x < 0.0f) r = SM_PI - r;
    return y < 0.0f ? -r : r;
}

/* 2^x within 4e-7 relative for -126 <= x <= 127 (0 below, infinity above): the nearest whole power into the exponent,
 * what is left (-0.5 to 0.5) a polynomial of degree 6 (its Taylor series, whose next term is 1.2e-7 there). */
static inline float sm_exp2(float x) {
    if (x < -126.0f) return 0.0f;
    if (x > 127.0f) return INFINITY;
    float w = floorf(x + 0.5f), f = x - w;
    float p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * (0.00133336f + f * 0.00015403f)))));
    return p * sm_asf((uint32_t)((int32_t)w + 127) << 23);
}

/* log2(x) for x > 0 (normal) within 2e-7 of max(1, |log2 x|): the exponent's bits and the odd series of degree 7 in
 * (m - 1) / (m + 1) of the mantissa m in [sqrt(1/2), sqrt(2)) (its next term under 5e-8). */
static inline float sm_log2(float x) {
    uint32_t u = sm_asu(x);
    int32_t e = (int32_t)(u >> 23) - 127;
    float m = sm_asf((u & 0x007fffffu) | 0x3f800000u);
    if (m > 1.41421356f) m *= 0.5f, e++;
    float t = (m - 1.0f) / (m + 1.0f), s = t * t;
    return (float)e + t * (2.88539008f + s * (0.96179669f + s * (0.57707801f + s * 0.41219858f)));
}

/* ---- vectors ---- */

static inline vec2 v2(float x, float y) { vec2 r = {x, y}; return r; }
static inline vec3 v3(float x, float y, float z) { vec3 r = {x, y, z}; return r; }
static inline vec4 v4(float x, float y, float z, float w) { vec4 r = {x, y, z, w}; return r; }

static inline vec2 v2_add(vec2 a, vec2 b) { return v2(a.x + b.x, a.y + b.y); }
static inline vec2 v2_sub(vec2 a, vec2 b) { return v2(a.x - b.x, a.y - b.y); }
static inline vec2 v2_scale(vec2 a, float s) { return v2(a.x * s, a.y * s); }
static inline float v2_dot(vec2 a, vec2 b) { return a.x * b.x + a.y * b.y; }
static inline float v2_cross(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }   /* (the z of the 3D cross) */
static inline float v2_length(vec2 a) { return sqrtf(v2_dot(a, a)); }

static inline vec3 v3_add(vec3 a, vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline vec3 v3_sub(vec3 a, vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline vec3 v3_mul(vec3 a, vec3 b) { return v3(a.x * b.x, a.y * b.y, a.z * b.z); }
static inline vec3 v3_scale(vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline vec3 v3_neg(vec3 a) { return v3(-a.x, -a.y, -a.z); }
static inline float v3_dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline vec3 v3_cross(vec3 a, vec3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float v3_length(vec3 a) { return sqrtf(v3_dot(a, a)); }
/* (the zero vector stays zero) */
static inline vec3 v3_normalize(vec3 a) {
    float l2 = v3_dot(a, a);
    return l2 > 0.0f ? v3_scale(a, 1.0f / sqrtf(l2)) : a;
}
static inline vec3 v3_lerp(vec3 a, vec3 b, float t) { return v3(sm_lerp(a.x, b.x, t), sm_lerp(a.y, b.y, t), sm_lerp(a.z, b.z, t)); }
static inline vec3 v3_min(vec3 a, vec3 b) { return v3(fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z)); }
static inline vec3 v3_max(vec3 a, vec3 b) { return v3(fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z)); }
/* a reflected about the unit normal n */
static inline vec3 v3_reflect(vec3 a, vec3 n) { return v3_sub(a, v3_scale(n, 2.0f * v3_dot(a, n))); }

static inline vec4 v4_add(vec4 a, vec4 b) { return v4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
static inline vec4 v4_scale(vec4 a, float s) { return v4(a.x * s, a.y * s, a.z * s, a.w * s); }
static inline float v4_dot(vec4 a, vec4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

/* ---- matrices ---- */

static inline mat4 m4_identity(void) {
    mat4 r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    return r;
}
static inline float m4_at(const mat4* a, int row, int col) { return a->m[col * 4 + row]; }

/* a * b: b applied first */
static inline mat4 m4_mul(const mat4* a, const mat4* b) {
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++)
            r.m[c * 4 + k] = a->m[k] * b->m[c * 4] + a->m[4 + k] * b->m[c * 4 + 1] + a->m[8 + k] * b->m[c * 4 + 2] + a->m[12 + k] * b->m[c * 4 + 3];
    return r;
}
static inline vec4 m4_mul_v4(const mat4* a, vec4 v) {
    const float* m = a->m;
    return v4(m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12] * v.w, m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13] * v.w,
              m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w, m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w);
}
/* a point (w = 1), divided by its w when the matrix projects */
static inline vec3 m4_mul_point(const mat4* a, vec3 p) {
    vec4 r = m4_mul_v4(a, v4(p.x, p.y, p.z, 1.0f));
    return r.w != 1.0f && r.w != 0.0f ? v3(r.x / r.w, r.y / r.w, r.z / r.w) : v3(r.x, r.y, r.z);
}
/* a direction (w = 0) */
static inline vec3 m4_mul_dir(const mat4* a, vec3 d) {
    vec4 r = m4_mul_v4(a, v4(d.x, d.y, d.z, 0.0f));
    return v3(r.x, r.y, r.z);
}
static inline mat4 m4_transpose(const mat4* a) {
    mat4 r;
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++) r.m[c * 4 + k] = a->m[k * 4 + c];
    return r;
}
static inline mat4 m4_translate(vec3 t) {
    mat4 r = m4_identity();
    r.m[12] = t.x, r.m[13] = t.y, r.m[14] = t.z;
    return r;
}
static inline mat4 m4_scale(vec3 s) {
    mat4 r = m4_identity();
    r.m[0] = s.x, r.m[5] = s.y, r.m[10] = s.z;
    return r;
}
/* `angle` about the axis (any length but zero), as glRotate turns */
static inline mat4 m4_rotate(float angle, vec3 axis) {
    vec3 a = v3_normalize(axis);
    float c = cosf(angle), s = sinf(angle), k = 1.0f - c;
    mat4 r = m4_identity();
    r.m[0] = a.x * a.x * k + c, r.m[1] = a.y * a.x * k + a.z * s, r.m[2] = a.z * a.x * k - a.y * s;
    r.m[4] = a.x * a.y * k - a.z * s, r.m[5] = a.y * a.y * k + c, r.m[6] = a.z * a.y * k + a.x * s;
    r.m[8] = a.x * a.z * k + a.y * s, r.m[9] = a.y * a.z * k - a.x * s, r.m[10] = a.z * a.z * k + c;
    return r;
}
/* glFrustum's, and glOrtho's: clip z from -w to w (the GPU takes it so: docs/machine/gpu.md) */
static inline mat4 m4_frustum(float l, float r, float b, float t, float n, float f) {
    mat4 m = {{0}};
    m.m[0] = 2.0f * n / (r - l), m.m[5] = 2.0f * n / (t - b);
    m.m[8] = (r + l) / (r - l), m.m[9] = (t + b) / (t - b), m.m[10] = -(f + n) / (f - n), m.m[11] = -1.0f;
    m.m[14] = -2.0f * f * n / (f - n);
    return m;
}
/* gluPerspective's: the vertical field of view, width over height */
static inline mat4 m4_perspective(float fovy, float aspect, float n, float f) {
    float t = n * tanf(0.5f * fovy);
    return m4_frustum(-t * aspect, t * aspect, -t, t, n, f);
}
static inline mat4 m4_ortho(float l, float r, float b, float t, float n, float f) {
    mat4 m = m4_identity();
    m.m[0] = 2.0f / (r - l), m.m[5] = 2.0f / (t - b), m.m[10] = -2.0f / (f - n);
    m.m[12] = -(r + l) / (r - l), m.m[13] = -(t + b) / (t - b), m.m[14] = -(f + n) / (f - n);
    return m;
}
/* gluLookAt's: the eye at `eye` looking at `at`, `up` up (not along the view) */
static inline mat4 m4_look_at(vec3 eye, vec3 at, vec3 up) {
    vec3 f = v3_normalize(v3_sub(at, eye)), s = v3_normalize(v3_cross(f, up)), u = v3_cross(s, f);
    mat4 m = m4_identity();
    m.m[0] = s.x, m.m[4] = s.y, m.m[8] = s.z;
    m.m[1] = u.x, m.m[5] = u.y, m.m[9] = u.z;
    m.m[2] = -f.x, m.m[6] = -f.y, m.m[10] = -f.z;
    m.m[12] = -v3_dot(s, eye), m.m[13] = -v3_dot(u, eye), m.m[14] = v3_dot(f, eye);
    return m;
}
/* The inverse of any invertible matrix (cofactors; one division), and whether it had one: else the identity. */
static inline int m4_inverse(const mat4* a, mat4* out) {
    const float* m = a->m;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0f) {
        *out = m4_identity();
        return 0;
    }
    float d = 1.0f / det;
    for (int i = 0; i < 16; i++) out->m[i] = inv[i] * d;
    return 1;
}
/* The inverse of a rotation and translation (an orthonormal 3 x 3 and a move): the transposed turn, the move turned back. */
static inline mat4 m4_inverse_rigid(const mat4* a) {
    const float* m = a->m;
    mat4 r = m4_identity();
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 3; k++) r.m[c * 4 + k] = m[k * 4 + c];
    vec3 t = v3(m[12], m[13], m[14]);
    r.m[12] = -(r.m[0] * t.x + r.m[4] * t.y + r.m[8] * t.z);
    r.m[13] = -(r.m[1] * t.x + r.m[5] * t.y + r.m[9] * t.z);
    r.m[14] = -(r.m[2] * t.x + r.m[6] * t.y + r.m[10] * t.z);
    return r;
}
/* the matrix normals are turned by: the inverse transposed of the upper 3 x 3 (in a mat4, the rest the identity's) */
static inline mat4 m4_normal(const mat4* a) {
    mat4 u = *a, inv, r;
    u.m[3] = u.m[7] = u.m[11] = u.m[12] = u.m[13] = u.m[14] = 0.0f, u.m[15] = 1.0f;
    m4_inverse(&u, &inv);
    r = m4_transpose(&inv);
    return r;
}

/* ---- quaternions ---- */

static inline quat q_identity(void) { quat q = {0, 0, 0, 1}; return q; }
static inline quat q_axis_angle(vec3 axis, float angle) {
    vec3 a = v3_normalize(axis);
    float s = sinf(0.5f * angle);
    quat q = {a.x * s, a.y * s, a.z * s, cosf(0.5f * angle)};
    return q;
}
/* a then b is b * a: a * b turns by b first */
static inline quat q_mul(quat a, quat b) {
    quat r = {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    return r;
}
static inline quat q_conjugate(quat q) { quat r = {-q.x, -q.y, -q.z, q.w}; return r; }
static inline quat q_normalize(quat q) {
    float l2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w, s = l2 > 0.0f ? 1.0f / sqrtf(l2) : 0.0f;
    quat r = {q.x * s, q.y * s, q.z * s, q.w * s};
    return l2 > 0.0f ? r : q_identity();
}
/* v turned by the unit quaternion q */
static inline vec3 q_rotate(quat q, vec3 v) {
    vec3 u = v3(q.x, q.y, q.z), t = v3_scale(v3_cross(u, v), 2.0f);
    return v3_add(v3_add(v, v3_scale(t, q.w)), v3_cross(u, t));
}
static inline mat4 q_to_m4(quat q) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z, xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    mat4 m = m4_identity();
    m.m[0] = 1.0f - 2.0f * (yy + zz), m.m[1] = 2.0f * (xy + wz), m.m[2] = 2.0f * (xz - wy);
    m.m[4] = 2.0f * (xy - wz), m.m[5] = 1.0f - 2.0f * (xx + zz), m.m[6] = 2.0f * (yz + wx);
    m.m[8] = 2.0f * (xz + wy), m.m[9] = 2.0f * (yz - wx), m.m[10] = 1.0f - 2.0f * (xx + yy);
    return m;
}
/* the shorter way from a to b, t from 0 to 1; near each other a normalised lerp */
static inline quat q_slerp(quat a, quat b, float t) {
    float c = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (c < 0.0f) b.x = -b.x, b.y = -b.y, b.z = -b.z, b.w = -b.w, c = -c;
    float ka = 1.0f - t, kb = t;
    if (c < 0.9995f) {
        float o = acosf(c), s = 1.0f / sinf(o);
        ka = sinf(ka * o) * s, kb = sinf(kb * o) * s;
    }
    quat r = {a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb, a.w * ka + b.w * kb};
    return q_normalize(r);
}

/* ---- what games ask of space ---- */

/* The six planes of what a projection-and-view matrix sees (Gribb and Hartmann): each (a, b, c, d) with a point inside
 * where a x + b y + c z + d >= 0, normalised, so that the sum is a distance. */
typedef struct { vec4 p[6]; } sm_frustum;
static inline sm_frustum sm_frustum_of(const mat4* clip) {
    const float* m = clip->m;
    sm_frustum f;
    for (int i = 0; i < 6; i++) {
        int row = i >> 1;
        float s = (i & 1) ? -1.0f : 1.0f;
        vec4 p = v4(m[3] + s * m[row], m[7] + s * m[4 + row], m[11] + s * m[8 + row], m[15] + s * m[12 + row]);
        float l = sqrtf(p.x * p.x + p.y * p.y + p.z * p.z);
        f.p[i] = l > 0.0f ? v4_scale(p, 1.0f / l) : p;
    }
    return f;
}
/* whether a sphere is (maybe) seen: outside no plane by more than its radius */
static inline int sm_sees_sphere(const sm_frustum* f, vec3 c, float radius) {
    for (int i = 0; i < 6; i++)
        if (f->p[i].x * c.x + f->p[i].y * c.y + f->p[i].z * c.z + f->p[i].w < -radius) return 0;
    return 1;
}
/* whether a box (its two corners) is (maybe) seen: the corner most inside each plane is inside it */
static inline int sm_sees_box(const sm_frustum* f, vec3 lo, vec3 hi) {
    for (int i = 0; i < 6; i++) {
        vec4 p = f->p[i];
        float x = p.x >= 0 ? hi.x : lo.x, y = p.y >= 0 ? hi.y : lo.y, z = p.z >= 0 ? hi.z : lo.z;
        if (p.x * x + p.y * y + p.z * z + p.w < 0.0f) return 0;
    }
    return 1;
}
/* Where a ray (origin, direction) enters a box, along it: the distance, or -1 if it misses (the slabs' method; an
 * origin inside is 0). */
static inline float sm_ray_box(vec3 o, vec3 d, vec3 lo, vec3 hi) {
    float near = 0.0f, far = INFINITY;
    const float* op = &o.x; const float* dp = &d.x; const float* lp = &lo.x; const float* hp = &hi.x;
    for (int k = 0; k < 3; k++) {
        if (dp[k] == 0.0f) {
            if (op[k] < lp[k] || op[k] > hp[k]) return -1.0f;
            continue;
        }
        float inv = 1.0f / dp[k], t0 = (lp[k] - op[k]) * inv, t1 = (hp[k] - op[k]) * inv;
        if (t0 > t1) { float s = t0; t0 = t1; t1 = s; }
        if (t0 > near) near = t0;
        if (t1 < far) far = t1;
        if (near > far) return -1.0f;
    }
    return near;
}

#endif
