// Part of the tiny C runtime in ../libc.c: just what small Linux programs here need.
#pragma once
#include <stdarg.h>
#include <stddef.h>
typedef struct FILE FILE;
extern FILE* stdout;
extern FILE* stderr;
int printf(const char* fmt, ...);
int fprintf(FILE* f, const char* fmt, ...);
int vfprintf(FILE* f, const char* fmt, va_list ap);
int puts(const char* s);
int fflush(FILE* f);
