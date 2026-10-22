// Globals: arrays, structs, pointers among them, static locals, constant
// tables, string literals, addresses folded into loads' immediates.
#include "gvm.h"

#define NI __attribute__((noinline))

int counter;
int table[64];
const short ctab[8] = {1, -2, 3, -4, 5, -6, 7, -8};
struct point {
  int x, y;
  char name[8];
} pts[4] = {{1, 2, "a"}, {3, 4, "bb"}, {5, 6, "ccc"}, {7, 8, "dddd"}};
struct point *ptrs[4] = {&pts[3], &pts[2], &pts[1], &pts[0]};
const char *names[] = {"zero", "one", "two"};
static long long big = 0x123456789abcdefll;
double dtab[3] = {0.5, 1.5, 2.5};
char buf[100];
int *pcount = &counter;
int arr2d[4][5];

NI int bump(void) {
  static int calls;
  return ++calls;
}

NI int get(int i) { return table[i]; }
NI void put(int i, int v) { table[i] = v; }
NI int getpt(int i) { return pts[i].y + ptrs[i]->x; }
NI int ctabsum(void) {
  int s = 0;
  for (int i = 0; i < 8; i++)
    s += ctab[i];
  return s;
}

int main(void) {
  for (int i = 0; i < 64; i++)
    put(i, i * 3);
  int s = 0;
  for (int i = 0; i < 64; i++)
    s += get(i);
  CHECK_EQ(s, 3 * 63 * 64 / 2);
  CHECK_EQ(ctabsum(), -4);
  CHECK_EQ(getpt(0), 2 + 7);
  CHECK_EQ(getpt(3), 8 + 1);
  CHECK(strcmp(pts[2].name, "ccc") == 0);
  CHECK(strcmp(ptrs[0]->name, "dddd") == 0);
  CHECK(strcmp(names[2], "two") == 0);
  CHECK(big == 0x123456789abcdefll);
  big += 1;
  CHECK(big == 0x123456789abcdf0ll);
  CHECK(dtab[1] + dtab[2] == 4.0);
  bump();
  bump();
  CHECK_EQ(bump(), 3);
  *pcount = 5;
  CHECK_EQ(counter, 5);
  for (int i = 0; i < 4; i++)
    for (int j = 0; j < 5; j++)
      arr2d[i][j] = i * 10 + j;
  CHECK_EQ(arr2d[3][4], 34);
  CHECK_EQ(*(&arr2d[0][0] + 13), 23);
  sprintf(buf, "%s-%d-%x", names[1], -42, 0xbeef);
  CHECK(strcmp(buf, "one--42-beef") == 0);
  puts("globals ok");
  return 0;
}
