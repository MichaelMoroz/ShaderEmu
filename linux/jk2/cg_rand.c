/* The client's game's random numbers, apart from the game's (build.sh compiles codeJK2/cgame with Q_flrand, Q_irand
   and rand named CG_flrand, CG_irand and CG_rand). In the 2002 library the two drew from one state, so what the
   client's game drew for its effects (how many sparks, whether a particle was in view) moved the game's own numbers:
   on the machine a float compared a last bit differently in an effect, the game drew another number at its next
   frame, and its NPCs did other things than on the build computer (docs/games/jk2.md, "Is it the same game"). Apart,
   the game plays the same whatever the client's game drew, and the client's game can go to another core later.
   The generators are the game's (q_math.c's flrand and irand, MSVC's rand), on states of their own. */
#include <stdint.h>

static uint32_t cg_holdrand = 0x89abcdef;
static unsigned cg_rand_state = 1;

float CG_flrand(float min, float max)
{
    float result;
    cg_holdrand = cg_holdrand * 214013u + 2531011u;
    result = (float)(cg_holdrand >> 17);
    return ((result * (max - min)) / (float)32768) + min;
}

int CG_irand(int min, int max)
{
    max++;
    cg_holdrand = cg_holdrand * 214013u + 2531011u;
    return (int)((((cg_holdrand >> 17) * (unsigned)(max - min)) >> 15) + min);
}

int CG_rand(void)
{
    cg_rand_state = cg_rand_state * 214013u + 2531011u;
    return (int)(cg_rand_state >> 16 & 0x7fff);
}
