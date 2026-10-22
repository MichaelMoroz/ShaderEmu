// switch: dense (a jump table), sparse (a tree of compares), with fallthrough,
// and computed goto (block addresses, an indirect branch).
#include "gvm.h"

#define NI __attribute__((noinline))

NI static int dense(int x) {
  switch (x) {
  case 0: return 10;
  case 1: return 21;
  case 2: return 32;
  case 3: return 43;
  case 4: return 54;
  case 5: return 65;
  case 6: return 76;
  case 7: return 87;
  case 8: return 98;
  case 9: return 109;
  case 10: return 120;
  case 11: return 131;
  default: return -1;
  }
}

NI static int sparse(unsigned x) {
  switch (x) {
  case 1: return 1;
  case 100: return 2;
  case 1000: return 3;
  case 100000: return 4;
  case 0x80000000u: return 5;
  case 0xffffffffu: return 6;
  case 77: return 7;
  default: return 0;
  }
}

NI static int fall(int x) {
  int r = 0;
  switch (x) {
  case 0: r += 1; // fallthrough
  case 1: r += 10; // fallthrough
  case 2: r += 100; break;
  case 3: r += 1000; // fallthrough
  default: r += 10000;
  }
  return r;
}

NI static int interp(const unsigned char *code) {
  static void *ops[] = {&&op_halt, &&op_inc, &&op_dbl, &&op_neg};
  int acc = 1;
  goto *ops[*code++];
op_inc:
  acc++;
  goto *ops[*code++];
op_dbl:
  acc *= 2;
  goto *ops[*code++];
op_neg:
  acc = -acc;
  goto *ops[*code++];
op_halt:
  return acc;
}

int main(void) {
  for (int i = -3; i < 15; i++)
    CHECK_EQ(dense(i), i >= 0 && i <= 11 ? 10 + 11 * i : -1);
  CHECK_EQ(sparse(1), 1);
  CHECK_EQ(sparse(100), 2);
  CHECK_EQ(sparse(1000), 3);
  CHECK_EQ(sparse(100000), 4);
  CHECK_EQ(sparse(0x80000000u), 5);
  CHECK_EQ(sparse(0xffffffffu), 6);
  CHECK_EQ(sparse(77), 7);
  CHECK_EQ(sparse(78), 0);
  CHECK_EQ(sparse(0), 0);
  CHECK_EQ(fall(0), 111);
  CHECK_EQ(fall(1), 110);
  CHECK_EQ(fall(2), 100);
  CHECK_EQ(fall(3), 11000);
  CHECK_EQ(fall(9), 10000);
  static const unsigned char prog[] = {1, 2, 2, 3, 1, 0};
  CHECK_EQ(interp(prog), -7);
  puts("switch ok");
  return 0;
}
