// setjmp and longjmp (the runtime's): values kept in callee-saved registers and in memory across the jump,
// nested buffers, longjmp(b, 0) returning 1.
#include "gvm.h"
#include <setjmp.h>

static jmp_buf outer, inner;
static volatile int depth;

__attribute__((noinline)) static void dive(int n) {
  volatile char pad[100];
  pad[0] = n;
  depth = n + pad[0] - n;
  if (n == 10)
    longjmp(inner, n);
  dive(n + 1);
}

__attribute__((noinline)) static int run(int base) {
  int keep = base * 3; // live across the setjmp
  int r = setjmp(inner);
  if (r == 0) {
    dive(0);
    return -1;
  }
  return keep + r;
}

int main(void) {
  CHECK_EQ(run(5), 25);
  CHECK_EQ(depth, 10);
  volatile int count = 0;
  int r = setjmp(outer);
  count++;
  if (r == 0)
    longjmp(outer, 0);
  CHECK_EQ(r, 1);
  CHECK_EQ(count, 2);
  puts("longjmp ok");
  return 0;
}
