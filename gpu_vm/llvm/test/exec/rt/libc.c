// The test runtime (gpu_vm/llvm/test/exec): libc's few functions over gvm_cpu's
// semihosted syscalls (r12 the number, r5-r10 the arguments, r5 the result).
// Built with -ffreestanding -fno-math-errno (no calls to itself from its
// loops). Enough for the exec tests and GCC's c-torture tests; not a libc.
#include "gvm.h"
#include <assert.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>

static long syscall3(long n, long a, long b, long c) {
  register long r12 __asm__("r12") = n;
  register long r5 __asm__("r5") = a;
  register long r6 __asm__("r6") = b;
  register long r7 __asm__("r7") = c;
  __asm__ volatile("syscall" : "+r"(r5) : "r"(r12), "r"(r6), "r"(r7) : "memory");
  return r5;
}

void _exit(int code) {
  syscall3(93, code, 0, 0);
  for (;;)
    ;
}

static void (*atexit_fn[32])(void);
static int natexit;
int atexit(void (*f)(void)) {
  if (natexit == 32)
    return -1;
  atexit_fn[natexit++] = f;
  return 0;
}

void exit(int code) {
  while (natexit > 0)
    atexit_fn[--natexit]();
  _exit(code);
}

void abort(void) {
  write(2, "abort\n", 6);
  _exit(134);
}

void __assert_fail(const char *e, const char *file, int line) {
  fprintf(stderr, "%s:%d: assertion %s failed\n", file, line, e);
  abort();
}

ssize_t write(int fd, const void *buf, size_t n) {
  return syscall3(64, fd, (long)buf, n);
}

ssize_t read(int fd, void *buf, size_t n) {
  return syscall3(63, fd, (long)buf, n);
}

//===-- memory and strings -------------------------------------------------===//

void *memcpy(void *d, const void *s, size_t n) {
  unsigned char *dp = d;
  const unsigned char *sp = s;
  if (n >= 4096) {
    __builtin_gpuvm_copy(d, s, n);
    return d;
  }
  if ((((unsigned)dp | (unsigned)sp) & 3) == 0) {
    for (; n >= 4; n -= 4, dp += 4, sp += 4)
      *(unsigned *)dp = *(const unsigned *)sp;
  }
  while (n--)
    *dp++ = *sp++;
  return d;
}

void *mempcpy(void *d, const void *s, size_t n) {
  return (char *)memcpy(d, s, n) + n;
}

void *memmove(void *d, const void *s, size_t n) {
  unsigned char *dp = d;
  const unsigned char *sp = s;
  if (n >= 4096) {
    __builtin_gpuvm_copy(d, s, n);
    return d;
  }
  if (dp <= sp || dp >= sp + n) {
    while (n--)
      *dp++ = *sp++;
  } else {
    dp += n;
    sp += n;
    while (n--)
      *--dp = *--sp;
  }
  return d;
}

void *memset(void *d, int c, size_t n) {
  unsigned char *dp = d;
  if (n >= 4096) {
    __builtin_gpuvm_fill(d, (unsigned char)c * 0x01010101u, n);
    return d;
  }
  while (n--)
    *dp++ = (unsigned char)c;
  return d;
}

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *p = a, *q = b;
  for (; n; n--, p++, q++)
    if (*p != *q)
      return *p - *q;
  return 0;
}

void *memchr(const void *s, int c, size_t n) {
  const unsigned char *p = s;
  for (; n; n--, p++)
    if (*p == (unsigned char)c)
      return (void *)p;
  return 0;
}

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}

size_t strnlen(const char *s, size_t m) {
  size_t n = 0;
  while (n < m && s[n])
    n++;
  return n;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b)
    a++, b++;
  return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  for (; n; n--, a++, b++) {
    if (*a != *b)
      return (unsigned char)*a - (unsigned char)*b;
    if (!*a)
      return 0;
  }
  return 0;
}

char *strcpy(char *d, const char *s) {
  char *r = d;
  while ((*d++ = *s++))
    ;
  return r;
}

char *stpcpy(char *d, const char *s) {
  while ((*d = *s++))
    d++;
  return d;
}

char *strncpy(char *d, const char *s, size_t n) {
  char *r = d;
  for (; n && *s; n--)
    *d++ = *s++;
  for (; n; n--)
    *d++ = 0;
  return r;
}

char *strcat(char *d, const char *s) {
  strcpy(d + strlen(d), s);
  return d;
}

char *strncat(char *d, const char *s, size_t n) {
  char *e = d + strlen(d);
  for (; n && *s; n--)
    *e++ = *s++;
  *e = 0;
  return d;
}

char *strchr(const char *s, int c) {
  for (;; s++) {
    if (*s == (char)c)
      return (char *)s;
    if (!*s)
      return 0;
  }
}

char *strrchr(const char *s, int c) {
  const char *r = 0;
  for (;; s++) {
    if (*s == (char)c)
      r = s;
    if (!*s)
      return (char *)r;
  }
}

char *strstr(const char *h, const char *n) {
  size_t k = strlen(n);
  for (; *h; h++)
    if (!strncmp(h, n, k))
      return (char *)h;
  return k ? 0 : (char *)h;
}

size_t strspn(const char *s, const char *a) {
  size_t n = 0;
  while (s[n] && strchr(a, s[n]))
    n++;
  return n;
}

size_t strcspn(const char *s, const char *r) {
  size_t n = 0;
  while (s[n] && !strchr(r, s[n]))
    n++;
  return n;
}

char *strpbrk(const char *s, const char *a) {
  s += strcspn(s, a);
  return *s ? (char *)s : 0;
}

char *strdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *d = malloc(n);
  if (d)
    memcpy(d, s, n);
  return d;
}

//===-- ctype ---------------------------------------------------------------===//

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isprint(int c) { return c >= 0x20 && c < 0x7f; }
int iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7f; }
int ispunct(int c) { return isprint(c) && !isalnum(c) && c != ' '; }
int isxdigit(int c) {
  return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

//===-- stdlib --------------------------------------------------------------===//

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
div_t div(int a, int b) {
  div_t r = {a / b, a % b};
  return r;
}
ldiv_t ldiv(long a, long b) {
  ldiv_t r = {a / b, a % b};
  return r;
}

unsigned long long strtoull(const char *s, char **end, int base) {
  while (isspace(*s))
    s++;
  int neg = 0;
  if (*s == '-' || *s == '+')
    neg = *s++ == '-';
  if ((base == 0 || base == 16) && s[0] == '0' && (s[1] | 32) == 'x') {
    s += 2;
    base = 16;
  } else if (base == 0) {
    base = s[0] == '0' ? 8 : 10;
  }
  unsigned long long v = 0;
  for (;; s++) {
    int d = isdigit(*s) ? *s - '0' : isalpha(*s) ? (*s | 32) - 'a' + 10 : 99;
    if (d >= base)
      break;
    v = v * base + d;
  }
  if (end)
    *end = (char *)s;
  return neg ? -v : v;
}
long long strtoll(const char *s, char **end, int base) {
  return (long long)strtoull(s, end, base);
}
unsigned long strtoul(const char *s, char **end, int base) {
  return (unsigned long)strtoull(s, end, base);
}
long strtol(const char *s, char **end, int base) {
  return (long)strtoull(s, end, base);
}
int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }

static unsigned long long rand_state = 1;
int rand(void) {
  rand_state = rand_state * 6364136223846793005ull + 1442695040888963407ull;
  return (int)(rand_state >> 33);
}
void srand(unsigned seed) { rand_state = seed; }
char *getenv(const char *name) { return 0; }

static void swap_bytes(char *a, char *b, size_t n) {
  for (; n; n--, a++, b++) {
    char t = *a;
    *a = *b;
    *b = t;
  }
}

void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *)) {
  char *b = base;
  for (size_t i = 1; i < n; i++)
    for (size_t j = i; j > 0 && cmp(b + (j - 1) * size, b + j * size) > 0; j--)
      swap_bytes(b + (j - 1) * size, b + j * size, size);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *)) {
  const char *b = base;
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    int c = cmp(key, b + mid * size);
    if (c == 0)
      return (void *)(b + mid * size);
    if (c < 0)
      hi = mid;
    else
      lo = mid + 1;
  }
  return 0;
}

// A bump allocator over a static heap; a block's size is kept before it.
static unsigned char heap[16 << 20] __attribute__((aligned(16)));
static size_t heap_used;

void *malloc(size_t n) {
  size_t need = (n + 16 + 15) & ~15u;
  if (heap_used + need > sizeof heap)
    return 0;
  unsigned char *p = heap + heap_used + 16;
  *(size_t *)(p - 16) = n;
  heap_used += need;
  return p;
}

void free(void *p) { (void)p; }

void *calloc(size_t n, size_t m) {
  void *p = malloc(n * m);
  if (p)
    memset(p, 0, n * m);
  return p;
}

void *realloc(void *p, size_t n) {
  void *q = malloc(n);
  if (p && q) {
    size_t old = *(size_t *)((unsigned char *)p - 16);
    memcpy(q, p, old < n ? old : n);
  }
  return q;
}

//===-- math ---------------------------------------------------------------===//

float sqrtf(float x) { return __builtin_sqrtf(x); }
float floorf(float x) { return __builtin_floorf(x); }
float ceilf(float x) { return __builtin_ceilf(x); }
float truncf(float x) { return __builtin_truncf(x); }
float fabsf(float x) { return __builtin_fabsf(x); }
float fminf(float a, float b) { return __builtin_fminf(a, b); }
float fmaxf(float a, float b) { return __builtin_fmaxf(a, b); }
float copysignf(float x, float y) { return __builtin_copysignf(x, y); }
double fabs(double x) { return __builtin_fabs(x); }
double copysign(double x, double y) { return __builtin_copysign(x, y); }

double trunc(double x) {
  if (!(fabs(x) < 4503599627370496.0)) // 2^52: an integer already, or NaN
    return x;
  return (double)(long long)x;
}
double floor(double x) {
  double t = trunc(x);
  return t > x ? t - 1 : t;
}
double ceil(double x) {
  double t = trunc(x);
  return t < x ? t + 1 : t;
}
double sqrt(double x) {
  if (!(x > 0) || x == __builtin_inf())
    return x == 0 || x == __builtin_inf() ? x : __builtin_nan("");
  double r = (double)__builtin_sqrtf((float)x);
  for (int i = 0; i < 4; i++)
    r = 0.5 * (r + x / r);
  return r;
}
double fmod(double x, double y) {
  if (y == 0 || x != x || y != y)
    return __builtin_nan("");
  double q = trunc(x / y);
  return x - q * y;
}
double ldexp(double x, int e) {
  while (e > 0)
    x *= 2, e--;
  while (e < 0)
    x *= 0.5, e++;
  return x;
}

//===-- stdio ----------------------------------------------------------------===//

struct gvm_file {
  int fd;
};
static struct gvm_file files[3] = {{0}, {1}, {2}};
FILE *stdin = &files[0], *stdout = &files[1], *stderr = &files[2];

// printf: %d %i %u %x %X %o %c %s %p %n %%, the hh h l ll z j t sizes, width
// and precision (also *), the flags '-' '0' '+' ' ' '#', and %f %e %g as a
// fixed point (approximately).
struct out {
  char *buf; // or NULL: to fd
  size_t cap; // the buffer's room (snprintf)
  int fd;
  size_t n;
  char tmp[128];
  size_t t;
};

static void flush(struct out *o) {
  if (!o->buf && o->t)
    write(o->fd, o->tmp, o->t);
  o->t = 0;
}

static void put(struct out *o, char c) {
  if (o->buf) {
    if (o->n + 1 < o->cap)
      o->buf[o->n] = c;
  } else {
    o->tmp[o->t++] = c;
    if (o->t == sizeof o->tmp)
      flush(o);
  }
  o->n++;
}

static void pad(struct out *o, const char *pre, const char *s, int len,
                int width, int left, char fill) {
  int plen = strlen(pre);
  int total = plen + len;
  if (!left && fill == ' ')
    for (int i = total; i < width; i++)
      put(o, ' ');
  for (int i = 0; i < plen; i++)
    put(o, pre[i]);
  if (!left && fill == '0')
    for (int i = total; i < width; i++)
      put(o, '0');
  for (int i = 0; i < len; i++)
    put(o, s[i]);
  if (left)
    for (int i = total; i < width; i++)
      put(o, ' ');
}

static int utoa(char *b, unsigned long long v, unsigned base, int upper) {
  char t[24];
  int n = 0;
  do {
    unsigned d = (unsigned)(v % base);
    t[n++] = d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10;
    v /= base;
  } while (v);
  for (int i = 0; i < n; i++)
    b[i] = t[n - 1 - i];
  return n;
}

static int vfmt(struct out *o, const char *f, va_list ap) {
  for (; *f; f++) {
    if (*f != '%') {
      put(o, *f);
      continue;
    }
    f++;
    int left = 0, width = 0, prec = -1, lng = 0, plus = 0, space = 0, alt = 0;
    char fill = ' ';
    for (;; f++) {
      if (*f == '-')
        left = 1;
      else if (*f == '0')
        fill = '0';
      else if (*f == '+')
        plus = 1;
      else if (*f == ' ')
        space = 1;
      else if (*f == '#')
        alt = 1;
      else
        break;
    }
    if (*f == '*') {
      width = va_arg(ap, int);
      if (width < 0)
        left = 1, width = -width;
      f++;
    }
    while (*f >= '0' && *f <= '9')
      width = width * 10 + (*f++ - '0');
    if (*f == '.') {
      prec = 0;
      f++;
      if (*f == '*') {
        prec = va_arg(ap, int);
        f++;
      }
      while (*f >= '0' && *f <= '9')
        prec = prec * 10 + (*f++ - '0');
    }
    int half = 0;
    while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'j' || *f == 't' ||
           *f == 'L' || *f == 'q') {
      if (*f == 'l' || *f == 'j' || *f == 'q' || *f == 'L')
        lng++;
      if (*f == 'h')
        half++;
      f++;
    }
    if (left)
      fill = ' ';
    char b[400];
    int n = 0;
    const char *pre = "";
    switch (*f) {
    case 'd':
    case 'i': {
      long long v = lng >= 2 ? va_arg(ap, long long) : va_arg(ap, int);
      if (half == 1)
        v = (short)v;
      else if (half >= 2)
        v = (signed char)v;
      unsigned long long u = v < 0 ? -(unsigned long long)v : v;
      pre = v < 0 ? "-" : plus ? "+" : space ? " " : "";
      if (!(prec == 0 && u == 0))
        n = utoa(b, u, 10, 0);
      if (prec >= 0) {
        fill = ' ';
        if (n < prec) {
          memmove(b + prec - n, b, n);
          memset(b, '0', prec - n);
          n = prec;
        }
      }
      break;
    }
    case 'u':
    case 'x':
    case 'X':
    case 'o': {
      unsigned long long v =
          lng >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
      if (half == 1)
        v = (unsigned short)v;
      else if (half >= 2)
        v = (unsigned char)v;
      unsigned base = *f == 'u' ? 10 : *f == 'o' ? 8 : 16;
      if (alt && v && base == 16)
        pre = *f == 'X' ? "0X" : "0x";
      if (!(prec == 0 && v == 0))
        n = utoa(b, v, base, *f == 'X');
      if (prec >= 0) {
        fill = ' ';
        if (n < prec) {
          memmove(b + prec - n, b, n);
          memset(b, '0', prec - n);
          n = prec;
        }
      }
      if (alt && base == 8 && (n == 0 || b[0] != '0'))
        pre = "0";
      break;
    }
    case 'p':
      pre = "0x";
      n = utoa(b, (unsigned)va_arg(ap, void *), 16, 0);
      break;
    case 'c':
      b[n++] = (char)va_arg(ap, int);
      break;
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      int len = prec >= 0 ? (int)strnlen(s, prec) : (int)strlen(s);
      pad(o, "", s, len, width, left, ' ');
      continue;
    }
    case 'n':
      *va_arg(ap, int *) = o->n;
      continue;
    case 'f':
    case 'F':
    case 'g':
    case 'G':
    case 'e':
    case 'E': {
      double v = va_arg(ap, double);
      if (prec < 0)
        prec = 6;
      if (prec > 30)
        prec = 30;
      if (v != v) {
        memcpy(b, "nan", 3);
        n = 3;
        break;
      }
      if (v < 0 || (v == 0 && __builtin_signbit(v)))
        pre = "-", v = -v;
      else if (plus)
        pre = "+";
      if (v > 1e300) {
        memcpy(b, "inf", 3);
        n = 3;
        break;
      }
      if (*f == 'g' || *f == 'G') {
        // the shortest of %f with the precision's significant digits
        int digits = prec ? prec : 1, mag = 0;
        double t = v;
        while (t >= 10)
          t /= 10, mag++;
        while (t > 0 && t < 1)
          t *= 10, mag--;
        prec = digits - 1 - mag;
        if (prec < 0)
          prec = 0;
      }
      double scale = 1;
      for (int i = 0; i < prec; i++)
        scale *= 10;
      double whole_d = floor(v);
      unsigned long long frac =
          (unsigned long long)((v - whole_d) * scale + 0.5);
      if (frac >= (unsigned long long)scale)
        whole_d += 1, frac -= (unsigned long long)scale;
      if (whole_d < 1e19) {
        n = utoa(b, (unsigned long long)whole_d, 10, 0);
      } else {
        memcpy(b, "1e19+", 5);
        n = 5;
      }
      if (prec > 0) {
        b[n++] = '.';
        char t[40];
        int k = utoa(t, frac, 10, 0);
        for (int i = k; i < prec; i++)
          b[n++] = '0';
        for (int i = 0; i < k; i++)
          b[n++] = t[i];
      }
      break;
    }
    case '%':
      b[n++] = '%';
      break;
    default:
      b[n++] = '%';
      b[n++] = *f;
      break;
    }
    pad(o, pre, b, n, width, left, fill);
  }
  return o->n;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
  struct out o;
  o.buf = 0, o.cap = 0, o.fd = f->fd, o.n = 0, o.t = 0;
  int n = vfmt(&o, fmt, ap);
  flush(&o);
  return n;
}

int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
  struct out o;
  o.buf = buf, o.cap = cap, o.fd = -1, o.n = 0, o.t = 0;
  int n = vfmt(&o, fmt, ap);
  if (cap)
    buf[(size_t)n < cap ? (size_t)n : cap - 1] = 0;
  return n;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
  return vsnprintf(buf, (size_t)-1 >> 1, fmt, ap);
}

int printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(stdout, fmt, ap);
  va_end(ap);
  return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vfprintf(f, fmt, ap);
  va_end(ap);
  return n;
}

int sprintf(char *buf, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsprintf(buf, fmt, ap);
  va_end(ap);
  return n;
}

int snprintf(char *buf, size_t cap, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, cap, fmt, ap);
  va_end(ap);
  return n;
}

int fputc(int c, FILE *f) {
  char ch = c;
  write(f->fd, &ch, 1);
  return (unsigned char)c;
}
int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *s, FILE *f) {
  write(f->fd, s, strlen(s));
  return 0;
}

int puts(const char *s) {
  fputs(s, stdout);
  fputc('\n', stdout);
  return 0;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f) {
  write(f->fd, p, size * n);
  return n;
}

int fflush(FILE *f) { return 0; }

int getchar(void) {
  unsigned char c;
  return read(0, &c, 1) == 1 ? c : EOF;
}
