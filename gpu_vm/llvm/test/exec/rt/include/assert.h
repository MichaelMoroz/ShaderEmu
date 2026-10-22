// The test runtime's assert (gpu_vm/llvm/test/exec/rt).
#ifndef GVM_ASSERT_H
#define GVM_ASSERT_H
void __assert_fail(const char *e, const char *file, int line)
    __attribute__((noreturn));
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : __assert_fail(#e, __FILE__, __LINE__))
#endif
#endif
