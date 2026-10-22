// The test runtime's stdlib (gpu_vm/llvm/test/exec/rt).
#ifndef GVM_STDLIB_H
#define GVM_STDLIB_H
#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff

void exit(int) __attribute__((noreturn));
void _exit(int) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int atexit(void (*f)(void));
void *malloc(size_t n);
void *calloc(size_t n, size_t m);
void *realloc(void *p, size_t n);
void free(void *p);
int abs(int x);
long labs(long x);
long long llabs(long long x);
int atoi(const char *s);
long atol(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));
int rand(void);
void srand(unsigned seed);
char *getenv(const char *name);
typedef struct {
  int quot, rem;
} div_t;
typedef struct {
  long quot, rem;
} ldiv_t;
div_t div(int a, int b);
ldiv_t ldiv(long a, long b);
#define alloca(n) __builtin_alloca(n)
#endif
