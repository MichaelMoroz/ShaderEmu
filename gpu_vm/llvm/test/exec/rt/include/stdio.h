// The test runtime's stdio (gpu_vm/llvm/test/exec/rt): output to gvm_cpu's
// stdout and stderr only.
#ifndef GVM_STDIO_H
#define GVM_STDIO_H
#include <stdarg.h>
#include <stddef.h>

typedef struct gvm_file FILE;
extern FILE *stdin, *stdout, *stderr;
#define EOF (-1)
#define BUFSIZ 1024

int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t n, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int puts(const char *s);
int fputs(const char *s, FILE *f);
int putchar(int c);
int fputc(int c, FILE *f);
int putc(int c, FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
int fflush(FILE *f);
int getchar(void);
// files: stdin, stdout and stderr only (fopen gives NULL)
FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *f);
int fclose(FILE *f);
int feof(FILE *f);
int ferror(FILE *f);
int getc(FILE *f);
int fgetc(FILE *f);
int ungetc(int c, FILE *f);
size_t fread(void *p, size_t size, size_t n, FILE *f);
char *fgets(char *s, int n, FILE *f);
void clearerr(FILE *f);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
#define _IONBF 2
#define _IOLBF 1
#define _IOFBF 0
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
