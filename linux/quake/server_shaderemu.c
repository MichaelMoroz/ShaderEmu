/*
 * The server's frame on a worker core (docs/quake.md, docs/multicore.md). A frame of a
 * single-player game is the server's physics and QuakeC, then the client's picture of it. Here
 * the physics runs on a worker while this core draws, and the two go at their own pace: the
 * client draws frames from the server's last two answers, moving things between them as it
 * does in a network game, and the server begins its next frame when its last one is done.
 *
 * What the server does on a worker it does to its own data (the edicts, the QuakeC globals,
 * the messages it is writing), which this core leaves alone meanwhile: a console command that
 * may be the server's waits for the frame to end. What the server has in common with the
 * client it does not touch from there: console lines, commands and cvar changes are kept and
 * done by this core afterwards, an error ends the job and is raised here, random numbers are
 * its own, and so is the length of its frame (sv_frametime). The build gives every variable
 * its own 16 bytes, for two cores must not store to the same 16.
 *
 * QUAKE_SERVER says otherwise:
 *   inline  the server on this core, as the game has it (and as on a machine without workers)
 *   late    on this core, its messages sent at the end of the client's frame
 *   wait    on a worker, and the client's frame waits for it at its end
 * The last two do the same thing in the same order, so the server's state must be the same
 * with both (QUAKE_SUM, with host_framerate set); and it is the same as inline's.
 */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "quakedef.h"
#include "mcw.h"

enum { MODE_UNSET = -1, MODE_INLINE, MODE_LATE, MODE_WAIT, MODE_APART };
enum { OUT_NONE, OUT_RUNNING, OUT_DONE };	/* a frame of the server's that began in SE_ServerPhysics */
enum { KEPT_PRINT, KEPT_COMMAND, KEPT_CVAR };

/* what the worker writes of ours while it has the job */
static struct {
	unsigned used, lost, failed, seed;
	char error[240];
	char kept[4096];	/* a kind, then text ending in 0 (a cvar: its name, 0, its value, 0) */
	unsigned sent;		/* the frame's messages were made there too: they are in `sends` */
	unsigned sends_used, reliable_out;
	unsigned sends[(NET_MAXMESSAGE + MAX_DATAGRAM + 64) / 4];	/* reliable?, socket, bytes, then the bytes */
} work __attribute__ ((aligned (16)));
static jmp_buf job_abort;
static int mode = MODE_UNSET, out;
static float rate = 1;
static int draining;
static float since;		/* time the client's frames have taken since the server's last began */
float sv_frametime;		/* the server's host_frametime */
unsigned se_server_cycles, se_server_frames, se_server_passes, se_server_between;	/* for the quakestat: lines */

static void setup (void)
{
	const char *how = getenv ("QUAKE_SERVER");

	if (mode != MODE_UNSET)
		return;
	work.seed = 1;
	mode = MODE_APART;
	if (how && !strcmp (how, "inline"))
		mode = MODE_INLINE;
	else if (how && !strcmp (how, "late"))
		mode = MODE_LATE;
	else if (how && !strcmp (how, "wait"))
		mode = MODE_WAIT;
	if (mode >= MODE_WAIT) {
		if (mcw_open (1) > 0)
			atexit (mcw_close);
		else
			mode = MODE_INLINE;
	}
	Sys_Printf ("quake: the server's frame is on %s\n", mode >= MODE_WAIT ? "a worker core" : "this core");
}

int SE_ServerOnWorker (void) { return mode >= MODE_WAIT && mcw_on_worker (); }

/* Whether the client draws between the server's frames (and so moves things between them). */
int SE_ServerApart (void)
{
	setup ();
	return mode == MODE_APART;
}

/* rand() for the server's code: the C library's has one state, which the client uses too */
int SE_ServerRand (void)
{
	work.seed = work.seed * 1103515245u + 12345u;
	return (work.seed >> 16) & 0x7fff;
}

static void keep (int kind, const char *a, const char *b)
{
	unsigned la = strlen (a) + 1, lb = b ? strlen (b) + 1 : 0;

	if (work.used + 1 + la + lb > sizeof work.kept) {
		work.lost++;
		return;
	}
	work.kept[work.used++] = (char)kind;
	memcpy (work.kept + work.used, a, la);
	work.used += la;
	if (b) {
		memcpy (work.kept + work.used, b, lb);
		work.used += lb;
	}
}

void SE_ServerKeepPrint (const char *text) { keep (KEPT_PRINT, text, NULL); }
void SE_ServerKeepCommand (const char *text) { keep (KEPT_COMMAND, text, NULL); }
void SE_ServerKeepCvar (const char *name, const char *value) { keep (KEPT_CVAR, name, value); }

/* Host_Error and Sys_Error on the worker: the job ends, and this core raises it */
void SE_ServerWorkerError (const char *text)
{
	strncpy (work.error, text, sizeof work.error - 1);
	work.failed = 1;
	longjmp (job_abort, 1);
}

/*
 * NET_SendMessage and NET_SendUnreliableMessage on the worker: the message is kept, and goes
 * from this core when the frame is taken (the client reads its end of the connection
 * meanwhile). One reliable message a frame, as the connection allows.
 */
int SE_ServerKeepSend (int reliable, void *sock, const void *data, int bytes)
{
	unsigned words = 3 + ((unsigned)bytes + 3) / 4;

	if (work.sends_used + words > sizeof work.sends / 4)
		return -1;
	work.sends[work.sends_used] = (unsigned)reliable;
	work.sends[work.sends_used + 1] = (unsigned)sock;
	work.sends[work.sends_used + 2] = (unsigned)bytes;
	memcpy (&work.sends[work.sends_used + 3], data, bytes);
	work.sends_used += words;
	if (reliable)
		work.reliable_out = 1;
	return 1;
}

int SE_ServerCanSend (qsocket_t *sock) { return !work.reliable_out && sock->canSend && sock->driverdata; }

/* The messages of a frame that has ended. */
void SE_ServerSend (void)
{
	unsigned at;

	if (!work.sent) {
		SV_SendClientMessages ();
		return;
	}
	for (at = 0; at < work.sends_used; at += 3 + (work.sends[at + 2] + 3) / 4) {
		sizebuf_t message;

		memset (&message, 0, sizeof message);
		message.data = (byte *)&work.sends[at + 3];
		message.cursize = message.maxsize = (int)work.sends[at + 2];
		if (work.sends[at])
			NET_SendMessage ((qsocket_t *)work.sends[at + 1], &message);
		else
			NET_SendUnreliableMessage ((qsocket_t *)work.sends[at + 1], &message);
	}
	work.sent = work.sends_used = work.reliable_out = 0;
}

static unsigned cycles (void)
{
	unsigned v;

	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

static uint32_t physics_job (uint32_t and_send, uint32_t unused)
{
	unsigned began = cycles ();

	if (!setjmp (job_abort)) {
		SV_Physics ();
		/* and what the client is told of it, when that is the everyday kind */
		if (and_send && svs.clients->active && svs.clients->spawned && !svs.clients->sendsignon
		    && !svs.clients->dropasap && !svs.clients->message.overflowed) {
			SV_SendClientMessages ();
			work.sent = 1;
		}
	}
	return cycles () - began;
}

/* QUAKE_SUM=N: a sum over every entity's fields each N frames of the server, which a change
 * to where the server runs must leave as it was (with host_framerate set, so that a frame's
 * length is not the machine's speed). */
static void sum_frame (void)
{
	static int every = -1;
	static unsigned frames;
	unsigned sum = 0, i, words = progs->entityfields, e;

	if (every < 0)
		every = getenv ("QUAKE_SUM") ? atoi (getenv ("QUAKE_SUM")) : 0;
	/* QUAKE_TRACK=N: where the player is, each N frames of the server */
	{
		static int track = -1;
		static unsigned tracked;

		if (track < 0)
			track = getenv ("QUAKE_TRACK") ? atoi (getenv ("QUAKE_TRACK")) : 0;
		/* QUAKE_TEST_EXIT=F: at frame F of the server the player is put into the level's exit */
		if (track && getenv ("QUAKE_TEST_EXIT") && tracked + 1 == (unsigned)atoi (getenv ("QUAKE_TEST_EXIT"))) {
			for (e = 2; e < sv.num_edicts; e++) {
				edict_t *t = EDICT_NUM (e);

				if (!t->free && !strcmp (pr_strings + t->v.classname, "trigger_changelevel") && t->v.touch) {
					Sys_Printf ("quake: test: the player touches the exit (entity %u)\n", e);
					pr_global_struct->time = sv.time;
					pr_global_struct->self = EDICT_TO_PROG (t);
					pr_global_struct->other = EDICT_TO_PROG (EDICT_NUM (1));
					PR_ExecuteProgram (t->v.touch);
					break;
				}
			}
		}
		if (track && ++tracked % track == 0 && sv.num_edicts > 1) {
			edict_t *p = EDICT_NUM (1);

			Sys_Printf ("quake: track %u: time %u ms, at %d %d %d, speed %d %d %d, on ground %d, health %d\n", tracked,
				(unsigned)(sv.time * 1000), (int)p->v.origin[0], (int)p->v.origin[1], (int)p->v.origin[2],
				(int)p->v.velocity[0], (int)p->v.velocity[1], (int)p->v.velocity[2], ((int)p->v.flags & FL_ONGROUND) != 0, (int)p->v.health);
		}
	}
	if (!every || ++frames % every)
		return;
	for (e = 0; e < sv.num_edicts; e++) {
		const unsigned *v = (const unsigned *)&EDICT_NUM (e)->v;

		if (EDICT_NUM (e)->free)
			continue;
		/* (but for the world's model: its name is not among QuakeC's strings, and where it
		 * is from them depends on where the heap is, which the environment's size moves) */
		for (i = 0; i < words; i++)
			if (e != 0 || i != (unsigned)((const unsigned *)&EDICT_NUM (0)->v.model - (const unsigned *)&EDICT_NUM (0)->v))
				sum = (sum << 5 | sum >> 27) + v[i];
	}
	Sys_Printf ("quake: server frame %u: %u entities, time %u ms, state %08x\n", frames, sv.num_edicts, (unsigned)(sv.time * 1000), sum);
}

/* Called a few times in a frame of the client's (VID_Phase): a worker stopped at a page nobody
 * has touched is helped on from here. */
void SE_ServerPoll (void)
{
	static int inside;
	extern void Host_ServerFrame (void);

	if (out != OUT_RUNNING || !mcw_done (1))
		return;
	/* and one whose frame has ended is given its next now, not at the client's next frame:
	 * the worker would stand still for half a frame of the client's, on average */
	if (mode == MODE_APART && sv.active && !inside) {
		inside = 1;
		Host_ServerFrame ();
		inside = 0;
	}
}

/* The client's frame took this long. */
void SE_ServerClock (float seconds) { since += seconds; }

/* The length of the server's frame that begins now: the time since its last one began. */
float SE_ServerFrameTime (void)
{
	float length = since, real = since;

	since = 0;
	length = length > 0.1f ? 0.1f : length < 0.001f ? 0.001f : length;
	rate = mode == MODE_APART && real > length ? length / real : 1;
	return length;
}

/* How fast the server's time goes against the clock's (a frame of its own is 0.1 s of the
 * game at most, as a frame of the game always was): the client's time goes as fast between
 * the server's frames, or it would be at the next one before that has come. */
float SE_ServerRate (void) { return rate; }

/* The client drew a frame between two of the server's. */
void SE_ServerBetween (void) { se_server_between++; }

/* Waits for the worker, if it has a frame, and does what it kept for this core. */
static void settle (int raise)
{
	unsigned at;

	if (out != OUT_RUNNING)
		return;
	se_server_passes += mcw_wait (1);
	se_server_cycles += mcw_result (1);
	se_server_frames++;
	out = OUT_DONE;
	sum_frame ();
	for (at = 0; at < work.used; ) {
		int kind = work.kept[at++];
		char *a = work.kept + at;

		at += strlen (a) + 1;
		if (kind == KEPT_PRINT)
			Con_Printf ("%s", a);
		else if (kind == KEPT_COMMAND)
			Cbuf_AddText (a);
		else {
			Cvar_Set (a, work.kept + at);
			at += strlen (work.kept + at) + 1;
		}
	}
	work.used = 0;
	if (work.lost) {
		Con_Printf ("(%u of the server's lines lost)\n", work.lost);
		work.lost = 0;
	}
	if (work.failed) {
		work.failed = 0;
		if (raise)
			Host_Error ("%s", work.error);
		Con_Printf ("%s\n", work.error);
	}
}

/*
 * Before a frame of the server's: 2 if a worker still has the last one (and this one is not
 * to be), 1 if one has ended whose messages have not gone yet, 0 if there is nothing out.
 */
int SE_ServerTake (void)
{
	if (out == OUT_RUNNING) {
		if (mode == MODE_APART && !mcw_done (1))
			return 2;
		settle (1);
	}
	if (out == OUT_DONE) {
		out = OUT_NONE;
		return 1;
	}
	return 0;
}

/* In place of SV_Physics(): 1 if the frame's messages are to wait (see SE_ServerTake, SE_ServerFinish). */
int SE_ServerPhysics (void)
{
	setup ();
	/* a game that is being played, and nothing else: loading and leaving are this core's */
	if (mode == MODE_INLINE || cls.state != ca_connected || cls.signon != SIGNONS || cls.demoplayback || svs.maxclients != 1) {
		SV_Physics ();
		sum_frame ();
		return 0;
	}
	if (mode == MODE_LATE) {
		physics_job (0, 0);
		sum_frame ();
		out = OUT_DONE;
	} else {
		mcw_post (1, physics_job, mode == MODE_APART, 0);
		out = OUT_RUNNING;
	}
	return 1;
}

/* The end of the client's frame: 1 if the server's messages are to go now. */
int SE_ServerFinish (void)
{
	if (mode == MODE_APART || out == OUT_NONE)
		return 0;
	settle (1);
	out = OUT_NONE;
	return 1;
}

/* Before anything on this core that reads or changes the server's data. */
void SE_ServerSettle (void) { settle (0); }

/* The server is going: whatever frame was out is forgotten. */
void SE_ServerDrop (void)
{
	settle (0);
	out = OUT_NONE;
	since = 0;
	work.sent = work.sends_used = work.reliable_out = 0;
	/*
	 * And the client reads what it was last sent. With the server's messages sent late (at
	 * the end of the client's frame, or when a worker's frame is taken) one may be unread
	 * here, and the connection takes one reliable message at a time: the "reconnect" of a
	 * change of level could not be sent, and the client read the new level as if it were
	 * still in the old one ("i >= cl.maxclients").
	 */
	if (mode != MODE_INLINE && mode != MODE_UNSET && sv.active && cls.state == ca_connected && !cls.demoplayback && !draining) {
		draining = 1;
		CL_ReadFromServer ();
		draining = 0;
	}
}

/* A console command is about to run. Most are the client's; any other may be the server's. */
void SE_ServerCommand (const char *name)
{
	static const char *const clients[] = {"impulse", "bf", "centerview", "play", "playvol", "stopsound", "v_cshift",
		"echo", "bind", "alias", "wait", "toggleconsole", "togglemenu", "messagemode", "messagemode2", "sizeup",
		"sizedown", "screenshot", "cd", "hold", NULL};
	int i;

	if (out != OUT_RUNNING || name[0] == '+' || name[0] == '-' || !Q_strncasecmp ((char *)name, "menu_", 5))
		return;
	for (i = 0; clients[i]; i++)
		if (!Q_strcasecmp ((char *)name, (char *)clients[i]))
			return;
	settle (0);
}
