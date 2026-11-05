/* programs/linux/semath.h checked on the host against doubles (docs/system/semath.md): the special functions' largest
 * errors over sweeps, against the bounds semath.h states; the matrices and quaternions against each other.
 *
 *     gcc -O2 -I programs/linux tools/semath_test.c -lm -o semath_test && ./semath_test     (in WSL)
 *
 * Prints a line a check and PASS or FAIL; exits 1 if any failed. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "semath.h"

static int failed;
static void check(const char* what, double got, double bound) {
    int ok = got <= bound;
    failed |= !ok;
    printf("%-46s %.3g (bound %.3g)  %s\n", what, got, bound, ok ? "PASS" : "FAIL");
}
static double m4_diff(const mat4* a, const mat4* b) {
    double d = 0;
    for (int i = 0; i < 16; i++) d = fmax(d, fabs((double)a->m[i] - b->m[i]));
    return d;
}
static unsigned seed = 12345;
static float rnd(float lo, float hi) {
    seed = seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(seed >> 8) / 16777216.0f;
}
static vec3 rnd3(float r) { return v3(rnd(-r, r), rnd(-r, r), rnd(-r, r)); }

int main(void) {
    double e;
    /* ---- the special functions ---- */
    e = 0;
    for (int i = 0; i < 2000000; i++) {
        float x = expf(rnd(-80.0f, 80.0f));
        e = fmax(e, fabs(sm_rsqrt(x) * sqrt((double)x) - 1.0));
    }
    check("sm_rsqrt, relative", e, 4.8e-6);
    e = 0;
    for (int i = 0; i < 2000000; i++) {
        float y = rnd(-10, 10), x = rnd(-10, 10);
        e = fmax(e, fabs(sm_atan2(y, x) - atan2((double)y, (double)x)));
    }
    check("sm_atan2, radians", e, 1.2e-5);
    e = 0;
    for (int i = 0; i < 2000000; i++) {
        float x = rnd(-126.0f, 127.0f);
        double want = exp2((double)x);
        e = fmax(e, fabs(sm_exp2(x) / want - 1.0));
    }
    check("sm_exp2, relative", e, 4e-7);
    e = 0;
    for (int i = 0; i < 2000000; i++) {
        float x = expf(rnd(-80.0f, 80.0f));
        double want = log2((double)x);
        e = fmax(e, fabs(sm_log2(x) - want) / fmax(1.0, fabs(want)));
    }
    check("sm_log2, of max(1, |log2 x|)", e, 2e-7);
    /* ---- matrices ---- */
    e = 0;
    for (int i = 0; i < 20000; i++) {
        mat4 t = m4_translate(rnd3(50)), r = m4_rotate(rnd(-7, 7), rnd3(1)), s = m4_scale(v3(rnd(0.2f, 5), rnd(0.2f, 5), rnd(0.2f, 5)));
        mat4 tr = m4_mul(&t, &r), m = m4_mul(&tr, &s), inv, back, id = m4_identity();
        if (!m4_inverse(&m, &inv)) { e = 1e9; break; }
        back = m4_mul(&m, &inv);
        e = fmax(e, m4_diff(&back, &id));
    }
    check("m4_inverse: m * inverse - identity", e, 2e-5);
    e = 0;
    for (int i = 0; i < 20000; i++) {
        vec3 move = rnd3(50);
        mat4 t = m4_translate(move), r = m4_rotate(rnd(-7, 7), rnd3(1)), m = m4_mul(&t, &r), a = m4_inverse_rigid(&m), b;
        m4_inverse(&m, &b);
        e = fmax(e, m4_diff(&a, &b) / (1.0 + v3_length(move)));
    }
    check("m4_inverse_rigid against m4_inverse, of 1 + |move|", e, 2e-6);
    e = 0;
    for (int i = 0; i < 20000; i++) {
        float a = rnd(-7, 7);
        vec3 axis = rnd3(1), v = rnd3(10);
        quat q = q_axis_angle(axis, a);
        mat4 m = m4_rotate(a, axis), qm = q_to_m4(q);
        vec3 by_m = m4_mul_dir(&m, v), by_q = q_rotate(q, v);
        e = fmax(e, fmax(m4_diff(&m, &qm), v3_length(v3_sub(by_m, by_q)) / 10.0));
    }
    check("q_axis_angle, q_to_m4, q_rotate against m4_rotate", e, 2e-6);
    e = 0;
    for (int i = 0; i < 20000; i++) {
        quat a = q_axis_angle(rnd3(1), rnd(-3, 3)), b = q_axis_angle(rnd3(1), rnd(-3, 3));
        vec3 v = rnd3(1);
        vec3 seq = q_rotate(b, q_rotate(a, v)), prod = q_rotate(q_mul(b, a), v);
        quat s0 = q_slerp(a, b, 0), s1 = q_slerp(a, b, 1);
        double d0 = fmin(fabs(s0.x - a.x) + fabs(s0.y - a.y) + fabs(s0.z - a.z) + fabs(s0.w - a.w),
                         fabs(s0.x + a.x) + fabs(s0.y + a.y) + fabs(s0.z + a.z) + fabs(s0.w + a.w));
        double d1 = fmin(fabs(s1.x - b.x) + fabs(s1.y - b.y) + fabs(s1.z - b.z) + fabs(s1.w - b.w),
                         fabs(s1.x + b.x) + fabs(s1.y + b.y) + fabs(s1.z + b.z) + fabs(s1.w + b.w));
        e = fmax(e, fmax(v3_length(v3_sub(seq, prod)), fmax(d0, d1)));
    }
    check("q_mul as turns one after another; q_slerp's ends", e, 2e-6);
    {   /* a look-at's view puts `at` on the -z axis at its distance, and its turn is orthonormal */
        e = 0;
        for (int i = 0; i < 20000; i++) {
            vec3 eye = rnd3(100), at = rnd3(100);
            mat4 v = m4_look_at(eye, at, v3(0, 1, 0));
            vec3 p = m4_mul_point(&v, at);
            double dist = v3_length(v3_sub(at, eye));
            e = fmax(e, (fabs(p.x) + fabs(p.y) + fabs(-p.z - dist)) / fmax(dist, 1.0));
            mat4 vt = m4_transpose(&v), turn = v, vv;
            turn.m[12] = turn.m[13] = turn.m[14] = 0, vt.m[3] = vt.m[7] = vt.m[11] = 0;
            vv = m4_mul(&vt, &turn);
            mat4 id = m4_identity();
            e = fmax(e, m4_diff(&vv, &id));
        }
        check("m4_look_at: the point looked at, orthonormal", e, 2e-5);
    }
    {   /* a perspective's near and far planes go to clip z = -w and w */
        mat4 p = m4_perspective(sm_radians(70), 16.0f / 9.0f, 0.5f, 400.0f);
        vec4 n = m4_mul_v4(&p, v4(0, 0, -0.5f, 1)), f = m4_mul_v4(&p, v4(0, 0, -400.0f, 1));
        check("m4_perspective: near at -1, far at +1 (z / w)", fmax(fabs(n.z / n.w + 1), fabs(f.z / f.w - 1)), 1e-5);
        mat4 o = m4_ortho(-2, 6, -1, 3, 1, 9);
        vec3 c = m4_mul_point(&o, v3(6, 3, -9)), d = m4_mul_point(&o, v3(-2, -1, -1));
        check("m4_ortho: its box's corners at the clip box's", fmax(fmax(fabs(c.x - 1), fabs(c.y - 1)), fmax(fmax(fabs(c.z - 1), fabs(d.x + 1)), fmax(fabs(d.y + 1), fabs(d.z + 1)))), 1e-6);
    }
    {   /* the frustum: points inside and outside a projection-and-view, as their clip coordinates say */
        mat4 p = m4_perspective(sm_radians(60), 1.5f, 1, 100), v = m4_look_at(v3(3, 2, 10), v3(0, 0, 0), v3(0, 1, 0));
        mat4 clip = m4_mul(&p, &v);
        sm_frustum f = sm_frustum_of(&clip);
        int wrong = 0;
        for (int i = 0; i < 200000; i++) {
            vec3 q = rnd3(120);
            vec4 c = m4_mul_v4(&clip, v4(q.x, q.y, q.z, 1));
            int inside = c.w > 0 && fabsf(c.x) <= c.w && fabsf(c.y) <= c.w && fabsf(c.z) <= c.w;
            float margin = fminf(fminf(c.w - fabsf(c.x), c.w - fabsf(c.y)), c.w - fabsf(c.z));
            if (fabsf(margin) < 1e-3f * fabsf(c.w) + 1e-4f) continue;   /* (on a plane: either) */
            wrong += sm_sees_sphere(&f, q, 0) != inside;
            if (!inside && sm_sees_box(&f, q, q)) wrong += 1;   /* a box of one point is that point */
        }
        check("sm_frustum_of, sm_sees_sphere and sm_sees_box: wrong", wrong, 0);
    }
    {   /* rays against a box: those aimed at its middle hit it where they enter its sphere or later, those aimed away miss */
        vec3 lo = v3(-1, -2, -3), hi = v3(2, 1, 1), mid = v3_scale(v3_add(lo, hi), 0.5f);
        int wrong = 0;
        for (int i = 0; i < 200000; i++) {
            vec3 o = v3_add(mid, v3_scale(v3_normalize(rnd3(1)), rnd(5, 50)));
            float t = sm_ray_box(o, v3_normalize(v3_sub(mid, o)), lo, hi), away = sm_ray_box(o, v3_normalize(v3_sub(o, mid)), lo, hi);
            vec3 at = v3_add(o, v3_scale(v3_normalize(v3_sub(mid, o)), t));
            int on = at.x >= lo.x - 1e-3f && at.x <= hi.x + 1e-3f && at.y >= lo.y - 1e-3f && at.y <= hi.y + 1e-3f && at.z >= lo.z - 1e-3f && at.z <= hi.z + 1e-3f;
            wrong += t < 0 || !on || away >= 0;
        }
        check("sm_ray_box: wrong", wrong, 0);
    }
    check("sm_fixed and back: 1.5 and -2.25", fabs(sm_from_fixed(sm_fixed(1.5f)) - 1.5) + fabs(sm_from_fixed(sm_fixed(-2.25f)) + 2.25) +
          (sm_fixed(1.5f) != 98304) + (sm_fixed(-2.25f) != -147456), 0);
    printf(failed ? "FAIL\n" : "PASS\n");
    return failed;
}
