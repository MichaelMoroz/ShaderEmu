// The view on a worker core: it walks the tiles while the first core draws the other panels (both only read the
// game), then draws from its lists while the first core makes the next world update (docs/games/aoe.md).
// AOE_VIEW=inline is the same order on the first core; a run under AOE_NOWINDOW has no workers.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define protected public
#define private public
#include "../include/drawarea.h"
#include "../include/view.h"
#undef protected
#undef private
#include "aoe_cores.h"

extern "C" {
typedef void (*mcw_fn)(uint32_t, uint32_t);
int mcw_open(int limit);
void mcw_post(int k, mcw_fn fn, uint32_t a0, uint32_t a1);
int mcw_done(int k);
int mcw_wait(int k);
void mcw_rest(void);
void mcw_touch(void* memory, unsigned bytes);
void mcw_touch_code(const void* code, unsigned bytes);
extern char __DATA_BEGIN__[], _end[], etext[];
void _start(void);
}

// What the two sides watch lie in texels of their own.
static struct { volatile int v; char pad[12]; } walked __attribute__((aligned(16)));
static struct { unsigned start, walked_at, end; char pad[4]; } job_counts __attribute__((aligned(16)));
static inline unsigned my_instructions(void) { unsigned n; __asm__ volatile("rdinstret %0" : "=r"(n)); return n; }
static struct { RGE_View* view; int busy, inline_job, workers, opened, wanted; char pad[8]; } st __attribute__((aligned(16)));

// The parts of a frame (aoe_cores.h): the first core's by the machine's time (rdtime, 5 kHz) and instructions, the view's job's
// instructions summed at each join
static inline unsigned my_time(void) { unsigned t; __asm__ volatile("rdtime %0" : "=r"(t)); return t; }
static struct { unsigned at[AOE_PH_COUNT], time[AOE_PH_COUNT], ins_at[AOE_PH_COUNT], ins[AOE_PH_COUNT], n[AOE_PH_COUNT];
                unsigned walk, raster, jobs, from_time, from_ins; } ph;
void aoe_phase(int part, int begin) {
    if (begin) { ph.at[part] = my_time(); ph.ins_at[part] = my_instructions(); return; }
    ph.time[part] += my_time() - ph.at[part];
    ph.ins[part] += my_instructions() - ph.ins_at[part];
    ph.n[part]++;
}
void aoe_phases_reset(void) {
    memset(&ph, 0, sizeof ph);
    ph.from_time = my_time(); ph.from_ins = my_instructions();
}
extern "C" unsigned aoe_path_calls, aoe_path_wrong;
void aoe_phases_print(long frames) {
    static const char* names[AOE_PH_COUNT] = {"wait for the walk", "world update", "wait at the join", "paint"};
    if (frames < 1) frames = 1;
    unsigned all = my_time() - ph.from_time;
    fprintf(stderr, "aoephase: %ld frames, %u.%u ms a frame, core 0 %u instructions a frame\n", frames, all / 5 / frames,
            all * 2 / frames % 10, (my_instructions() - ph.from_ins) / frames);
    for (int i = 0; i < AOE_PH_COUNT; i++)
        fprintf(stderr, "aoephase: %-18s %5u.%u ms a frame (%u%%), %7u instructions a frame, %u times\n", names[i], ph.time[i] / 5 / frames,
                ph.time[i] * 2 / frames % 10, all ? (unsigned) (100.0 * ph.time[i] / all) : 0, ph.ins[i] / frames, ph.n[i]);
    fprintf(stderr, "aoephase: the view's job %u: walk %u, raster %u instructions a frame\n", ph.jobs, ph.walk / frames, ph.raster / frames);
    if (aoe_path_calls) fprintf(stderr, "aoephase: path distances %u, %u of them not as the doubles\n", aoe_path_calls, aoe_path_wrong);
}

// AOE_SCROLL_COPY=words: a scroll moves the view by words, on 8 pixels across, as before (OverlayMemCopy); read here, on the first core
struct AoeScrollWay aoe_scroll_way = {1};
int aoe_view_wanted(void) {
    if (!st.opened) {
        st.opened = 1;
        const char* c = getenv("AOE_SCROLL_COPY");
        aoe_scroll_way.words = (c && !strcmp(c, "words")) || getenv("AOE_NOWINDOW") != 0;   // (qemu has no machine's copy)
        if (getenv("AOE_PICSUM")) fprintf(stderr, "aoebase %p\n", (void*) _start);
        const char* e = getenv("AOE_VIEW");
        st.wanted = !e || strcmp(e, "whole");   // whole: as before, without any of this
        if (st.wanted && !getenv("AOE_NOWINDOW") && !(e && !strcmp(e, "inline"))) {
            st.workers = mcw_open(2);   // the view's, and the one a song is made on (aoe_music.cpp)
            if (st.workers > 0) {
                mcw_touch_code((const void*) _start, (unsigned) (etext - (char*) _start));
                mcw_touch(__DATA_BEGIN__, (unsigned) (_end - __DATA_BEGIN__));
            }
        }
    }
    return st.wanted;
}

int aoe_view_second_worker(void) { aoe_view_wanted(); return st.workers >= 2 ? 2 : 0; }
int aoe_view_busy(void) { return st.busy; }
int aoe_view_in_job(void) { return st.inline_job || (st.workers && mcw_on_worker()); }
void aoe_view_walked(void) { job_counts.walked_at = my_instructions(); walked.v = 1; }
void aoe_view_poll(void) { if (st.busy && st.workers) mcw_done(1); }

// A list that grows in the job: a piece of this, which is never given back. A big picture's row index asks too.
static char arena[1 << 21] __attribute__((aligned(16)));
static struct { unsigned used; char pad[12]; } arena_at __attribute__((aligned(16)));
extern "C" void* aoe_any_calloc(size_t n, size_t size) {
    if (!aoe_view_in_job() || !mcw_on_worker()) return calloc(n, size);
    unsigned bytes = ((unsigned) (n * size) + 15) & ~15u;
    if (arena_at.used + bytes > sizeof arena) { fprintf(stderr, "aoe: the job's memory is used up\n"); _Exit(9); }
    void* p = arena + arena_at.used;
    arena_at.used += bytes;
    memset(p, 0, bytes);
    return p;
}
extern "C" void aoe_any_free(void* p) {
    if ((char*) p >= arena && (char*) p < arena + sizeof arena) return;
    free(p);
}

extern void aoe_asm_for_worker(void);
RGE_View* aoe_view_posted(void) { return st.view; }
// The view's own copies of the page's and the save area's objects: its clip lists, and no surface to lock.
static TDrawArea* area_copy;
static TDrawSystem* system_copy;
static TDrawArea* area_real;

static int first_copy_ok = -1;   // the first core's CSR 0x8b0: 1 when the machine has the copy (aoe_view_first_copy)
struct AoeViewStack aoe_view_stack;

static void job(uint32_t view, uint32_t) {
    job_counts.start = my_instructions();
    // the stack this job runs on (aoe_cores.h, aoe_view_core): noted once, the same worker's each time
    char here;
    unsigned lo = (unsigned) &here - (60u << 10), hi = (unsigned) &here + 1024;
    if (st.workers > 0 && (aoe_view_stack.lo != lo || aoe_view_stack.hi != hi)) { aoe_view_stack.lo = lo; aoe_view_stack.hi = hi; }
    ((RGE_View*) view)->RGE_View::draw();
    walked.v = 1;
    job_counts.end = my_instructions();
}

void aoe_view_post(RGE_View* view) {
    aoe_view_join();
    TDrawArea* real = view->TPanel::render_area == area_copy ? area_real : view->TPanel::render_area;
    if (!system_copy) {
        system_copy = (TDrawSystem*) malloc(sizeof(TDrawSystem));
        area_copy = (TDrawArea*) calloc(1, sizeof(TDrawArea));
    }
    if (real != area_real || area_copy->SpanList == 0) {
        memcpy(system_copy, real->DrawSystem, sizeof(TDrawSystem));
        system_copy->DrawType = 1;   // TDrawArea::Lock and Unlock then touch no surface
        area_real = real;
    }
    // the save area keeps its pixels' place for good
    if (view->save_area1 && view->save_area1->DrawSystem != system_copy) {
        view->save_area1->Lock("aoe_view", 1);
        view->save_area1->DrawSystem = system_copy;
    }
    TSpan_List_Manager* own = area_copy->SpanList;
    long made[5];
    memcpy(made, area_copy->aoe_clip_made, sizeof made);
    if (!real->Lock("aoe_view", 1)) { walked.v = 1; view->TPanel::render_area = real; view->RGE_View::draw(); return; }
    memcpy(area_copy, real, sizeof(TDrawArea));
    real->Unlock("aoe_view");
    if (!own) { own = new TSpan_List_Manager(real->Width, real->Height); made[4] = 0; }
    area_copy->SpanList = own;
    area_copy->CurSpanList = own;
    memcpy(area_copy->aoe_clip_made, made, sizeof made);
    area_copy->aoe_whole_clip_owed = 0;
    area_copy->DrawSystem = system_copy;
    area_copy->DrawDc = 0;
    view->TPanel::render_area = area_copy;
    view->cur_render_area = area_copy;

    if (first_copy_ok < 0) { unsigned v; __asm__ volatile("csrr %0, 0x8b0" : "=r"(v)); first_copy_ok = v == 1; if (getenv("AOE_DEBUG")) fprintf(stderr, "aoe: the machine's copy %s\n", first_copy_ok ? "by the first core" : "none: by words"); }
    aoe_asm_for_worker();
    walked.v = 0;
    st.view = view;
    st.busy = 1;
    if (st.workers > 0) {
        mcw_post(1, job, (uint32_t) view, 0);
    } else {
        st.inline_job = 1;
        job((uint32_t) view, 0);
        st.inline_job = 0;
    }
}

extern void aoe_lat(int kind, int a, int b);
// The scroll's machine copy: asked of the first core by the view's job, since only that core may ask for one (CSR 0x8b0 reads
// 0 on a worker). A worker's request on the card went unmade, and the view's rows stayed where they were under the new strips.
static struct { const void* volatile src; void* volatile dst; volatile unsigned n, state; } copy_ask __attribute__((aligned(16)));
static void copy_now(const void* src, void* dst, unsigned n) {
    __asm__ volatile("csrw 0x8b1, %0\n csrw 0x8b2, %1\n csrw 0x8b3, %2\n csrw 0x8b0, %3" :: "r"(src), "r"(dst), "r"(n), "r"(1) : "memory");
}
static void copy_serve(void) {
    if (copy_ask.state != 1) return;
    copy_now(copy_ask.src, copy_ask.dst, copy_ask.n);
    copy_ask.state = 2;
}
int aoe_view_first_copy(const void* src, void* dst, unsigned bytes) {
    if (first_copy_ok <= 0) return 0;
    if (!aoe_view_core()) { copy_now(src, dst, bytes); return 1; }
    copy_ask.src = src; copy_ask.dst = dst; copy_ask.n = bytes;
    __asm__ volatile("" ::: "memory");
    copy_ask.state = 1;
    while (copy_ask.state != 2) __asm__ volatile(".word 0x0000100f");   // (mc_end_pass: the first core makes it)
    copy_ask.state = 0;
    return 1;
}

void aoe_view_wait_walked(void) {
    if (!st.busy) return;
    aoe_lat('w', 0, 0);
    aoe_phase(AOE_PH_WALKED, 1);
    while (!walked.v) {
        copy_serve();
        if (mcw_done(1)) break;
        mcw_rest();
    }
    aoe_phase(AOE_PH_WALKED, 0);
    aoe_lat('x', 0, 0);
}

void aoe_view_join(void) {
    if (!st.busy) return;
    aoe_lat('j', 0, 0);
    aoe_phase(AOE_PH_JOIN, 1);
    if (st.workers > 0) while (!mcw_done(1)) { copy_serve(); mcw_rest(); }   // (a scroll's copy the job asks for is made here too)
    aoe_phase(AOE_PH_JOIN, 0);
    aoe_lat('k', (job_counts.walked_at - job_counts.start) / 1000, (job_counts.end - job_counts.walked_at) / 1000);
    ph.walk += job_counts.walked_at - job_counts.start; ph.raster += job_counts.end - job_counts.walked_at; ph.jobs++;
    st.busy = 0;
}

// The script's "check" (host.cpp): the world's update run to its end, then a picture as the view draws it now (whole: every tile
// again, as after a jump), joined
extern void aoe_draw_window_now(void);
void aoe_view_check_draw(int whole) {
    aoe_world_finish();
    aoe_view_join();
    RGE_View* view = st.view;
    if (!view) return;
    if (whole) view->render_terrain_mode = 0;
    view->set_redraw(TPanel::RedrawNormal);
    aoe_draw_window_now();
    aoe_view_join();
    aoe_view_deferred();   // (the panels over the view, as the frame's end draws them)
}

// Panels over the view
enum { DEFERRED = 16 };
static TPanel* deferred[DEFERRED];
static int deferred_count;
void aoe_view_defer(TPanel* panel) {
    for (int i = 0; i < deferred_count; i++) if (deferred[i] == panel) return;
    if (deferred_count < DEFERRED) deferred[deferred_count++] = panel;
}
void aoe_view_deferred(void) {
    int n = deferred_count;
    deferred_count = 0;
    for (int i = 0; i < n; i++) deferred[i]->draw();
}
