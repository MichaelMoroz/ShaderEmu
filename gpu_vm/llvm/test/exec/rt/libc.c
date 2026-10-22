// The test runtime (gpu_vm/llvm/test/exec): libc's few functions over gvm_cpu's
// semihosted syscalls (r12 the number, r5-r10 the arguments, r5 the result).
// Built with -ffreestanding (no calls to itself from its loops).
#include "gvm.h"
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

static void (*atexit_fn[8])(void);
static int natexit;
int atexit(void (*f)(void)) {
  if (natexit == 8)
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

ssize_t write(int fd, const void *buf, size_t n) {
  return syscall3(64, fd, (long)buf, n);
}

ssize_t read(int fd, void *buf, size_t n) {
  return syscall3(63, fd, (long)buf, n);
}

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

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n])
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

char *strchr(const char *s, int c) {
  for (;; s++) {
    if (*s == (char)c)
      return (char *)s;
    if (!*s)
      return 0;
  }
}

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }

int atoi(const char *s) {
  int n = 0, neg = 0;
  if (*s == '-')
    neg = 1, s++;
  while (*s >= '0' && *s <= '9')
    n = n * 10 + (*s++ - '0');
  return neg ? -n : n;
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

// printf: %d %i %u %x %X %o %c %s %p %%, the l and ll sizes, width, '-' and
// '0', and %f/%g/%e as a fixed point with a precision (6 by default).
struct out {
  char *buf; // or NULL: to stdout
  size_t n;
  char tmp[128];
  size_t t;
};

static void flush(struct out *o) {
  if (!o->buf && o->t)
    write(1, o->tmp, o->t);
  o->t = 0;
}

static void put(struct out *o, char c) {
  if (o->buf) {
    o->buf[o->n] = c;
  } else {
    o->tmp[o->t++] = c;
    if (o->t == sizeof o->tmp)
      flush(o);
  }
  o->n++;
}

static void pad(struct out *o, const char *s, int len, int width, int left,
                char fill) {
  if (!left && fill == '0' && len > 0 && *s == '-') {
    put(o, '-');
    s++, len--, width--;
  }
  if (!left)
    for (int i = len; i < width; i++)
      put(o, fill);
  for (int i = 0; i < len; i++)
    put(o, s[i]);
  if (left)
    for (int i = len; i < width; i++)
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
    int left = 0, width = 0, prec = -1, lng = 0;
    char fill = ' ';
    for (;; f++) {
      if (*f == '-')
        left = 1;
      else if (*f == '0')
        fill = '0';
      else if (*f == '+' || *f == ' ' || *f == '#')
        ;
      else
        break;
    }
    if (*f == '*') {
      width = va_arg(ap, int);
      f++;
    }
    while (*f >= '0' && *f <= '9')
      width = width * 10 + (*f++ - '0');
    if (*f == '.') {
      prec = 0;
      f++;
      while (*f >= '0' && *f <= '9')
        prec = prec * 10 + (*f++ - '0');
    }
    while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'j' || *f == 't') {
      if (*f == 'l' || *f == 'j')
        lng++;
      f++;
    }
    char b[64];
    int n = 0;
    switch (*f) {
    case 'd':
    case 'i': {
      long long v = lng >= 2 ? va_arg(ap, long long) : va_arg(ap, int);
      unsigned long long u = v < 0 ? -(unsigned long long)v : v;
      if (v < 0)
        b[n++] = '-';
      n += utoa(b + n, u, 10, 0);
      break;
    }
    case 'u':
    case 'x':
    case 'X':
    case 'o': {
      unsigned long long v =
          lng >= 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
      n = utoa(b, v, *f == 'u' ? 10 : *f == 'o' ? 8 : 16, *f == 'X');
      break;
    }
    case 'p':
      b[0] = '0', b[1] = 'x';
      n = 2 + utoa(b + 2, (unsigned)va_arg(ap, void *), 16, 0);
      break;
    case 'c':
      b[n++] = (char)va_arg(ap, int);
      break;
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      int len = strlen(s);
      if (prec >= 0 && prec < len)
        len = prec;
      pad(o, s, len, width, left, ' ');
      continue;
    }
    case 'f':
    case 'g':
    case 'e': {
      double v = va_arg(ap, double);
      if (prec < 0)
        prec = 6;
      if (v != v) {
        b[0] = 'n', b[1] = 'a', b[2] = 'n';
        n = 3;
        break;
      }
      if (v < 0)
        b[n++] = '-', v = -v;
      if (v > 1e18) {
        b[n++] = 'i', b[n++] = 'n', b[n++] = 'f';
        break;
      }
      double scale = 1;
      for (int i = 0; i < prec; i++)
        scale *= 10;
      unsigned long long whole = (unsigned long long)v;
      unsigned long long frac =
          (unsigned long long)((v - (double)whole) * scale + 0.5);
      if (frac >= (unsigned long long)scale)
        whole++, frac -= (unsigned long long)scale;
      n += utoa(b + n, whole, 10, 0);
      if (prec > 0) {
        b[n++] = '.';
        char t[24];
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
    pad(o, b, n, width, left, fill);
  }
  return o->n;
}

int printf(const char *fmt, ...) {
  struct out o;
  o.buf = 0, o.n = 0, o.t = 0;
  va_list ap;
  va_start(ap, fmt);
  int n = vfmt(&o, fmt, ap);
  va_end(ap);
  flush(&o);
  return n;
}

int sprintf(char *buf, const char *fmt, ...) {
  struct out o;
  o.buf = buf, o.n = 0, o.t = 0;
  va_list ap;
  va_start(ap, fmt);
  int n = vfmt(&o, fmt, ap);
  va_end(ap);
  buf[n] = 0;
  return n;
}

int putchar(int c) {
  char ch = c;
  write(1, &ch, 1);
  return (unsigned char)c;
}

int puts(const char *s) {
  write(1, s, strlen(s));
  write(1, "\n", 1);
  return 0;
}

// libm's few: the ops (the test runtime is built with -fno-math-errno).
float sqrtf(float x) { return __builtin_sqrtf(x); }
float floorf(float x) { return __builtin_floorf(x); }
float ceilf(float x) { return __builtin_ceilf(x); }
float truncf(float x) { return __builtin_truncf(x); }
float fabsf(float x) { return __builtin_fabsf(x); }
float fminf(float a, float b) { return __builtin_fminf(a, b); }
float fmaxf(float a, float b) { return __builtin_fmaxf(a, b); }
double fabs(double x) { return __builtin_fabs(x); }
