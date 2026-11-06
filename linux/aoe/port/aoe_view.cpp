// The view on a worker core: it walks the tiles while the first core draws the other panels (both only read the
// game), then draws from its lists while the first core makes the next world update (docs/games/aoe.md).
// AOE_VIEW=inline is the same order on the first core; a run under AOE_NOWINDOW has no workers.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <new>
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
static struct { unsigned start, walked_at, end, posted; } job_times __attribute__((aligned(16)));   // (the clock's, for aoephase)
static inline unsigned my_instructions(void) { unsigned n; __asm__ volatile("rdinstret %0" : "=r"(n)); return n; }
static inline unsigned my_time(void);
static struct { RGE_View* view; int busy, inline_job, workers, opened, wanted, band_base, bands; } st __attribute__((aligned(16)));
static int tiles_worker;
static struct { volatile int v; char pad[12]; } job_finished __attribute__((aligned(16)));
static struct { volatile int v; char pad[12]; } copy_passed __attribute__((aligned(16)));
static int copy_owed;   // (the view's worker's: a copy asked and not yet seen made)
void aoe_view_copy_settle(void);

// The parts of a frame (aoe_cores.h): the first core's by the machine's time (rdtime, 5 kHz) and instructions, the view's job's
// instructions summed at each join
static inline unsigned my_time(void) { unsigned t; __asm__ volatile("rdtime %0" : "=r"(t)); return t; }
static struct { unsigned at[AOE_PH_COUNT], time[AOE_PH_COUNT], ins_at[AOE_PH_COUNT], ins[AOE_PH_COUNT], n[AOE_PH_COUNT];
                unsigned walk, raster, jobs, from_time, from_ins, walk_t, raster_t, cycle_t, start_t; } ph;
void aoe_phase(int part, int begin) {
    if (begin) { ph.at[part] = my_time(); ph.ins_at[part] = my_instructions(); return; }
    ph.time[part] += my_time() - ph.at[part];
    ph.ins[part] += my_instructions() - ph.ins_at[part];
    ph.n[part]++;
}
// (the counters the view's worker adds to are cleared by it, at its next job's start, when this word moves: the first core writes
// none of them, so none is cleared beside an addition)
static struct { volatile unsigned gen; char pad[12]; } phases_gen __attribute__((aligned(16)));
static struct { unsigned seen; char pad[12]; } phases_seen __attribute__((aligned(16)));
static void phases_worker_reset(void) {
    if (phases_seen.seen == phases_gen.gen) return;
    phases_seen.seen = phases_gen.gen;
    { extern unsigned aoe_band_times[]; memset(aoe_band_times, 0, (AOE_RASTER_BANDS + 1) * sizeof(unsigned)); }
    { extern unsigned aoe_copy_t[]; aoe_copy_t[0] = aoe_copy_t[1] = 0; }
    { extern unsigned aoe_mode0_all, aoe_mode0_far; aoe_mode0_all = aoe_mode0_far = 0; }
    { extern unsigned aoe_rl_t[]; memset(aoe_rl_t, 0, 5 * sizeof(unsigned)); }
}
void aoe_phases_reset(void) {
    phases_gen.gen++;
    if (!st.busy) phases_worker_reset();   // (no job out: cleared here)
    memset(&ph, 0, sizeof ph);
    { extern unsigned aoe_tiles_t[]; memset(aoe_tiles_t, 0, 4 * sizeof(unsigned)); }
    { extern unsigned aoe_join_late[2]; aoe_join_late[0] = aoe_join_late[1] = 0; }
    { extern unsigned aoe_join_parts[4]; memset(aoe_join_parts, 0, sizeof(unsigned) * 4); }
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
    if (ph.jobs) fprintf(stderr, "aoephase: a job: started %u.%u ms after its post, walk %u.%u ms, raster %u.%u ms, joined %u.%u ms after its post\n",
                         ph.start_t * 2 / ph.jobs / 10, ph.start_t * 2 / ph.jobs % 10, ph.walk_t * 2 / ph.jobs / 10, ph.walk_t * 2 / ph.jobs % 10,
                         ph.raster_t * 2 / ph.jobs / 10, ph.raster_t * 2 / ph.jobs % 10, ph.cycle_t * 2 / ph.jobs / 10, ph.cycle_t * 2 / ph.jobs % 10);
    {
        extern unsigned aoe_band_times[];
        if (ph.jobs) fprintf(stderr, "aoephase: a job's bands from the go to their ends: %u.%u, %u.%u, %u.%u, %u.%u ms\n",
                             aoe_band_times[0] * 2 / ph.jobs / 10, aoe_band_times[0] * 2 / ph.jobs % 10, aoe_band_times[1] * 2 / ph.jobs / 10,
                             aoe_band_times[1] * 2 / ph.jobs % 10, aoe_band_times[2] * 2 / ph.jobs / 10, aoe_band_times[2] * 2 / ph.jobs % 10,
                             aoe_band_times[3] * 2 / ph.jobs / 10, aoe_band_times[3] * 2 / ph.jobs % 10);
    }
    {
        extern unsigned aoe_tiles_t[];
        extern unsigned aoe_copy_t[];
        if (aoe_copy_t[1]) fprintf(stderr, "aoephase: the job past its scroll %u.%u ms after its start\n", aoe_copy_t[0] * 2 / aoe_copy_t[1] / 10, aoe_copy_t[0] * 2 / aoe_copy_t[1] % 10);
        if (aoe_tiles_t[2]) fprintf(stderr, "aoephase: the walk's tiles: %u jobs, begun %u.%u ms after the job, %u.%u ms and %u instructions each\n", aoe_tiles_t[2],
                                    aoe_tiles_t[3] * 2 / aoe_tiles_t[2] / 10, aoe_tiles_t[3] * 2 / aoe_tiles_t[2] % 10,
                                    aoe_tiles_t[0] * 2 / aoe_tiles_t[2] / 10, aoe_tiles_t[0] * 2 / aoe_tiles_t[2] % 10, aoe_tiles_t[1] / aoe_tiles_t[2]);
    }
    {
        extern unsigned aoe_join_late[2], aoe_join_parts[4];
        if (ph.jobs) fprintf(stderr, "aoephase: join parts (job end to seen, bands, tiles, settle): %u, %u, %u, %u (0.2 ms)\n", aoe_join_parts[0] / ph.jobs,
                             aoe_join_parts[1] / ph.jobs, aoe_join_parts[2] / ph.jobs, aoe_join_parts[3] / ph.jobs);
        if (ph.jobs) fprintf(stderr, "aoephase: a join done %u.%u ms after the job's end, %u.%u ms after it began\n", aoe_join_late[0] * 2 / ph.jobs / 10,
                             aoe_join_late[0] * 2 / ph.jobs % 10, aoe_join_late[1] * 2 / ph.jobs / 10, aoe_join_late[1] * 2 / ph.jobs % 10);
    }
    {
        extern unsigned aoe_rl_t[];
        if (aoe_rl_t[4]) fprintf(stderr, "aoephase: the raster's last draws begun %u.%u ms after the job, split in %u.%u ms, the tiles waited for %u.%u ms, the job's end %u.%u ms after its bands\n",
                                 aoe_rl_t[0] * 2 / aoe_rl_t[4] / 10, aoe_rl_t[0] * 2 / aoe_rl_t[4] % 10, aoe_rl_t[1] * 2 / aoe_rl_t[4] / 10,
                                 aoe_rl_t[1] * 2 / aoe_rl_t[4] % 10, aoe_rl_t[2] * 2 / aoe_rl_t[4] / 10, aoe_rl_t[2] * 2 / aoe_rl_t[4] % 10,
                                 aoe_rl_t[3] * 2 / aoe_rl_t[4] / 10, aoe_rl_t[3] * 2 / aoe_rl_t[4] % 10);
    }
    {
        extern unsigned aoe_mode0_all, aoe_mode0_far;
        fprintf(stderr, "aoephase: pictures drawn whole %u, of them by a scroll past the view %u\n", aoe_mode0_all, aoe_mode0_far);
    }
    if (aoe_path_calls) fprintf(stderr, "aoephase: path distances %u, %u of them not as the doubles\n", aoe_path_calls, aoe_path_wrong);
}

// AOE_SCROLL_COPY=words: a scroll moves the view by words, on 8 pixels across, as before (OverlayMemCopy); read here, on the first core
enum { JOB_WORKERS = 5 };   // (aoe_view_jobs': workers 4 to 8)
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
            const char* jw = getenv("AOE_JOB_WORKERS");
            const char* rb = getenv("AOE_RASTER_BANDS");   // (0: the last draws on the view's worker alone, the way before)
            int jobs = jw ? atoi(jw) : JOB_WORKERS, bands = rb ? atoi(rb) : AOE_RASTER_BANDS;
            if (bands > AOE_RASTER_BANDS) bands = AOE_RASTER_BANDS;
            // the view's, the one a song is made on (aoe_music.cpp), the game's data inflated ahead (aoe_asm.cpp), those of
            // aoe_view_jobs, the raster's band workers (aoe_raster_levels) and the walk's tiles' (aoe_view_tiles_start)
            st.workers = mcw_open(4 + jobs + bands);
            st.band_base = 4 + jobs;
            st.bands = st.workers >= 3 + jobs + bands ? bands : 0;
            tiles_worker = st.workers >= 4 + jobs + bands ? 4 + jobs + bands : 0;
            if (st.workers > 0) {
                mcw_touch_code((const void*) _start, (unsigned) (etext - (char*) _start));
                mcw_touch(__DATA_BEGIN__, (unsigned) (_end - __DATA_BEGIN__));
            }
        }
    }
    return st.wanted;
}

int aoe_view_second_worker(void) { aoe_view_wanted(); return st.workers >= 2 ? 2 : 0; }
int aoe_view_third_worker(void) { aoe_view_wanted(); return st.workers >= 3 ? 3 : 0; }

// n jobs fn(a0[i], a1[i]) on workers 4 to 8, as many at once as there are, joined before the return; on the first core where there are
// none (AOE_VIEW=inline, AOE_NOWINDOW). What a job stores is its own, and the first core stored to it first (the zone maps' floods:
// zone_map.cpp)
void aoe_view_jobs(void (*fn)(unsigned, unsigned), const unsigned* a0, const unsigned* a1, int n) {
    aoe_view_wanted();
    int have = st.band_base - 4 < st.workers - 3 ? st.band_base - 4 : st.workers - 3;
    if (have <= 0) { for (int i = 0; i < n; i++) fn(a0[i], a1[i]); return; }
    int next = 0, out[JOB_WORKERS] = {0};
    while (next < n) {
        for (int k = 0; k < have && next < n; k++) {
            if (out[k] && !mcw_done(4 + k)) continue;
            mcw_post(4 + k, (mcw_fn) fn, a0[next], a1[next]);
            out[k] = 1;
            next++;
        }
        if (next < n) mcw_rest();
    }
    for (int k = 0; k < have; k++) if (out[k]) mcw_wait(4 + k);
}
int aoe_view_busy(void) { return st.busy; }
int aoe_view_in_job(void) { return st.inline_job || (st.workers && mcw_on_worker()); }
void aoe_view_walked(void) { job_counts.walked_at = my_instructions(); job_times.walked_at = my_time(); walked.v = 1; }
static void copy_serve(void);
// (and a scroll's copy the job asks for, made at once: the walk waits for it; TPanel::handle_paint asks between panels)
void aoe_view_poll(void) { if (st.busy && st.workers) { mcw_done(1); copy_serve(); } }

// The walk's tiles (view.cpp's aoe_terrain_loop, from the walk's copies) on a worker of their own, from when the copies are made, beside
// the rest of the walk and the world's update: posted with the view's job by the first core, it waits for the walk's word, and the view's
// job waits for it before its last draws. With no such worker, or a walk not split, the view's worker makes them after the walk.
static struct { volatile int go; RGE_View* view; char pad[8]; } tiles_go __attribute__((aligned(16)));
static struct { volatile int done; char pad[12]; } tiles_done __attribute__((aligned(16)));
static int tiles_posted, tiles_frame;
unsigned aoe_tiles_t[4] __attribute__((aligned(16)));   // (aoephase: the tiles' job's time from the go and its instructions, summed; jobs)
static struct { unsigned t0, t1, i0, i1; } tiles_meas __attribute__((aligned(16)));
static void tiles_job(uint32_t, uint32_t frame) {
    while (tiles_go.go != (int) frame && tiles_go.go != -(int) frame) __asm__ volatile(".word 0x0000100f" ::: "memory");   // (mc_end_pass)
    tiles_meas.t0 = my_time(); tiles_meas.i0 = my_instructions();
    if (tiles_go.go == (int) frame) tiles_go.view->aoe_terrain_loop();
    tiles_meas.t1 = my_time(); tiles_meas.i1 = my_instructions();
    __asm__ volatile("" ::: "memory");
    tiles_done.done = (int) frame;
}
int aoe_view_tiles_start(RGE_View* view) {   // (the walk: 0 when no tiles' worker waits for it)
    if (!tiles_posted) return 0;
    tiles_go.view = view;
    __asm__ volatile("" ::: "memory");
    tiles_go.go = tiles_frame;
    return 1;
}
void aoe_view_tiles_wait(RGE_View* view) {     // (the walk given back)
    if (!tiles_posted || tiles_go.go != tiles_frame) { view->aoe_terrain_loop(); return; }
    while (tiles_done.done != tiles_frame) __asm__ volatile(".word 0x0000100f" ::: "memory");
}

// A list that grows in the job: a piece of this, which is never given back. A big picture's row index asks too.
static char arena[1 << 21] __attribute__((aligned(16)));
static struct { unsigned used; char pad[12]; } arena_at __attribute__((aligned(16)));
static char band_arena[AOE_RASTER_BANDS][1 << 18] __attribute__((aligned(16)));
static struct { unsigned used; char pad[12]; } band_arena_at[AOE_RASTER_BANDS] __attribute__((aligned(16)));
extern "C" void* aoe_any_calloc(size_t n, size_t size) {
    unsigned bytes = ((unsigned) (n * size) + 15) & ~15u;
    {   // (a band's own: its span nodes grow on its worker beside the others)
        unsigned sp;
        __asm__("mv %0, sp" : "=r"(sp));
        for (int k = 0; k < AOE_RASTER_BANDS; k++)
            if (aoe_bands_on && sp - aoe_band_stack[k].lo < aoe_band_stack[k].hi - aoe_band_stack[k].lo) {
                if (band_arena_at[k].used + bytes > sizeof band_arena[k]) { fprintf(stderr, "aoe: a band's memory is used up\n"); _Exit(9); }
                void* p = band_arena[k] + band_arena_at[k].used;
                band_arena_at[k].used += bytes;
                memset(p, 0, bytes);
                return p;
            }
    }
    if (!aoe_view_in_job() || !mcw_on_worker()) return calloc(n, size);
    if (arena_at.used + bytes > sizeof arena) { fprintf(stderr, "aoe: the job's memory is used up\n"); _Exit(9); }
    void* p = arena + arena_at.used;
    arena_at.used += bytes;
    memset(p, 0, bytes);
    return p;
}
extern "C" void aoe_any_free(void* p) {
    if ((char*) p >= arena && (char*) p < arena + sizeof arena) return;
    if ((char*) p >= band_arena[0] && (char*) p < band_arena[0] + sizeof band_arena) return;
    free(p);
}

extern void aoe_asm_for_worker(void);

// The raster's last draws by bands of rows (view.cpp's RGE_View::aoe_draw_levels; docs/games/aoe.md, "Speed in play"). A band job is
// posted with each view's job and waits, ending its passes, until the view's worker comes to its last draws: then it draws the rows of
// its band, each pixel with the same draws in the same order as the whole view, and says so. A view's job that ends with no last draws
// sends them away. What a band writes is its rows of the page and its own drawing state and word.
static struct { volatile int go; RGE_View* view; long lo[AOE_RASTER_BANDS + 1], hi[AOE_RASTER_BANDS + 1]; } raster __attribute__((aligned(16)));
static struct { volatile int done; unsigned t0, t1, n; } band_done[AOE_RASTER_BANDS] __attribute__((aligned(16)));
unsigned aoe_band_times[AOE_RASTER_BANDS + 1] __attribute__((aligned(16)));
#define band_t aoe_band_times
static unsigned go_t __attribute__((aligned(16)));
static int raster_frame;
// Each band's clip mask and terrain masks (RGE_View::aoe_band_frame): made by the first core for the view's size, their span nodes
// stored to there first; in texels of their own, as the band writes its pools' words
enum { BAND_MASTER, BAND_TERRAIN, BAND_FOG, BAND_MASKS };
static char band_mask_room[BAND_MASKS][AOE_RASTER_BANDS + 1][(sizeof(TSpan_List_Manager) + 15) & ~15] __attribute__((aligned(16)));
static TSpan_List_Manager* band_mask[BAND_MASKS][AOE_RASTER_BANDS + 1];
static int band_lines = -1, band_pixels = -1;
static void band_masks_for(RGE_View* view) {
    TSpan_List_Manager* m = view->Master_Clip_Mask;
    if (!m || (band_lines == m->Num_Lines && band_pixels == m->Num_Pixels)) return;
    for (int i = 0; i < BAND_MASKS; i++)
        for (int b = 0; b <= AOE_RASTER_BANDS; b++) {
            if (band_mask[i][b]) continue;   // (a pool kept: only its lines' arrays and limits are set at each frame)
            band_mask[i][b] = new (band_mask_room[i][b]) TSpan_List_Manager(m->Num_Pixels, m->Num_Lines);
        }
    band_lines = m->Num_Lines; band_pixels = m->Num_Pixels;
}
static int bands_for(RGE_View* v) {
    TSpan_List_Manager* m = v->Master_Clip_Mask;
    TSpan_List_Manager* t = v->Terrain_Clip_Mask;
    TSpan_List_Manager* f = v->Terrain_Fog_Clip_Mask;
    return st.workers > 0 && mcw_on_worker() && aoe_view_core() && band_lines == m->Num_Lines && band_pixels == m->Num_Pixels
           && t->Num_Lines == m->Num_Lines && f->Num_Lines == m->Num_Lines && t->Num_Pixels == m->Num_Pixels && f->Num_Pixels == m->Num_Pixels
           ? st.bands : 0;
}
// The walk's tiles after it, from its copies (view.cpp, aoe_sp): where it keeps its terrain work, unless AOE_WALK=whole (the way before)
static int walk_whole = -1;
int aoe_view_split_walk(void) {
    if (walk_whole < 0) { const char* e = getenv("AOE_WALK"); walk_whole = e && !strcmp(e, "whole"); }
    return !walk_whole;
}
// The walk keeps its terrain work for the bands (view.cpp, aoe_tr): where there are bands, unless AOE_TERRAIN=walk (the way before)
static int terrain_walk = -1;
int aoe_view_record_terrain(void) {
    if (terrain_walk < 0) { const char* e = getenv("AOE_TERRAIN"); terrain_walk = e && !strcmp(e, "walk"); }
    RGE_View* v = st.view;
    return !terrain_walk && v && bands_for(v) > 0;
}
static void set_lines(TSpan_List_Manager* x, TSpan_List_Manager* m) {
    x->Line_Head_Ptrs = m->Line_Head_Ptrs; x->Line_Tail_Ptrs = m->Line_Tail_Ptrs;
    x->LeftMostPx = m->LeftMostPx; x->RightMostPx = m->RightMostPx; x->Span_Count = m->Span_Count;
    x->Num_Lines = m->Num_Lines; x->Num_Pixels = m->Num_Pixels; x->Min_Span_Px = m->Min_Span_Px; x->Max_Span_Px = m->Max_Span_Px;
}
static inline void end_pass(void) { __asm__ volatile(".word 0x0000100f" ::: "memory"); }   // (mc_end_pass)
static void band_job(uint32_t k, uint32_t frame) {
    char here;
    aoe_band_stack[k].lo = (unsigned) &here - (60u << 10);
    aoe_band_stack[k].hi = (unsigned) &here + 1024;
    __asm__ volatile("" ::: "memory");
    while (raster.go != (int) frame && raster.go != -(int) frame) end_pass();
    band_done[k].t0 = my_time();
    if (raster.go == (int) frame)
        raster.view->aoe_band_frame(raster.lo[k + 1], raster.hi[k + 1], band_mask[BAND_MASTER][k + 1], band_mask[BAND_TERRAIN][k + 1],
                                    band_mask[BAND_FOG][k + 1]);
    band_done[k].t1 = my_time();
    __asm__ volatile("" ::: "memory");
    band_done[k].done = (int) frame;
}
void aoe_band_split(RGE_View* v, long lo, long hi, int parts, long* bound);
static int split_lines = -1;   // AOE_BANDS=lines: the bands of equal lines, as before
unsigned aoe_rl_t[5] __attribute__((aligned(16)));
static unsigned rl_end __attribute__((aligned(16)));   // (aoephase: from the job's start to aoe_raster_levels, to its split made, to the tiles had, to the go; jobs)
void aoe_raster_levels(RGE_View* v) {
    unsigned rl0 = my_time();
    if (split_lines < 0) { const char* e = getenv("AOE_BANDS"); split_lines = e && !strcmp(e, "lines"); }
    TSpan_List_Manager* m = v->Master_Clip_Mask;
    aoe_view_copy_settle();   // (the scroll's copy of the page made before anything draws on it)
    int bands = bands_for(v);
    extern int aoe_tiles_owed;
    unsigned rl1 = my_time();
    // (the walk's tiles may still be made: the bands follow their record as it grows, view.cpp's aoe_terrain_band; the split weighs it
    // only once whole. Waited for after the last draws)
    int tiles_out = aoe_tiles_owed && tiles_posted && tiles_go.go == tiles_frame;
    if (aoe_tiles_owed && !tiles_out) { aoe_view_tiles_wait(v); aoe_tiles_owed = 0; extern void aoe_view_tiles_counted(void); aoe_view_tiles_counted(); }
    unsigned rl2 = my_time();
    if (!bands) {
        v->aoe_band_frame(-0x7fffffff, 0x7fffffff, m, v->Terrain_Clip_Mask, v->Terrain_Fog_Clip_Mask);
        if (tiles_out) { aoe_view_tiles_wait(v); aoe_tiles_owed = 0; extern void aoe_view_tiles_counted(void); aoe_view_tiles_counted(); }
        return;
    }
    long lo = m->Min_Line, hi = m->Max_Line, n = hi - lo + 1;
    // (by what each would draw, on 4 lines: a texel of the masks' arrays of a line each has one writer; every line in one)
    long bound[AOE_RASTER_BANDS + 2];
    if (split_lines) for (int b = 1; b <= bands; b++) bound[b] = (lo + n * b / (bands + 1)) & ~3L;
    else aoe_band_split(v, lo, hi, bands + 1, bound);
    for (int b = 0; b <= bands; b++) {
        raster.lo[b] = b ? bound[b] : -0x7fffffff;
        raster.hi[b] = b < bands ? bound[b + 1] - 1 : 0x7fffffff;
    }
    if (n <= 0) for (int b = 0; b <= bands; b++) { raster.lo[b] = b ? 1 : -0x7fffffff; raster.hi[b] = b ? 0 : 0x7fffffff; }
    for (int b = 0; b <= bands; b++) {   // (each band's masks: the view's arrays, their own span nodes and lines)
        TSpan_List_Manager* x = band_mask[BAND_MASTER][b];
        set_lines(x, m);
        x->Min_Line = raster.lo[b] > lo ? raster.lo[b] : lo; x->Max_Line = raster.hi[b] < hi ? raster.hi[b] : hi;
        set_lines(band_mask[BAND_TERRAIN][b], v->Terrain_Clip_Mask);
        set_lines(band_mask[BAND_FOG][b], v->Terrain_Fog_Clip_Mask);
    }
    for (int k = 0; k < bands; k++) aoe_asm_for_band(k);   // (each band starts as this worker stands)
    unsigned rl3 = my_time();
    aoe_rl_t[0] += rl0 - job_times.start; aoe_rl_t[1] += rl3 - rl2; aoe_rl_t[2] += rl2 - rl1; aoe_rl_t[4]++;
    raster.view = v;
    __asm__ volatile("" ::: "memory");
    go_t = my_time();
    raster.go = raster_frame;
    v->aoe_band_frame(raster.lo[0], raster.hi[0], band_mask[BAND_MASTER][0], band_mask[BAND_TERRAIN][0], band_mask[BAND_FOG][0]);
    band_t[0] += my_time() - go_t;
    for (int k = 0; k < bands; k++) while (band_done[k].done != raster_frame) end_pass();
    for (int k = 0; k < bands; k++) band_t[k + 1] += band_done[k].t1 - go_t;
    if (tiles_out) { aoe_view_tiles_wait(v); aoe_tiles_owed = 0; extern void aoe_view_tiles_counted(void); aoe_view_tiles_counted(); }
    rl_end = my_time();
}
RGE_View* aoe_view_posted(void) { return st.view; }
// The view's own copies of the page's and the save area's objects: its clip lists, and no surface to lock.
static TDrawArea* area_copy;
static TDrawSystem* system_copy;
static TDrawArea* area_real;

static int copy_wait = -1;   // AOE_COPY_WAIT=1: the first core stays after the post until the job is past its scroll (the polls do it now)
static int first_copy_ok = -1;   // the first core's CSR 0x8b0: 1 when the machine has the copy (aoe_view_first_copy)
struct AoeViewStack aoe_view_stack;
// A picture with the view drawn again since the last present (the caption's frames: host.cpp)
int aoe_view_drew;
// (a present with no view drawn while the view draws others, in a game, is the same picture: not a frame; a menu's are frames)
static unsigned last_drew_at;
int aoe_view_new_picture(void) {
    unsigned t = my_time();
    int d = aoe_view_drew || (unsigned) (t - last_drew_at) > 5000;   // (a second of the machine's 5 kHz)
    if (aoe_view_drew) last_drew_at = t;
    aoe_view_drew = 0;
    return d;
}
static void job(uint32_t view, uint32_t frame) {
    phases_worker_reset();
    job_counts.start = my_instructions();
    job_times.start = my_time();
    // the stack this job runs on (aoe_cores.h, aoe_view_core): noted once, the same worker's each time
    char here;
    unsigned lo = (unsigned) &here - (60u << 10), hi = (unsigned) &here + 1024;
    if (st.workers > 0 && (aoe_view_stack.lo != lo || aoe_view_stack.hi != hi)) { aoe_view_stack.lo = lo; aoe_view_stack.hi = hi; }
    ((RGE_View*) view)->RGE_View::draw();
    aoe_view_copy_settle();
    walked.v = 1;
    if (frame && raster.go != (int) frame) { __asm__ volatile("" ::: "memory"); raster.go = -(int) frame; }   // (no last draws: the bands go)
    if (frame && tiles_posted && tiles_go.go != (int) frame) { __asm__ volatile("" ::: "memory"); tiles_go.go = -(int) frame; }   // (and the tiles')
    job_counts.end = my_instructions();
    job_times.end = my_time();
    __asm__ volatile("" ::: "memory");
    job_finished.v = 1;   // (the join looks here first: mcw_done saw the job's end 6 ms later in the drag)
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
    // (made again when the page's size changes: a change of layout, port/aoe_layout.cpp; the old one's clips kept the view to its size)
    static long own_w, own_h;
    if (own && (own_w != real->Width || own_h != real->Height)) {
        if (getenv("AOE_DEBUG")) fprintf(stderr, "aoe: the view's area %ld x %ld now, the view %ld x %ld\n", (long) real->Width, (long) real->Height,
                                         (long) view->pnl_wid, (long) view->pnl_hgt);
        delete own; own = 0; memset(made, 0, sizeof made);
    }
    if (!own) { own = new TSpan_List_Manager(real->Width, real->Height); made[4] = 0; own_w = real->Width; own_h = real->Height; }
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
    aoe_view_drew = 1;
    walked.v = 0;
    st.view = view;
    st.busy = 1;
    if (st.workers > 0) {
        if (st.bands) {
            band_masks_for(view);
            aoe_bands_on = 1;
            raster_frame = (raster_frame + 1) & 0x3fffffff;
            if (!raster_frame) raster_frame = 1;
            for (int k = 0; k < st.bands; k++) mcw_post(st.band_base + k, band_job, (uint32_t) k, (uint32_t) raster_frame);
            tiles_posted = tiles_worker && aoe_view_split_walk();
            if (tiles_posted) { tiles_frame = raster_frame; mcw_post(tiles_worker, tiles_job, 0, (uint32_t) raster_frame); }
        }
        job_times.posted = my_time();
        job_finished.v = 0;
        if (copy_wait < 0) copy_wait = getenv("AOE_COPY_WAIT") != 0;
        copy_passed.v = 0;
        __asm__ volatile("" ::: "memory");
        mcw_post(1, job, (uint32_t) view, st.bands ? (uint32_t) raster_frame : 0);
        // the scroll's copy, which the job asks of this core within its first passes, made at once (the walk waits for it): this core
        // stays until the job is past it (aoe_view_copy_passed, from view.cpp's draw)
        if (first_copy_ok > 0 && copy_wait) while (!copy_passed.v && !mcw_done(1)) { copy_serve(); mcw_rest(); }   // (AOE_COPY_WAIT=1)
        copy_serve();
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
unsigned aoe_copy_t[2] __attribute__((aligned(16)));
unsigned aoe_mode0_all __attribute__((aligned(16))), aoe_mode0_far __attribute__((aligned(16)));   // (aoephase: the pictures the view drew whole, and how many for a scroll past it)
void aoe_view_copy_passed(void) { if (aoe_view_core()) { aoe_copy_t[0] += my_time() - job_times.start; aoe_copy_t[1]++; __asm__ volatile("" ::: "memory"); copy_passed.v = 1; } }
static void copy_now(const void* src, void* dst, unsigned n) {
    __asm__ volatile("csrw 0x8b1, %0\n csrw 0x8b2, %1\n csrw 0x8b3, %2\n csrw 0x8b0, %3" :: "r"(src), "r"(dst), "r"(n), "r"(1) : "memory");
}
struct AoeCopyAsked aoe_copy_asked;   // (copy_ask.state's 1 seen by the first core's sprites: aoe_asm.cpp)
static void copy_serve(void) {
    if (copy_ask.state != 1) return;
    copy_now(copy_ask.src, copy_ask.dst, copy_ask.n);
    aoe_copy_asked.v = 0;
    copy_ask.state = 2;
}
void aoe_view_copy_serve(void) { copy_serve(); }
int aoe_view_first_copy(const void* src, void* dst, unsigned bytes) {
    if (first_copy_ok <= 0) return 0;
    if (!aoe_view_core()) { copy_now(src, dst, bytes); return 1; }
    aoe_view_copy_settle();   // (one asked before: made first)
    copy_ask.src = src; copy_ask.dst = dst; copy_ask.n = bytes;
    __asm__ volatile("" ::: "memory");
    copy_ask.state = 1;
    aoe_copy_asked.v = 1;
    // (not waited for here: the walk reads none of the page's pixels; the raster waits for it before the bands draw: aoe_view_copy_settle)
    copy_owed = 1;
    return 1;
}
// The view's worker: a scroll's copy it asked of the first core, made before what follows writes the page
void aoe_view_copy_settle(void) {
    if (!copy_owed) return;
    while (copy_ask.state != 2) __asm__ volatile(".word 0x0000100f" ::: "memory");   // (mc_end_pass: the first core makes it)
    copy_ask.state = 0;
    copy_owed = 0;
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

unsigned aoe_join_late[2] __attribute__((aligned(16)));
unsigned aoe_join_parts[4] __attribute__((aligned(16)));
static unsigned join_t0;
void aoe_view_join(void) {
    if (!st.busy) return;
    join_t0 = my_time();
    aoe_lat('j', 0, 0);
    aoe_phase(AOE_PH_JOIN, 1);
    if (st.workers > 0) while (!job_finished.v && !mcw_done(1)) { copy_serve(); mcw_rest(); }   // (a scroll's copy the job asks for is made here too)
    if (st.workers > 0) while (!mcw_done(1)) mcw_rest();
    unsigned jt1 = my_time();
    for (int k = 0; k < st.bands; k++) while (!mcw_done(st.band_base + k)) mcw_rest();
    unsigned jt2 = my_time();
    if (tiles_posted) {
        while (!mcw_done(tiles_worker)) mcw_rest();
        tiles_posted = 0;
        aoe_tiles_t[0] += tiles_meas.t1 - tiles_meas.t0; aoe_tiles_t[1] += tiles_meas.i1 - tiles_meas.i0; aoe_tiles_t[2]++;
        aoe_tiles_t[3] += tiles_meas.t0 - job_times.start;
    }
    unsigned jt3 = my_time();
    { extern void aoe_view_tiles_settle(void); aoe_view_tiles_settle(); }   // (the tiles' view bytes the job kept apart: view.cpp)
    { extern void aoe_view_swap_lists(RGE_View*); if (st.view) aoe_view_swap_lists(st.view); }   // (the job's lists, swapped here: view.cpp)
    { extern unsigned aoe_join_parts[4]; aoe_join_parts[0] += jt1 - job_times.end; aoe_join_parts[1] += jt2 - jt1; aoe_join_parts[2] += jt3 - jt2;
      aoe_join_parts[3] += my_time() - jt3; }
    aoe_phase(AOE_PH_JOIN, 0);
    aoe_lat('k', (job_counts.walked_at - job_counts.start) / 1000, (job_counts.end - job_counts.walked_at) / 1000);
    ph.walk += job_counts.walked_at - job_counts.start; ph.raster += job_counts.end - job_counts.walked_at; ph.jobs++;
    ph.start_t += job_times.start - job_times.posted; ph.walk_t += job_times.walked_at - job_times.start;
    ph.raster_t += job_times.end - job_times.walked_at; ph.cycle_t += my_time() - job_times.posted;
    aoe_rl_t[3] += job_times.end - rl_end;
    { extern unsigned aoe_join_late[2]; aoe_join_late[0] += my_time() - job_times.end; aoe_join_late[1] += my_time() - join_t0; }
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
    int bands = st.bands;
    if (whole) st.bands = 0;   // (the picture drawn whole by the view's worker alone: it checks the raster's bands too)
    aoe_draw_window_now();
    aoe_view_join();
    st.bands = bands;
    aoe_view_deferred();   // (the panels over the view, as the frame's end draws them)
}

// Before a change of layout (port/aoe_layout.cpp): the view given back the page's real area, so that the game's new layout sizes it
// by the page's new size and not by the view's copy of the old one (which kept the view to 640 x 354 at 800 x 600), and the copies
// made again at the next post
void aoe_view_unhook(void) {
    RGE_View* view = st.view;
    if (view && area_real && view->TPanel::render_area == area_copy) {
        view->TPanel::render_area = area_real;
        view->cur_render_area = area_real;
    }
    // the save area too (the view's picture kept between draws): given back its own system, unlocked, so the layout sizes it anew;
    // with the copy's system it kept the old picture, which the view then showed in the corner of the larger page
    if (view && area_real && view->save_area1 && view->save_area1->DrawSystem == system_copy) {
        view->save_area1->DrawSystem = area_real->DrawSystem;
        view->save_area1->Unlock("aoe_view");
    }
    area_real = 0;
}

// After a change of layout (port/aoe_layout.cpp): the view draws every tile again at its new size, not only what a scroll uncovers
void aoe_view_whole(void) {
    RGE_View* view = st.view;
    if (!view) return;
    view->render_terrain_mode = 0;
    view->set_redraw(TPanel::RedrawNormal);
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
