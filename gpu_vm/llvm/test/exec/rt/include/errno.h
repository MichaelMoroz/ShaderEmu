// The test runtime's errno (gpu_vm/llvm/test/exec/rt).
#ifndef GVM_ERRNO_H
#define GVM_ERRNO_H
extern int errno;
#define ENOENT 2
#define EIO 5
#define EBADF 9
#define ENOMEM 12
#define EINVAL 22
#define ERANGE 34
#endif
