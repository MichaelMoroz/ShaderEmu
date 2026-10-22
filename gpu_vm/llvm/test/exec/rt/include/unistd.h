// The test runtime's unistd (gpu_vm/llvm/test/exec/rt).
#ifndef GVM_UNISTD_H
#define GVM_UNISTD_H
#include <stddef.h>
#include <sys/types.h>
ssize_t read(int fd, void *buf, size_t n);
ssize_t write(int fd, const void *buf, size_t n);
int close(int fd);
off_t lseek(int fd, off_t off, int whence);
void _exit(int) __attribute__((noreturn));
#endif
