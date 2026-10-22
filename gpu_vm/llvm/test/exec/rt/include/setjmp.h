// The test runtime's setjmp (gpu_vm/llvm/test/exec/rt/setjmp.s): ra, sp, s0-s23.
#ifndef GVM_SETJMP_H
#define GVM_SETJMP_H
typedef unsigned jmp_buf[26];
int setjmp(jmp_buf b) __attribute__((returns_twice));
void longjmp(jmp_buf b, int v) __attribute__((noreturn));
int _setjmp(jmp_buf b) __attribute__((returns_twice));
void _longjmp(jmp_buf b, int v) __attribute__((noreturn));
#endif
