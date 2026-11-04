/* The game's rand() and srand() (codeJK2/game, built into its module by build.sh): glibc's and musl's are different
   generators, so the build computer's program and the machine's would play different games from one seed. This is
   the one the game was written against (MSVC's: 15 bits, which its random() and crandom() macros assume). The
   module's names are its own (build.sh's module()), so the engine and the libraries keep the C library's. */
#include "qcommon/se_slot.h"
unsigned se_rand_state = 1;

void srand(unsigned seed)
{
    se_rand_state = seed;
}

int rand(void)
{
    SE_NOT_AHEAD(SE_WHY_RAND);  /* (a player made ahead draws none of the game's: qcommon/se_slot.h) */
    se_rand_state = se_rand_state * 214013u + 2531011u;
    return (int)(se_rand_state >> 16 & 0x7fff);
}
