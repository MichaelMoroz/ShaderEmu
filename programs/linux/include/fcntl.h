#pragma once
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
int open(const char* path, int flags, ...);
// Read or write `n` bytes at `offset`; these do not move the file position.
long pread(int fd, void* buf, unsigned long n, unsigned long offset);
long pwrite(int fd, const void* buf, unsigned long n, unsigned long offset);
int sched_yield(void);
