/* rt/divdi3.c against the host's 64-bit division: cc -O2 -Dmain_test test_rt.c divdi3.c (with its functions renamed)
   ... run on the host: every case of random, small, power-of-two-ish and edge operands. */
#include <stdio.h>
#include <stdint.h>
#define __udivdi3 t_udivdi3
#define __umoddi3 t_umoddi3
#define __divdi3 t_divdi3
#define __moddi3 t_moddi3
#define __udivmoddi4 t_udivmoddi4
#define __divmoddi4 t_divmoddi4
#include "divdi3.c"
static uint64_t st = 88172645463325252ULL;
static uint64_t rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
static uint64_t shape(void)
{
	uint64_t x = rnd();
	switch (rnd() % 8) {
	case 0: return x >> (rnd() % 64);
	case 1: return x & 0xffffffff;
	case 2: return x & 0xffff;
	case 3: return (uint64_t)1 << (rnd() % 64);
	case 4: return ((uint64_t)1 << (rnd() % 64)) - 1;
	case 5: return ~(x >> (rnd() % 64));
	case 6: return rnd() % 4;
	default: return x;
	}
}
int main(void)
{
	long bad = 0, n = 0;
	for (long i = 0; i < 50000000; i++) {
		uint64_t a = shape(), b = shape();
		if (!b) continue;
		n++;
		unsigned long long r; uint64_t q = t_udivmoddi4(a, b, &r);
		if (q != a / b || r != a % b || t_udivdi3(a, b) != a / b || t_umoddi3(a, b) != a % b) {
			if (bad++ < 10) printf("u %llx / %llx: %llx %llx\n", (unsigned long long)a, (unsigned long long)b, (unsigned long long)q, (unsigned long long)r);
		}
		int64_t sa = (int64_t)a, sb = (int64_t)b;
		if (sb == -1 && sa == INT64_MIN) continue;
		long long sr; int64_t sq = t_divmoddi4(sa, sb, &sr);
		if (sq != sa / sb || sr != sa % sb || t_divdi3(sa, sb) != sa / sb || t_moddi3(sa, sb) != sa % sb) {
			if (bad++ < 10) printf("s %llx / %llx\n", (unsigned long long)a, (unsigned long long)b);
		}
	}
	printf("%ld cases, %ld wrong: %s\n", n, bad, bad ? "FAIL" : "PASS");
	return bad != 0;
}
