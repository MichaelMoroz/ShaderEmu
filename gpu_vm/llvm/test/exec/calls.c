// Calls: many arguments (registers and the stack), 64-bit and double
// arguments split across the two, structs by value and returned, varargs,
// recursion, function pointers.
#include "gvm.h"
#include <stdarg.h>

#define NI __attribute__((noinline))

NI int many(int a, int b, int c, int d, int e, int f, int g, int h, int i,
            int j, int k) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i +
         10 * j + 11 * k;
}

NI long long mixed(int a, long long b, int c, long long d, int e, long long f,
                   double g, int h) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + (long long)g + h;
}

NI double dargs(double a, float b, double c, int d, double e, double f) {
  return a * 2 + b + c * 3 + d + e - f;
}

NI char chars(char a, unsigned char b, short c, unsigned short d) {
  return a + b + c + d;
}

struct small {
  char c;
  short s;
};
struct pair {
  int a, b;
};
struct big {
  int v[9];
  double d;
};

NI struct small mksmall(int x) {
  struct small s = {(char)x, (short)(x * 3)};
  return s;
}
NI struct pair swap(struct pair p) {
  struct pair r = {p.b, p.a};
  return r;
}
NI struct big mkbig(struct big b, int k) {
  for (int i = 0; i < 9; i++)
    b.v[i] += k;
  b.d *= 2;
  return b;
}
NI int sumbig(struct big b) {
  int s = 0;
  for (int i = 0; i < 9; i++)
    s += b.v[i];
  b.v[0] = 999; // the callee's copy
  return s + (int)b.d;
}

NI int vsum(int n, ...) {
  va_list ap;
  va_start(ap, n);
  int s = 0;
  for (int i = 0; i < n; i++)
    s += va_arg(ap, int);
  va_end(ap);
  return s;
}

NI double vmix(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  double s = 0;
  for (; *fmt; fmt++) {
    switch (*fmt) {
    case 'i': s += va_arg(ap, int); break;
    case 'l': s += (double)va_arg(ap, long long); break;
    case 'd': s += va_arg(ap, double); break;
    case 'p': s += va_arg(ap, struct pair).a; break;
    case 'b': s += va_arg(ap, struct big).v[8]; break;
    }
  }
  va_end(ap);
  return s;
}

NI static int vforward(const char *fmt, va_list ap) {
  char buf[64];
  int n = 0;
  for (; *fmt; fmt++)
    n += va_arg(ap, int) * (*fmt - '0');
  sprintf(buf, "%d", n);
  return atoi(buf);
}
NI static int vcall(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vforward(fmt, ap);
  va_end(ap);
  return r;
}

NI int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
NI int ack(int m, int n) {
  if (m == 0)
    return n + 1;
  if (n == 0)
    return ack(m - 1, 1);
  return ack(m - 1, ack(m, n - 1));
}

static int twice(int x) { return 2 * x; }
static int thrice(int x) { return 3 * x; }
NI int apply(int (*f)(int), int x) { return f(x); }
static int (*volatile table[])(int) = {twice, thrice};

int main(void) {
  CHECK_EQ(many(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11), 506);
  CHECK_EQ(mixed(1, 2, 3, 0x100000000ll, 5, -7, 8.5, 9),
           1 + 4 + 9 + 0x400000000ll + 25 - 42 + 8 + 9);
  CHECK(dargs(1.5, 2.25f, 3.0, 4, 5.5, 0.25) == 3.0 + 2.25 + 9.0 + 4 + 5.5 - 0.25);
  CHECK_EQ(chars(-1, 255, -300, 65535), (char)(-1 + 255 - 300 + 65535));
  struct small s = mksmall(5);
  CHECK_EQ(s.c, 5);
  CHECK_EQ(s.s, 15);
  struct pair p = swap((struct pair){3, 4});
  CHECK_EQ(p.a, 4);
  CHECK_EQ(p.b, 3);
  struct big b;
  for (int i = 0; i < 9; i++)
    b.v[i] = i;
  b.d = 1.25;
  struct big c = mkbig(b, 10);
  CHECK_EQ(b.v[3], 3);
  CHECK_EQ(c.v[3], 13);
  CHECK(c.d == 2.5);
  CHECK_EQ(sumbig(b), 36 + 1);
  CHECK_EQ(b.v[0], 0);
  CHECK_EQ(vsum(5, 1, 2, 3, 4, 5), 15);
  CHECK_EQ(vsum(12, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12), 78);
  CHECK(vmix("ildipb", 1, 2ll, 3.5, 4, (struct pair){5, 6}, c) == 1 + 2 + 3.5 + 4 + 5 + 18);
  CHECK_EQ(vcall("123", 10, 20, 30), 10 + 40 + 90);
  CHECK_EQ(fib(20), 6765);
  CHECK_EQ(ack(2, 3), 9);
  CHECK_EQ(apply(twice, 21), 42);
  CHECK_EQ(table[1](5), 15);
  CHECK_EQ(table[0](table[1](2)), 12);
  puts("calls ok");
  return 0;
}
