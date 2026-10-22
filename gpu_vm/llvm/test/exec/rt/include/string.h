// The test runtime's string functions (gpu_vm/llvm/test/exec/rt).
#ifndef GVM_STRING_H
#define GVM_STRING_H
#include <stddef.h>

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
void *mempcpy(void *d, const void *s, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy(char *d, const char *s, size_t n);
char *stpcpy(char *d, const char *s);
char *strcat(char *d, const char *s);
char *strncat(char *d, const char *s, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
size_t strspn(const char *s, const char *a);
size_t strcspn(const char *s, const char *r);
char *strpbrk(const char *s, const char *a);
char *strdup(const char *s);
#endif
