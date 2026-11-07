// The world's update in slices on the first core (docs/games/aoe.md, "The world in slices"): it runs on a stack of its own
// and gives way between two objects' updates once its slice is spent, so a picture does not wait for a whole update. While
// it is out the world is as one object's update left it: the view's walk and the panels read it, nothing writes it, and an
// input that is not a move finishes the update before it is handled. The world's clock stands still while it is out.
// AOE_WORLD=whole is an update a frame as before; so are AOE_TICK_MS runs and AOE_VIEW=inline (the state sums' runs).
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../include/world.h"
#include "../include/tribegam.h"
#include "aoe_cores.h"

unsigned long aoe_clock_ms(void);

// ra, sp, s0 to s11, then fs0 to fs11: what a call keeps
struct AoeContext { unsigned regs[14]; float fregs[12]; };
extern "C" void aoe_context_swap(AoeContext* from, AoeContext* to);
__asm__(
    ".text\n.globl aoe_context_swap\n.type aoe_context_swap, @function\n"
    "aoe_context_swap:\n"
    "sw ra,0(a0)\n sw sp,4(a0)\n sw s0,8(a0)\n sw s1,12(a0)\n sw s2,16(a0)\n sw s3,20(a0)\n sw s4,24(a0)\n sw s5,28(a0)\n"
    "sw s6,32(a0)\n sw s7,36(a0)\n sw s8,40(a0)\n sw s9,44(a0)\n sw s10,48(a0)\n sw s11,52(a0)\n"
    "fsw fs0,56(a0)\n fsw fs1,60(a0)\n fsw fs2,64(a0)\n fsw fs3,68(a0)\n fsw fs4,72(a0)\n fsw fs5,76(a0)\n"
    "fsw fs6,80(a0)\n fsw fs7,84(a0)\n fsw fs8,88(a0)\n fsw fs9,92(a0)\n fsw fs10,96(a0)\n fsw fs11,100(a0)\n"
    "lw ra,0(a1)\n lw sp,4(a1)\n lw s0,8(a1)\n lw s1,12(a1)\n lw s2,16(a1)\n lw s3,20(a1)\n lw s4,24(a1)\n lw s5,28(a1)\n"
    "lw s6,32(a1)\n lw s7,36(a1)\n lw s8,40(a1)\n lw s9,44(a1)\n lw s10,48(a1)\n lw s11,52(a1)\n"
    "flw fs0,56(a1)\n flw fs1,60(a1)\n flw fs2,64(a1)\n flw fs3,68(a1)\n flw fs4,72(a1)\n flw fs5,76(a1)\n"
    "flw fs6,80(a1)\n flw fs7,84(a1)\n flw fs8,88(a1)\n flw fs9,92(a1)\n flw fs10,96(a1)\n flw fs11,100(a1)\n"
    "ret\n"
    ".size aoe_context_swap, .-aoe_context_swap\n");

static inline unsigned now_time(void) { unsigned t; __asm__ volatile("rdtime %0" : "=r"(t)); return t; }   // 5 kHz

enum { STACK = 512 << 10 };
static struct {
    int mode;                 // 0 whole, 1 slices
    int running, done;        // an update begun and not ended; ended and not yet taken by handle_game_update
    unsigned char state;      // what world->update() said
    RGE_Game_World* world;
    AoeContext outside, inside;
    char* stack;
    unsigned deadline;        // rdtime at which the slice gives way
    unsigned long out_at, out_ms;   // the clock when it gave way, and how long it has been out in this update
    unsigned update_ticks, least_ticks;   // the time an update should take at most, a slice's least
    unsigned work, last_work;   // the machine's time inside this update, and in the last whole one
    unsigned left_at;           // when the last slice gave way
} w;
int aoe_world_inside;         // the world's update runs on its own stack now (the clock and the yields look at it)
unsigned aoe_world_deadline;

static void body(void) {
    w.state = w.world->update();
    w.done = 1;
    w.running = 0;
    aoe_world_inside = 0;
    aoe_context_swap(&w.inside, &w.outside);
    abort();   // (never resumed after its end)
}

int aoe_world_slices(void) {
    static int read;
    if (!read) {
        read = 1;
        const char* e = getenv("AOE_WORLD");
        const char* v = getenv("AOE_VIEW");
        w.mode = !(e && !strcmp(e, "whole")) && !getenv("AOE_TICK_MS") && !(v && (!strcmp(v, "inline") || !strcmp(v, "whole")))
                 && !getenv("AOE_NOWINDOW");
        // an update in AOE_UPDATE_MS (70) of the real clock, as many pictures as that leaves; a slice at least AOE_SLICE_MS (4)
        const char* f = getenv("AOE_UPDATE_MS");
        const char* s = getenv("AOE_SLICE_MS");
        w.update_ticks = (f ? atoi(f) : 70) * 5;
        w.least_ticks = (s ? atoi(s) : 4) * 5;
        if (w.mode) w.stack = (char*) malloc(STACK);
        if (!w.stack) w.mode = 0;
        if (getenv("AOE_DEBUG")) fprintf(stderr, "aoe: the world %s\n", w.mode ? "in slices" : "whole");
    }
    return w.mode;
}

void aoe_world_picture(void) {}

static void resume(unsigned deadline) {
    aoe_world_deadline = deadline;
    if (w.running && w.out_at) { w.out_ms += aoe_clock_ms() - w.out_at; w.out_at = 0; }
    unsigned t = now_time();
    aoe_world_inside = 1;
    aoe_context_swap(&w.outside, &w.inside);
    aoe_world_inside = 0;
    w.left_at = now_time();
    w.work += w.left_at - t;
    if (w.running) w.out_at = aoe_clock_ms();
}

// handle_game_update's update: 1 and its state when one has ended, 0 while it is still out
int aoe_world_run(RGE_Game_World* world, unsigned char* state) {
    if (!aoe_world_slices()) { *state = world->update(); return 1; }
    if (!w.running && !w.done) {
        w.world = world;
        w.running = 1;
        w.out_at = w.out_ms = 0;
        w.work = 0;
        memset(&w.inside, 0, sizeof w.inside);
        w.inside.regs[0] = (unsigned) body;
        w.inside.regs[1] = ((unsigned) (w.stack + STACK) - 64) & ~15u;
    }
    if (w.running) {
        // The world's share of the time is its work over the update's time (the last update's work): the slice is the
        // time outside it since the last slice in that ratio. More work than the update's time: the update whole.
        unsigned t = now_time(), outside = t - w.left_at, slice;
        if (outside > w.update_ticks) outside = w.update_ticks;
        if (w.last_work * 10 >= w.update_ticks * 9) slice = 0x7fffffff;
        else slice = (unsigned) ((unsigned long long) outside * w.last_work / (w.update_ticks - w.last_work));
        if (slice < w.least_ticks) slice = w.least_ticks;
        resume(t + slice);
    }
    if (!w.done) return 0;
    w.last_work = w.work;
    w.done = 0;
    *state = w.state;
    return 1;
}

// Before an input that is not a move, a save, a quit: the update to its end (handle_game_update takes it as ever)
void aoe_world_finish(void) {
    while (w.running) resume(now_time() + 0x7fffffff);
}

// The script's "save NAME" (host.cpp): the game saved as its Save window saves it, the update first at its end
extern RGE_Base_Game* rge_base_game;
int aoe_script_save(const char* name) {
    char file[80];
    aoe_world_finish();
    snprintf(file, sizeof file, "%s.gam", name);
    return rge_base_game ? ((TRIBE_Game*) rge_base_game)->save_game(file) : 0;
}

// and "load NAME": as the Load window loads savegame/NAME.gam
int aoe_script_load(const char* name) {
    char file[80];
    aoe_world_finish();
    snprintf(file, sizeof file, "%s.gam", name);
    return rge_base_game ? ((TRIBE_Game*) rge_base_game)->load_game(file) : 0;
}

// RGE_Object_List::update, between two objects
void aoe_world_give_way(void) {
    if (aoe_world_inside) aoe_context_swap(&w.inside, &w.outside);
}

// The clock the world sees: still while it was out (an AI's budget in milliseconds is its own work's)
unsigned long aoe_world_clock(unsigned long real) { return aoe_world_inside ? real - w.out_ms : real; }
int aoe_world_busy(void) { return w.running; }
