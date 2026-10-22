// The test runtime's declarations (gpu_vm/llvm/test/exec): a few of libc's
// functions over gvm_cpu's semihosted calls.
#ifndef GVM_TEST_H
#define GVM_TEST_H
typedef unsigned int size_t;
typedef int ssize_t;
#define NULL ((void *)0)
void exit(int) __attribute__((noreturn));
void _exit(int) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int atexit(void (*f)(void));
ssize_t write(int fd, const void *buf, size_t n);
ssize_t read(int fd, void *buf, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strchr(const char *s, int c);
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void free(void *p);
int puts(const char *s);
int putchar(int c);
int printf(const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int abs(int x);
long labs(long x);
int atoi(const char *s);

// A test's checks: print where one failed and exit 1.
#define CHECK(c)                                                              \
  do {                                                                        \
    if (!(c)) {                                                               \
      printf("%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);            \
      exit(1);                                                                \
    }                                                                         \
  } while (0)
#define CHECK_EQ(a, b)                                                        \
  do {                                                                        \
    long long _a = (long long)(a), _b = (long long)(b);                       \
    if (_a != _b) {                                                           \
      printf("%s:%d: %s == %lld, want %lld\n", __FILE__, __LINE__, #a, _a,    \
             _b);                                                             \
      exit(1);                                                                \
    }                                                                         \
  } while (0)
#endif
