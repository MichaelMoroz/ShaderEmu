// Quake-like float kernels (Quake III's q_math.c and renderer, written as the game does them), for the cost of a call
// on gpu_vm (bundles: CYCLE on gvm_cpu) and on the RISC-V machine (instructions: qemu's trace, run.sh rv). Each kernel
// runs REPS times between two marks; main prints a sum of the results (the same on both within float rounding).
//   bash gpu_vm/llvm/test/libm/run.sh kernels
float sinf(float), cosf(float), sqrtf(float), powf(float, float), atan2f(float, float);

#define REPS 64
#define NI __attribute__((noinline))
typedef float vec3_t[3];

#ifdef __gpuvm__
static inline unsigned clock_now(void) {
  unsigned c;
  __asm__ volatile("mfsr %0, 12" : "=r"(c));
  return c;
}
#else
static inline unsigned clock_now(void) { return 0; }
#endif
static unsigned marks[64];
static int nmarks;
// a call between kernels: on gpu_vm it reads CYCLE; on RISC-V the trace counts instructions between its calls
NI void qk_mark(void) {
  marks[nmarks++] = clock_now();
  __asm__ volatile("" ::: "memory");
}

// q_math.c
NI float VectorNormalize(vec3_t v) {
  float length = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
  length = sqrtf(length);
  if (length) {
    float ilength = 1 / length;
    v[0] *= ilength;
    v[1] *= ilength;
    v[2] *= ilength;
  }
  return length;
}

NI void MatrixMultiply(float in1[3][3], float in2[3][3], float out[3][3]) {
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      out[i][j] = in1[i][0] * in2[0][j] + in1[i][1] * in2[1][j] + in1[i][2] * in2[2][j];
}

#define M_PI_F 3.14159265358979323846f
NI void AngleVectors(const vec3_t angles, vec3_t forward, vec3_t right, vec3_t up) {
  float angle, sr, sp, sy, cr, cp, cy;
  angle = angles[1] * (M_PI_F * 2 / 360);
  sy = sinf(angle);
  cy = cosf(angle);
  angle = angles[0] * (M_PI_F * 2 / 360);
  sp = sinf(angle);
  cp = cosf(angle);
  angle = angles[2] * (M_PI_F * 2 / 360);
  sr = sinf(angle);
  cr = cosf(angle);
  forward[0] = cp * cy;
  forward[1] = cp * sy;
  forward[2] = -sp;
  right[0] = (-1 * sr * sp * cy + -1 * cr * -sy);
  right[1] = (-1 * sr * sp * sy + -1 * cr * cy);
  right[2] = -1 * sr * cp;
  up[0] = (cr * sp * cy + -sr * -sy);
  up[1] = (cr * sp * sy + -sr * cy);
  up[2] = cr * cp;
}

NI void AnglesToAxis(const vec3_t angles, vec3_t axis[3]) {
  vec3_t right;
  AngleVectors(angles, axis[0], right, axis[2]);
  axis[1][0] = -right[0];
  axis[1][1] = -right[1];
  axis[1][2] = -right[2];
}

// the renderer's tables (tr_init.c): a sine wave of 1024 entries, the gamma ramp by pow
static float sinTable[1024];
NI void SinTable(void) {
  for (int i = 0; i < 1024; i++) sinTable[i] = sinf(i * (2.0f * M_PI_F / 1023));
}

static unsigned char gammatable[256];
NI void GammaTable(float gamma) {
  for (int i = 0; i < 256; i++) {
    int inf = (int)(255 * powf(i / 255.0f, 1.0f / gamma) + 0.5f);
    gammatable[i] = inf < 0 ? 0 : inf > 255 ? 255 : (unsigned char)inf;
  }
}

// a model's vertices by a 3x4 matrix (the fmad kernel)
static float verts[64][3], out[64][3];
NI void TransformVerts(const float m[3][4], int n) {
  for (int i = 0; i < n; i++)
    for (int j = 0; j < 3; j++)
      out[i][j] = m[j][0] * verts[i][0] + m[j][1] * verts[i][1] + m[j][2] * verts[i][2] + m[j][3];
}

// vectoangles (q_math.c): atan2 and sqrt
NI void vectoangles(const vec3_t value1, vec3_t angles) {
  float forward, yaw, pitch;
  if (value1[1] == 0 && value1[0] == 0) {
    yaw = 0;
    pitch = value1[2] > 0 ? 90 : 270;
  } else {
    yaw = value1[0] ? atan2f(value1[1], value1[0]) * 180 / M_PI_F : value1[1] > 0 ? 90 : 270;
    if (yaw < 0) yaw += 360;
    forward = sqrtf(value1[0] * value1[0] + value1[1] * value1[1]);
    pitch = atan2f(value1[2], forward) * 180 / M_PI_F;
    if (pitch < 0) pitch += 360;
  }
  angles[0] = -pitch;
  angles[1] = yaw;
  angles[2] = 0;
}

volatile float qk_sink;
float qk_run(void) {
  static vec3_t v[REPS], ang[REPS], axis[REPS][3];
  static float m1[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9.5f}}, m2[3][3] = {{0.5f, 1, 0}, {0, 1, 2}, {3, 0, 1}}, m3[3][3];
  static const float m34[3][4] = {{0.9f, 0.1f, 0, 5}, {-0.1f, 0.9f, 0.2f, -3}, {0, -0.2f, 0.95f, 1}};
  float s = 0;
  for (int i = 0; i < REPS; i++) {
    v[i][0] = i * 0.37f - 3, v[i][1] = 1 + i * 0.11f, v[i][2] = 2 - i * 0.05f;
    ang[i][0] = i * 5.6f - 90, ang[i][1] = i * 11.3f, ang[i][2] = i * 2.1f - 40;
  }
  for (int i = 0; i < 64; i++) verts[i][0] = i, verts[i][1] = 64 - i, verts[i][2] = i * 0.5f;
  nmarks = 0;
  qk_mark();
  for (int i = 0; i < REPS; i++) s += VectorNormalize(v[i]);
  qk_mark();
  for (int i = 0; i < REPS; i++) MatrixMultiply(m1, m2, m3), m1[i % 3][i % 3] = m3[(i + 1) % 3][i % 3] * 0.01f;
  qk_mark();
  for (int i = 0; i < REPS; i++) AnglesToAxis(ang[i], axis[i]);
  qk_mark();
  for (int i = 0; i < REPS; i++) vectoangles(v[i], ang[i]);
  qk_mark();
  for (int i = 0; i < REPS; i++) TransformVerts(m34, 64);
  qk_mark();
  SinTable();
  qk_mark();
  GammaTable(1.3f);
  qk_mark();
  for (int i = 0; i < REPS; i++) s += v[i][0] + v[i][1] + v[i][2] + ang[i][0] + ang[i][1] + axis[i][1][2] + axis[i][2][0];
  for (int i = 0; i < 3; i++) s += m3[i][0] + m3[i][1] + m3[i][2];
  for (int i = 0; i < 64; i++) s += out[i][0] + out[i][1] + out[i][2];
  for (int i = 0; i < 1024; i += 7) s += sinTable[i];
  for (int i = 0; i < 256; i += 3) s += gammatable[i];
  return s;
}

const char *qk_names[] = {"VectorNormalize", "MatrixMultiply", "AnglesToAxis", "vectoangles", "TransformVerts (64)",
                          "SinTable (1024)", "GammaTable (256)"};
const int qk_calls[] = {REPS, REPS, REPS, REPS, REPS, 1, 1};
#define QK_N 7

#ifdef __gpuvm__
#include "gvm.h"
#else
#include <stdio.h>
#endif
int main(void) {
  float s = qk_run();
  qk_sink = s;
  printf("sum %.3f\n", s);
#ifdef __gpuvm__
  for (int k = 0; k < QK_N; k++) {
    unsigned b = marks[k + 1] - marks[k], v = (b * 10 + qk_calls[k] / 2) / qk_calls[k];
    printf("%-22s %8u.%u bundles a call\n", qk_names[k], v / 10, v % 10);
  }
#endif
  return 0;
}
