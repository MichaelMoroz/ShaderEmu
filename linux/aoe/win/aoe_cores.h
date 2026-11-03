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
#define fog_next_shape (aoe_core_words[mcw_on_worker() ? 1 : 0].fog)
#define SDI_Capture_Info (aoe_core_words[mcw_on_worker() ? 1 : 0].capture)
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
