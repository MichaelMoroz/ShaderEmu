// The OpenGL 1.1 OpenJK's renderer (code/rd-vanilla) calls, on the machine's GPU (docs/games/jk2.md, "The picture"):
// Half-Life's (linux/halflife/gl_shaderemu.c, included as it is) over Quake III's draws (linux/quake3/gpu_shaderemu.c).
// build.sh renames the renderer's calls glX to jk2_glX; here each goes to that file's s_glX once the window is open
// (jk2glOpen), and before that, or with no window at all (a run with no Nano-X server: the sums), does what nullgl.c
// does, so that the game is the same with a picture or without.
#include "../halflife/gl_shaderemu.c"
#include "mcwx.h"

// what gl_shaderemu.c asks of Half-Life's drawing on a worker, which this game does not have
int SE_RenderWho(void) { return 0; }
int SE_RenderOut(void) { return 0; }
uint32_t SE_RenderOnCore0(uint32_t (*fn)(void*), void* args) { return fn(args); }
// (se_asks, which gl_shaderemu.c names in a block of its own, is defined in sys_shaderemu.cpp)

static int on;   // the window is open and the GPU draws
int jk2glOn(void) { return on; }

// ---- the drawing on a worker, a frame behind (docs/games/jk2.md, "The drawing on a worker") ----
// At a frame's end (R_IssueRenderCommands) rd-vanilla's commands are given to a worker of their own, which runs its
// back end (RB_ExecuteRenderCommands: the shaders' stages, Ghoul2's skinning, this file's GL) on a stack of its own
// while this core makes the next frame into the other backEndData. The frame is shown (the GPU's list swapped) by this
// core when the job is done, at the next give or any settle. Any GL call this core makes while the job is out (a
// texture uploaded, a sync of the renderer's) settles it first. JK2_RENDER=core: the drawing this core's, as before.
uint32_t jk2_call_on_stack(uint32_t (*fn)(uint32_t), uint32_t arg, void* stack_top);
__asm__(".text\n.globl jk2_call_on_stack\n.type jk2_call_on_stack, @function\njk2_call_on_stack:\n"
        "	addi a2, a2, -16\n	sw ra, 12(a2)\n	sw sp, 8(a2)\n	mv sp, a2\n	mv t0, a0\n	mv a0, a1\n	jalr t0\n"
        "	lw ra, 12(sp)\n	lw sp, 8(sp)\n	ret\n");
#define RENDER_STACK (256 * 1024)
static uint8_t render_stack[RENDER_STACK] __attribute__((aligned(16)));
static struct __attribute__((aligned(16))) { int mode, worker, out, presenting; unsigned gives, settles, gl_settles, waits; } rw0;  // this core's
static struct __attribute__((aligned(16))) { void (*fn)(const void*); const void* arg; int swap, unused; } job;  // the given frame: the worker's while out
static struct __attribute__((aligned(16))) { volatile unsigned go, unused[3]; } job_go;  // this core's: the frame may begin
static void gl_swap(void);
static void memo_init(void);
static void env_init(void);
static int on_render(void) {
    char here;
    return (uint8_t*)&here >= render_stack && (uint8_t*)&here < render_stack + RENDER_STACK;
}
// ---- the front end of a frame's view on a worker of its own (tr_main.cpp's R_SE_RenderViewSplit and R_SE_FrontJob;
// docs/games/jk2.md, "The front end apart"): posted by this core when it has given the view's command, waited for by
// the drawing's worker at that command (jk2FrontAwait) and by this core where it next writes tr's view (jk2FrontSettle).
// JK2_FRONT_WORKER=0: the drawing's worker makes it, as before ----
#define FRONT_STACK (128 * 1024)
static uint8_t front_stack[FRONT_STACK] __attribute__((aligned(16)));
static struct __attribute__((aligned(16))) { int worker, out, tried, waits; } fw0;   // this core's
static struct __attribute__((aligned(16))) { void (*fn)(void*); void* arg; int unused[2]; } front_job;   // the worker's while out
static int on_front(void) {
    char here;
    return (uint8_t*)&here >= front_stack && (uint8_t*)&here < front_stack + FRONT_STACK;
}
// (0 this core, 1 the drawing's worker, 2 the front end's: tr_main.cpp keeps R_RotateForEntity's scratch by it)
#ifndef SE_DRAW_B
uint8_t* b_jk2glHalfStack(void);   // (the drawing's second part's stack: tr_main.cpp's fourth scratch)
static int on_half(void) {
    char here;
    static uint8_t* at;
    if (!at) at = b_jk2glHalfStack();
    return (uint8_t*)&here >= at && (uint8_t*)&here < at + RENDER_STACK;
}
#else
static int on_half(void) { return 0; }
#endif
int jk2RenderWorkerHere(void) { return on_render() ? 1 : on_front() ? 2 : on_half() ? 3 : 0; }
void jk2ServerServe(void);   // (the game's frame's worker: below)
// ---- a large draw's indices made into the GPU's words by helper workers (docs/games/jk2.md, "The drawing's stores"):
// fast_triangles on the drawing's worker makes each vertex's four words once, into the frame's memory, with the indices
// beside them, takes the draw's command and room (gpuDrawsRoom) and puts a task; two helpers this core posts with the
// frame (jk2RenderGo) write the room, four words an index, each every other task, until the drawing says its frame is
// done and waits for them. The drawing stored those words itself, about three an index more than a vertex's: its write
// cache ended most of its passes. JK2_EXPAND=0: the drawing makes them, as before ----
#define EXP_HELPERS 2
#define EXP_TASKS 1024
#define EXP_LEAST 96
typedef struct { uint32_t* room; const uint32_t* made; const uint16_t* idx; uint32_t count; } exp_task;
static exp_task exp_tasks[EXP_TASKS];   // the drawing's
static struct __attribute__((aligned(16))) { volatile uint32_t put, end, end_gen, unused; } exp_w;   // the drawing's
static struct __attribute__((aligned(16))) { volatile uint32_t gen, unused[3]; } exp_h[EXP_HELPERS];   // each helper's
static struct __attribute__((aligned(16))) { uint32_t gen, base, n, on; mcw_handle h[EXP_HELPERS]; } exp0;   // this core's
static struct __attribute__((aligned(16))) { uint32_t gen, base, n, unused; } exp_cfg;   // this core's, read by the drawing
static uint32_t exp_server(uint32_t base, uint32_t who) {
    const uint32_t gen = exp_cfg.gen;
    uint32_t next = base + who;
    for (;;) {
        const uint32_t put = exp_w.put;
        while ((int32_t)(put - next) > 0) {
            const exp_task* t = &exp_tasks[next % EXP_TASKS];
            uint32_t* to = t->room;
            const uint32_t* made = t->made;
            const uint16_t* idx = t->idx;
            for (uint32_t i = 0; i < t->count; i++, to += 4) {
                const uint32_t* m = &made[idx[i] * 4];
                uint32_t a = m[0], b = m[1], c = m[2], d = m[3];
                to[0] = a, to[1] = b, to[2] = c, to[3] = d;
            }
            next += EXP_HELPERS;
        }
        if (exp_w.end_gen == gen && (int32_t)(exp_w.end - next) <= 0) break;
        mc_end_pass();
    }
    exp_h[who].gen = gen;
    return 0;
}
// (this core, at the frame's give: the helpers of the frame before joined, this frame's posted)
static void exp_give(void) {
    for (uint32_t k = 0; k < exp0.n; k++) mcw_join(exp0.h[k]);
    exp0.n = 0;
    if (!exp0.on) {
        const char* e = getenv("JK2_EXPAND");
        exp0.on = e && !strcmp(e, "0") ? 2 : 1;
        // (what the drawing's worker and the helpers store to: this core's store first, a worker stops at a page no core has)
        mcw_touch(exp_tasks, sizeof exp_tasks), mcw_touch((void*)&exp_w, sizeof exp_w), mcw_touch((void*)exp_h, sizeof exp_h);
    }
    exp_cfg.n = 0;
    if (exp0.on != 1) return;
    exp0.gen++, exp0.base = exp_w.put;   // (the drawing's frame before is done: its count is final)
    exp_cfg.gen = exp0.gen, exp_cfg.base = exp0.base;
    for (uint32_t k = 0; k < EXP_HELPERS; k++) {
        mcw_handle h = mcw_try_call(exp_server, exp0.base, k);
        if (!h) break;
        exp0.h[exp0.n++] = h;
    }
    if (exp0.n < EXP_HELPERS) {   // (too few: none, the drawing makes its words; those posted end at once)
        exp_cfg.n = 0;
        exp_w.end = exp0.base, exp_w.end_gen = exp0.gen;   // (this core's store to the drawing's word: the drawing is done)
        for (uint32_t k = 0; k < exp0.n; k++) mcw_join(exp0.h[k]);
        exp0.n = 0;
        return;
    }
    exp_cfg.n = exp0.n;
}
// (the drawing, at its frame's end: the helpers told and waited for)
static void exp_finish(void) {
    if (!exp_cfg.n) return;
    exp_w.end = exp_w.put, exp_w.end_gen = exp_cfg.gen;
    for (uint32_t k = 0; k < exp_cfg.n; k++)
        while (exp_h[k].gen != exp_cfg.gen) mc_end_pass();
}
static void half_frame(uint32_t given);   // (the drawing on two workers: below)
static void half_none(void);
static uint32_t render_body(uint32_t given) {
    while (job_go.go != given) mc_end_pass();   // (until this core has given the frame's skins out: SE_SkinGive)
    half_frame(given);
    job.fn(job.arg);
    half_none();
    exp_finish();
    return 0;
}
static uint32_t render_job(uint32_t a0, uint32_t a1) {
    (void)a1;
    return jk2_call_on_stack(render_body, a0, render_stack + RENDER_STACK);
}
extern char _start[], etext[];
static void render_open(void) {
    const char* how = getenv("JK2_RENDER");
    rw0.mode = 0;
    if (how && !strcmp(how, "core")) return;
    int n = mcw_open(getenv("JK2_WORKERS") ? atoi(getenv("JK2_WORKERS")) : 32);   // (as sys_shaderemu.cpp's: the first to open decides)
    if (n <= 0) return;
    mcw_touch_code(_start, (unsigned)(etext - _start));   // (a worker stops at a page of code no core has run)
    mcw_touch(render_stack, RENDER_STACK);
    mcw_touch(&job, sizeof job);
    // (a worker on one of the first 16 cores, with the larger write cache: the drawing stores every vertex's words; the
    // highest such, as mcw_try_call takes the lowest free one first: a job of the slots or the music's that took the
    // drawing's or the mix's worker while it was free held its next frame back)
    rw0.worker = n;
    for (int k = n; k >= 1; k--)
        if (mcw_fast(k)) { rw0.worker = k; break; }
    rw0.mode = how && !strcmp(how, "menus") ? 2 : 1;
    printf("jk2: the drawing on worker %d (%s write cache), a frame behind\n", rw0.worker, mcw_fast(rw0.worker) ? "a large" : "a small");
}
// (in play only: a menu's few draws of many vertices are faster on this core, whose write cache is the larger)
int SE_InPlay(void);
// (JK2_RENDER=menus: the menus' frames too)
int jk2RenderOn(void) { return on && rw0.mode && (rw0.mode == 2 || SE_InPlay()); }
// The front end's view done (where this core writes tr's view or what the front end reads)
void jk2FrontSettle(void) {
    if (!fw0.out || on_front() || on_render()) return;
    while (!mcw_done(fw0.worker)) jk2ServerServe(), mcw_rest_for(&fw0.worker, 1), fw0.waits++;
    fw0.out = 0;
}
// The frame out done and shown (its front end not waited for: at the give, the drawing waits for it itself)
void jk2RenderSettleBack(void) {
    if (!rw0.out || on_render() || on_front()) return;
    while (!mcw_done(rw0.worker)) jk2ServerServe(), mcw_rest_for(&rw0.worker, 1), rw0.waits++;
    rw0.out = 0, rw0.settles++;
    if (job.swap) job.swap = 0, gl_swap();
}
// The frame out done and shown, and the front end's view
void jk2RenderSettle(void) {
    jk2FrontSettle();
    jk2RenderSettleBack();
}
// (a GL call of this core's while the frame is out: the drawing's state is the worker's until it is done)
#define SETTLE() do { if (rw0.out && !on_render()) rw0.gl_settles++, jk2RenderSettle(); } while (0)
#ifdef SE_DRAW_B
#define HALF_WORKER (-1)
#else
#define HALF_WORKER hw0.worker
static int half_on(void);
static void half_settle(void);
static void half_post(uint32_t gen);
static uint32_t half_job(uint32_t gen, uint32_t a1);
static struct __attribute__((aligned(16))) { int worker, out, tried, unused; } hw0;   // this core's (the drawing on two workers: below)
#endif
void jk2RenderGive(void (*fn)(const void*), const void* arg) {
    // (the frame before done; not this frame's front end, which the drawing waits for itself and this core where it next
    // writes tr's view: jk2RenderSettle here held this core about 17 passes a frame of the walk)
    jk2RenderSettleBack();
    job.fn = fn, job.arg = arg, job.swap = 0;
    mcw_post(rw0.worker, render_job, rw0.gives + 1, 0);
#ifndef SE_DRAW_B
    half_post(rw0.gives + 1);   // (after the drawing's, whose worker it must not take; the drawing begins after job_go)
#endif
    rw0.out = 1, rw0.gives++;
}
// ---- the mix painted on a worker of its own while the next frame's game runs (sys_shaderemu.cpp's SE_MixGive;
// docs/games/jk2.md, "The mix on a worker") ----
#define MIX_STACK (128 * 1024)
static uint8_t mix_stack[MIX_STACK] __attribute__((aligned(16)));
static struct __attribute__((aligned(16))) { int worker, out, waits, unused; } mw0;   // this core's
static struct __attribute__((aligned(16))) { void (*fn)(void); int unused[3]; } mix_job;   // the worker's while out
static uint32_t mix_body(uint32_t unused) {
    (void)unused;
    mix_job.fn();
    return 0;
}
static uint32_t mix_post(uint32_t a0, uint32_t a1) {
    (void)a0, (void)a1;
    return jk2_call_on_stack(mix_body, 0, mix_stack + MIX_STACK);
}
int jk2MixOn(void) {
    if (!mw0.worker && rw0.mode) {   // (a worker of the larger caches other than the drawing's)
        const char* how = getenv("JK2_MIX");
        mw0.worker = -1;
        if (!(how && !strcmp(how, "core"))) {
            for (int k = mcw_count(); k >= 1; k--)   // (the highest but the drawing's: render_open)
                if (k != rw0.worker && k != HALF_WORKER && mcw_fast(k)) { mw0.worker = k; break; }
            if (mw0.worker > 0) mcw_touch(mix_stack, MIX_STACK), mcw_touch(&mix_job, sizeof mix_job);
        }
    }
    return on && mw0.worker > 0;
}
int jk2MixWorkerHere(void);
int jk2MixOut(void) { return mw0.out; }   // (snd_dma.cpp's se_snd_defer: a sound started now waits for the mix)
void jk2MixSettle(void) {
    if (!mw0.out || jk2MixWorkerHere() || on_render() || on_front()) return;   // (only this core waits for the mix)
    while (!mcw_done(mw0.worker)) jk2ServerServe(), mcw_rest_for(&mw0.worker, 1), mw0.waits++;
    mw0.out = 0;
}
unsigned jk2MixWaits(void) {
    unsigned w = mw0.waits;
    mw0.waits = 0;
    return w;
}
void jk2MixGive(void (*fn)(void)) {
    jk2MixSettle();
    mix_job.fn = fn;
    mcw_post(mw0.worker, mix_post, 0, 0);
    mw0.out = 1;
}

// ---- the game's next frame on a worker of its own beside the client's (JK2_SPLIT=worker: cl_main.cpp's CL_Frame,
// docs/games/jk2.md, "The game's frame apart"). What only this core may do (allocations, the zone, cvars, commands,
// files) the worker asks for and waits (jk2ServerOnCore0): this core makes it wherever it waits ----
#define SERVER_STACK (512 * 1024)   // (se_slot.h's SE_SERVER_STACK: after the job slots' stacks, so SE_Core() knows it)
extern uint8_t* const se_server_stack;
#define server_stack se_server_stack
static struct __attribute__((aligned(16))) { int worker, out, waits, asks; } sw0;   // this core's
static struct __attribute__((aligned(16))) { void (*fn)(void*); void* arg; int unused[2]; } server_job;   // the worker's while out
static struct __attribute__((aligned(16))) { volatile unsigned seq; uint32_t (*fn)(void*); void* args; unsigned unused; } sv_ask;   // the worker's
static struct __attribute__((aligned(16))) { volatile unsigned seq; uint32_t result; unsigned unused[2]; } sv_answer;   // this core's
int jk2ServerHere(void) {
    char here;
    return (uint32_t)((uint8_t*)&here - server_stack) < SERVER_STACK;
}
void SE_ArenaOpen(void);
void SE_ArenaDrain(void);
static uint32_t server_body(uint32_t unused) {
    (void)unused;
    SE_ArenaDrain();   // (the blocks other cores freed since its last frame, at a fixed point)
    server_job.fn(server_job.arg);
    return 0;
}
static uint32_t server_post(uint32_t a0, uint32_t a1) {
    (void)a0, (void)a1;
    return jk2_call_on_stack(server_body, 0, server_stack + SERVER_STACK);
}
int jk2ServerOn(void) {
    if (!sw0.worker && rw0.mode) {   // (a worker of the larger caches other than the drawing's and the mix's)
        sw0.worker = -1;
        jk2MixOn();
        for (int k = mcw_count(); k >= 1; k--)
            if (k != rw0.worker && k != mw0.worker && k != fw0.worker && k != HALF_WORKER && mcw_fast(k)) { sw0.worker = k; break; }
        if (sw0.worker > 0) {
            mcw_touch(server_stack, SERVER_STACK), mcw_touch(&server_job, sizeof server_job);
            mcw_touch(&sv_ask, sizeof sv_ask);
            SE_ArenaOpen();   // (its allocations from an arena of its own: sys_shaderemu.cpp)
            printf("jk2: the game's frame on worker %d beside the client's\n", sw0.worker);
        }
    }
    return on && sw0.worker > 0;
}
uint32_t jk2ServerOnCore0(uint32_t (*fn)(void*), void* args) {
    if (!jk2ServerHere()) return fn(args);
    sv_ask.fn = fn, sv_ask.args = args;
    mc_publish();   // (the call before its number)
    sv_ask.seq = sv_ask.seq + 1;
    while (sv_answer.seq != sv_ask.seq) mc_end_pass();
    return sv_answer.result;
}
// What the worker asks made now, if it asks (and its stop at a page or a system call served): wherever this core waits
void jk2ServerServe(void) {
    if (!sw0.out) return;
    if (sv_ask.seq != sv_answer.seq) {
        sv_answer.result = sv_ask.fn(sv_ask.args);
        sv_answer.seq = sv_ask.seq;
        sw0.asks++;
    }
    mcw_done(sw0.worker);
}
// (an ask of the game's frame's worker answered now, if there is one: this core polls at the client game's calls into the
// engine, cl_cgame.cpp, so the game's frame does not wait for this core's settle at its frame's end)
void jk2ServerPoll(void) {
    if (sw0.out && sv_ask.seq != sv_answer.seq && !mcw_on_worker()) {   // (not a slot's job: this core's alone)
        sv_answer.result = sv_ask.fn(sv_ask.args);
        sv_answer.seq = sv_ask.seq;
        sw0.asks++;
    }
}
void jk2ServerSettle(void) {
    if (!sw0.out) return;
    while (!mcw_done(sw0.worker)) jk2ServerServe(), mcw_rest_for(&sw0.worker, 1), sw0.waits++;
    sw0.out = 0;
}
void jk2ServerGive(void (*fn)(void*), void* arg) {
    jk2ServerSettle();
    static unsigned gives, deepest;   // (JK2_SERVER_DEPTH=1: how deep the frame's stack went, every 100 frames)
    static int depth_on = -1;
    if (depth_on < 0) depth_on = getenv("JK2_SERVER_DEPTH") != NULL;
    if (depth_on && (++gives <= 10 || gives % 100 == 0)) {
        const uint32_t* w = (const uint32_t*)server_stack;
        unsigned k = 0;
        while (k < SERVER_STACK / 4 && !w[k]) k++;
        if (SERVER_STACK - k * 4 > deepest) deepest = SERVER_STACK - k * 4;
        printf("jk2: the game's frame's stack %u bytes deep at most\n", deepest);
    }
    server_job.fn = fn, server_job.arg = arg;
    mcw_post(sw0.worker, server_post, 0, 0);
    sw0.out = 1;
}
void jk2ServerStats(unsigned* waits, unsigned* asks) {
    *waits = sw0.waits, *asks = sw0.asks;
    sw0.waits = sw0.asks = 0;
}

// (the front end's worker: its job, chosen and given; "the front end of a frame's view" above)
static uint32_t front_body(uint32_t unused) {
    (void)unused;
    front_job.fn(front_job.arg);
    return 0;
}
static uint32_t front_post(uint32_t a0, uint32_t a1) {
    (void)a0, (void)a1;
    return jk2_call_on_stack(front_body, 0, front_stack + FRONT_STACK);
}
int jk2FrontOn(void) {
    if (!fw0.tried && rw0.mode && on) {   // (a worker of the larger caches other than the drawing's, the mix's and the game's)
        const char* e = getenv("JK2_FRONT_WORKER");
        fw0.tried = 1, fw0.worker = -1;
        if (!(e && !strcmp(e, "0"))) {
            jk2MixOn();
            for (int k = mcw_count(); k >= 1; k--)
                if (k != rw0.worker && k != mw0.worker && k != sw0.worker && k != HALF_WORKER && mcw_fast(k)) { fw0.worker = k; break; }
        }
        if (fw0.worker > 0) {
            mcw_touch(front_stack, FRONT_STACK), mcw_touch(&front_job, sizeof front_job);
            printf("jk2: the front end of the view on worker %d\n", fw0.worker);
        }
    }
    return on && fw0.worker > 0 && jk2RenderOn();
}
void jk2FrontGive(void (*fn)(void*), void* arg) {
    jk2FrontSettle();
    front_job.fn = fn, front_job.arg = arg;
    mcw_post(fw0.worker, front_post, 0, 0);
    fw0.out = 1;
}
// (the drawing's worker, or this core drawing a frame itself: until the front end's worker has made view seq)
void jk2FrontAwait(volatile unsigned* made, unsigned seq) {
    while ((int)(*made - seq) < 0) mc_end_pass();
}

// The given frame may begin (posted first, so that no helper's job takes its worker)
void jk2RenderGo(void) {
    exp_give();   // (the helpers that make its large draws' words: before it may begin)
    job_go.go = rw0.gives;
}

// ---- Ghoul2's skinning on helper workers (tr_ghoul2.cpp, SE_SkinGive): the frame's surfaces in parts ----
static mcw_handle skin_jobs[16];
static int skin_count;
int jk2SkinPost(uint32_t (*fn)(uint32_t, uint32_t), uint32_t a0, uint32_t a1) {
    if (skin_count >= 16) return 0;
    mcw_handle h = mcw_try_call(fn, a0, a1);
    if (h) skin_jobs[skin_count++] = h;
    return h != 0;
}
void jk2SkinJoin(void) {
    for (int i = 0; i < skin_count; i++) mcw_join(skin_jobs[i]);
    skin_count = 0;
}
// ---- the handover's helpers (codeJK2/game/g_main.cpp, SE_GameGive and SE_GameTake): ranges of entities on lent workers
// while this core makes the Ghoul2 copies; 0 when no worker was free (the caller makes the range itself) ----
static mcw_handle hand_jobs[8];
static int hand_count;
int jk2HandPost(uint32_t (*fn)(uint32_t, uint32_t), uint32_t a0, uint32_t a1) {
    if (hand_count >= 8 || !rw0.mode) return 0;
    mcw_handle h = mcw_try_call(fn, a0, a1);
    if (h) hand_jobs[hand_count++] = h;
    return h != 0;
}
void jk2HandJoin(void) {
    for (int i = 0; i < hand_count; i++) mcw_join(hand_jobs[i]);
    hand_count = 0;
}
// ---- the mix's parts (snd_mix.cpp, "The mix in parts"): helpers posted at S_Update, joined at the next ----
static mcw_handle mixpart_jobs[4];
static int mixpart_count;
int jk2MixPartPost(uint32_t (*fn)(uint32_t, uint32_t), uint32_t a0, uint32_t a1) {
    if (mixpart_count >= 4 || !rw0.mode) return 0;
    mcw_handle h = mcw_try_call(fn, a0, a1);
    if (h) mixpart_jobs[mixpart_count++] = h;
    return h != 0;
}
void jk2MixPartJoin(void) {
    for (int i = 0; i < mixpart_count; i++) mcw_join(mixpart_jobs[i]);
    mixpart_count = 0;
}
void jk2Touch(void* memory, unsigned bytes) { mcw_touch(memory, bytes); }
void jk2WaitPass(void) { mc_end_pass(); }
// The worker's RB_SwapBuffers: whether the swap is left to this core (the worker draws); else the window's own
int jk2RenderSwapMark(void) {
    if (!on_render()) return 0;
    job.swap = 1;
    return 1;
}
// The window's frame (its input, keys and counts) once a frame is given, its GPU list shown at the settle
void SE_WindowFrame(void);
void jk2RenderPresent(void) {
    rw0.presenting = 1;
    SE_WindowFrame();
    rw0.presenting = 0;
}
// The worker's state served while this core waits elsewhere (a page or a system call it stopped at)
void jk2RenderServe(void) {
    if (rw0.out) mcw_done(rw0.worker);
}
void jk2RenderStats(unsigned* gives, unsigned* settles, unsigned* gl_settles, unsigned* waits) {
    *gives = rw0.gives, *settles = rw0.settles, *gl_settles = rw0.gl_settles, *waits = rw0.waits;
    rw0.gives = rw0.settles = rw0.gl_settles = rw0.waits = 0;
}

int jk2glOpen(unsigned window) {
    if (on) return 0;
    if (!getenv("HLGL_WORKERS")) setenv("HLGL_WORKERS", "0", 0);   // (none yet: textures are converted by this core)
    if (!getenv("HL_POOL_MB")) setenv("HL_POOL_MB", "36", 0);   // (the demo's level and menus at r_picmip 2: 31.6 MB)
    if (hlglOpen(window) < 0) return -1;
    on = 1;
    render_open();
    memo_init();
    env_init();
    return 0;
}
static int depth_seen;   // a draw of this frame was depth-tested (fast_triangles: what is drawn before it is behind it)
static int gl_frames, gl_log_at = -1;
static void gl_swap(void) {
    hlglSwap();
    depth_seen = 0;
    if (gl_log_at < 0) gl_log_at = getenv("JK2_GL_LOG") ? atoi(getenv("JK2_GL_LOG")) : 0;
    gl_frames++;
}
// JK2_HOLD=F (sys_shaderemu.cpp, at the game's frame F): the frame given last drawn and shown, its list the last in RAM
// and done, and the game held there, for a capture (rvc_cpu --until "jk2: holding" --save-ram, tools/gpu_reference.py)
void jk2glHold(int frame) {
    jk2RenderSettle();
    if (on) seglSettle();
    printf("jk2: holding at frame %d, swap %d\n", frame, gl_frames);
    fflush(stdout);
    for (;;) sleep(1);
}
void jk2glSwap(void) {
    if (!on || rw0.presenting) return;   // (a frame given to the worker: shown when it is done)
    SETTLE();
    gl_swap();
}
void jk2glStats(unsigned* in_gpu, unsigned* gpu_size, unsigned* left_out, unsigned* vertices) {
    unsigned tiles;
    jk2RenderSettle();   // (the counts are the drawing's: RVC_RACES found this core clearing one the worker added to)
    if (on) hlglStats(&tiles, in_gpu, gpu_size, left_out, vertices);
    else *in_gpu = *gpu_size = *left_out = *vertices = 0;
}

typedef unsigned int GLenum;
typedef unsigned int GLbitfield;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;
typedef unsigned char GLubyte;

#define ON_THEN(call) do { if (on) { SETTLE(); call; } } while (0)
void jk2_glAlphaFunc(GLenum f, GLclampf r) { (void)f, (void)r; }
void jk2_glArrayElement(GLint i) { ON_THEN(s_glArrayElement(i)); }
void jk2_glBegin(GLenum m) { ON_THEN(s_glBegin(m)); }
void jk2_glEnd(void) { ON_THEN(s_glEnd()); }
void jk2_glBindTexture(GLenum t, GLuint n) { ON_THEN(s_glBindTexture(t, n)); }
void jk2_glBlendFunc(GLenum s, GLenum d) { ON_THEN(s_glBlendFunc(s, d)); }
void jk2_glCallList(GLuint l) { (void)l; }
void jk2_glClear(GLbitfield m) { ON_THEN(s_glClear(m)); }
void jk2_glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) { ON_THEN(s_glClearColor(r, g, b, a)); }
void jk2_glClearDepth(GLclampd d) { (void)d; }
void jk2_glClearStencil(GLint s) { (void)s; }
void jk2_glClipPlane(GLenum p, const GLdouble* e) { (void)p, (void)e; }
void jk2_glColor3f(GLfloat r, GLfloat g, GLfloat b) { ON_THEN(s_glColor3f(r, g, b)); }
void jk2_glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { ON_THEN(s_glColor4f(r, g, b, a)); }
void jk2_glColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a) { ON_THEN(s_glColor4ub(r, g, b, a)); }
void jk2_glColor4ubv(const GLubyte* v) { ON_THEN(s_glColor4ubv(v)); }
void jk2_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) { (void)r, (void)g, (void)b, (void)a; }
void jk2_glColorPointer(GLint size, GLenum type, GLsizei stride, const void* p) { ON_THEN(s_glColorPointer(size, type, stride, p)); }
void jk2_glCopyTexImage2D(GLenum t, GLint l, GLenum f, GLint x, GLint y, GLsizei w, GLsizei h, GLint b) {}
void jk2_glCopyTexSubImage2D(GLenum t, GLint l, GLint xo, GLint yo, GLint x, GLint y, GLsizei w, GLsizei h) {}
void jk2_glCullFace(GLenum m) { (void)m; }
void jk2_glDeleteLists(GLuint l, GLsizei r) { (void)l, (void)r; }
void jk2_glDeleteTextures(GLsizei n, const GLuint* t) { ON_THEN(s_glDeleteTextures(n, t)); }
void jk2_glDepthFunc(GLenum f) { (void)f; }
void jk2_glDepthMask(GLboolean f) { (void)f; }
void jk2_glDepthRange(GLclampd n, GLclampd f) { (void)n, (void)f; }
void jk2_glDisable(GLenum c) { ON_THEN(s_glDisable(c)); }
void jk2_glEnable(GLenum c) { ON_THEN(s_glEnable(c)); }
void jk2_glDisableClientState(GLenum a) { ON_THEN(s_glDisableClientState(a)); }
void jk2_glEnableClientState(GLenum a) { ON_THEN(s_glEnableClientState(a)); }
void jk2_glDrawArrays(GLenum m, GLint first, GLsizei count) { ON_THEN(s_glDrawArrays(m, first, count)); }
void jk2_glDrawBuffer(GLenum m) { (void)m; }
// (gl_shaderemu.c's primitive holds MAX_PRIMITIVE vertices: the renderer's triangles in pieces of whole triangles)
// rd-vanilla's triangles (docs/games/jk2.md, "Fewer instructions"): its arrays as it always sets them (positions of
// floats, two float coordinates, colours of four bytes) and 32-bit indices, drawn as draw_vertices() draws them (the
// same words), but each vertex made once, over the range of vertices the indices name, and an index then a copy of
// its vertex's four words: about 20 instructions an index of a model against 165, where every index copied a vertex
// twice on the way. The coordinates' least and most and whether all have one colour are over that range (rd-vanilla's
// arrays hold the surfaces' vertices, all of them named). Anything else, and a pulled offset, goes the plain way.
// JK2_GL=plain: always.
static int fast_off = -1, fast_passes;
static uint32_t made[MAX_PRIMITIVE * 4] __attribute__((aligned(16)));   // a vertex's words, from the range's first
static int fast_ok(void) {
    if (fast_off < 0) {
        const char* g = getenv("JK2_GL");
        fast_off = g && !strcmp(g, "plain");
        fast_passes = g && !strcmp(g, "passes");
    }
    return !fast_off && va_pos.on && va_pos.type == GL_FLOAT && va_pos.size >= 3 && va_coord.on && va_coord.type == GL_FLOAT &&
        va_coord.size >= 2 && va_colour.on && va_colour.type == GL_UNSIGNED_BYTE && va_colour.size == 4 && !(offsetting && offset_pull != 0);
}
// A large draw that is as it was (docs/games/jk2.md, "Fewer instructions"): the menu's turning logo is 4,320 indices of
// 1,399 vertices whose first stage (its colour, its own coordinates) is the same every frame, the model turning by the
// matrix. Its GPU words are kept in GPU memory of their own (hlglKeptMemory), with what they were made of, and a draw of
// the same state, indices, vertices, coordinates and colours is drawn from there: a comparison of its inputs, about a
// third of making it again, and none of its 69 KB stored. Draws of 600 indices and more, the last 8. JK2_GL=nomemo: not.
#define MEMO_SLOTS 8
#define MEMO_LEAST 600
typedef struct { int count, n, far, frame; uint32_t keep, colour, ka, kb; unsigned used; gpu_state s; uint32_t *in, *idx; } memo_t;
// The arrays' name, when rd-vanilla says what they hold (jk2glKey: tess filled with one static model's surface at a frame,
// tr_surface.cpp's RB_SurfaceMesh): a kept draw made of the same is the same but for its coordinates and colours, which
// are still compared (0: no name, everything compared)
static uint32_t key_a, key_b;
static uint32_t key_gen = 1;   // (raised when the models are made again: a surface's place may be another's then)
void jk2glKey(const void* surface, int frame) { key_a = (uint32_t)(uintptr_t)surface, key_b = surface ? (uint32_t)frame ^ key_gen << 20 : 0; }
void jk2glKeysForget(void) { key_gen++; }
static memo_t memo[MEMO_SLOTS];
static uint8_t* memo_gpu;
static int memo_state = -1;   // -1 not yet asked, 0 off, 1 on
static unsigned memo_clock;
// (*shape: a kept draw of the same state and size whose words differ: one that changes every frame, as the logo's
// second stage, which takes its coordinates from the eye)
static memo_t* memo_find(const gpu_state* s, int count, int n, int far, uint32_t keep, const uint8_t* pos, int ps,
                         const uint8_t* tc, int ts, const uint8_t* col, int cs, const uint32_t* idx, uint32_t lo, memo_t** shape) {
    *shape = 0;
    for (int e = 0; e < MEMO_SLOTS; e++) {
        memo_t* m = &memo[e];
        if (!m->used || m->count != count || m->n != n || m->far != far || m->keep != keep || memcmp(&m->s, s, sizeof *s)) continue;
        *shape = m;
        const uint32_t* in = m->in;
        int k = 0;
        if (key_a && m->ka == key_a && m->kb == key_b) {   // (the same model's surface and frame: its positions and indices)
            for (; k < n; k++, in += 6) {
                const uint32_t* t = (const uint32_t*)(tc + (size_t)k * ts);
                if (t[0] != in[3] || t[1] != in[4] || *(const uint32_t*)(col + (size_t)k * cs) != in[5]) break;
            }
            if (k == n) return m;
            continue;
        }
        for (; k < n; k++, in += 6) {
            const uint32_t* p = (const uint32_t*)(pos + (size_t)k * ps);
            const uint32_t* t = (const uint32_t*)(tc + (size_t)k * ts);
            if (p[0] != in[0] || p[1] != in[1] || p[2] != in[2] || t[0] != in[3] || t[1] != in[4] ||
                *(const uint32_t*)(col + (size_t)k * cs) != in[5]) break;
        }
        if (k < n) continue;
        int i = 0;
        for (; i < count && idx[i] - lo == m->idx[i]; i++) {
        }
        if (i == count) return m;
    }
    return 0;
}
// The slot a new large draw is kept in: the one used longest ago
// (made on this core when the window opens: the drawing may be on a worker, which may not malloc)
static void memo_init(void) {
    const char* g = getenv("JK2_GL");
    memo_state = 0;
    if (!(g && !strcmp(g, "nomemo")) && (memo_gpu = hlglKeptMemory(MEMO_SLOTS * MAX_PRIMITIVE * 16))) {
        memo_state = 1;
        for (int e = 0; e < MEMO_SLOTS && memo_state; e++)
            if (!(memo[e].in = malloc(MAX_PRIMITIVE * 6 * 4)) || !(memo[e].idx = malloc(MAX_PRIMITIVE * 4))) memo_state = 0;
        for (int e = 0; e < MEMO_SLOTS && memo_state; e++)
            mcw_touch(memo[e].in, MAX_PRIMITIVE * 6 * 4), mcw_touch(memo[e].idx, MAX_PRIMITIVE * 4);
        mcw_touch(memo_gpu, MEMO_SLOTS * MAX_PRIMITIVE * 16);
    }
}
static memo_t* memo_slot(void) {
    if (memo_state <= 0) return 0;
    memo_t* best = &memo[0];
    for (int e = 1; e < MEMO_SLOTS; e++)
        if (memo[e].used < best->used) best = &memo[e];
    return best;
}

static int arrays_ok(void) {   // (JK2_ARRAYS=0: not; fast_triangles)
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("JK2_ARRAYS");
        on = !(e && !strcmp(e, "0"));
    }
    return on;
}
// (the colour as the plain way makes it of four bytes: alpha's complement on top)
#define WORD_OF(c) ((255u - ((c) >> 24)) << 24 | ((c) & 0xff) << 16 | ((c) & 0xff00) | ((c) >> 16 & 0xff))
// ---- rd-vanilla's arrays made where the GPU reads them (docs/games/jk2.md, "The arrays where they are made"; tr_shade.cpp's
// SE_TessBegin and SE_StageRooms): its positions, a stage's colours and coordinates in rooms of the frame's GPU memory, which
// a draw points at instead of copying them; how many vertices its arrays hold, which spares the search of the indices for
// their range. On the drawing's worker only, which builds the list.
// JK2_ROOMS=0: none, the arrays copied as before; =check: the range searched too, and a line if an index lies past the count.
static int rooms_mode = -1;   // 0 off, 1 on, 2 check
static const void *room_xyz, *room_col, *room_tc0, *room_tc1;
static int va_count;          // the vertex arrays' vertices, as rd-vanilla says (0: not said, searched)
static int rooms_on(void) {
    if (rooms_mode < 0) {
        const char* e = getenv("JK2_ROOMS");
        rooms_mode = e && !strcmp(e, "0") ? 0 : e && !strcmp(e, "check") ? 2 : 1;
    }
    return rooms_mode;
}
static unsigned rooms_given, rooms_refused, rooms_uncut;
static int rooms_frame;   // (JK2_ROOMS_LOG=N: a line every N frames)
static int rooms_log = -1;
void gpuUsedLast(unsigned* u);
unsigned int seglFrameLeft(void);
static unsigned rooms_least_left = 0xffffffffu;   // (the least of the frame's memory left seen)
static void rooms_count(void) {
    if (rooms_log < 0) rooms_log = getenv("JK2_ROOMS_LOG") ? atoi(getenv("JK2_ROOMS_LOG")) : 0;
    if (rooms_log > 0) {
        const unsigned left = seglFrameLeft();
        if (left < rooms_least_left) rooms_least_left = left;
    }
    if (rooms_log > 0 && gl_frames != rooms_frame) {
        unsigned used[4];
        gpuUsedLast(used);
        if (gl_frames % rooms_log == 0)
            printf("jk2rooms%s: frame %d, %u rooms, %u refused, %u not cut; least left %u KB; blocks refused %u, at the last merge %u blocks, %u KB, %u commands\n",
                   rw0.mode ? "" : "-b", gl_frames, rooms_given, rooms_refused, rooms_uncut, rooms_least_left >> 10, used[0], used[1], used[2], used[3]),
                rooms_given = rooms_refused = rooms_uncut = 0, rooms_least_left = 0xffffffffu;
        rooms_frame = gl_frames;
    }
}
void* jk2glTessRoom(unsigned bytes) {
    if (!on || !rooms_on() || !on_render() || !arrays_ok()) return 0;
    rooms_count();
    void* probe = gpuFrameMemory(bytes);   // (a refused room is copied by the draw, which takes more: no room kept back)
    if (!probe) {
        rooms_refused++;
        return 0;
    }
    gpuFrameTrim(probe, bytes);
    rooms_given++;
    return probe;
}
int jk2glTessTrim(void* at, unsigned bytes) {
    const int cut = on && at ? gpuFrameTrim(at, bytes) : 0;
    if (!cut) rooms_uncut++;
    return cut;
}
void jk2glRooms(const void* xyz, const void* colours, const void* tc0, const void* tc1) {
    room_xyz = xyz, room_col = colours, room_tc0 = tc0, room_tc1 = tc1;
}
void jk2glVertexCount(int n) { va_count = rooms_on() ? n : 0; }
static int fast_triangles(int count, const uint32_t* idx) {
    count -= count % 3;
    if (count < 3) return 1;
    uint32_t lo, hi;
    if (va_count > 0 && rooms_mode == 1) {
        lo = 0, hi = (uint32_t)va_count - 1;   // (every index names one of the arrays' vertices)
    } else {
        lo = idx[0], hi = idx[0];
        for (int i = 1; i < count; i++) {
            uint32_t k = idx[i];
            if (k < lo) lo = k;
            if (k > hi) hi = k;
        }
        if (va_count > 0 && hi >= (uint32_t)va_count) {
            static int said;
            if (!said++) printf("jk2gl: an index %u past the arrays' %d vertices\n", hi, va_count);
        }
        if (va_count > 0) lo = 0, hi = (uint32_t)va_count - 1;   // (check: the same range as without it)
    }
    int n = (int)(hi - lo) + 1;
    if (n > MAX_PRIMITIVE) return 0;
    gpu_state s;
    if (!draw_state(&s)) {
        if (gl_log_at > 0 && gl_log_at == gl_frames) printf("jk2gl: draw %d indices skipped: texture %d not in GPU memory\n", count, (int)bound);
        return 1;
    }
    // Two things the GPU's passes do otherwise than OpenGL (docs/games/jk2.md, "What the menu was missing"):
    // - one depth buffer a frame, and a pass draws after every pass before it whatever the list's order: rd-vanilla's 2D
    //   (z 0, the near plane, mostly depth-tested) covered the 3D a menu draws after it (the turning logo: OpenGL
    //   clears the depth for a scene), which the shim cannot clear. So 2D before the frame's first 3D draw goes to the
    //   depth-tested passes at the far plane (z made -1 under rd-vanilla's ortho of 0 to 1): what comes later in 3D is
    //   in front of it; and 2D after it to the passes without depth: on top, as in the list. JK2_GL=passes: not.
    // - "added" scales by alpha, which GL_ONE, GL_ONE does not: the menu's saber halos, whose alpha is a wave from -1
    //   to 1, were drawn at their alpha, so not at all half the time. Their alpha is made 1 here.
    int far = 0;
    uint32_t keep = 0xffffffffu;
    if (blending && blend_src == GL_ONE && blend_dst == GL_ONE) keep = 0x00ffffffu;
    if (!fast_passes) {
        float m[16];
        seglGetMatrix(GL_PROJECTION, m);	// (columns: rd-vanilla's 2D is an ortho of depth 0 to 1)
        unsigned pass = s.fragment >> 16 & 15;
        if (!(m[10] == -2.0f && m[14] == -1.0f && m[11] == 0.0f && m[15] == 1.0f)) {
            depth_seen = 1;
        } else if (!depth_seen) {
            far = 1;   // 2D before the frame's 3D: behind it
            if (pass & 4) s.fragment -= GPU_PASS(4);
        } else if (!(pass & 4)) {
            s.fragment += GPU_PASS(pass ? 4 : 5);   // 2D after it: on top (OpenGL's 3D had its own depth cleared)
        }
    }
    const uint32_t far_z = bits(-0.9999f);
    if (gl_log_at > 0 && gl_log_at == gl_frames) {   // JK2_GL_LOG=F: frame F's draws, one a line
        float m[16];
        const float* p0 = (const float*)(va_pos.p + (size_t)idx[0] * va_pos.stride);
        seglGetMatrix(GL_PROJECTION, m);
        printf("jk2gl: draw %d indices %d vertices, texture %d, pass %u, depth %d, blend %x %x, far %d, colour %08x, "
               "first %.1f %.1f %.1f, projection %.3f %.3f %.3f %.3f\n", count, n, texturing ? (int)bound : -1,
               s.fragment >> 16 & 15, depth_test, blending ? blend_src : 0, blending ? blend_dst : 0, far,
               *(const uint32_t*)(va_colour.p + (size_t)idx[0] * va_colour.stride), p0[0], p0[1], p0[2], m[0], m[5], m[10], m[14]);
    }
    const uint8_t *pos = va_pos.p + (size_t)lo * va_pos.stride, *tc = va_coord.p + (size_t)lo * va_coord.stride,
                  *col = va_colour.p + (size_t)lo * va_colour.stride;
    int ps = va_pos.stride, ts = va_coord.stride, cs = va_colour.stride;
    memo_t *kept = 0, *shape;
    if (count >= MEMO_LEAST && memo_state != 0 && !replace) {
        if ((kept = memo_find(&s, count, n, far, keep, pos, ps, tc, ts, col, cs, idx, lo, &shape))) {
            gpu_state ks = s;
            ks.colour = kept->colour;
            gpuDrawAt(&ks, gpuAddress(memo_gpu + (size_t)(kept - memo) * MAX_PRIMITIVE * 16), count);
            kept->used = ++memo_clock, kept->frame = gl_frames;
            drawn_vertices += (uint32_t)count;
            if (gl_log_at > 0 && gl_log_at == gl_frames) printf("jk2gl: draw %d indices %d vertices kept, slot %d\n", count, n, (int)(kept - memo));
            return 1;
        }
        if (shape && shape->frame >= gl_frames - 1 && (gl_frames & 15)) {
            shape->frame = gl_frames;   // (it changes from frame to frame: kept as it is, and tried again every 16th frame)
            kept = 0;
        } else {
            kept = shape ? shape : memo_slot();
            if (kept && kept->used && kept->frame >= gl_frames - 1) seglSettle();   // (the list being drawn may read it)
        }
        if (gl_log_at > 0 && gl_log_at == gl_frames) printf("jk2gl: draw %d indices %d vertices not kept (memo %d), slot %d\n", count, n, memo_state, kept ? (int)(kept - memo) : -1);
    }
    gpu_state s0 = s;   // (the state as the memo compares it, before its colour)
    int textured = (s.fragment & 0xff) != GPU_COLOUR;
    float low_s = 0, low_t = 0, high_s = 0, high_t = 0;
    uint32_t first = *(const uint32_t*)col;
    int one = 1;
    // The arrays drawn by the GPU (gpu.h's VERTEX_ARRAYS; docs/games/jk2.md, "The drawing's stores"): the vertices'
    // positions, coordinates and colours copied into the frame's memory as they are, in the one pass that finds their
    // range and whether they are of one colour, and the indices as they are (a memcpy from the arrays' first); the GPU
    // makes each vertex as the words below would have (the coordinates from the same least, 16 bits or a tagged vertex's
    // 12, one colour or each its own), to the pixel. Not for a memo's draw. JK2_ARRAYS=0: the words made here, as before
    uint32_t *P = 0, *T = 0, *C = 0;
    int zp = 0, zt = 0, zc = 0;   // (made where the GPU reads them: rd-vanilla's rooms, pointed at, not copied)
    if (!kept && arrays_ok()) {
        if (rooms_mode) {
            zp = room_xyz && va_pos.p == room_xyz && ps == 16 && gpuFrameHolds(pos, (unsigned)n * 16);
            zt = ((room_tc0 && va_coord.p == room_tc0) || (room_tc1 && va_coord.p == room_tc1)) && ts == 8 &&
                 gpuFrameHolds(tc, (unsigned)n * 8);
            zc = room_col && va_colour.p == room_col && cs == 4 && gpuFrameHolds(col, (unsigned)n * 4);
        }
        if ((P = zp ? (uint32_t*)(uintptr_t)pos : gpuFrameMemory((unsigned)n * 16)) &&
            (T = zt ? (uint32_t*)(uintptr_t)tc : gpuFrameMemory((unsigned)n * 8)))
            C = zc ? (uint32_t*)(uintptr_t)col : gpuFrameMemory((unsigned)n * 4);
    }
    if (C && zp && zt && zc) {   // (all three where they are: their range and colours only looked at)
        const float* t = (const float*)tc;
        const uint32_t* c = (const uint32_t*)col;
        low_s = high_s = t[0], low_t = high_t = t[1];
        for (int k = 0; k < n; k++, t += 2) {
            const float ss = t[0], tt = t[1];
            if (ss < low_s) low_s = ss;
            if (ss > high_s) high_s = ss;
            if (tt < low_t) low_t = tt;
            if (tt > high_t) high_t = tt;
            one &= c[k] == first;
        }
        if (!textured) low_s = low_t = high_s = high_t = 0;
        else if (!(s.fragment & GPU_CLAMPED)) low_s = floor_of(low_s), low_t = floor_of(low_t);
        else low_s = low_t = 0;
    } else if (C) {
        const float* t0 = (const float*)tc;
        low_s = high_s = t0[0], low_t = high_t = t0[1];
        for (int k = 0; k < n; k++) {
            const uint32_t* p = (const uint32_t*)(pos + (size_t)k * ps);
            const float* t = (const float*)(tc + (size_t)k * ts);
            const uint32_t c = *(const uint32_t*)(col + (size_t)k * cs);
            uint32_t *pk = P + 4 * k, *tk = T + 2 * k;
            const float ss = t[0], tt = t[1];
            if (!zp) pk[0] = p[0], pk[1] = p[1], pk[2] = p[2];
            if (!zt) tk[0] = ((const uint32_t*)t)[0], tk[1] = ((const uint32_t*)t)[1];
            if (!zc) C[k] = c;
            if (ss < low_s) low_s = ss;
            if (ss > high_s) high_s = ss;
            if (tt < low_t) low_t = tt;
            if (tt > high_t) high_t = tt;
            one &= c == first;
        }
        if (!textured) low_s = low_t = high_s = high_t = 0;
        else if (!(s.fragment & GPU_CLAMPED)) low_s = floor_of(low_s), low_t = floor_of(low_t);
        else low_s = low_t = 0;
    } else if (textured) {
        const float* t0 = (const float*)tc;
        low_s = high_s = t0[0], low_t = high_t = t0[1];
        for (int k = 1; k < n; k++) {
            const float* t = (const float*)(tc + (size_t)k * ts);
            float ss = t[0], tt = t[1];
            if (ss < low_s) low_s = ss;
            if (ss > high_s) high_s = ss;
            if (tt < low_t) low_t = tt;
            if (tt > high_t) high_t = tt;
            one &= *(const uint32_t*)(col + (size_t)k * cs) == first;
        }
        if (!(s.fragment & GPU_CLAMPED)) low_s = floor_of(low_s), low_t = floor_of(low_t);
        else low_s = low_t = 0;
    } else {
        for (int k = 1; k < n && one; k++) one = *(const uint32_t*)(col + (size_t)k * cs) == first;
    }
    first = WORD_OF(first) & keep;
    if (replace && textured) one = 1, first = 0x00ffffff;
    if (C) {
        const int small = !(one || high_s - low_s >= 4 || high_t - low_t >= 4);
        const int ls = (int)low_s, lt = (int)low_t;
        uint32_t* I = ls >= -2048 && ls <= 2047 && lt >= -2048 && lt <= 2047 && (float)ls == low_s && (float)lt == low_t
                          ? gpuFrameMemory((unsigned)count * 4) : 0;
        if (I) {
            if (lo == 0)
                memcpy(I, idx, (size_t)count * 4);
            else
                for (int i = 0; i < count; i++) I[i] = idx[i] - lo;
            const int all_one = !small;   // (the plain way: every vertex the first's colour, or a replacing texture's white)
            s.vertex = GPU_ARRAYS;
            s.colour = all_one ? first : 0;
            s.more[0] = gpuAddress(P), s.more[1] = gpuAddress(T), s.more[2] = gpuAddress(C);
            s.more[3] = (far ? 1u : 0) | (all_one ? 2u : 0) | (small ? 4u : 0) | (keep != 0xffffffffu ? 8u : 0) | 16u |
                        ((uint32_t)ls & 0xfff) << 8 | ((uint32_t)lt & 0xfff) << 20;
            gpuDrawAt(&s, gpuAddress(I), count);
            drawn_vertices += (uint32_t)count;
            return 1;
        }
    }
    uint32_t* table = 0;
    // (a large draw on the drawing's worker with the frame's helpers: its vertices' words in the frame's memory, where a
    // helper makes the indices' words from them; "a large draw's indices" above)
    uint32_t* mk = made;
    int helped = 0;
    if (!kept && count >= EXP_LEAST && exp_cfg.n && on_render() && exp_w.put - exp_cfg.base < EXP_TASKS - 1 &&
        (one || high_s - low_s >= 4 || high_t - low_t >= 4 || n <= 256)) {
        uint32_t* fm = gpuFrameMemory((unsigned)n * 16);
        if (fm) mk = fm, helped = 1;
    }
    if (one || high_s - low_s >= 4 || high_t - low_t >= 4) {
        s.colour = first;
        for (int k = 0; k < n; k++) {
            const float* p = (const float*)(pos + (size_t)k * ps);
            const float* t = (const float*)(tc + (size_t)k * ts);
            uint32_t* m = &mk[k * 4];
            m[0] = bits(p[0]), m[1] = bits(p[1]), m[2] = far ? far_z : bits(p[2]);
            m[3] = ((uint32_t)(int32_t)((t[0] - low_s) * 1024.0f) & 0xffff) | (uint32_t)(int32_t)((t[1] - low_t) * 1024.0f) << 16;
        }
    } else if (n <= 256) {
        // a vertex's colour is the table's word for its tag, which is its place in the range
        s.vertex = GPU_TAGGED | GPU_TABLE;
        if (!(table = gpuFrameMemory(256 * 4))) return 1;
        s.colour = gpuAddress(table);
        for (int k = 0; k < n; k++) {
            uint32_t c = *(const uint32_t*)(col + (size_t)k * cs);
            const float* p = (const float*)(pos + (size_t)k * ps);
            const float* t = (const float*)(tc + (size_t)k * ts);
            uint32_t* m = &mk[k * 4];
            table[k] = WORD_OF(c) & keep;
            m[0] = bits(p[0]), m[1] = bits(p[1]), m[2] = far ? far_z : bits(p[2]);
            m[3] = ((uint32_t)((t[0] - low_s) * 1024.0f) & 0xfff) | ((uint32_t)((t[1] - low_t) * 1024.0f) & 0xfff) << 12 | (uint32_t)k << 24;
        }
    } else {
        // more vertices than a table has colours: a table each 255 indices, as the plain way
        s.vertex = GPU_TAGGED | GPU_TABLE;
        for (int done = 0; done < count;) {
            int c = count - done > 255 ? 255 : count - done;
            if (!(table = gpuFrameMemory(256 * 4))) return 1;
            s.colour = gpuAddress(table);
            uint32_t* to = gpuDrawRoom(&s, c, c);
            if (!to) return 1;
            for (int i = 0; i < c; i++, to += 4) {
                uint32_t k = idx[done + i] - lo, w = *(const uint32_t*)(col + (size_t)k * cs);
                const float* p = (const float*)(pos + (size_t)k * ps);
                const float* t = (const float*)(tc + (size_t)k * ts);
                table[i] = WORD_OF(w) & keep;
                to[0] = bits(p[0]), to[1] = bits(p[1]), to[2] = far ? far_z : bits(p[2]);
                to[3] = ((uint32_t)((t[0] - low_s) * 1024.0f) & 0xfff) | ((uint32_t)((t[1] - low_t) * 1024.0f) & 0xfff) << 12 | (uint32_t)i << 24;
            }
            done += c;
        }
        drawn_vertices += (uint32_t)count;
        return 1;
    }
    if (helped) {   // (the draw's command and room taken here, its indices beside its vertices; a helper writes the room)
        uint16_t* ix = gpuFrameMemory((unsigned)count * 2);
        uint32_t* room = ix ? gpuFrameMemory((unsigned)count * 16) : 0;
        uint32_t block, first_mesh;
        uint32_t* c = room ? gpuDrawsRoom(1, (unsigned)count, 1u << (s.fragment >> 16 & 15), &block, &first_mesh) : 0;
        if (c) {
            for (int i = 0; i < count; i++) ix[i] = (uint16_t)(idx[i] - lo);
            gpuDrawPut(c, &s, block, gpuAddress(room), first_mesh, count);
            exp_task* t = &exp_tasks[exp_w.put % EXP_TASKS];
            t->room = room, t->made = mk, t->idx = ix, t->count = (uint32_t)count;
            exp_w.put = exp_w.put + 1;
            drawn_vertices += (uint32_t)count;
            return 1;
        }
    }
    uint32_t* to;
    if (kept && !table) {
        to = (uint32_t*)(memo_gpu + (size_t)(kept - memo) * MAX_PRIMITIVE * 16);
        uint32_t* in = kept->in;
        for (int k = 0; k < n; k++, in += 6) {
            const uint32_t* p = (const uint32_t*)(pos + (size_t)k * ps);
            const uint32_t* t = (const uint32_t*)(tc + (size_t)k * ts);
            in[0] = p[0], in[1] = p[1], in[2] = p[2], in[3] = t[0], in[4] = t[1], in[5] = *(const uint32_t*)(col + (size_t)k * cs);
        }
        for (int i = 0; i < count; i++) kept->idx[i] = idx[i] - lo;
        kept->s = s0, kept->count = count, kept->n = n, kept->far = far, kept->keep = keep, kept->colour = s.colour;
        kept->ka = key_a, kept->kb = key_b;
        kept->used = ++memo_clock, kept->frame = gl_frames;
        gpuDrawAt(&s, gpuAddress(to), count);
    } else if (!(to = gpuDrawRoom(&s, count, count))) {
        return 1;
    }
    for (int i = 0; i < count; i++, to += 4) {
        const uint32_t* m = &mk[(idx[i] - lo) * 4];
        uint32_t a = m[0], b = m[1], c = m[2], d = m[3];
        to[0] = a, to[1] = b, to[2] = c, to[3] = d;
    }
    drawn_vertices += (uint32_t)count;
    return 1;
}
// The world's faces kept (docs/games/jk2.md, "The world's faces kept"; rd-vanilla's tr_surface.cpp): a face's words for
// one kind of its coordinates, made once by jk2glKeptWords as fast_triangles makes a vertex's, and copied into the draw
// of each stage that has them by jk2glKeptFaces, the stage's one colour the draw's. The coordinates are from 0 when
// they lie in 0 to 63 (as fast_triangles has a clamped texture's: the light maps), else from the face's own floor, not
// its draw's (in a texture that repeats, the same texels; 2 from jk2glKeptWords). 0 from jk2glKeptWords: coordinates
// over 64 repeats, which a word does not hold; 0 from jk2glKeptFaces: a draw fast_triangles would make otherwise (a
// clamped texture with words from a floor, an offset, no texture), which rd-vanilla then draws from tess.
// JK2_GL=plain: none.
int jk2glKeptOn(void) {
    fast_ok();
    return !fast_off;
}
int jk2glKeptWords(uint32_t* to, const float* points, int stride, int st, const int* idx, int count, int verts) {
    float low_s = points[st], low_t = points[st + 1], high_s = low_s, high_t = low_t;
    for (int k = 1; k < verts; k++) {
        const float* t = points + (size_t)k * stride + st;
        if (t[0] < low_s) low_s = t[0];
        if (t[0] > high_s) high_s = t[0];
        if (t[1] < low_t) low_t = t[1];
        if (t[1] > high_t) high_t = t[1];
    }
    int floored = 2;   // (coordinates from 0, as fast_triangles makes a clamped texture's: either texture takes them)
    if (low_s >= 0.0f && low_t >= 0.0f && high_s < 63.0f && high_t < 63.0f) {
        floored = 1, low_s = low_t = 0.0f;
    } else {
        low_s = floor_of(low_s), low_t = floor_of(low_t);
        if (!(high_s - low_s < 63.0f && high_t - low_t < 63.0f)) return 0;
    }
    for (int i = 0; i < count; i++, to += 4) {
        const float* p = points + (size_t)idx[i] * stride;
        to[0] = bits(p[0]), to[1] = bits(p[1]), to[2] = bits(p[2]);
        to[3] = ((uint32_t)(int32_t)((p[st] - low_s) * 1024.0f) & 0xffff) | (uint32_t)(int32_t)((p[st + 1] - low_t) * 1024.0f) << 16;
    }
    return floored == 2 ? 2 : 1;
}
// Draws of words made elsewhere (the weather's particles, by its helper: rd-vanilla's tr_WorldEffects.cpp), as
// fast_triangles makes a vertex of a draw of many colours: positions, coordinates from 0 in 12 bits each and a tag, the
// tag a colour of the draw's table. `tables` holds each draw's 256 colours as the GL's RGBA bytes, made the GPU's words
// here, with the state the GL has. 0: a 2D state, which those draws never have (the caller draws them otherwise).
int jk2glTaggedDraws(const uint32_t* words, uint32_t* tables, const int* counts, int draws) {
    if (!on) return 1;
    fast_ok();
    gpu_state s;
    if (!draw_state(&s)) return 1;   // (its texture not in GPU memory: not drawn, as fast_triangles)
    if (!fast_passes) {
        float m[16];
        seglGetMatrix(GL_PROJECTION, m);
        if (m[10] == -2.0f && m[14] == -1.0f && m[11] == 0.0f && m[15] == 1.0f) return 0;
        depth_seen = 1;
    }
    uint32_t keep = blending && blend_src == GL_ONE && blend_dst == GL_ONE ? 0x00ffffffu : 0xffffffffu;
    s.vertex = GPU_TAGGED | GPU_TABLE;
    for (int d = 0; d < draws; d++, tables += 256) {
        const int tags = (counts[d] + 2) / 3 > 256 ? 256 : (counts[d] + 2) / 3;   // (at most a tag a triangle)
        for (int k = 0; k < tags; k++) tables[k] = WORD_OF(tables[k]) & keep;
        s.colour = gpuAddress(tables);
        if (!gpuDrawAt(&s, gpuAddress(words), counts[d])) return 1;
        drawn_vertices += (uint32_t)counts[d];
        words += (size_t)counts[d] * 4;
    }
    return 1;
}
// (the GPU done with the list in flight: before memory it reads where it lies, the kept faces' words, is let go)
void jk2glSettle(void) {
    if (on) seglSettle();
}
// (floored: some face's words are from its floor, which a clamped texture does not take)
int jk2glKeptFaces(uint32_t rgba, const uint32_t* const* words, const int* counts, int faces, int floored) {
    if (!on) return 1;
    gpu_state s;
    if (!draw_state(&s)) return 1;   // (its texture not in GPU memory: not drawn, as fast_triangles)
    if ((s.fragment & 0xff) == GPU_COLOUR || (floored && (s.fragment & GPU_CLAMPED)) || (offsetting && offset_pull != 0)) return 0;
    depth_seen = 1;
    uint32_t keep = blending && blend_src == GL_ONE && blend_dst == GL_ONE ? 0x00ffffffu : 0xffffffffu;
    s.colour = replace ? 0x00ffffffu : WORD_OF(rgba) & keep;
    // (drawn where the words lie, faces whose words follow each other in one draw: copied into the frame's list they
    // filled its 512 KB of vertices, and what came after them, the HUD, found no room)
    for (int f = 0; f < faces;) {
        int count = counts[f], g = f + 1;
        while (g < faces && words[g] == words[g - 1] + counts[g - 1] * 4 && count + counts[g] <= MAX_PRIMITIVE) count += counts[g++];
        if (!gpuDrawAt(&s, gpuAddress(words[f]), count)) return 1;
        drawn_vertices += (uint32_t)count;
        f = g;
    }
    return 1;
}
// A Ghoul2 surface skinned by the GPU's vertex stage (gpu.h's VERTEX_BONES; docs/games/jk2.md, "Ghoul2 on the GPU"):
// the model's own triangles, vertices and texture coordinates where they lie (memory of the program's, which the GPU
// reads where it is: no MMU), its bone references' matrices and the entity's light copied into the frame's memory, the
// state the GL has (texture, pass, blend) as fast_triangles takes it. `rgba` is the stage's colour (with a light, its
// alpha alone), `light` twelve floats (from where and the red multiplier, its colour and the green one, the light from
// everywhere and the blue one: gpu.h's three texels) or 0. 0: not drawn so, the caller skins it on this core.
#define GPU_BONES 0x40000u
int jk2glSkinned(const void* tris, int count, const void* verts, const void* coords, const float* bones, int nbones,
                 const float* light, uint32_t rgba) {
    if (!on || count < 3 || ((uintptr_t)verts & 3) || ((uintptr_t)coords & 3) || ((uintptr_t)tris & 3)) return 0;
    SETTLE();
    if (offsetting && offset_pull != 0) return 0;
    fast_ok();   // (fast_passes asked)
    gpu_state s;
    if (!draw_state(&s)) return 1;   // (its texture not in GPU memory: not drawn, as fast_triangles)
    if (!fast_passes) {   // (3D: what 2D comes after it goes on top)
        float m[16];
        seglGetMatrix(GL_PROJECTION, m);
        if (m[10] == -2.0f && m[14] == -1.0f && m[11] == 0.0f && m[15] == 1.0f) return 0;
        depth_seen = 1;
    }
    uint32_t keep = blending && blend_src == GL_ONE && blend_dst == GL_ONE ? 0x00ffffffu : 0xffffffffu;
    uint32_t word = ((255u - (rgba >> 24)) << 24 | (rgba & 0xff) << 16 | (rgba & 0xff00) | (rgba >> 16 & 0xff)) & keep;
    if (replace && (s.fragment & 0xff) != GPU_COLOUR) word = 0x00ffffffu, light = 0;
    unsigned bytes = (unsigned)nbones * 48u + (light ? 48u : 0u);
    float* room = gpuFrameMemory(bytes);
    if (!room) return 1;
    memcpy(room, bones, (size_t)nbones * 48);
    uint32_t light_at = 0;
    if (light) {
        float* l = room + nbones * 12;
        memcpy(l, light, 48);
        light_at = gpuAddress(l);
    }
    s.vertex = GPU_BONES, s.colour = word;
    s.more[0] = (uint32_t)(uintptr_t)verts, s.more[1] = gpuAddress(room), s.more[2] = (uint32_t)(uintptr_t)coords, s.more[3] = light_at;
    uint32_t* c = gpuDrawAt(&s, (uint32_t)(uintptr_t)tris, count);
    if (!c) return 1;
    c[4] &= ~0x400u;   // (not compact: gpuDrawAt's draws are)
    drawn_vertices += (uint32_t)count;
    if (gl_log_at > 0 && gl_log_at == gl_frames)
        printf("jk2gl: skinned %d indices, %d bones, texture %d, pass %u, colour %08x, lit %d\n", count, nbones,
               texturing ? (int)bound : -1, s.fragment >> 16 & 15, word, light != 0);
    return 1;
}

// ---- an environment-mapped stage on the GPU (gpu.h's VERTEX_ENV; docs/games/jk2.md, "The menu's logo") ----
// rd-vanilla's tcGen environment makes every vertex's coordinates from the eye each frame: a turning model's shine was
// made and copied again every frame (the menu's logo, 4,320 indices). Here the stage's positions and normals are kept
// in GPU memory a drawn vertex each, with what they were made of, and a draw of the same state, indices, positions,
// normals and colour is drawn from there with the eye's place, in the model's space, its only new words: the GPU
// makes the coordinates as RB_CalcEnvironmentTexCoords does. 0: not drawn so (the caller makes them). JK2_ENV=cpu: never.
#define ENV_SLOTS 4
#define GPU_ENV 0x80000u
typedef struct { int count, n, frame; uint32_t keep, colour, ka, kb; unsigned used; gpu_state s; uint32_t *in, *idx; uint8_t* gpu; } env_t;
static env_t envs[ENV_SLOTS];
static int env_state = -1;   // -1 not yet asked, 0 off, 1 on
static unsigned env_clock;
static void env_init(void) {
    const char* g = getenv("JK2_ENV");
    env_state = 0;
    if (g && !strcmp(g, "cpu")) return;
    uint8_t* gpu = hlglKeptMemory(ENV_SLOTS * MAX_PRIMITIVE * 32);
    if (!gpu) return;
    env_state = 1;
    for (int e = 0; e < ENV_SLOTS && env_state; e++) {
        envs[e].gpu = gpu + (size_t)e * MAX_PRIMITIVE * 32;
        if (!(envs[e].in = malloc(MAX_PRIMITIVE * 7 * 4)) || !(envs[e].idx = malloc(MAX_PRIMITIVE * 4))) env_state = 0;
        else mcw_touch(envs[e].in, MAX_PRIMITIVE * 7 * 4), mcw_touch(envs[e].idx, MAX_PRIMITIVE * 4);
    }
    mcw_touch(gpu, ENV_SLOTS * MAX_PRIMITIVE * 32);
}
#define ENV_WORD(c) ((255u - ((c) >> 24)) << 24 | ((c) & 0xff) << 16 | ((c) & 0xff00) | ((c) >> 16 & 0xff))
int jk2glEnvElements(int count, const uint32_t* idx, const float* normal, const float* eye) {
    if (!on || env_state <= 0 || count < 3 || count % 3 || count > MAX_PRIMITIVE - 1) return 0;
    SETTLE();
    if (!fast_ok()) return 0;
    uint32_t lo = idx[0], hi = idx[0];
    for (int i = 1; i < count; i++) {
        uint32_t k = idx[i];
        if (k < lo) lo = k;
        if (k > hi) hi = k;
    }
    int n = (int)(hi - lo) + 1;
    if (n > MAX_PRIMITIVE) return 0;
    float m[16];
    seglGetMatrix(GL_PROJECTION, m);
    if (m[10] == -2.0f && m[14] == -1.0f && m[11] == 0.0f && m[15] == 1.0f) return 0;   // (2D: fast_triangles' way)
    gpu_state s;
    if (!draw_state(&s)) return 1;
    if (!fast_passes) depth_seen = 1;
    const uint8_t *pos = va_pos.p + (size_t)lo * va_pos.stride, *col = va_colour.p + (size_t)lo * va_colour.stride;
    const float* nor = normal + (size_t)lo * 4;
    int ps = va_pos.stride, cs = va_colour.stride;
    uint32_t first = *(const uint32_t*)col;
    for (int k = 1; k < n; k++)
        if (*(const uint32_t*)(col + (size_t)k * cs) != first) return 0;   // (a colour a vertex: the CPU's way)
    uint32_t keep = blending && blend_src == GL_ONE && blend_dst == GL_ONE ? 0x00ffffffu : 0xffffffffu;
    uint32_t word = ENV_WORD(first) & keep;
    if (replace && (s.fragment & 0xff) != GPU_COLOUR) word = 0x00ffffffu;
    s.vertex = GPU_ENV, s.colour = word;
    // a slot of the same state and shape whose positions, normals and indices are these
    env_t* hit = 0;
    for (int e = 0; e < ENV_SLOTS && !hit; e++) {
        env_t* x = &envs[e];
        if (!x->used || x->count != count || x->n != n || x->keep != keep || memcmp(&x->s, &s, sizeof s)) continue;
        if (key_a && x->ka == key_a && x->kb == key_b) {   // (the same model's surface and frame: positions, normals, indices)
            hit = x;
            break;
        }
        const uint32_t* in = x->in;
        int k = 0;
        for (; k < n; k++, in += 6) {
            const uint32_t* p = (const uint32_t*)(pos + (size_t)k * ps);
            const uint32_t* q = (const uint32_t*)(nor + (size_t)k * 4);
            if (p[0] != in[0] || p[1] != in[1] || p[2] != in[2] || q[0] != in[3] || q[1] != in[4] || q[2] != in[5]) break;
        }
        if (k < n) continue;
        int i = 0;
        for (; i < count && idx[i] - lo == x->idx[i]; i++) {
        }
        if (i == count) hit = x;
    }
    if (!hit) {   // (made again in the slot used longest ago)
        hit = &envs[0];
        for (int e = 1; e < ENV_SLOTS; e++)
            if (envs[e].used < hit->used) hit = &envs[e];
        if (hit->used && hit->frame >= gl_frames - 1) seglSettle();   // (the list being drawn may read it)
        uint32_t* in = hit->in;
        for (int k = 0; k < n; k++, in += 6) {
            const uint32_t* p = (const uint32_t*)(pos + (size_t)k * ps);
            const uint32_t* q = (const uint32_t*)(nor + (size_t)k * 4);
            in[0] = p[0], in[1] = p[1], in[2] = p[2], in[3] = q[0], in[4] = q[1], in[5] = q[2];
        }
        uint32_t* to = (uint32_t*)hit->gpu;
        uint32_t* normals = to + MAX_PRIMITIVE * 4;
        for (int i = 0; i < count; i++, to += 4, normals += 4) {
            uint32_t k = idx[i] - lo;
            const uint32_t* v = hit->in + k * 6;
            hit->idx[i] = k;
            to[0] = v[0], to[1] = v[1], to[2] = v[2], to[3] = 0;
            normals[0] = v[3], normals[1] = v[4], normals[2] = v[5], normals[3] = 0;
        }
        hit->s = s, hit->count = count, hit->n = n, hit->keep = keep, hit->ka = key_a, hit->kb = key_b;
    }
    hit->used = ++env_clock, hit->frame = gl_frames;
    s.more[0] = gpuAddress(hit->gpu + MAX_PRIMITIVE * 16);
    s.more[1] = bits(eye[0]), s.more[2] = bits(eye[1]), s.more[3] = bits(eye[2]);
    gpuDrawAt(&s, gpuAddress(hit->gpu), count);
    drawn_vertices += (uint32_t)count;
    if (gl_log_at > 0 && gl_log_at == gl_frames) printf("jk2gl: environment %d indices %d vertices, texture %d, pass %u\n", count, n,
                                                       texturing ? (int)bound : -1, s.fragment >> 16 & 15);
    return 1;
}
#undef ENV_WORD

#undef WORD_OF

void jk2_glDrawElements(GLenum m, GLsizei count, GLenum type, const void* indices) {
    if (!on) return;
    SETTLE();
    if (m == GL_TRIANGLES && type == GL_UNSIGNED_INT && fast_ok()) {
        int piece = MAX_PRIMITIVE / 3 * 3;
        int done = 0;
        for (; done < count; done += piece)
            if (!fast_triangles(count - done < piece ? count - done : piece, (const uint32_t*)indices + done)) break;
        if (done >= count) return;
        // (a range of vertices longer than a primitive: the rest the plain way)
        indices = (const uint32_t*)indices + done, count -= done;
    }
    if (m != GL_TRIANGLES || count <= MAX_PRIMITIVE) {
        s_glDrawElements(m, count, type, indices);
        return;
    }
    int size = type == GL_UNSIGNED_SHORT ? 2 : type == GL_UNSIGNED_INT ? 4 : 1, piece = MAX_PRIMITIVE / 3 * 3;
    for (int done = 0; done < count; done += piece)
        s_glDrawElements(m, count - done < piece ? count - done : piece, type, (const char*)indices + (size_t)done * size);
}
void jk2_glEndList(void) {}
void jk2_glNewList(GLuint l, GLenum m) { (void)l, (void)m; }
void jk2_glFinish(void) {}
static GLuint lists = 1;
GLuint jk2_glGenLists(GLsizei range) {
    GLuint first = lists;
    lists += range;
    return first;
}
GLenum jk2_glGetError(void) { return 0; }
void jk2_glGetFloatv(GLenum name, GLfloat* v) {
    if (on) { SETTLE(); s_glGetFloatv(name, v); }
    else memset(v, 0, 16 * sizeof(GLfloat));
}
void jk2_glGetIntegerv(GLenum name, GLint* v) {
    switch (name) {
    case 0x84E2: *v = 1; return;              // GL_MAX_TEXTURE_UNITS_ARB: one, the renderer's plain paths
    case 0x0D05: case 0x0CF5: *v = 4; return; // GL_PACK_ALIGNMENT, GL_UNPACK_ALIGNMENT (0 made the screenshot of a save divide by it)
    // GL_MAX_TEXTURE_SIZE: 512 on the GPU (the menus' and loading screens' pictures of 1,024 were 12 of its 24 MB, and
    // the window is 640 wide); without a window as nullgl.c
    case 0x0D33: *v = on ? 512 : 2048; return;
    }
    if (on) { SETTLE(); s_glGetIntegerv(name, v); }
    else if (name == 0x0BA2) v[0] = v[1] = 0, v[2] = 640, v[3] = 480;
    else *v = 0;
}
const GLubyte* jk2_glGetString(GLenum name) {
    switch (name) {
    case 0x1F00: return (const GLubyte*)"ShaderEmu";
    case 0x1F01: return (const GLubyte*)(on ? "the machine's GPU" : "null");
    case 0x1F02: return (const GLubyte*)"1.1";
    default: return (const GLubyte*)"";   // no extension: the renderer takes its plain paths
    }
}
GLboolean jk2_glIsEnabled(GLenum c) { if (!on) return 0; SETTLE(); return s_glIsEnabled(c); }
void jk2_glLineWidth(GLfloat w) { (void)w; }
void jk2_glLoadIdentity(void) { ON_THEN(s_glLoadIdentity()); }
void jk2_glLoadMatrixf(const GLfloat* m) { ON_THEN(s_glLoadMatrixf(m)); }
void jk2_glMatrixMode(GLenum m) { ON_THEN(s_glMatrixMode(m)); }
void jk2_glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) { ON_THEN(s_glOrtho(l, r, b, t, n, f)); }
void jk2_glPolygonMode(GLenum f, GLenum m) { (void)f, (void)m; }
void jk2_glPolygonOffset(GLfloat f, GLfloat u) { ON_THEN(s_glPolygonOffset(f, u)); }
void jk2_glPopMatrix(void) { ON_THEN(s_glPopMatrix()); }
void jk2_glPushMatrix(void) { ON_THEN(s_glPushMatrix()); }
// a black picture, for a saved game's screenshot (the rows padded to 4 bytes)
void jk2_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels) {
    int bytes = format == 0x1907 ? 3 : 4;
    (void)x, (void)y, (void)type;
    memset(pixels, 0, (size_t)((width * bytes + 3) & ~3) * height);
}
void jk2_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { (void)x, (void)y, (void)w, (void)h; }
void jk2_glShadeModel(GLenum m) { (void)m; }
void jk2_glStencilFunc(GLenum f, GLint r, GLuint m) { (void)f, (void)r, (void)m; }
void jk2_glStencilMask(GLuint m) { (void)m; }
void jk2_glStencilOp(GLenum a, GLenum b, GLenum c) { (void)a, (void)b, (void)c; }
void jk2_glTexCoord2f(GLfloat s, GLfloat t) { ON_THEN(s_glTexCoord2f(s, t)); }
void jk2_glTexCoord2fv(const GLfloat* v) { ON_THEN(s_glTexCoord2fv(v)); }
void jk2_glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* p) { ON_THEN(s_glTexCoordPointer(size, type, stride, p)); }
void jk2_glTexEnvf(GLenum t, GLenum n, GLfloat v) { ON_THEN(s_glTexEnvf(t, n, v)); }
void jk2_glTexImage2D(GLenum t, GLint l, GLint internal, GLsizei w, GLsizei h, GLint b, GLenum format, GLenum type, const void* p) {
    ON_THEN(s_glTexImage2D(t, l, internal, w, h, b, format, type, p));
}
void jk2_glTexParameterf(GLenum t, GLenum n, GLfloat v) { ON_THEN(s_glTexParameterf(t, n, v)); }
void jk2_glTexParameterfv(GLenum t, GLenum n, const GLfloat* v) { ON_THEN(s_glTexParameterfv(t, n, v)); }
void jk2_glTexParameteri(GLenum t, GLenum n, GLint v) { ON_THEN(s_glTexParameteri(t, n, v)); }
void jk2_glTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, const void* p) {
    ON_THEN(s_glTexSubImage2D(t, l, x, y, w, h, format, type, p));
}
void jk2_glTranslatef(GLfloat x, GLfloat y, GLfloat z) { ON_THEN(s_glTranslatef(x, y, z)); }
void jk2_glScalef(GLfloat x, GLfloat y, GLfloat z) { ON_THEN(s_glScalef(x, y, z)); }
void jk2_glVertex2f(GLfloat x, GLfloat y) { ON_THEN(s_glVertex2f(x, y)); }
void jk2_glVertex3f(GLfloat x, GLfloat y, GLfloat z) { ON_THEN(s_glVertex3f(x, y, z)); }
void jk2_glVertex3fv(const GLfloat* v) { ON_THEN(s_glVertex3fv(v)); }
void jk2_glVertexPointer(GLint size, GLenum type, GLsizei stride, const void* p) { va_count = 0; ON_THEN(s_glVertexPointer(size, type, stride, p)); }
void jk2_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) { ON_THEN(s_glViewport(x, y, w, h)); }

// (the mix's worker runs on a stack of its own, where mcw_on_worker() is false: sys_shaderemu.cpp's SE_OnFirstCore asks)
int jk2MixWorkerHere(void) {
    char here;
    return (uint8_t*)&here >= mix_stack && (uint8_t*)&here < mix_stack + MIX_STACK;
}

// ---- the drawing on two workers (docs/games/jk2.md, "The drawing on two workers"; tr_backend.cpp's SE_DrawList) ----
// A second worker, posted by this core with the frame, waits until the drawing hands it the second part of its 3D list
// (jk2DrawHalfGive, with the upper half of what is left of the frame's list: gpu_shaderemu.c's gpuHalfSplit) or says it
// has none (render_body), draws it with the second copies of the back end and of this file (SE_DRAW_B, made by build.sh)
// on that copy's own stack, and says it is done; the drawing waits for it and moves its commands after its own.
typedef struct { uint8_t gpu[256]; int swaps, frames, rooms, unused; } gl_half;   // (in the renderer's 512 bytes)
#ifndef SE_DRAW_B
uint8_t* b_jk2glHalfStack(void);   // (the second copy's: its stack, as on_render's)
void se_b_SE_DrawHalfB(void* half);   // (the renderer's second copy: tr_backend.cpp's SE_DrawHalfB)
static struct __attribute__((aligned(16))) { volatile uint32_t gen, state; void* arg; uint32_t unused; } half_go;   // the drawing's: 1 given, 2 none
static struct __attribute__((aligned(16))) { volatile uint32_t gen, unused[3]; } half_done;   // the half's worker's
static struct __attribute__((aligned(16))) { uint32_t gen, given, unused[2]; } half_d;   // the drawing's
static uint32_t half_body(uint32_t arg) {
    se_b_SE_DrawHalfB((void*)(uintptr_t)arg);
    return 0;
}
static uint32_t half_job(uint32_t gen, uint32_t a1) {
    (void)a1;
    for (;;) {
        if (half_go.gen == gen) {
            if (half_go.state == 1) jk2_call_on_stack(half_body, (uint32_t)(uintptr_t)half_go.arg, b_jk2glHalfStack() + RENDER_STACK);
            break;
        }
        mc_end_pass();
    }
    half_done.gen = gen;
    return 0;
}
// (core 0, at the give: whether the frame's second part may be drawn; the job is taken by a free worker, never run on this
// core, whose waiting there for the drawing would hold the drawing back: it waits for this core's skins, job_go)
static mcw_handle half_h;
static struct __attribute__((aligned(16))) { volatile uint32_t gen, unused[3]; } half_posted;   // this core's: the frame posted for
static int half_on(void) {
    if (!hw0.tried) {
        const char* e = getenv("JK2_HALF");
        hw0.tried = 1, hw0.unused = rw0.mode && !(e && !strcmp(e, "0"));   // (JK2_HALF=0: one part, as before)
        if (hw0.unused) {
            mcw_touch(b_jk2glHalfStack(), RENDER_STACK);
            mcw_touch(&half_go, sizeof half_go), mcw_touch((void*)&half_done, sizeof half_done), mcw_touch(&half_d, sizeof half_d);
            mcw_touch((void*)&half_posted, sizeof half_posted);
            printf("jk2: the drawing's second part on a free worker\n");
        }
    }
    return hw0.unused;
}
static void half_settle(void) {
    if (!hw0.out) return;
    mcw_join(half_h);
    hw0.out = 0;
}
static void half_post(uint32_t gen) {
    half_settle();
    if (!half_on()) return;
    mcw_handle h = mcw_try_call(half_job, gen, 0);
    if (!h) return;   // (no worker free: this frame drawn in one part)
    half_h = h, hw0.out = 1, half_posted.gen = gen;
}
// (the drawing's: its 3D list's second part given, the frame's list split; 0: drawn whole by the drawing)
static void half_frame(uint32_t given) { half_d.gen = given; }
static void half_none(void) {   // (no part given this frame: the half's worker told so)
    if (half_posted.gen == half_d.gen && half_d.given != half_d.gen) half_go.state = 2, half_go.gen = half_d.gen;
}
int jk2DrawHalfGive(void* half, void* glp) {
    gl_half* g = glp;
    if (!on_render() || half_posted.gen != half_d.gen || half_d.given == half_d.gen || !gpuHalfSplit(g->gpu)) return 0;
    g->swaps = swaps, g->frames = gl_frames, g->rooms = rooms_on();
    half_go.arg = half, half_go.state = 1, half_go.gen = half_d.gen;
    half_d.given = half_d.gen;
    return 1;
}
int jk2DrawHalfJoin(void* glp) {   // (how many passes the drawing waited: tr_backend.cpp moves the cut by it)
    gl_half* g = glp;
    int waited = 0;
    while (half_done.gen != half_d.gen) {
        mc_end_pass();
        waited++;
    }
    gpuHalfMerge(g->gpu);
    return waited;
}
#else
static void half_frame(uint32_t given) { (void)given; }
static void half_none(void) {}
int jk2DrawHalfGive(void* half, void* glp) { (void)half, (void)glp; return 0; }
int jk2DrawHalfJoin(void* glp) { (void)glp; return 0; }
#endif
// (the second copy's, SE_DRAW_B: this file's state for the part, as the drawing's where it is the frame's)
void jk2glHalfInit(const void* glp) {
    const gl_half* g = glp;
    on = 1, depth_seen = 1, memo_state = 0, env_state = 0, exp_cfg.n = 0;
    swaps = g->swaps, gl_frames = g->frames, rooms_mode = g->rooms;
    gpuHalfBegin(g->gpu);
}
void jk2glHalfEnd(void* glp) { gpuHalfEnd(((gl_half*)glp)->gpu); }
uint8_t* jk2glHalfStack(void) { return render_stack; }
