// The test runtime's time (gpu_vm/llvm/test/exec/rt): TIME (100 kHz) through gvm_cpu.
#ifndef GVM_TIME_H
#define GVM_TIME_H
typedef long long time_t;
typedef long clock_t;
#define CLOCKS_PER_SEC 100000
time_t time(time_t *t);
clock_t clock(void);
#endif
