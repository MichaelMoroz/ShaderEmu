// SPDX-License-Identifier: GPL-3.0-or-later
// ShaderEmu: OpenCNC3D's brain with no picture (docs/games/opencnc3d.md, "The brain"). It starts a
// mission of the cartridge through the CNC_* interface, prints its objects as the engine has them,
// plays TICKS ticks with no input from a fixed seed, and prints a sum of the game's state every
// EVERY ticks. The build computer's program (OPENCNC3D_HOST=1 build.sh) and the machine's must
// print the same lines.
//   opencnc3d-brain CONTENT/ MISSIONS/ SCEN [TICKS [EVERY [SEED]]]     (3000 300 0)
// SEED 0 is the game's own start, RandomClass(0) (brain_shaderemu.cpp); another seeds both generators.
// CONTENT holds the theater's MIX (its palette is all the brain needs of the 1995 data);
// MISSIONS the mission's .INI and .BIN; both end in '/'.
#include <stdio.h>
#include <stdlib.h>

extern "C" int ShaderEmu_Start(const char* content, const char* dir, const char* scen, int build_level, int seed);
extern "C" int ShaderEmu_Advance(void);
extern "C" int ShaderEmu_Dump(void);
extern "C" int ShaderEmu_Frame(void);
extern "C" unsigned ShaderEmu_State_Sum(int* counts);
extern "C" void ShaderEmu_Set_Legacy_Render(int on);

// instructions so far: the machine's own count (rvc_cpu and the card), none on the build computer
static unsigned instructions(void)
{
#if defined(__riscv)
    unsigned n;
    __asm__ volatile("rdinstret %0" : "=r"(n));
    return n;
#else
    return 0;
#endif
}

static void line(const char* what, int tick)
{
    int n[7];
    unsigned s = ShaderEmu_State_Sum(n);
    printf("ocnc: %s tick %d frame %d sum %08x inf %d unit %d bld %d air %d terr %d bullet %d anim %d\n", what, tick,
           ShaderEmu_Frame(), s, n[0], n[1], n[2], n[3], n[4], n[5], n[6]);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 4) {
        fprintf(stderr, "opencnc3d-brain CONTENT/ MISSIONS/ SCEN [TICKS [EVERY [SEED]]]\n");
        return 2;
    }
    int ticks = argc > 4 ? atoi(argv[4]) : 3000;
    int every = argc > 5 ? atoi(argv[5]) : 300;
    int seed = argc > 6 ? atoi(argv[6]) : 0;
    // The 1995 view drawn into the hidden page at every tick: nothing here reads it, so not
    // unless OPENCNC3D_RENDER=1 (the game's state is the same either way: docs/games/opencnc3d.md)
    const char* render = getenv("OPENCNC3D_RENDER");
    ShaderEmu_Set_Legacy_Render(render && atoi(render));
    printf("ocnc: the 1995 view %s\n", render && atoi(render) ? "drawn every tick" : "not drawn");
    unsigned i0 = instructions();
    if (!ShaderEmu_Start(argv[1], argv[2], argv[3], 1, seed)) {
        printf("ocnc: %s did not start\n", argv[3]);
        return 1;
    }
    unsigned i1 = instructions();
    printf("ocnc: started %s, %u instructions\n", argv[3], i1 - i0);
    ShaderEmu_Dump();
    line("start", 0);
    unsigned best = ~0u, worst = 0, since = instructions();
    unsigned long long sum = 0;
    int t;
    for (t = 1; t <= ticks; t++) {
        unsigned a = instructions();
        if (!ShaderEmu_Advance()) {
            printf("ocnc: advance said no at tick %d\n", t);
            break;
        }
        unsigned d = instructions() - a;
        sum += d;
        if (d < best)
            best = d;
        if (d > worst)
            worst = d;
        if (t % every == 0) {
            line("at", t);
            printf("ocnc: ticks %d-%d %u instructions a tick (fewest %u, most %u)\n", t - every + 1, t,
                   (instructions() - since) / every, best, worst);
            best = ~0u;
            worst = 0;
            since = instructions();
        }
    }
    printf("ocnc: %d ticks, %u instructions a tick\n", t - 1, t > 1 ? (unsigned)(sum / (unsigned)(t - 1)) : 0);
    printf("ocnc: done\n");
    return 0;
}
