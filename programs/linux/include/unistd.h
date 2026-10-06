#pragma once
#include <stddef.h>
long read(int fd, void* buf, size_t n);
long write(int fd, const void* buf, size_t n);
int close(int fd);
int usleep(unsigned microseconds);
unsigned sleep(unsigned seconds);
