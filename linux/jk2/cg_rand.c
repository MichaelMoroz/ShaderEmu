/* The client's game's random numbers, apart from the game's (build.sh compiles codeJK2/cgame with Q_flrand, Q_irand
   and rand named CG_flrand, CG_irand and CG_rand). In the 2002 library the two drew from one state, so what the
   client's game drew for its effects (how many sparks, whether a particle was in view) moved the game's own numbers:
   on the machine a float compared a last bit differently in an effect, the game drew another number at its next
   frame, and its NPCs did other things than on the build computer (docs/games/jk2.md, "Is it the same game"). Apart,
   the game plays the same whatever the client's game drew, and the client's game can go to another core later.
   The generators are the game's (q_math.c's flrand and irand, MSVC's rand), on states of their own.
   A player drawn (CG_AddCEntity of an ET_PLAYER, cg_ents.cpp) draws from a state of its own, made from its number and
   the frame's time (SE_CGRandFor): the players are made on several cores at once (docs/games/jk2.md, "The players on
   workers"), and so draw the same numbers in whatever order and on whatever core they are made. A state a job slot. */
#include <stdint.h>
#include "qcommon/se_slot.h"

typedef struct { uint32_t hold, state, on, unused; } __attribute__((aligned(16))) cg_rand_t;
static cg_rand_t cg_r[SE_SLOTS + 2] = { { 0x89abcdef, 1, 0, 0 } };  /* [0] the client game's; [1 + slot] a player's */

static cg_rand_t *cg_now(void)
{
    const int s = SE_Slot();
    return cg_r[1 + s].on ? &cg_r[1 + s] : &cg_r[0];
}

void SE_CGRandFor(int number, int time)
{
    cg_rand_t *r = &cg_r[1 + SE_Slot()];
    uint32_t h = 2166136261u;
    h = (h ^ (uint32_t)number) * 16777619u;
    h = (h ^ (uint32_t)time) * 16777619u;
    h ^= h >> 15;
    r->hold = h, r->state = h * 2654435761u + 1, r->on = 1;
}

void SE_CGRandEnd(void)
{
    cg_r[1 + SE_Slot()].on = 0;
}

float CG_flrand(float min, float max)
{
    cg_rand_t *r = cg_now();
    float result;
    r->hold = r->hold * 214013u + 2531011u;
    result = (float)(r->hold >> 17);
    return ((result * (max - min)) / (float)32768) + min;
}

int CG_irand(int min, int max)
{
    cg_rand_t *r = cg_now();
    max++;
    r->hold = r->hold * 214013u + 2531011u;
    return (int)((((r->hold >> 17) * (unsigned)(max - min)) >> 15) + min);
}

int CG_rand(void)
{
    cg_rand_t *r = cg_now();
    r->state = r->state * 214013u + 2531011u;
    return (int)(r->state >> 16 & 0x7fff);
}
