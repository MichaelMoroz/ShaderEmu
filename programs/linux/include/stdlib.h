#pragma once
#include <stddef.h>
void exit(int status) __attribute__((noreturn));
char* getenv(const char* name);
double strtod(const char* s, char** end);
int atoi(const char* s);
