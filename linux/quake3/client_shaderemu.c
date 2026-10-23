/*
 * The client's game on a worker core (docs/games/quake3.md, "The client's game a frame ahead"): it
 * writes what it would have drawn into a list, and this core makes the GPU's commands of the
 * list a frame later. QUAKE3_CLIENT=inline, or no second worker: this core's, as it was.
 */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "client/client.h"
#include "mcwx.h"
#include "se_q3.h"

enum { MODE_UNSET = -1, MODE_INLINE, MODE_WORKER };
#define WORKER		2
#define STACK_BYTES	0x80000
#define FIRST_FRAMES	4		/* of a game this core makes itself before the worker has them */
#define LIST(frame)	(1 + (frame) % SE_LISTS)	/* which of the lists a frame is written to */

unsigned	se_client_stack;	/* its stack's top (q_shared.h, SE_OnWorker) */
unsigned	se_client_jobs, se_client_cycles, se_client_passes;	/* for the q3stat: lines */
int		se_client_list;		/* the list of the frame the worker is at (server_shaderemu.c keeps by it) */

/* what a frame is given, by its list: this core's, written before the frame is asked for */
static struct { int serverTime, stereo, demo, cmdNumber, snapNumber, snapTime, unused[2]; } given[SE_LISTS] __attribute__ ((aligned (16)));
/* how many frames have been asked for: this core's. The worker goes on to the next without waiting to be told */
static struct { unsigned frames, unused[3]; } asked __attribute__ ((aligned (16)));
/* how many it has made, what the last cost, and a frame's two words for the next move: the worker's */
static struct { unsigned frames, cycles, jobs, unused; } made __attribute__ ((aligned (16)));
static struct { int has, value; float scale; int unused; } answer[SE_LISTS] __attribute__ ((aligned (16)));
static char	stack[STACK_BYTES] __attribute__ ((aligned (16)));
static int	mode = MODE_UNSET, first_frames;
static unsigned	taken;			/* frames this core has taken */
static mcw_handle	job;		/* the worker's call (mcwx.h), while it is out */

intptr_t cgame_vmMain (int, int, int, int, int, int, int, int, int, int, int, int, int);
uint32_t se_call_on_stack (uint32_t (*fn) (uint32_t), uint32_t arg, void *stack_top);

/* The level's items of a frame, made on a helper core beside the worker's (cg_ents.c, SE_SideItems):
   asked for by the worker (their numbers), done by the helper, kept up by this core. */
#define SIDE_MOST	256
unsigned	se_side_stack;		/* its stack's top (q_shared.h, SE_OnWorker) */
static char	side_stack[SE_THINK_STACK] __attribute__ ((aligned (16)));
static struct { unsigned seq, count, list, unused; int numbers[SIDE_MOST]; } side_task __attribute__ ((aligned (16)));
static struct { unsigned seq, unused[3]; } side_done __attribute__ ((aligned (16)));
static struct { unsigned quit, up, unused[2]; } side_life __attribute__ ((aligned (16)));
static mcw_handle	side_out;
static int		side_wanted, side_open, side_placed;	/* this core's; the worker's two, in a frame */
/* for the q3stat: lines, read and never cleared there: the worker's (frames, items, passes waited) and the helper's */
struct se_side_counts	se_side_worker __attribute__ ((aligned (16))), se_side_helper __attribute__ ((aligned (16)));

static void setup (void)
{
	const char	*how;
	unsigned	i;

	if (mode != MODE_UNSET)
		return;
	how = getenv ("QUAKE3_CLIENT");
	mode = MODE_INLINE;
	if (!(how && !strcmp (how, "inline")) && SE_Workers () >= WORKER) {
		mode = MODE_WORKER;
		se_client_stack = (unsigned)(uintptr_t)(stack + STACK_BYTES);
		for (i = 4096; i <= 0x20000; i += 4096)
			stack[STACK_BYTES - i] = 0;
		SE_SceneTouch (mcw_touch);
		side_wanted = !(getenv ("QUAKE3_SIDE") && !strcmp (getenv ("QUAKE3_SIDE"), "inline"));
		se_side_stack = (unsigned)(uintptr_t)(side_stack + SE_THINK_STACK);
		mcw_touch (side_stack + SE_THINK_STACK - 0x10000, 0x10000);
		SE_SoundSideTouch (mcw_touch);
		mcw_touch (&se_side_worker, sizeof se_side_worker);
		mcw_touch (&se_side_helper, sizeof se_side_helper);
	}
	printf ("quake3: the client's game is on %s\n", mode == MODE_WORKER ? "a worker core, a frame ahead" : "this core");
}

/* ---- on the worker ---- */

static void side_join (void);

static uint32_t frame_body (uint32_t list)
{
	if (!setjmp (se_worker_abort[1].at)) {
		se_client_list = (int)list;
		answer[list - 1].has = 0;
		SE_SceneRecord ((int)list);
		SE_SoundRecord ((int)list);
		cgame_vmMain (CG_DRAW_ACTIVE_FRAME, given[list - 1].serverTime, given[list - 1].stereo, given[list - 1].demo, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	}
	side_join ();
	return 0;
}

/* The frame's items asked of the helper (1), or the worker makes them as they come (0). */
int SE_SideOpen (void)
{
	side_open = SE_OnWorker () == 2 && side_life.up;
	side_placed = 0;
	if (side_open)
		side_task.count = 0;
	return side_open;
}

void SE_SideAsk (int number)
{
	if (side_task.count < SIDE_MOST)
		side_task.numbers[side_task.count++] = number;
}

void SE_SideGo (void)
{
	side_task.list = (unsigned)se_client_list;
	side_task.seq++;
}

/* The next item's place in the frame's scene, if the helper has it. */
int SE_ScenePlaceSide (void)
{
	if (side_placed >= (int)side_task.count)
		return 0;
	SE_SceneSidePlace (side_placed++);
	return 1;
}

/* On the helper: what it was asked */
const int *SE_SideTask (int *count)
{
	*count = (int)side_task.count;
	return side_task.numbers;
}

/* At the frame's end: the helper is done with it, and its sounds' places are this core's. */
static void side_join (void)
{
	if (!side_open)
		return;
	se_side_worker.n[0]++, se_side_worker.n[1] += side_task.count;
	while (side_done.seq != side_task.seq) {
		mc_end_pass ();
		se_side_worker.n[2]++;
	}
	SE_SoundSideReplay ();
	side_open = 0;
}

static uint32_t side_body (uint32_t unused)
{
	SE_SceneSideBegin ((int)side_task.list);
	cgame_vmMain (CG_SE_SIDE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
	return 0;
}

/* The helper's job: the frames' items as they are asked for, until this core says to stop. */
static uint32_t side_job (uint32_t unused0, uint32_t unused1)
{
	unsigned	seen = side_done.seq, began;

	for (;;) {
		while (side_task.seq == seen && !side_life.quit)
			mc_end_pass ();
		if (side_task.seq == seen)
			return 0;
		seen = side_task.seq;
		began = SE_Cycles ();
		se_call_on_stack (side_body, 0, side_stack + SE_THINK_STACK);
		se_side_helper.n[0] += SE_Cycles () - began;
		side_done.seq = seen;
	}
}

/* The frames asked for, one after another: the next may have been asked for while the last was made. */
static uint32_t client_job (uint32_t a, uint32_t b)
{
	made.jobs++;
	while (made.frames < asked.frames) {
		unsigned	began = SE_Cycles ();

		se_call_on_stack (frame_body, LIST (made.frames + 1), stack + STACK_BYTES);
		made.cycles = SE_Cycles () - began;
		made.frames++;
	}
	return 0;
}

int SE_ClientPinned (int *cmdNumber, int *snapshotNumber, int *serverTime)
{
	if (SE_OnWorker () != WORKER)
		return 0;
	if (cmdNumber)
		*cmdNumber = given[se_client_list - 1].cmdNumber;
	if (snapshotNumber)
		*snapshotNumber = given[se_client_list - 1].snapNumber;
	if (serverTime)
		*serverTime = given[se_client_list - 1].snapTime;
	return 1;
}

int SE_ClientUserCmdValue (int value, float scale)
{
	if (SE_OnWorker () != WORKER)
		return 0;
	answer[se_client_list - 1].has = 1, answer[se_client_list - 1].value = value, answer[se_client_list - 1].scale = scale;
	return 1;
}

/* ---- on this core ---- */

/* The worker is given the frames asked for, if it is not at them already. */
static void start (void)
{
	if (side_out)
		mcw_ready (side_out);	/* (a page it stopped at is brought) */
	if (job && mcw_ready (job))
		mcw_join (job), job = 0;
	if (!job && made.frames < asked.frames) {
		/* (the helper first: the job sees it is there) */
		if (!side_out && side_wanted) {
			side_life.quit = 0;
			side_out = mcw_try_call (side_job, 0, 0);
			side_life.up = side_out != 0;
		}
		job = mcw_call (client_job, 0, 0);
	}
}

/* One more frame is asked for: its list was last drawn from two frames ago. */
static void ask (int serverTime, int stereo, int demo)
{
	int	list = LIST (asked.frames + 1);

	given[list - 1].serverTime = serverTime, given[list - 1].stereo = stereo, given[list - 1].demo = demo;
	given[list - 1].cmdNumber = cl.cmdNumber, given[list - 1].snapNumber = cl.snap.messageNum, given[list - 1].snapTime = cl.snap.serverTime;
	asked.frames++;
	start ();
}

/* Until the worker has made so many frames. */
static void wait (unsigned frames)
{
	while (made.frames < frames) {
		start ();
		if (made.frames >= frames)
			break;	/* (no worker core: this one made them, there and then) */
		{
			mcw_handle	on[2] = { job, side_out };

			mcw_rest_on (on, 2);	/* (gpu_vm: those two have the whole pass) */
		}
		se_client_passes++;
		SE_ServerPoll ();	/* (the server's frame is not kept waiting for this core's) */
		SE_ScenePoll ();
	}
}

/* The next frame is taken: what the worker kept of it is done here. Which list its picture is; 0 when it ended in an error. */
static int take (void)
{
	int	list = LIST (taken + 1), failed;

	wait (taken + 1);
	taken++;
	se_client_cycles += made.cycles;
	se_client_jobs++;
	failed = SE_WorkerKept (list);
	SE_SoundReplay (list);
	if (answer[list - 1].has)
		cl.cgameUserCmdValue = answer[list - 1].value, cl.cgameSensitivity = answer[list - 1].scale;
	return failed ? 0 : list;
}

void SE_ClientSettle (void)
{
	wait (asked.frames);
}

void SE_ClientPoll (void)
{
	if (job)
		mcw_ready (job);	/* (a page it stopped at is brought) */
	if (side_out)
		mcw_ready (side_out);
}

void SE_ClientReset (void)
{
	while (taken < asked.frames)
		take ();
	first_frames = 0;
	if (side_out) {
		side_life.quit = 1, side_life.up = 0;
		mcw_join (side_out);
		side_out = 0;
	}
}

/* Whether the next frame of the client's game may be the worker's. */
static int ahead (void)
{
	return mode == MODE_WORKER && clc.state == CA_ACTIVE && !clc.demoplaying && !se_wanted && cl.snap.valid &&
		first_frames >= FIRST_FRAMES && clc.lastExecutedServerCommand >= cl.snap.serverCommandNum;
}

void SE_ClientFrame (int serverTime, int stereo, int demo)
{
	int	drawn;

	setup ();
	if (!ahead () || taken == asked.frames) {
		int	first = ahead ();

		/* this core's, as it always was: frames the worker has are taken and not shown */
		while (taken < asked.frames)
			if (!take ())
				SE_WorkerRaise (LIST (taken));
		first_frames = clc.state == CA_ACTIVE ? first_frames + 1 : 0;
		SE_SceneSum (1);
		VM_Call (cgvm, CG_DRAW_ACTIVE_FRAME, serverTime, stereo, demo);
		SE_SceneSum (0);
		/* (the first of the worker's: it makes this frame again, for this core to show next) */
		if (first)
			ask (serverTime, stereo, demo);
		return;
	}
	/* the next is asked for behind the one the worker is at, which is waited for and shown */
	if (asked.frames - taken < 2)
		ask (serverTime, stereo, demo);
	drawn = take ();
	if (!drawn)
		SE_WorkerRaise (LIST (taken));
	SE_SceneSum (1);
	SE_SceneReplay (drawn);
	SE_SceneSum (0);
}
