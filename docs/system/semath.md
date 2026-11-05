# semath: vectors, matrices and quaternions

`programs/linux/semath.h` is the maths games and graphics share on the machine: one header of static inline functions on
floats (the F instructions; doubles are compiler-rt's software here), with no state and nothing to link. A program built
in the tree finds it with `-I programs/linux` (where `gles.c` is, which the GL programs have already); a program compiled
in the machine by `cc` finds it as `<semath.h>` (`linux/tcc/build.sh` puts it in `/usr/include`).

- **Types**: `vec2`, `vec3`, `vec4`, `mat4` and `quat`, plain structs of floats.
- **Matrices** are column-major, as OpenGL and `gles.c` keep them (`m[c * 4 + r]`), a vector a column on the right:
  `m4_mul`, `m4_mul_v4`, `m4_mul_point` (divided by w where it projects), `m4_mul_dir`, `m4_translate`, `m4_scale`,
  `m4_rotate` (glRotate's), `m4_frustum`, `m4_perspective` (gluPerspective's), `m4_ortho`, `m4_look_at`
  (gluLookAt's), `m4_inverse` (any invertible matrix), `m4_inverse_rigid` (a turn and a move), `m4_normal` (the
  normals' matrix), `m4_transpose`. Clip z runs from -w to w, as the GPU takes it (`docs/machine/gpu.md`).
- **Quaternions**: `q_axis_angle`, `q_mul` (`q_mul(b, a)` is a and then b), `q_rotate`, `q_to_m4`, `q_slerp` (the
  shorter way), `q_normalize`, `q_conjugate`.
- **Space**: `sm_frustum_of` the six planes a projection-and-view matrix sees, `sm_sees_sphere` and `sm_sees_box`,
  `sm_ray_box` where a ray enters a box.
- **Numbers**: `sm_clamp`, `sm_lerp`, `sm_smoothstep`, `sm_radians`, `sm_fixed` and `sm_from_fixed` (the GPU's 16.16).

Where `sinf`, `cosf`, `sqrtf` and the rest of the float libm (`linux/userland/libm`, 14 to 58 instructions a call) are more
than a game needs, there are cheaper ones, each within the bound it states:

| function | bound | how |
|---|---|---|
| `sm_rsqrt(x)` | 4.8e-6, relative | the bits' guess and two Newton steps: no division, no fsqrt |
| `sm_atan2(y, x)` | 1.2e-5 radians | the octant by compares, a minimax polynomial of degree 9 |
| `sm_exp2(x)` | 4e-7, relative | the nearest whole power into the exponent, a polynomial of degree 6 |
| `sm_log2(x)` | 2e-7 of max(1, \|log2 x\|) | the exponent's bits, an odd series of degree 7 |

`tools/semath_test.c` checks all of it on the host against doubles (5 November 2026, every check PASS: `sm_rsqrt` 4.73e-6,
`sm_atan2` 1.17e-5, `sm_exp2` 2.39e-7, `sm_log2` 1.36e-7; `m4_inverse` against the identity 1.53e-5 over turns, moves of 50
and scales of 0.2 to 5; the quaternions against `m4_rotate` 7.9e-7; the frustum and the ray against clip coordinates and a
box, 0 wrong of 200,000 each):

    gcc -O2 -I programs/linux tools/semath_test.c -lm -o semath_test && ./semath_test     (in WSL)

A change to the header runs that test, and keeps it compiling with `rv32-cc -Wall -Wextra` and with `cc` in the machine.
