// The exec tests' header (gpu_vm/llvm/test/exec): the runtime's libc (rt/include) and the checks.
#ifndef GVM_TEST_H
#define GVM_TEST_H
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int ssize_t;
ssize_t write(int fd, const void *buf, size_t n);
ssize_t read(int fd, void *buf, size_t n);

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
