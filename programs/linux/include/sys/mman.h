#pragma once
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_FAILED ((void*)-1)
void* mmap(void* addr, unsigned long length, int prot, int flags, int fd, long offset);
