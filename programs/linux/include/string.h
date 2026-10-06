#pragma once
#include <stddef.h>
size_t strlen(const char* s);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
char* strstr(const char* hay, const char* needle);
void* memcpy(void* d, const void* s, size_t n);
void* memset(void* d, int c, size_t n);
int memcmp(const void* a, const void* b, size_t n);
