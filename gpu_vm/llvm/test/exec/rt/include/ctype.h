// The test runtime's ctype (gpu_vm/llvm/test/exec/rt): ASCII.
#ifndef GVM_CTYPE_H
#define GVM_CTYPE_H
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isupper(int c);
int islower(int c);
int isprint(int c);
int isxdigit(int c);
int ispunct(int c);
int iscntrl(int c);
int toupper(int c);
int tolower(int c);
#endif
