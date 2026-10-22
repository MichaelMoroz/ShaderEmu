// The test runtime's fcntl (gpu_vm/llvm/test/exec/rt): constants only.
#ifndef GVM_FCNTL_H
#define GVM_FCNTL_H
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_EXCL 0200
int open(const char *path, int flags, ...);
#endif
