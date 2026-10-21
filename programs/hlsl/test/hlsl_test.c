/*
 * hlsl_test: the compiler on this computer, its code run by a small RV32IMF emulator that keeps a
 * lane's budget (CSR 0x0ba) and its rounds: every result is compared with C, and every index made
 * in steps with the budget must equal the same made in one call.
 *   gcc -O2 -I.. hlsl_test.c ../hlsl.c ../hlsl_parse.c ../hlsl_gen.c ../hlsl_back.c -lm -o hlsl_test
 *   ./hlsl_test [FILE.hlsl WIDTH HEIGHT OUT.ppm [uniform=v,v,v ...]]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hlsl.h"

/* ---- the emulator ---- */

#define MEM (32u << 20)
#define CODE_AT 0x10000u
#define CTX_AT 0x100000u
#define OUT_AT 0x200000u
#define STACK_AT 0x1f00000u
#define STOP 0x1fffff0u

static uint8_t *mem;
static uint32_t x[32], f[32], pc;
static uint64_t steps_run;
static long budget;	/* -1: no end */
static long used;

static uint32_t rd32(uint32_t a)
{
	if (a > MEM - 4 || (a & 3)) { fprintf(stderr, "read at %08x (pc %08x)\n", a, pc); exit(2); }
	return *(uint32_t *)(mem + a);
}

static void wr32(uint32_t a, uint32_t v)
{
	if (a > MEM - 4 || (a & 3)) { fprintf(stderr, "write at %08x (pc %08x)\n", a, pc); exit(2); }
	*(uint32_t *)(mem + a) = v;
}

static float F(int r) { float v; memcpy(&v, &f[r], 4); return v; }
static void SF(int r, float v) { memcpy(&f[r], &v, 4); }

static int32_t sx(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

static uint32_t cvt_w(float v, int rm, int uns)
{
	double r;

	if (v != v) return uns ? 0xffffffffu : 0x7fffffffu;
	switch (rm) {
	case 1: r = trunc(v); break;
	case 2: r = floor(v); break;
	case 3: r = ceil(v); break;
	default: r = nearbyint(v);
	}
	if (uns) return r <= 0 ? 0 : r >= 4294967295.0 ? 0xffffffffu : (uint32_t)r;
	return r <= -2147483648.0 ? 0x80000000u : r >= 2147483647.0 ? 0x7fffffffu : (uint32_t)(int32_t)r;
}

/* runs until ra's stop; 0, or 1 when the budget ran out */
static int run(void)
{
	while (pc != STOP) {
		uint32_t i = rd32(pc), op = i & 0x7f, rd = i >> 7 & 31, f3 = i >> 12 & 7, rs1 = i >> 15 & 31, rs2 = i >> 20 & 31, f7 = i >> 25;
		uint32_t a = x[rs1], b = x[rs2], next = pc + 4;
		int32_t imm_i = sx(i >> 20, 12), imm_s = sx((i >> 25) << 5 | (i >> 7 & 31), 12);
		int32_t imm_b = sx((i >> 31) << 12 | (i >> 7 & 1) << 11 | (i >> 25 & 0x3f) << 5 | (i >> 8 & 0xf) << 1, 13);
		int32_t imm_j = sx((i >> 31) << 20 | (i >> 12 & 0xff) << 12 | (i >> 20 & 1) << 11 | (i >> 21 & 0x3ff) << 1, 21);

		if (budget >= 0 && used >= budget) return 1;
		used++, steps_run++;
		switch (op) {
		case 0x37: x[rd] = i & 0xfffff000u; break;
		case 0x17: x[rd] = pc + (i & 0xfffff000u); break;
		case 0x6f: x[rd] = pc + 4; next = pc + imm_j; break;
		case 0x67: { uint32_t t = (a + imm_i) & ~1u; x[rd] = pc + 4; next = t; break; }
		case 0x63: {
			int take;

			switch (f3) {
			case 0: take = a == b; break;
			case 1: take = a != b; break;
			case 4: take = (int32_t)a < (int32_t)b; break;
			case 5: take = (int32_t)a >= (int32_t)b; break;
			case 6: take = a < b; break;
			case 7: take = a >= b; break;
			default: fprintf(stderr, "bad branch\n"); exit(2);
			}
			if (take) next = pc + imm_b;
			break;
		}
		case 0x03: if (f3 != 2) goto bad; x[rd] = rd32(a + imm_i); break;
		case 0x23: if (f3 != 2) goto bad; wr32(a + imm_s, b); break;
		case 0x07: if (f3 != 2) goto bad; f[rd] = rd32(a + imm_i); break;
		case 0x27: if (f3 != 2) goto bad; wr32(a + imm_s, f[rs2]); break;
		case 0x13: {
			uint32_t sh = rs2;

			switch (f3) {
			case 0: x[rd] = a + imm_i; break;
			case 1: x[rd] = a << sh; break;
			case 2: x[rd] = (int32_t)a < imm_i; break;
			case 3: x[rd] = a < (uint32_t)imm_i; break;
			case 4: x[rd] = a ^ imm_i; break;
			case 5: x[rd] = (i >> 30 & 1) ? (uint32_t)((int32_t)a >> sh) : a >> sh; break;
			case 6: x[rd] = a | imm_i; break;
			case 7: x[rd] = a & imm_i; break;
			}
			break;
		}
		case 0x33:
			if (f7 == 1) {
				switch (f3) {
				case 0: x[rd] = a * b; break;
				case 4: x[rd] = b == 0 ? 0xffffffffu : ((int32_t)a == INT32_MIN && (int32_t)b == -1) ? a : (uint32_t)((int32_t)a / (int32_t)b); break;
				case 5: x[rd] = b == 0 ? 0xffffffffu : a / b; break;
				case 6: x[rd] = b == 0 ? a : ((int32_t)a == INT32_MIN && (int32_t)b == -1) ? 0 : (uint32_t)((int32_t)a % (int32_t)b); break;
				case 7: x[rd] = b == 0 ? a : a % b; break;
				default: goto bad;
				}
				break;
			}
			switch (f3) {
			case 0: x[rd] = f7 == 0x20 ? a - b : a + b; break;
			case 1: x[rd] = a << (b & 31); break;
			case 2: x[rd] = (int32_t)a < (int32_t)b; break;
			case 3: x[rd] = a < b; break;
			case 4: x[rd] = a ^ b; break;
			case 5: x[rd] = f7 == 0x20 ? (uint32_t)((int32_t)a >> (b & 31)) : a >> (b & 31); break;
			case 6: x[rd] = a | b; break;
			case 7: x[rd] = a & b; break;
			}
			break;
		case 0x73:
			if (f3 != 2 || rs1 != 0) goto bad;
			if ((i >> 20) == 0x0ba) x[rd] = budget < 0 ? 0xffffffffu : (uint32_t)(budget - used + 1);
			else goto bad;
			break;
		case 0x53: {
			float p = F(rs1), q = F(rs2);

			switch (f7) {
			case 0x00: SF(rd, p + q); break;
			case 0x04: SF(rd, p - q); break;
			case 0x08: SF(rd, p * q); break;
			case 0x0c: SF(rd, p / q); break;
			case 0x2c: SF(rd, sqrtf(p)); break;
			case 0x10: {
				uint32_t s = f3 == 0 ? f[rs2] & 0x80000000u : f3 == 1 ? ~f[rs2] & 0x80000000u : (f[rs1] ^ f[rs2]) & 0x80000000u;

				f[rd] = (f[rs1] & 0x7fffffffu) | s;
				break;
			}
			case 0x14: SF(rd, f3 == 0 ? (p != p ? q : q != q ? p : p < q ? p : q) : (p != p ? q : q != q ? p : p > q ? p : q)); break;
			case 0x50: x[rd] = f3 == 2 ? p == q : f3 == 1 ? p < q : p <= q; break;
			case 0x60: x[rd] = cvt_w(p, f3 == 7 ? 0 : (int)f3, rs2 == 1); break;
			case 0x68: SF(rd, rs2 == 1 ? (float)a : (float)(int32_t)a); break;
			case 0x70: if (f3 != 0) goto bad; x[rd] = f[rs1]; break;
			case 0x78: f[rd] = a; break;
			default: goto bad;
			}
			break;
		}
		default:
		bad:
			fprintf(stderr, "instruction %08x at %08x not known\n", i, pc);
			exit(2);
		}
		x[0] = 0;
		pc = next;
	}
	return 0;
}

static uint32_t callee_saved[32], sp_before;

/* a step: its return value, or -1 when the budget ran out */
static int step(uint32_t index, uint32_t round, long give)
{
	int r, k;

	for (k = 0; k < 32; k++) x[k] = 0xdead0000u + k, f[k] = 0x7fc00000u + k;
	x[0] = 0, x[1] = STOP, x[2] = STACK_AT, x[10] = index, x[11] = CTX_AT, x[12] = round;
	memcpy(callee_saved, x, sizeof x);
	sp_before = x[2];
	pc = CODE_AT, budget = give, used = 0;
	r = run();
	if (r) return -1;
	for (k = 8; k < 28; k++)
		if ((k == 8 || k == 9 || k >= 18) && x[k] != callee_saved[k]) { fprintf(stderr, "x%d not kept\n", k); exit(2); }
	if (x[2] != sp_before) { fprintf(stderr, "sp not kept\n"); exit(2); }
	return (int)x[10];
}


static uint32_t texels;

static void load(hlsl_program *p, uint32_t width_in_indices)
{
	uint32_t bytes;
	const uint8_t *code = hlsl_code(p, &bytes);
	uint32_t *ctx = hlsl_ctx(p);

	memcpy(mem + CODE_AT, code, bytes);
	texels = hlsl_stats(p).texels;
	ctx[0] = OUT_AT, ctx[1] = 0, ctx[2] = width_in_indices, ctx[3] = 16 * texels;
	memcpy(mem + CTX_AT, ctx, 16 + 1024);
}

static void sync_ctx(hlsl_program *p) { memcpy(mem + CTX_AT + 16, (uint8_t *)hlsl_ctx(p) + 16, 1024); }

/* index made in one call, with no end */
static void make_whole(uint32_t index)
{
	if (step(index, 0, -1) != 0) { fprintf(stderr, "a whole call not through\n"); exit(2); }
}

static unsigned long stats_rounds, stats_cut, stats_too_long, stats_steps;

/* index made as the lanes make it: steps with what a pass has left, rounds until through */
static void make_steps(uint32_t index, unsigned *seed)
{
	uint32_t round = 0, at = OUT_AT + 16 * texels * index;
	uint8_t keep[96];
	int r;

	for (;;) {
		long give;

		*seed = *seed * 1103515245u + 12345u;
		give = 64 + (*seed >> 8) % 8192;	/* a pass's leftover, or a whole one */
		if ((*seed >> 4 & 3) == 0) give = 8192;
		memcpy(keep, mem + at, 16 * texels);
		r = step(index, round, give);
		stats_steps++;
		if (r < 0) {
			/* cut: what it wrote is lost, and it goes again with a whole pass */
			stats_cut++;
			memcpy(mem + at, keep, 16 * texels);
			r = step(index, round, 8192);
			if (r < 0) {
				/* longer than a pass: made by a core, with no end */
				stats_too_long++;
				memcpy(mem + at, keep, 16 * texels);
				r = step(index, round, -1);
				while (r == 1) r = step(index, ++round, -1);
			}
		}
		if (r == 0) break;
		round++;
		stats_rounds++;
		if (round > 100000) { fprintf(stderr, "index %u never through\n", index); exit(2); }
	}
}

/* ---- tests ---- */

static int failures;

static void expect_close(const char *what, float got, float want, float tol)
{
	float err = fabsf(got - want), scale = fabsf(want) > 1 ? fabsf(want) : 1;

	if (!(err <= tol * scale) && !(got != got && want != want)) {
		printf("FAIL %s: %.7g, want %.7g\n", what, got, want);
		failures++;
	}
}

static hlsl_program *compile(const char *src, int flags)
{
	char err[256];
	hlsl_program *p = hlsl_compile_ex(src, "main", flags, err, sizeof err);

	if (!p) {
		printf("FAIL compile: %s\n--\n%s\n", err, src);
		failures++;
	}
	return p;
}

/* one expression's four values at the pixel (0, 0) against C */
static void expr4(const char *expr, float a, float b, float c, float d, float tol)
{
	char src[2048];
	hlsl_program *p;
	float got[4];
	int k;

	snprintf(src, sizeof src, "float4 main(float4 pos : SV_Position) : SV_Target {\n return %s;\n}\n", expr);
	if (!(p = compile(src, HLSL_RAW))) return;
	load(p, 1);
	memset(mem + OUT_AT, 0xcc, 64);
	make_whole(0);
	memcpy(got, mem + OUT_AT, 16);
	for (k = 0; k < 4; k++) {
		char what[300];
		float want[4];

		want[0] = a, want[1] = b, want[2] = c, want[3] = d;
		snprintf(what, sizeof what, "%s [%d]", expr, k);
		expect_close(what, got[k], want[k], tol);
	}
	hlsl_free(p);
}

static void program4(const char *src, float a, float b, float c, float d, float tol)
{
	hlsl_program *p;
	float got[4], want[4] = { a, b, c, d };
	int k;
	unsigned seed = 7;

	if (!(p = compile(src, HLSL_RAW))) return;
	load(p, 1);
	memset(mem + OUT_AT, 0xcc, 64);
	make_whole(0);
	memcpy(got, mem + OUT_AT, 16);
	for (k = 0; k < 4; k++) {
		char what[64];

		snprintf(what, sizeof what, "program %d", k);
		if (fabsf(got[k] - want[k]) > tol * (fabsf(want[k]) > 1 ? fabsf(want[k]) : 1)) {
			printf("FAIL %s: %.7g, want %.7g\n--\n%s\n", what, got[k], want[k], src);
			failures++;
		}
	}
	/* and in steps */
	memset(mem + OUT_AT, 0xcc, 64);
	make_steps(0, &seed);
	if (memcmp(got, mem + OUT_AT, 16)) {
		printf("FAIL program in steps differs\n--\n%s\n", src);
		failures++;
	}
	hlsl_free(p);
}

static void compile_error(const char *src, const char *want)
{
	char err[256];
	hlsl_program *p = hlsl_compile_ex(src, "main", HLSL_RAW, err, sizeof err);

	if (p) { printf("FAIL no error for: %s\n", src); failures++; hlsl_free(p); return; }
	if (!strstr(err, want)) { printf("FAIL error '%s', want '%s'\n", err, want); failures++; }
}

static void unit_tests(void)
{
	float v;

	expr4("float4(1, 2, 3, 4)", 1, 2, 3, 4, 0);
	expr4("float4(pos.xy, pos.zw)", 0.5f, 0.5f, 0, 1, 0);
	expr4("float4(1.5 + 2.25, 3 * 4.5, 7 / 2.0, 10 - 20)", 3.75f, 13.5f, 3.5f, -10, 0);
	expr4("float4(sqrt(2.0 + pos.x), abs(-3 + pos.x), min(pos.x, 0.25), max(pos.x, 0.75))", sqrtf(2.5f), 2.5f, 0.25f, 0.75f, 1e-6f);
	for (v = -7.3f; v < 8; v += 1.37f) {
		char e[512];

		snprintf(e, sizeof e, "float4(sin(%.9g + pos.z), cos(%.9g + pos.z), exp(%.9g * 0.3 + pos.z), log(%.9g * %.9g + 0.5 + pos.z))", v, v, v, v, v);
		expr4(e, sinf(v), cosf(v), expf(v * 0.3f), logf(v * v + 0.5f), 2e-5f);
		snprintf(e, sizeof e, "float4(floor(%.9g + pos.z), frac(%.9g + pos.z), atan(%.9g + pos.z), atan2(%.9g + pos.z, -1.3))", v, v, v, v);
		expr4(e, floorf(v), v - floorf(v), atanf(v), atan2f(v, -1.3f), 2e-5f);
		snprintf(e, sizeof e, "float4(pow(abs(%.9g) + 0.1 + pos.z, 1.7), exp2(%.9g + pos.z), log2(abs(%.9g) + 1 + pos.z), tan(%.9g * 0.2 + pos.z))", v, v, v, v);
		expr4(e, powf(fabsf(v) + 0.1f, 1.7f), exp2f(v), log2f(fabsf(v) + 1), tanf(v * 0.2f), 5e-5f);
		snprintf(e, sizeof e, "float4(ceil(%.9g + pos.z), round(%.9g + pos.z), fmod(%.9g + pos.z, 2.5), asin(%.9g / 9 + pos.z))", v, v, v, v);
		expr4(e, ceilf(v), rintf(v), fmodf(v, 2.5f), asinf(v / 9), 5e-5f);
	}
	expr4("float4(dot(float3(1,2,3), float3(4,5,6) + pos.z), length(float2(3,4) + pos.z), distance(float3(1,1,1) + pos.z, float3(2,3,4)), 0)",
	      32, 5, sqrtf(14), 0, 1e-6f);
	expr4("float4(normalize(float3(3, 0, 4) + pos.z), 1)", 0.6f, 0, 0.8f, 1, 1e-6f);
	expr4("float4(cross(float3(1, 0, 0) + pos.z, float3(0, 1, 0)), 7)", 0, 0, 1, 7, 0);
	expr4("float4(lerp(float2(0, 10), float2(10, 20), 0.25 + pos.z), clamp(5 + pos.z, 0, 2), saturate(-1 + pos.z))", 2.5f, 12.5f, 2, 0, 0);
	expr4("float4(smoothstep(0, 1, 0.5 + pos.z), step(0.5, 0.25 + pos.z), sign(-3.0 + pos.z), sign(pos.z))", 0.5f, 0, -1, 0, 0);
	expr4("float4(mul(float3x3(1,2,3, 4,5,6, 7,8,9), float3(1, 0, pos.w)), 0)", 4, 10, 16, 0, 0);
	expr4("float4(mul(float3(1, 0, pos.w), float3x3(1,2,3, 4,5,6, 7,8,9)), 0)", 8, 10, 12, 0, 0);
	expr4("float4(reflect(float3(1, -1, 0) + pos.z, float3(0, 1, 0)), 0)", 1, 1, 0, 0, 0);
	expr4("float4((int)(7.9 + pos.x), (int)(-7.9 + pos.x), 7 / 2, -7 % 3)", 8, -7, 3, -1, 0);
	expr4("float4((uint)(pos.x * 4) << 3, 13 & 6 | 1, 5 ^ 3, ~0)", 16, 5, 6, -1, 0);
	expr4("float4(pos.x > 0.25 ? 1 : 2, pos.x < 0.25 ? 1 : 2, (pos.x > 0 && pos.y > 1) ? 3 : 4, (pos.x > 1 || pos.y > 0) ? 5 : 6)", 1, 2, 4, 5, 0);
	expr4("float4(float2(1, 2).yx, float3(4, 5, 6).zz)", 2, 1, 6, 6, 0);
	expr4("float4(any(float3(0, pos.x, 0)), all(float2(1, pos.x)), any(float2(0, pos.z)), (float)(pos.x == 0.5))", 1, 1, 0, 1, 0);
	expr4("float4(max(3, (int)pos.y + 7), min(-2, (int)pos.y), abs(-5 + (int)pos.y), 0)", 7, -2, 5, 0, 0);

	program4("float sq(float x) { return x * x; }\n"
		 "void twice(inout float a, out float b) { a = a * 2; b = a + 1; }\n"
		 "float4 main(float4 pos : SV_Position) : SV_Target {\n"
		 " float a = 3 + pos.x, b;\n twice(a, b);\n"
		 " float s = 0;\n for (int i = 0; i < 5; i++) s += sq(i);\n"
		 " return float4(a, b, s, sq(pos.x));\n}\n", 7, 8, 30, 0.25f, 0);
	{
		/* the C values for the program above */
		float s = 0, t = 1;
		int n = 0, i, k = 0;
		char src[1024];

		for (i = 0; i < 1000; i++) { if (i % 3 == 0) continue; s += sqrtf((float)i); n++; if (s > 5000) break; }
		while (t < 1e6f) { t *= 1.01f; k++; }
		snprintf(src, sizeof src, "float4 main(float4 pos : SV_Position) : SV_Target {\n"
			 " float s = 0; int n = 0;\n"
			 " [loop] for (int i = 0; i < 1000; i++) { if (i %% 3 == 0) continue; s += sqrt((float)i); n++; if (s > 5000) break; }\n"
			 " float t = 1; int k = 0;\n while (t < 1e6) { t *= 1.01 + pos.z; k++; }\n"
			 " return float4(s, n, k, t);\n}\n");
		program4(src, s, (float)n, (float)k, t, 1e-5f);
	}
	program4("struct Ray { float3 o; float3 d; };\n"
		 "static const float3 pts[4] = { float3(1,0,0), float3(0,2,0), float3(0,0,3), float3(4,4,4) };\n"
		 "uniform float unused;\n"
		 "float3 at(Ray r, float t) { return r.o + r.d * t; }\n"
		 "float4 main(float4 pos : SV_Position) : SV_Target {\n"
		 " Ray r; r.o = float3(1, 2, 3); r.d = float3(0, 0, 1);\n"
		 " float3 p = at(r, 2.0);\n"
		 " int k = (int)(pos.x * 6);\n"
		 " float3 q = pts[k];\n"
		 " float a[3] = { 1, 2, 3 }; a[k - 1] = 9;\n"
		 " float3x3 m = float3x3(1,0,0, 0,1,0, 0,0,1); m[1] = float3(0, 2, 0);\n"
		 " return float4(p.z, q.z, a[1] + a[2], mul(m, float3(1,1,1)).y);\n}\n", 5, 4, 11, 2, 0);
	compile_error("float4 main() : SV_Target { return x; }", "no variable x");
	compile_error("float f(float a) { return f(a); }\nfloat4 main() : SV_Target { return f(1); }", "recursion");
	compile_error("float4 main() : SV_Target { float3 a = float3(1,2); return 0; }", "needs 3 values");
}

/* a heavy shader: every pixel in steps with random budgets equals the same in one call */
static void steps_test(void)
{
	static const char *src =
		"uniform float k;\n"
		"float4 main(float4 pos : SV_Position) : SV_Target {\n"
		" float3 p = float3(pos.xy * 0.01, 0); float d = 0; int i;\n"
		" for (i = 0; i < 40 + (int)pos.x % 37 * 9; i++) {\n"
		"  p = abs(p) - 0.3; p.xy = float2(p.x * 0.8 - p.y * 0.6, p.x * 0.6 + p.y * 0.8);\n"
		"  for (int j = 0; j < 3; j++) d += sin(p.x * k + j) * cos(p.y + d);\n"
		" }\n"
		" return float4(d, p.x, p.y, i);\n}\n";
	hlsl_program *p = hlsl_compile_ex(src, "main", 0, 0, 0);
	uint32_t w = 32, h = 16, n = w * h / 4, i;
	uint8_t *whole;
	unsigned seed = 1;
	float k = 1.7f;

	if (!p) { printf("FAIL steps shader\n"); failures++; return; }
	hlsl_set(p, "k", &k, 1);
	load(p, w / 4);
	sync_ctx(p);
	whole = malloc(16 * texels * n);
	memset(mem + OUT_AT, 0, 16 * texels * n);
	for (i = 0; i < n; i++) make_whole(i);
	for (i = 0; i < n; i++) memcpy(whole + 16 * texels * i, mem + OUT_AT + 16 * texels * i, 16);
	memset(mem + OUT_AT, 0x55, 16 * texels * n);
	stats_rounds = stats_cut = stats_too_long = stats_steps = 0;
	for (i = 0; i < n; i++) make_steps(i, &seed);
	for (i = 0; i < n; i++)
		if (memcmp(whole + 16 * texels * i, mem + OUT_AT + 16 * texels * i, 16)) {
			printf("FAIL steps: index %u differs\n", i);
			failures++;
			break;
		}
	printf("steps: %lu steps, %lu rounds, %lu cut, %lu longer than a pass; %u texels an index, longest step %u\n", stats_steps,
	       stats_rounds, stats_cut, stats_too_long, texels, hlsl_stats(p).longest_step);
	if (stats_too_long) { printf("FAIL steps longer than a pass\n"); failures++; }
	free(whole);
	hlsl_free(p);
}

static char *read_file(const char *path)
{
	FILE *fp = fopen(path, "rb");
	long n;
	char *s;

	if (!fp) { perror(path); exit(1); }
	fseek(fp, 0, SEEK_END);
	n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	s = malloc(n + 1);
	if (fread(s, 1, n, fp) != (size_t)n) exit(1);
	s[n] = 0;
	fclose(fp);
	return s;
}

/* a shader file drawn to a PPM, in steps as the lanes make it, against the same in whole calls */
static int render(int argc, char **argv)
{
	char err[512];
	char *src = read_file(argv[1]);
	int w = atoi(argv[2]), h = atoi(argv[3]), a;
	uint32_t n = (uint32_t)(w * h / 4), i;
	hlsl_program *p = hlsl_compile(src, "main", err, sizeof err);
	FILE *fp;
	unsigned seed = 3;
	uint64_t whole_instr, t0;
	struct hlsl_stats st;
	uint32_t *ref;

	if (!p) { printf("%s: %s\n", argv[1], err); return 1; }
	st = hlsl_stats(p);
	printf("%s: %u bytes of code, %u IR, %u yields, %u texels an index, longest step %u, %u spilled\n", argv[1], st.code_bytes,
	       st.instructions, st.yields, st.texels, st.longest_step, st.spilled);
	for (a = 5; a < argc; a++) {
		char name[64];
		float v[16];
		int c = 0;
		const char *s = strchr(argv[a], '=');

		if (!s) continue;
		snprintf(name, sizeof name, "%.*s", (int)(s - argv[a]), argv[a]);
		for (s++; *s && c < 16; c++) {
			v[c] = strtof(s, (char **)&s);
			if (*s == ',') s++;
		}
		if (!hlsl_set(p, name, v, c)) printf("no uniform %s\n", name);
	}
	load(p, (uint32_t)w / 4);
	sync_ctx(p);
	t0 = steps_run;
	for (i = 0; i < n; i++) make_whole(i);
	whole_instr = steps_run - t0;
	ref = malloc(4 * n * 4);
	for (i = 0; i < n; i++) memcpy(ref + 4 * i, mem + OUT_AT + 16 * texels * i, 16);
	memset(mem + OUT_AT, 0x55, 16 * texels * n);
	stats_rounds = stats_cut = stats_too_long = stats_steps = 0;
	t0 = steps_run;
	for (i = 0; i < n; i++) make_steps(i, &seed);
	printf("whole: %.1f instructions a pixel; steps: %.1f a pixel, %lu steps, %lu rounds, %lu cut, %lu longer than a pass\n",
	       (double)whole_instr / (4.0 * n), (double)(steps_run - t0) / (4.0 * n), stats_steps, stats_rounds, stats_cut, stats_too_long);
	for (i = 0; i < n; i++)
		if (memcmp(ref + 4 * i, mem + OUT_AT + 16 * texels * i, 16)) { printf("FAIL: index %u differs in steps\n", i); return 1; }
	{
		uint32_t sum = 2166136261u;

		for (i = 0; i < (uint32_t)(w * h); i++) sum = (sum ^ ref[i]) * 16777619u;
		printf("sum %08x\n", sum);
	}
	fp = fopen(argv[4], "wb");
	fprintf(fp, "P6\n%d %d\n255\n", w, h);
	for (i = 0; i < (uint32_t)(w * h); i++) {
		uint32_t px = ref[i];

		fputc(px >> 16 & 255, fp), fputc(px >> 8 & 255, fp), fputc(px & 255, fp);
	}
	fclose(fp);
	printf("wrote %s\n", argv[4]);
	return 0;
}

int main(int argc, char **argv)
{
	mem = calloc(1, MEM);
	if (argc >= 5) return render(argc, argv);
	unit_tests();
	steps_test();
	printf(failures ? "hlsl_test: %d FAILED\n" : "hlsl_test: PASS\n", failures);
	return failures != 0;
}
