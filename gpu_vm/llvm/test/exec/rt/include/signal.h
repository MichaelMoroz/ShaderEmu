// The test runtime's signal (gpu_vm/llvm/test/exec/rt): types only, no signals.
#ifndef GVM_SIGNAL_H
#define GVM_SIGNAL_H
typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIGINT 2
sighandler_t signal(int sig, sighandler_t h);
#endif
