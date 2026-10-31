// Single bytes: the trial's language is English.
#ifndef AOE_MBSTRING_H
#define AOE_MBSTRING_H
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
size_t _mbslen(const unsigned char*);
unsigned char* _mbsinc(const unsigned char*);
unsigned char* _mbsdec(const unsigned char*, const unsigned char*);
unsigned char* _mbsninc(const unsigned char*, size_t);
unsigned char* _mbscpy(unsigned char*, const unsigned char*);
unsigned char* _mbscat(unsigned char*, const unsigned char*);
unsigned char* _mbsncpy(unsigned char*, const unsigned char*, size_t);
unsigned char* _mbsncat(unsigned char*, const unsigned char*, size_t);
unsigned char* _mbschr(const unsigned char*, unsigned);
unsigned char* _mbsrchr(const unsigned char*, unsigned);
unsigned char* _mbsstr(const unsigned char*, const unsigned char*);
int _mbscmp(const unsigned char*, const unsigned char*);
int _mbsicmp(const unsigned char*, const unsigned char*);
unsigned char* _mbsupr(unsigned char*);
int _ismbcspace(unsigned);
int _ismbcdigit(unsigned);
int _ismbblead(unsigned);
#ifdef __cplusplus
}
#endif
#endif
