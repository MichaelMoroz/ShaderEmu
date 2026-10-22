// Loops: counted, nested, while with breaks, sorting, strings, a sieve.
#include "gvm.h"

#define NI __attribute__((noinline))

NI static void bubble(int *a, int n) {
  for (int i = 0; i < n; i++)
    for (int j = 0; j + 1 < n - i; j++)
      if (a[j] > a[j + 1]) {
        int t = a[j];
        a[j] = a[j + 1];
        a[j + 1] = t;
      }
}

NI static void quick(int *a, int lo, int hi) {
  if (lo >= hi)
    return;
  int p = a[(lo + hi) / 2], i = lo, j = hi;
  while (i <= j) {
    while (a[i] < p)
      i++;
    while (a[j] > p)
      j--;
    if (i <= j) {
      int t = a[i];
      a[i] = a[j];
      a[j] = t;
      i++, j--;
    }
  }
  quick(a, lo, j);
  quick(a, i, hi);
}

NI static int sieve(int n) {
  static char comp[10001];
  int count = 0;
  for (int i = 2; i <= n; i++) {
    if (comp[i])
      continue;
    count++;
    for (int j = i * 2; j <= n; j += i)
      comp[j] = 1;
  }
  return count;
}

NI static unsigned crc32(const unsigned char *p, int n) {
  unsigned c = 0xffffffffu;
  for (int i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++)
      c = (c >> 1) ^ (0xedb88320u & -(c & 1));
  }
  return ~c;
}

NI static int collatz(unsigned n) {
  int steps = 0;
  while (n != 1) {
    n = (n & 1) ? 3 * n + 1 : n / 2;
    steps++;
  }
  return steps;
}

NI static void reverse(char *s) {
  int n = strlen(s);
  for (int i = 0, j = n - 1; i < j; i++, j--) {
    char t = s[i];
    s[i] = s[j];
    s[j] = t;
  }
}

NI static long long matsum(int n) {
  static int m[16][16];
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
      m[i][j] = i * j - i + j;
  long long s = 0;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
      for (int k = 0; k < n; k++)
        s += (long long)m[i][k] * m[k][j];
  return s;
}

int main(void) {
  int a[100], b[100];
  unsigned h = 42;
  for (int i = 0; i < 100; i++) {
    h = h * 1664525u + 1013904223u;
    a[i] = b[i] = (int)(h >> 8) - (1 << 23);
  }
  bubble(a, 100);
  quick(b, 0, 99);
  for (int i = 0; i < 100; i++) {
    CHECK_EQ(a[i], b[i]);
    if (i)
      CHECK(a[i - 1] <= a[i]);
  }
  CHECK_EQ(sieve(10000), 1229);
  CHECK_EQ(crc32((const unsigned char *)"123456789", 9), 0xcbf43926u);
  CHECK_EQ(collatz(27), 111);
  char s[] = "hello, gpu_vm";
  reverse(s);
  CHECK(strcmp(s, "mv_upg ,olleh") == 0);
  long long ref = 0;
  for (int i = 0; i < 16; i++)
    for (int j = 0; j < 16; j++)
      for (int k = 0; k < 16; k++)
        ref += (long long)(i * k - i + k) * (k * j - k + j);
  CHECK_EQ(matsum(16), ref);
  int s1 = 0, s2 = 0;
  for (int i = 0; i < 1000; i++) {
    if (i % 7 == 3)
      continue;
    if (i == 900)
      break;
    s1 += i;
  }
  int i = 0;
  do {
    if (i % 7 != 3)
      s2 += i;
  } while (++i < 900);
  CHECK_EQ(s1, s2);
  puts("loops ok");
  return 0;
}
