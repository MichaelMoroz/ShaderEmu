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

static void job(uint32_t view, uint32_t) {
    job_counts.start = my_instructions();
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
void aoe_view_wait_walked(void) {
    if (!st.busy) return;
    aoe_lat('w', 0, 0);
    while (!walked.v) {
        if (mcw_done(1)) break;
        mcw_rest();
    }
    aoe_lat('x', 0, 0);
}

void aoe_view_join(void) {
    if (!st.busy) return;
    aoe_lat('j', 0, 0);
    if (st.workers > 0) mcw_wait(1);
    aoe_lat('k', (job_counts.walked_at - job_counts.start) / 1000, (job_counts.end - job_counts.walked_at) / 1000);
    st.busy = 0;
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
