// The view on a worker core (port/aoe_view.cpp; docs/games/aoe.md, "The view on a worker").
#ifndef AOE_CORES_H
#define AOE_CORES_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int mcw_on_worker(void);
int aoe_view_busy(void);          // a view's job has been posted and not joined
void aoe_view_join(void);         // wait for the job; afterwards nothing runs but this core
void aoe_view_walked(void);       // the job: the game's state will not be read any more
void aoe_view_poll(void);         // this core is at work: a worker stopped at a page goes on
int aoe_view_in_job(void);        // this code runs as the job (on a worker, or by the core that posted it)
void* aoe_any_calloc(size_t n, size_t size);   // what a job's growing lists ask for: never the heap on a worker
void aoe_any_free(void* p);
// What the walk sets around a shape and a panel's drawing must not see: one for each core, in texels of their own.
struct AoeCoreWords { long fog; int capture; char pad[8]; } __attribute__((aligned(16)));
extern struct AoeCoreWords aoe_core_words[2];
// A scroll by the machine's copy, the view on 16 pixels across (0), or by words on 8 (1): set by the first core before any view
struct AoeScrollWay { int words; char pad[12]; } __attribute__((aligned(16)));
extern struct AoeScrollWay aoe_scroll_way;
// Where a frame's time goes, between AOE_MARK's two lines (aoephase: at its end): the first core's parts by the clock word
enum { AOE_PH_WALKED, AOE_PH_UPDATE, AOE_PH_JOIN, AOE_PH_PAINT, AOE_PH_COUNT };
void aoe_phase(int part, int begin);
void aoe_phases_reset(void);
void aoe_phases_print(long frames);
// The world's update in slices (port/aoe_world.cpp): between two objects' updates it gives way once its time is spent
extern int aoe_world_inside;
extern unsigned aoe_world_deadline;
void aoe_world_give_way(void);
static inline void aoe_world_may_give_way(void) {
    if (aoe_world_inside) {
        unsigned t;
        __asm__ volatile("rdtime %0" : "=r"(t));
        if ((int) (t - aoe_world_deadline) >= 0) aoe_world_give_way();
    }
}
void aoe_world_finish(void);       // an update out is run to its end (before input that changes the game)
void aoe_world_picture(void);      // a picture begins
int aoe_world_slices(void);
// (int) (sqrt(d2) * 16.0) of the path finder, by a table (port/aoe_path.c)
extern unsigned short aoe_path_table[];
extern int aoe_path_made;
int aoe_path_dist_slow(int d2);
static inline int aoe_path_dist(int d2) {
    return aoe_path_made == 1 && (unsigned) d2 < 2 * 256 * 256 + 1 ? aoe_path_table[d2] : aoe_path_dist_slow(d2);
}
// Whether this runs on the view's worker: its stack, which the job notes at its start (mcw_on_worker is a call, at every
// sprite: 6% of the view's worker). Core 0 and the world's own stack are never in it.
struct AoeViewStack { unsigned lo, hi; char pad[8]; } __attribute__((aligned(16)));
extern struct AoeViewStack aoe_view_stack;
static inline int aoe_view_core(void) {
    unsigned sp;
    __asm__("mv %0, sp" : "=r"(sp));
    return sp - aoe_view_stack.lo < aoe_view_stack.hi - aoe_view_stack.lo;
}
// The machine's copy of a scroll, made by the first core: 0 when there is none (then by words). The first core makes one the view's job
// asks for wherever it looks (aoe_view_copy_serve): between panels and every 16 rows of its sprites
int aoe_view_first_copy(const void* src, void* dst, unsigned bytes);
struct AoeCopyAsked { volatile unsigned v; char pad[12]; } __attribute__((aligned(16)));
extern struct AoeCopyAsked aoe_copy_asked;
void aoe_view_copy_serve(void);
// The raster's band workers (aoe_view.cpp, aoe_raster_levels): the view's last draws by bands of rows, the view's worker the first band
// and these the others, each with its own drawing state (aoe_asm.cpp), found by the stack its job notes
#define AOE_RASTER_BANDS 7
struct AoeBandStack { unsigned lo, hi; char pad[8]; } __attribute__((aligned(16)));
extern struct AoeBandStack aoe_band_stack[AOE_RASTER_BANDS];
extern int aoe_bands_on;
void aoe_asm_for_band(int k);
#define fog_next_shape (aoe_core_words[aoe_view_core()].fog)
#define SDI_Capture_Info (aoe_core_words[aoe_view_core()].capture)
#ifdef __cplusplus
}
class RGE_View;
class TPanel;
class TDrawArea;
class TDrawSystem;
void aoe_view_post(RGE_View* view);      // from RGE_Main_View::draw
void aoe_view_wait_walked(void);         // and then the pick that needs this core
void aoe_view_defer(TPanel* panel);      // a panel over the view is drawn when the view is done
void aoe_view_deferred(void);
int aoe_view_wanted(void);
RGE_View* aoe_view_posted(void);
#endif
#endif
