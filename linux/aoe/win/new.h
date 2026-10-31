#include <new>
#include <stddef.h>
typedef int (*_PNH)(size_t);
extern "C" _PNH _set_new_handler(_PNH);
extern "C" int _set_new_mode(int);
