/*
 * The machine's float instructions (docs/fpu.md) against the compiler runtime's integer
 * routines for the same operations, on random operands and on operands near each other.
 *   fptest [PAIRS]        (200,000 by default; "fptest: done" at the end)
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* the soft-float routines, as the linker knows them: floats travel as their bits */
uint32_t __addsf3 (uint32_t, uint32_t);
uint32_t __subsf3 (uint32_t, uint32_t);
uint32_t __mulsf3 (uint32_t, uint32_t);
uint32_t __divsf3 (uint32_t, uint32_t);
int __ltsf2 (uint32_t, uint32_t);
int __lesf2 (uint32_t, uint32_t);
int __eqsf2 (uint32_t, uint32_t);
int __unordsf2 (uint32_t, uint32_t);
int32_t __fixsfsi (uint32_t);
uint32_t __fixunssfsi (uint32_t);
uint32_t __floatsisf (int32_t);
uint32_t __floatunsisf (uint32_t);

static uint32_t bits (float f) { union { float f; uint32_t u; } c = {f}; return c.u; }
static float number (uint32_t u) { union { uint32_t u; float f; } c = {u}; return c.f; }

#define NOINLINE __attribute__((noinline))
static NOINLINE uint32_t hw_add (uint32_t a, uint32_t b) { return bits (number (a) + number (b)); }
static NOINLINE uint32_t hw_sub (uint32_t a, uint32_t b) { return bits (number (a) - number (b)); }
static NOINLINE uint32_t hw_mul (uint32_t a, uint32_t b) { return bits (number (a) * number (b)); }
static NOINLINE uint32_t hw_div (uint32_t a, uint32_t b) { return bits (number (a) / number (b)); }
static NOINLINE uint32_t hw_sqrt (uint32_t a) { return bits (__builtin_sqrtf (number (a))); }
static NOINLINE uint32_t hw_neg (uint32_t a) { return bits (-number (a)); }
static NOINLINE uint32_t hw_abs (uint32_t a) { return bits (__builtin_fabsf (number (a))); }
static NOINLINE int hw_lt (uint32_t a, uint32_t b) { return number (a) < number (b); }
static NOINLINE int hw_le (uint32_t a, uint32_t b) { return number (a) <= number (b); }
static NOINLINE int hw_eq (uint32_t a, uint32_t b) { return number (a) == number (b); }
static NOINLINE int32_t hw_to_int (uint32_t a) { return (int32_t)number (a); }
static NOINLINE uint32_t hw_to_uint (uint32_t a) { return (uint32_t)number (a); }
static NOINLINE uint32_t hw_from_int (int32_t i) { return bits ((float)i); }
static NOINLINE uint32_t hw_from_uint (uint32_t i) { return bits ((float)i); }
static NOINLINE uint32_t hw_floor (uint32_t a) { return (uint32_t)(int32_t)__builtin_floorf (number (a)); }

/* the C library's square root, called so that the compiler cannot make it the instruction */
static float (*volatile library_sqrt) (float) = sqrtf;

static uint64_t seed = 88172645463325252ull;

static uint32_t random32 (void)
{
	seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
	return (uint32_t)(seed >> 16);
}

/* an operand: any bits, one with an exponent near the other's, almost the other, or a round number */
static uint32_t operand (uint32_t other, int kind)
{
	uint32_t r = random32 ();

	if (kind == 0)
		return r;
	if (kind == 1)
		return (r & 0x807fffffu) | (((other >> 23 & 255) + r % 5 - 2) & 255) << 23;
	if (kind == 2)
		return (other ^ (r & 0x80000007u)) + (r >> 8 & 3) - 1;
	return (r & 0x80000000u) | (100 + r % 60) << 23 | (r >> 8 & 0x7fffff & -(int)(r >> 4 & 1));
}

static int is_nan (uint32_t u) { return (u & 0x7fffffffu) > 0x7f800000u; }
static int denormal (uint32_t u) { return (u & 0x7f800000u) == 0 && (u & 0x7fffffu) != 0; }

enum { ADD, SUB, MUL, DIV, SQRT, NEG, ABS, LT, LE, EQ, TO_INT, TO_UINT, FROM_INT, FROM_UINT, FLOOR, KINDS };
static const char *names[KINDS] = {"fadd", "fsub", "fmul", "fdiv", "fsqrt", "fneg", "fabs", "flt", "fle", "feq",
	"fcvt.w.s", "fcvt.wu.s", "fcvt.s.w", "fcvt.s.wu", "floor"};
static long wrong[KINDS], last_place[KINDS], tiny[KINDS], tried[KINDS];

/* docs/fpu.md allows two things: a denormal taken for zero, and a result one or two off in its last place */
static void check (int kind, uint32_t a, uint32_t b, uint32_t got, uint32_t want)
{
	tried[kind]++;
	if (got == want || (is_nan (got) && is_nan (want)))
		return;
	if (denormal (want) || denormal (a) || denormal (b)) {
		tiny[kind]++;
		return;
	}
	if (got - want + 2 <= 4) {
		last_place[kind]++;
		return;
	}
	if (!wrong[kind]++)
		printf ("fptest: %s %08x %08x gave %08x, not %08x\n", names[kind], (unsigned)a, (unsigned)b, (unsigned)got, (unsigned)want);
}

int main (int argc, char **argv)
{
	long i, n = argc > 1 ? atol (argv[1]) : 200000;
	int k, bad = 0;

	for (i = 0; i < n; i++) {
		uint32_t a = operand (0, i & 1 ? 3 : 0), b = operand (a, (int)(i >> 1 & 3));
		int ordered = !__unordsf2 (a, b);
		float x = number (a);

		check (ADD, a, b, hw_add (a, b), __addsf3 (a, b));
		check (SUB, a, b, hw_sub (a, b), __subsf3 (a, b));
		check (MUL, a, b, hw_mul (a, b), __mulsf3 (a, b));
		check (DIV, a, b, hw_div (a, b), __divsf3 (a, b));
		check (SQRT, a, 0, hw_sqrt (a), bits (library_sqrt (x)));
		check (NEG, a, 0, hw_neg (a), a ^ 0x80000000u);
		check (ABS, a, 0, hw_abs (a), a & 0x7fffffffu);
		check (LT, a, b, hw_lt (a, b), ordered && __ltsf2 (a, b) < 0);
		check (LE, a, b, hw_le (a, b), ordered && __lesf2 (a, b) <= 0);
		check (EQ, a, b, hw_eq (a, b), ordered && __eqsf2 (a, b) == 0);
		if (!is_nan (a) && x > -2147483000.0f && x < 2147483000.0f) {
			check (TO_INT, a, 0, hw_to_int (a), __fixsfsi (a));
			check (FLOOR, a, 0, hw_floor (a), (uint32_t)(__fixsfsi (a) - (x < 0 && (float)__fixsfsi (a) != x)));
		}
		if (!is_nan (a) && x >= 0 && x < 4294967000.0f)
			check (TO_UINT, a, 0, hw_to_uint (a), __fixunssfsi (a));
		check (FROM_INT, a, 0, hw_from_int ((int32_t)a), __floatsisf ((int32_t)a));
		check (FROM_UINT, a, 0, hw_from_uint (a), __floatunsisf (a));
	}
	for (k = 0; k < KINDS; k++) {
		printf ("fptest: %-10s %ld tried, %ld wrong, %ld off in the last place, %ld with denormals\n", names[k], tried[k],
			wrong[k], last_place[k], tiny[k]);
		bad += wrong[k] != 0;
	}
	printf ("fptest: done, %d of %d instructions wrong\n", bad, KINDS);
	return bad != 0;
}
