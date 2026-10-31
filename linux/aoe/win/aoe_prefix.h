// Every file of the game starts with this: the C++ of Visual C++ 5.0 as GCC can read it.
#ifndef AOE_PREFIX_H
#define AOE_PREFIX_H
#define _WIN32 1
#define WIN32 1
#define _M_IX86 500
#define _MSC_VER 1100
#define _iobuf _IO_FILE
#include "windows.h"
#ifdef __cplusplus
#include "io.h"
#include "com.h"
#include "new.h"
#endif
#endif
// Visual C++'s rand: 15 bits (the game divides by 0x7FFF everywhere), and the same numbers as the game's own
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
int aoe_rand(void);
void aoe_srand(unsigned);
#ifdef __cplusplus
}
#endif
#undef RAND_MAX
#define RAND_MAX 0x7fff
#define rand aoe_rand
#define srand aoe_srand
