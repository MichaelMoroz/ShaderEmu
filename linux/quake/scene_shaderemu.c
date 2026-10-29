/*
 * The picture on a worker core (docs/games/quake.md, "The picture on a worker core", "Drawn without a
 * gap"). A frame's drawing, everything that writes the GPU's list and swaps it, is a worker's:
 * one call that stays, drawing frame after frame as this core gives them (SE_SceneFrame), from
 * one of two copies of what a frame reads in turn. While it draws frame N this core reads the
 * next messages, places the entities, makes frame N + 1's view and copy, and the worker goes on to
 * N + 1 as soon as N is swapped. Before anything that changes what drawing reads otherwise, it
 * is stopped and waited for (SE_SceneOwn). QUAKE_SCENE=inline: all here.
 *
 * Copied for a frame: the client's state, the entities drawn, the lights, the frame's count and
 * time, the blend, the view (refdef, origin and axes, its leaf), the light styles' values and
 * which particles are drawn (se_scene.h, r_part.c).
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "quakedef.h"
#include "mcwx.h"

extern vec3_t	vup, vpn, vright, r_origin;
extern mleaf_t	*r_viewleaf, *r_oldviewleaf;
extern int	d_lightstylevalue[256];

client_state_t	*se_cl_now = &cl;
entity_t	*se_ents_now = cl_entities, **se_vis_now = cl_visedicts;
int		*se_numvis_now = &cl_numvisedicts, *se_framecount_now = &host_framecount, *se_styles_now = d_lightstylevalue;
dlight_t	*se_dlights_now = cl_dlights;
float		*se_frametime_now = &host_frametime, *se_blend_now = v_blend;
unsigned	*se_realtime_now = &realtime_ms;
refdef_t	*se_refdef_now = &r_refdef;
vec3_t		*se_origin_now = &r_origin, *se_vpn_now = &vpn, *se_vright_now = &vright, *se_vup_now = &vup;
mleaf_t		**se_viewleaf_now = &r_viewleaf, **se_oldviewleaf_now = &r_oldviewleaf;
int		se_scene_split;		/* drawing on the worker: it asks nothing of the library */

/* A frame's copy (its entities at their own index, those of no index after them). */
typedef struct {
	client_state_t	cl;
	entity_t	*vis[MAX_VISEDICTS];
	dlight_t	dlights[MAX_DLIGHTS];
	refdef_t	refdef;
	vec3_t		origin, vpn, vright, vup;
	mleaf_t		*viewleaf, *oldviewleaf;
	int		numvis, framecount, styles[256], particles[8], runs, stage, unused2[2];
	volatile unsigned	recorded __attribute__ ((aligned (16)));	/* the 2D worker's: the frame it recorded */
	unsigned	run[96][8];		/* the 2D screen's runs (qgl.c), made by this core */
	float		frametime, blend[4];
	unsigned	realtime, frame, instructions, unused;
} slot_t;
static slot_t	slots[2] __attribute__ ((aligned (16)));
static entity_t	ents[2][MAX_EDICTS + MAX_VISEDICTS] __attribute__ ((aligned (16)));

static int		how = -1, running, split_last, drawing;
static mcw_handle	record_job[2];		/* the 2D screen's call of a copy, while it is not joined */
client_state_t	*se2_cl_now = &cl;	/* what the 2D screen reads (se_scene2d.h) */
int		*se2_framecount_now = &host_framecount;
float		*se2_frametime_now = &host_frametime;
unsigned	*se2_realtime_now = &realtime_ms;
static mcw_handle	job;
static unsigned		counted;
		/* frames whose swap this core has counted */
static volatile unsigned	published, stop;	/* this core's: the last frame given, and "no more" */
static volatile unsigned	ended;			/* the worker's: the last frame it swapped */
unsigned		se_scene_unrecorded;	/* frames whose 2D screen could not be recorded (and is not drawn) */
unsigned		se_scene_frames, se_scene_waits, se_scene_instructions, se_scene_window, se_scene_waited;	/* for the quakestat: lines */

void SE_ScreenWorker (void);	/* gl_screen.c */
int SE_ScreenRecord (unsigned *vertices, int quads, void *runs, int most);
int SE_ScreenSplitOk (void);
void SE_SceneFront (void);	/* gl_rmain.c */
void SE_ViewFront (void);	/* view.c */
void SE_LightFrame (void);	/* world_shaderemu.c */
int SE_LightWait (void);
int SE_WorldMade (void);
void SE_LightAheadFill (int slot);	/* gl_rlight.c */
void SE_LightAheadDraw (int slot);
int SE_Seated (void);		/* vid_shaderemu.c */
void SE_AfterSwap (void);
int SE_HoldLate (void);
int SE_HoldAt (unsigned ahead);
int SE_ParticlesApart (void);	/* r_part.c */
void SE_ParticlesView (int *view);
extern int	*se_particles_now;
extern cvar_t	lcd_x, r_mirroralpha;

/* the drawing worker's clock ticks (rdtime), written there only: its frames, its swaps, its waits for a frame */
unsigned se_draw_ticks[3];
unsigned se_scene_wait_ticks;	/* this core's, waiting for the drawing worker */

static unsigned ticks (void)
{
	unsigned v;

	__asm__ volatile ("rdtime %0" : "=r"(v));
	return v;
}

static unsigned instructions (void)
{
	unsigned v;

	__asm__ volatile ("rdinstret %0" : "=r"(v));
	return v;
}

static void real (void)
{
	se_cl_now = &cl;
	se_ents_now = cl_entities;
	se_vis_now = cl_visedicts;
	se_numvis_now = &cl_numvisedicts;
	se_framecount_now = &host_framecount;
	se_styles_now = d_lightstylevalue;
	se_dlights_now = cl_dlights;
	se_frametime_now = &host_frametime;
	se_blend_now = v_blend;
	se_realtime_now = &realtime_ms;
	se_refdef_now = &r_refdef;
	se_origin_now = &r_origin, se_vpn_now = &vpn, se_vright_now = &vright, se_vup_now = &vup;
	se_viewleaf_now = &r_viewleaf, se_oldviewleaf_now = &r_oldviewleaf;
	se_particles_now = NULL;
	se_scene_split = 0;
	se2_cl_now = &cl;
	se2_framecount_now = &host_framecount;
	se2_frametime_now = &host_frametime;
	se2_realtime_now = &realtime_ms;
}

/* The 2D screen of a copy, recorded on a worker of its own while another draws (SE_ScreenRecord). */
static uint32_t record_screen (uint32_t frame, uint32_t unused)
{
	slot_t	*s = &slots[frame & 1];
	int	runs;

	se2_cl_now = &s->cl;
	se2_framecount_now = &s->framecount;
	se2_frametime_now = &s->frametime;
	se2_realtime_now = &s->realtime;
	if ((runs = SE_ScreenRecord (qglStageMemory (s->stage), 1024, s->run, 96)) < 0) {
		runs = 0;
		se_scene_unrecorded++;
	}
	s->runs = runs;
	s->recorded = frame;
	return 0;
}

static void record_join (int which)
{
	/* (a job that settles waits for the drawing worker's hand to be submitted here) */
	while (record_job[which] && !mcw_ready (record_job[which])) {
		seglSwapTake ();
		mcw_rest ();
	}
	if (record_job[which])
		mcw_join (record_job[which]);
	record_job[which] = 0;
}

/* For the worker that draws, at its 2D screen: the runs of the frame it draws, when recorded. */
void SE_SceneRecorded (const unsigned **vertices, const void **runs, int *count)
{
	slot_t	*s = &slots[drawing & 1];

	while (s->recorded != (unsigned)drawing)
		mc_end_pass ();
	*vertices = qglStageMemory (s->stage);
	*runs = s->run;
	*count = s->runs;
}

/* The worker's call: the frames given, each from its copy, until it is told no more. */
static uint32_t scene_job (uint32_t unused0, uint32_t unused1)
{
	unsigned next = ended + 1, idle = ticks ();

	for (;;) {
		if (published >= next) {
			slot_t		*s = &slots[next & 1];
			unsigned	began = instructions (), at = ticks ();

			se_draw_ticks[2] += at - idle;
			se_cl_now = &s->cl;
			se_ents_now = ents[next & 1];
			se_vis_now = s->vis;
			se_numvis_now = &s->numvis;
			se_framecount_now = &s->framecount;
			se_styles_now = s->styles;
			se_dlights_now = s->dlights;
			se_frametime_now = &s->frametime;
			se_blend_now = s->blend;
			se_realtime_now = &s->realtime;
			se_refdef_now = &s->refdef;
			se_origin_now = &s->origin, se_vpn_now = &s->vpn, se_vright_now = &s->vright, se_vup_now = &s->vup;
			se_viewleaf_now = &s->viewleaf, se_oldviewleaf_now = &s->oldviewleaf;
			se_particles_now = s->particles;
			se_scene_split = 1;
			SE_LightAheadDraw (next & 1);
			drawing = next;
			SE_ScreenWorker ();
			s->instructions = instructions () - began;
			se_draw_ticks[0] += (idle = ticks ()) - at;
			ended = next++;
			continue;
		}
		if (stop)
			return 0;
		mc_end_pass ();
	}
}

/* The frames the worker has swapped since this core last looked: counted, held, said. */
static void count_swaps (void)
{
	seglSwapTake ();	/* (the worker's swap is this core's to submit: the GPU's lock is the window system's too) */
	while (counted != ended) {
		counted++;
		se_scene_instructions += slots[counted & 1].instructions;
		SE_AfterSwap ();
	}
}

/* Waits until the worker has swapped frame `frame`; the library is asked every fourth pass (a
 * page the worker stopped at is served then: it looks at every worker each time). */
static void wait_for (unsigned frame)
{
	int		passes = 0;
	unsigned	began;

	if ((int)(ended - frame) >= 0)
		return;
	began = instructions ();
	se_scene_waits++;
	se_scene_wait_ticks -= ticks ();
	while ((int)(ended - frame) < 0 && (++passes & 3 || !mcw_ready (job) || (int)(ended - frame) < 0)) {
		if (!(passes & 3) && record_job[0])
			mcw_ready (record_job[0]);	/* (the 2D worker's pages too) */
		if (!(passes & 3) && record_job[1])
			mcw_ready (record_job[1]);
		seglSwapTake ();
		mcw_rest ();
	}
	se_scene_waited += instructions () - began;
	se_scene_wait_ticks += ticks ();
}

/* Before this core changes what drawing reads: the worker draws what it was given and stops. */
void SE_SceneOwn (void)
{
	if (!running || mcw_on_worker ())
		return;
	wait_for (published);
	stop = 1;
	mcw_join (job);
	stop = 0;
	running = 0;
	record_join (0);
	record_join (1);
	real ();
	SE_LightAheadDraw (-1);
	count_swaps ();
}

/* Whether the last frame was given to the worker (the next frame's jobs are then given with it). */
int SE_SceneSplitLast (void) { return split_last; }

/* Which particle buffers the frames given and not yet swapped draw from (r_part.c). */
unsigned SE_SceneParticlesBusy (void)
{
	unsigned busy = 0;
	int i;

	for (i = 0; running && i < 2; i++)
		if (slots[i].particles[0] >= 0)
			busy |= 1u << slots[i].particles[0];
	return busy;
}

static int split_ok (void)
{
	return cls.state == ca_connected && cls.signon == SIGNONS && cl.worldmodel && SE_WorldMade () && key_dest == key_game
		&& !cl.intermission && !cl.paused && !lcd_x.value && r_mirroralpha.value == 1 && !SE_Seated () && !SE_HoldLate ()
		&& SE_ScreenSplitOk ();
}

/* Which models a frame did not draw, from the copies it drew (gl_rlight.c: no light is walked for them). */
static void culled_back (unsigned frame)
{
	slot_t	*s = &slots[frame & 1];
	int	i, at;

	if (!frame || s->frame != frame)
		return;
	for (i = 0; i < s->numvis; i++)
		if ((at = s->vis[i] - ents[frame & 1]) >= 0 && at < MAX_EDICTS)
			cl_entities[at].se_culled = ents[frame & 1][at].se_culled;
}

/* The copy of what frame `frame` draws. */
static void take_copy (unsigned frame)
{
	slot_t		*s = &slots[frame & 1];
	entity_t	*copies = ents[frame & 1];
	int		i;

	/* (the models' and sounds' lists only when the level is another: they change with nothing else) */
	if (s->cl.worldmodel != cl.worldmodel)
		s->cl = cl;
	else {
		memcpy (&s->cl, &cl, offsetof (client_state_t, model_precache));
		memcpy (s->cl.levelname, cl.levelname, sizeof cl - offsetof (client_state_t, levelname));
	}
	/* (of an entity, what is drawn: its place and after, not the messages' and the baseline's) */
	for (i = 0; i < cl_numvisedicts; i++) {
		entity_t	*e = cl_visedicts[i];
		int		at = e - cl_entities;

		if (at < 0 || at >= MAX_EDICTS)
			at = MAX_EDICTS + i;
		memcpy (&copies[at].origin, e->origin, sizeof *e - offsetof (entity_t, origin));
		s->vis[i] = &copies[at];
	}
	if (cl.viewentity > 0 && cl.viewentity < MAX_EDICTS)	/* (from behind it is drawn: R_DrawEntitiesOnList) */
		memcpy (&copies[cl.viewentity].origin, cl_entities[cl.viewentity].origin, sizeof (entity_t) - offsetof (entity_t, origin));
	memcpy (s->dlights, cl_dlights, sizeof s->dlights);
	s->numvis = cl_numvisedicts;
	s->framecount = host_framecount;
	s->frametime = host_frametime;
	s->realtime = realtime_ms;
	memcpy (s->blend, v_blend, sizeof s->blend);
	s->refdef = r_refdef;
	VectorCopy (r_origin, s->origin);
	VectorCopy (vpn, s->vpn);
	VectorCopy (vright, s->vright);
	VectorCopy (vup, s->vup);
	s->viewleaf = r_viewleaf;
	s->oldviewleaf = r_oldviewleaf;
	memcpy (s->styles, d_lightstylevalue, sizeof s->styles);
	SE_ParticlesView (s->particles);
	s->frame = frame;
}

/* In place of SCR_UpdateScreen in a frame of the host's. */
void SE_SceneFrame (void)
{
	unsigned	began, frame;

	if (how < 0) {
		const char *s = getenv ("QUAKE_SCENE");

		how = !(s && !strcmp (s, "inline")) && SE_Workers () > 0 && SE_ParticlesApart ();
		if (how) {
			mcw_touch (slots, sizeof slots);
			mcw_touch_code ((const void *)scene_job, 512);
		}
	}
	count_swaps ();
	split_last = how && split_ok ();
	if (!split_last) {
		SE_SceneOwn ();
		SCR_UpdateScreen ();
		return;
	}
	frame = published + 1;
	wait_for (frame - 2);	/* (its copy is the one frame - 2 was drawn from) */
	count_swaps ();
	began = instructions ();
	SE_ViewFront ();	/* the view, as V_RenderView makes it */
	SE_SceneFront ();	/* the light styles, the view's leaf and its blend, as R_SetupFrame makes them */
	if (SE_LightWait ())
		SE_LightFrame ();
	record_join (!(frame & 1));	/* (one recording at a time: frame - 1's; frame - 2's was joined then) */
	culled_back (ended);
	SE_LightAheadFill (frame & 1);
	SE_FrameJobs ();
	take_copy (frame);
	/* (the 2D screen: by a worker of its own, into the buffer no frame given out or list being drawn reads) */
	slots[frame & 1].stage = frame % 3;
	if (!(record_job[frame & 1] = mcw_try_call (record_screen, frame, 0))) {
		record_screen (frame, 0);
		se2_cl_now = &cl, se2_framecount_now = &host_framecount, se2_frametime_now = &host_frametime, se2_realtime_now = &realtime_ms;
	}
	published = frame;
	se_scene_frames++;
	if (!running) {
		stop = 0;
		seglSettle ();	/* (the worker settles the lists it hands, not one swapped here) */
		if ((job = mcw_try_call (scene_job, 0, 0)) != 0)
			running = 1;
		else {	/* (no worker has nothing to do: drawn here, from its copy) */
			stop = 1;
			scene_job (0, 0);
			stop = 0;
			real ();
			SE_LightAheadDraw (-1);
			count_swaps ();
		}
	} else
		mcw_ready (job);	/* (a page it stopped at is served) */
	se_scene_window += instructions () - began;
	if (!SE_LightWait ())
		SE_LightFrame ();
	V_UpdatePalette ();	/* (as at the end of SCR_UpdateScreen: the next frame's blend) */
	if (SE_HoldAt (published - counted))
		SE_SceneOwn ();	/* (a frame to be held: none after it is drawn) */
}
