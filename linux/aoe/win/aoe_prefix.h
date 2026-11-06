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
// inline (port/win32.cpp has the state): a map's making calls it a million times, and the game's own debug_rand with it,
// whose log is kept only when the command line said LOGRANDOM (basegame.cpp's aoe_debug_rand_log): the same numbers
#ifdef __cplusplus
extern "C" unsigned aoe_rand_word;
#else
extern unsigned aoe_rand_word;
#endif
static inline int aoe_rand_inline(void) { aoe_rand_word = aoe_rand_word * 214013u + 2531011u; return (aoe_rand_word >> 16) & 0x7fff; }
#define rand aoe_rand_inline
#define srand aoe_srand
#ifdef __cplusplus
extern int do_debug_random;
int aoe_debug_rand_log(char* source_name, int source_line);
inline int aoe_debug_rand(char* source_name, int source_line) {
    return __builtin_expect(do_debug_random, 0) ? aoe_debug_rand_log(source_name, source_line) : aoe_rand_inline();
}
#define debug_rand aoe_debug_rand
// the game's data read inline: 287 thousand reads of 2.6 bytes on average at Start Game, each within the 64 KB inflated
// (rge_fio.cpp's aoe_rge_read_more is the rest of rge_read; docs/games/aoe.md, "The load")
struct AoeRge { int handle; unsigned char flags; long point; unsigned char* current; };
extern AoeRge aoe_rge;
extern int ENABLE_COMPRESSION;
void aoe_rge_read_more(int handle, void* data, int size);
inline void aoe_rge_read(int handle, void* data, int size) {
    if (ENABLE_COMPRESSION != 0 && aoe_rge.flags == 0 && handle >= 0 && handle == aoe_rge.handle && size >= 0
        && size + aoe_rge.point < 0x10000) {
        unsigned char* d = (unsigned char*) data;
        const unsigned char* c = aoe_rge.current;
        if (size <= 8) {
            for (int i = 0; i < size; i++) d[i] = c[i];
        } else {
            __builtin_memcpy(d, c, size);
        }
        aoe_rge.point += size;
        aoe_rge.current += size;
        return;
    }
    aoe_rge_read_more(handle, data, size);
}
#define rge_read aoe_rge_read
// the random map's roots of whole numbers without doubles, the same numbers (port/aoe_path.c)
extern "C" long aoe_isqrt_more(long n);
extern "C" unsigned short aoe_path_table[];
extern "C" int aoe_path_made;
inline long aoe_isqrt(long n) { return n > 0 && n < 2 * 256 * 256 + 1 && aoe_path_made ? aoe_path_table[n] >> 4 : aoe_isqrt_more(n); }
extern "C" float aoe_sqrt_whole(long n);
#endif
