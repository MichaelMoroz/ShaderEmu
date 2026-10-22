// The test runtime's locale (gpu_vm/llvm/test/exec/rt): "C" only.
#ifndef GVM_LOCALE_H
#define GVM_LOCALE_H
struct lconv {
  char *decimal_point;
};
struct lconv *localeconv(void);
char *setlocale(int category, const char *locale);
#define LC_ALL 0
#endif
