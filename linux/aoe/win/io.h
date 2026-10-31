#ifndef AOE_IO_H
#define AOE_IO_H
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#define _O_RDONLY O_RDONLY
#define _O_WRONLY O_WRONLY
#define _O_RDWR O_RDWR
#define _O_CREAT O_CREAT
#define _O_TRUNC O_TRUNC
#define _O_APPEND O_APPEND
#define _O_EXCL O_EXCL
#define _O_BINARY 0
#define O_BINARY 0
#define _O_TEXT 0
#define O_TEXT 0
#define _S_IREAD 0400
#define _S_IWRITE 0200
#define S_IREAD 0400
#define S_IWRITE 0200
struct _finddata_t { unsigned attrib; long time_create, time_access, time_write; unsigned long size; char name[260]; };
#define _A_NORMAL 0
#define _A_RDONLY 1
#define _A_HIDDEN 2
#define _A_SYSTEM 4
#define _A_SUBDIR 0x10
#define _A_ARCH 0x20
#ifdef __cplusplus
extern "C" {
#endif
int aoe_open(const char* name, int flags, ...);
long _findfirst(const char*, struct _finddata_t*);
int _findnext(long, struct _finddata_t*);
int _findclose(long);
long _filelength(int);
#ifdef __cplusplus
}
#endif
#include <stdio.h>
#ifdef __cplusplus
extern "C"
#endif
FILE* aoe_fopen(const char*, const char*);
#define open aoe_open
#define fopen aoe_fopen
#define _open aoe_open
#define _close close
#define _read read
#define _write write
#define _lseek lseek
#define _tell(f) lseek((f), 0, SEEK_CUR)
#define tell(f) lseek((f), 0, SEEK_CUR)
#define _unlink aoe_unlink
#define _access aoe_access
#ifdef __cplusplus
extern "C" {
#endif
int aoe_unlink(const char*);
int aoe_access(const char*, int);
#ifdef __cplusplus
}
#endif
#endif
