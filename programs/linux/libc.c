// A tiny C runtime for static RV32IMA Linux programs: startup, the few system calls needed,
// printf, strings and three math functions. No C library is linked; floating point comes from
// compiler-rt's soft-float routines (see fetch.py).

#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

// ---- system calls (the generic 32-bit numbers RISC-V Linux uses) ----

static long syscall6(long n, long a, long b, long c, long d, long e, long f) {
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    register long a5 __asm__("a5") = f;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
    return a0;
}
#define SYS(n, a, b, c, d, e) syscall6(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), 0)

int open(const char* path, int flags, ...) { return (int)SYS(56, -100, path, flags, 0644, 0); }   // openat(AT_FDCWD)
int close(int fd) { return (int)SYS(57, fd, 0, 0, 0, 0); }
long read(int fd, void* buf, size_t n) { return SYS(63, fd, buf, n, 0, 0); }
long write(int fd, const void* buf, size_t n) { return SYS(64, fd, buf, n, 0, 0); }
// the 64-bit offset travels in two registers, low word first
long pread(int fd, void* buf, unsigned long n, unsigned long offset) { return SYS(67, fd, buf, n, offset, 0); }
long pwrite(int fd, const void* buf, unsigned long n, unsigned long offset) { return SYS(68, fd, buf, n, offset, 0); }
int sched_yield(void) { return (int)SYS(124, 0, 0, 0, 0, 0); }
void exit(int status) {
    SYS(94, status, 0, 0, 0, 0);   // exit_group
    for (;;) {}
}

struct timespec64 { int64_t sec; int64_t nsec; };

int gettimeofday(struct timeval* tv, struct timezone* tz) {
    struct timespec64 ts;
    (void)tz;
    SYS(403, 0, &ts, 0, 0, 0);   // clock_gettime64(CLOCK_REALTIME)
    tv->tv_sec = (long)ts.sec;
    tv->tv_usec = (long)((uint32_t)ts.nsec / 1000);
    return 0;
}

int usleep(unsigned microseconds) {
    struct timespec64 ts = {microseconds / 1000000, (int64_t)(microseconds % 1000000) * 1000};
    return (int)SYS(407, 1, 0, &ts, 0, 0);   // clock_nanosleep_time64(CLOCK_MONOTONIC)
}
unsigned sleep(unsigned seconds) {
    usleep(seconds * 1000000u);
    return 0;
}

// ---- startup ----

int main(int argc, char** argv);
static char** environment;

void start_c(long* sp) {
    int argc = (int)sp[0];
    char** argv = (char**)(sp + 1);
    environment = argv + argc + 1;
    exit(main(argc, argv));
}

// The kernel enters here with argc, argv and the environment on the stack.
__asm__(".globl _start\n_start:\n    mv a0, sp\n    andi sp, sp, -16\n    call start_c\n");

char* getenv(const char* name) {
    size_t n = strlen(name);
    for (char** e = environment; e && *e; e++)
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') return *e + n + 1;
    return 0;
}

// ---- strings ----

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}
int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char* a, const char* b, size_t n) {
    while (n && *a && *a == *b) a++, b++, n--;
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}
char* strstr(const char* hay, const char* needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (strncmp(hay, needle, n) == 0) return (char*)hay;
    return n ? 0 : (char*)hay;
}
void* memcpy(void* d, const void* s, size_t n) {
    char* dp = d;
    const char* sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}
void* memset(void* d, int c, size_t n) {
    char* dp = d;
    while (n--) *dp++ = (char)c;
    return d;
}
int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = a;
    const unsigned char* y = b;
    for (; n; n--, x++, y++)
        if (*x != *y) return *x - *y;
    return 0;
}

int atoi(const char* s) {
    int sign = 1, v = 0;
    while (*s == ' ') s++;
    if (*s == '-') sign = -1, s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return sign * v;
}

double strtod(const char* s, char** end) {
    double v = 0, scale = 1;
    int sign = 1;
    while (*s == ' ') s++;
    if (*s == '-') sign = -1, s++;
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            scale /= 10;
            v += (*s++ - '0') * scale;
        }
    }
    if (end) *end = (char*)s;
    return sign * v;
}

// ---- printf ----

struct FILE { int fd; };
static FILE out_file = {1}, err_file = {2};
FILE* stdout = &out_file;
FILE* stderr = &err_file;

typedef struct {
    char text[256];
    int used, total, fd;
} sink;

static void emit(sink* s, char c) {
    if (s->used == (int)sizeof s->text) {
        write(s->fd, s->text, (size_t)s->used);
        s->used = 0;
    }
    s->text[s->used++] = c;
    s->total++;
}

static void emit_padded(sink* s, const char* digits, int n, int width, int left, char pad) {
    for (int i = n; !left && i < width; i++) emit(s, pad);
    for (int i = 0; i < n; i++) emit(s, digits[i]);
    for (int i = n; left && i < width; i++) emit(s, ' ');
}

// Digits of v in `base`, most significant first; returns their count.
static int digits_of(char* out, unsigned long long v, unsigned base) {
    char tmp[24];
    int n = 0;
    do {
        unsigned d = (unsigned)(v % base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

int vfprintf(FILE* f, const char* fmt, va_list ap) {
    sink s;
    s.used = s.total = 0;
    s.fd = f->fd;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            emit(&s, *fmt);
            continue;
        }
        int left = 0, width = 0, precision = -1, longs = 0;
        char pad = ' ';
        fmt++;
        for (; *fmt == '-' || *fmt == '0'; fmt++) {
            if (*fmt == '-') left = 1;
            else pad = '0';
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') {
            precision = 0;
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') precision = precision * 10 + (*fmt++ - '0');
        }
        while (*fmt == 'l') longs++, fmt++;
        char buf[64];
        int n = 0;
        switch (*fmt) {
            case 'd': case 'i': {
                long long v = longs > 1 ? va_arg(ap, long long) : va_arg(ap, int);
                if (v < 0) buf[n++] = '-';
                n += digits_of(buf + n, (unsigned long long)(v < 0 ? -v : v), 10);
                break;
            }
            case 'u': case 'x': case 'p': {
                unsigned long long v = longs > 1 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
                n = digits_of(buf, v, *fmt == 'u' ? 10 : 16);
                break;
            }
            case 'f': case 'g': case 'e': {
                double v = va_arg(ap, double);
                if (precision < 0) precision = 6;
                if (precision > 9) precision = 9;
                if (v < 0) buf[n++] = '-', v = -v;
                unsigned long long scale = 1;
                for (int i = 0; i < precision; i++) scale *= 10;
                // whole and fractional parts as integers, rounded at the last printed digit
                unsigned long long whole = (unsigned long long)v;
                unsigned long long frac = (unsigned long long)((v - (double)whole) * (double)scale + 0.5);
                if (frac >= scale) whole++, frac -= scale;
                n += digits_of(buf + n, whole, 10);
                if (precision) {
                    char fd[24];
                    int k = digits_of(fd, frac, 10);
                    buf[n++] = '.';
                    for (int i = k; i < precision; i++) buf[n++] = '0';
                    for (int i = 0; i < k; i++) buf[n++] = fd[i];
                }
                break;
            }
            case 's': {
                const char* str = va_arg(ap, const char*);
                if (!str) str = "(null)";
                int len = (int)strlen(str);
                if (precision >= 0 && len > precision) len = precision;
                emit_padded(&s, str, len, width, left, ' ');
                continue;
            }
            case 'c': buf[n++] = (char)va_arg(ap, int); break;
            case '%': buf[n++] = '%'; break;
            default: buf[n++] = '%'; buf[n++] = *fmt; break;
        }
        emit_padded(&s, buf, n, width, left, pad);
    }
    if (s.used) write(s.fd, s.text, (size_t)s.used);
    return s.total;
}

int fprintf(FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}
int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}
int puts(const char* s) { return printf("%s\n", s); }
int fflush(FILE* f) {
    (void)f;
    return 0;   // nothing is buffered between calls
}

// ---- math ----

double fabs(double x) { return x < 0 ? -x : x; }

double sin(double x) {
    const double pi = M_PI;
    // bring x into -pi..pi, then fold into -pi/2..pi/2 where the series converges fast
    double turns = x / (2 * pi);
    x -= (double)(long)(turns + (turns < 0 ? -0.5 : 0.5)) * 2 * pi;
    if (x > pi / 2) x = pi - x;
    else if (x < -pi / 2) x = -pi - x;
    double x2 = x * x;
    return x * (1 + x2 * (-1.0 / 6 + x2 * (1.0 / 120 + x2 * (-1.0 / 5040 + x2 * (1.0 / 362880 +
               x2 * (-1.0 / 39916800 + x2 * (1.0 / 6227020800.0)))))));
}
double cos(double x) { return sin(x + M_PI / 2); }

double sqrt(double x) {
    if (x <= 0) return 0;
    // start from a power of two near the root, then Newton's method
    union { double d; uint64_t u; } v = {x};
    int exponent = (int)((v.u >> 52) & 0x7ff) - 1023;
    v.u = (uint64_t)(1023 + exponent / 2) << 52;
    double y = v.d;
    for (int i = 0; i < 7; i++) y = 0.5 * (y + x / y);
    return y;
}
