/*
 * The server's frame on a worker core (docs/games/quake.md, docs/machine/multicore.md), in a game for one
 * and in one this machine serves to others (docs/system/lan.md). A frame of the game is the server's physics and QuakeC, then the client's picture of it. Here
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
#include "mcwx.h"

enum { MODE_UNSET = -1, MODE_INLINE, MODE_LATE, MODE_WAIT, MODE_APART };
enum { OUT_NONE, OUT_RUNNING, OUT_DONE };	/* a frame of the server's that began in SE_ServerPhysics */
enum { KEPT_PRINT, KEPT_COMMAND, KEPT_CVAR };
/* a frame's messages kept on the worker: one player's largest, and room for four such */
enum { SENDS_ONE = (NET_MAXMESSAGE + MAX_DATAGRAM + 64) / 4, SENDS_WORDS = 4 * SENDS_ONE };

/* what the worker writes of ours while it has the job */
static struct {
	unsigned used, lost, failed, seed;
	unsigned began, ended;	/* the clock (rdtime) at the job's start and end */
	char error[240];
	char kept[4096];	/* a kind, then text ending in 0 (a cvar: its name, 0, its value, 0) */
	unsigned sent;		/* the frame's messages were made there too: they are in `sends` */
	unsigned sends_used;
	unsigned sends[SENDS_WORDS];	/* reliable?, socket, bytes, then the bytes */
} work __attribute__ ((aligned (16)));
static jmp_buf job_abort;
static int mode = MODE_UNSET, out;
static mcw_handle server_call, load_call;	/* the server's frame, a loader's job (mcwx.h) */
static unsigned server_serial, server_looked;
static volatile unsigned server_ended;	/* the worker's: the serial of the last frame it ended */
static float rate = 1;
static int draining;
static float since;		/* time the client's frames have taken since the server's last began */
float sv_frametime;		/* the server's host_frametime */
unsigned se_load_jobs;
unsigned se_server_cycles, se_server_frames, se_server_passes, se_server_between, se_server_held, se_server_behind;	/* for the quakestat: lines */
unsigned se_server_ticks[4];	/* clock ticks: call to start, start to end, end to seen, seen to the next call */
static unsigned called_at, seen_at;

/*
 * The program's workers, opened once: the server's frame has the first, the light maps, the
 * water and the particles those after it (world_shaderemu.c, r_part.c). QUAKE_WORKERS=N asks
 * for N in all (0: none).
 */
int SE_Workers (void)
{
	static int count = -1;

	if (count < 0) {
		const char *want = getenv ("QUAKE_WORKERS");

		count = mcw_open (want ? atoi (want) : SE_WORKERS);
		if (count > 0)
			atexit (mcw_close);
		else
			count = 0;
	}
	return count;
}

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
	if (mode >= MODE_WAIT && SE_Workers () < 1)
		mode = MODE_INLINE;
	Sys_Printf ("quake: the server's frame is on %s\n", mode >= MODE_WAIT ? "a worker core" : "this core");
}

/* The core running the server's frame, -1 between frames: written only by that worker, on 16 bytes of its own. */
static struct { volatile int core; int pad[3]; } server_on __attribute__ ((aligned (16))) = { -1 };

int SE_ServerOnWorker (void) { return mode >= MODE_WAIT && mcw_on_worker () && server_on.core == mcw_self (); }

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
 * meanwhile). One reliable message a frame to a connection, as it allows.
 */
int SE_ServerKeepSend (int reliable, void *sock, const void *data, int bytes)
{
	unsigned words = 3 + ((unsigned)bytes + 3) / 4;

	/* (a reliable one always has room: SE_ServerCanSend. An update that has none is lost) */
	if (work.sends_used + words > SENDS_WORDS)
		return reliable ? -1 : 1;
	work.sends[work.sends_used] = (unsigned)reliable;
	work.sends[work.sends_used + 1] = (unsigned)sock;
	work.sends[work.sends_used + 2] = (unsigned)bytes;
	memcpy (&work.sends[work.sends_used + 3], data, bytes);
	work.sends_used += words;
	return 1;
}

/* NET_CanSendMessage on the worker: nothing is sent from there, a long message's next part neither. */
int SE_ServerCanSend (qsocket_t *sock)
{
	unsigned at;

	if (!sock->canSend || sock->disconnected || work.sends_used + SENDS_ONE > SENDS_WORDS)
		return 0;
	for (at = 0; at < work.sends_used; at += 3 + (work.sends[at + 2] + 3) / 4)
		if (work.sends[at] && work.sends[at + 1] == (unsigned)sock)
			return 0;
	return sock->driver ? !sock->sendNext : sock->driverdata != NULL;
}

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

		if (((qsocket_t *)work.sends[at + 1])->disconnected)	/* dropped since */
			continue;
		memset (&message, 0, sizeof message);
		message.data = (byte *)&work.sends[at + 3];
		message.cursize = message.maxsize = (int)work.sends[at + 2];
		if (work.sends[at])
			NET_SendMessage ((qsocket_t *)work.sends[at + 1], &message);
		else
			NET_SendUnreliableMessage ((qsocket_t *)work.sends[at + 1], &message);
	}
	work.sent = work.sends_used = 0;
}

/* Whether every player is in the game and none is to be dropped: the worker tells them itself. */
static int all_playing (void)
{
	client_t *c = svs.clients;
	int i;

	for (i = 0; i < svs.maxclients; i++, c++)
		if (c->active && (!c->spawned || c->sendsignon || c->dropasap || c->message.overflowed))
			return 0;
	return svs.clients->active;
}

static unsigned cycles (void)
{
	unsigned v;

	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

static unsigned ticks (void)
{
	unsigned v;

	__asm__ volatile ("rdtime %0" : "=r"(v));
	return v;
}

static uint32_t physics_job (uint32_t and_send, uint32_t serial)
{
	unsigned began = cycles ();

	if (mcw_on_worker ())
		server_on.core = mcw_self ();
	work.began = ticks ();
	if (!setjmp (job_abort)) {
		SV_Physics ();
		/* and what the client is told of it, when that is the everyday kind */
		if (and_send && all_playing ()) {
			SV_SendClientMessages ();
			work.sent = 1;
		}
	}
	work.ended = ticks ();
	if (mcw_on_worker ())
		server_on.core = -1;
	server_ended = serial;
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
		/* QUAKE_TEST_PLACE=F,x,y,z,pitch,yaw: from frame F of the server the player stands there, still */
		if (track && getenv ("QUAKE_TEST_PLACE") && sv.num_edicts > 1) {
			int	at[6] = {0}, k;
			edict_t	*p = EDICT_NUM (1);

			sscanf (getenv ("QUAKE_TEST_PLACE"), "%d,%d,%d,%d,%d,%d", &at[0], &at[1], &at[2], &at[3], &at[4], &at[5]);
			if (tracked + 1 >= (unsigned)at[0]) {
				for (k = 0; k < 3; k++)
					p->v.origin[k] = at[k + 1];
				p->v.velocity[0] = p->v.velocity[1] = p->v.velocity[2] = 0;
				p->v.angles[0] = p->v.v_angle[0] = at[4], p->v.angles[1] = p->v.v_angle[1] = at[5];
				p->v.angles[2] = p->v.v_angle[2] = 0;
				p->v.fixangle = tracked + 1 == (unsigned)at[0];
				p->v.movetype = MOVETYPE_NONE;
				SV_LinkEdict (p, false);
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
		/* (but for the world's model and a player's name: they are not among QuakeC's strings,
		 * and where they are from them depends on where the heap is and what was put there
		 * before the level: the environment's size, a console command more in the program) */
		for (i = 0; i < words; i++)
			if ((e != 0 || i != (unsigned)((const unsigned *)&EDICT_NUM (0)->v.model - (const unsigned *)&EDICT_NUM (0)->v)) &&
			    (e == 0 || e > (unsigned)svs.maxclients || i != (unsigned)((const unsigned *)&EDICT_NUM (0)->v.netname - (const unsigned *)&EDICT_NUM (0)->v)))
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

	if (out != OUT_RUNNING)
		return;
	/* (asked of the library once a frame until the worker says it has ended: a page it stopped
	 * at is served then. A question of the library is some hundred instructions) */
	if (server_ended != server_serial && server_looked == (unsigned)host_framecount)
		return;
	server_looked = host_framecount;
	if (!mcw_ready (server_call))
		return;
	/* and one whose frame has ended is given its next now, not at the client's next frame:
	 * the worker would stand still for half a frame of the client's, on average */
	if (mode == MODE_APART && sv.active && !inside) {
		inside = 1;
		Host_ServerFrame ();
		inside = 0;
	}
}

/* Between the client's frames (one refused for the clock): a frame that has ended is followed
 * now, not at the client's next frame, which was 3.5 passes later on e1m5. */
void SE_ServerIdle (void)
{
	if (out == OUT_RUNNING && server_ended == server_serial)
		SE_ServerPoll ();
}

/* The client's frame took this long. */
void SE_ServerClock (float seconds) { since += seconds; }

/* The length of the server's frame that begins now: the time since its last one began. */
float SE_ServerFrameTime (void)
{
	float length = since, real = since;

	since = 0;
	length = length > 0.1f ? 0.1f : length < 0.001f ? 0.001f : length;
	if (mode == MODE_APART) {
		/*
		 * The client's time goes at the slower of the server's last two frames' rates, and a
		 * tenth slower still: a frame of the server's that takes longer than the one before
		 * would have the client at its end early, and what it draws standing still until
		 * the next (the player's own view too: it looked like being stuck).
		 */
		static float before = 1;
		float now = real > length ? length / real : 1;

		rate = 0.9f * (now < before ? now : before);
		before = now;
	} else
		rate = 1;
	return length;
}

/* How fast the server's time goes against the clock's (a frame of its own is 0.1 s of the
 * game at most, as a frame of the game always was): the client's time goes as fast between
 * the server's frames, or it would be at the next one before that has come. */
float SE_ServerRate (void) { return rate; }

/* Where the client's time was for a frame it drew: between the server's last two frames (0),
 * at the newer already (1: what it draws stands still), or behind the older (-1). */
void SE_ServerLerp (int where)
{
	if (where == 0)
		se_server_between++;
	else if (where > 0)
		se_server_held++;
	else
		se_server_behind++;
}

/* Waits for the worker, if it has a frame, and does what it kept for this core. */
static void settle (int raise)
{
	unsigned at;

	if (out != OUT_RUNNING)
		return;
	for (; !mcw_ready (server_call); se_server_passes++)
		mcw_rest ();
	se_server_cycles += mcw_join (server_call);
	server_call = 0;
	seen_at = ticks ();
	se_server_ticks[0] += work.began - called_at;
	se_server_ticks[1] += work.ended - work.began;
	se_server_ticks[2] += seen_at - work.ended;
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
		if (mode == MODE_APART && !mcw_ready (server_call))
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
	if (mode == MODE_INLINE || cls.state != ca_connected || cls.signon != SIGNONS || cls.demoplayback) {
		SV_Physics ();
		sum_frame ();
		return 0;
	}
	if (mode == MODE_LATE) {
		physics_job (0, 0);
		sum_frame ();
		out = OUT_DONE;
	} else {
		called_at = ticks ();
		if (seen_at)
			se_server_ticks[3] += called_at - seen_at;
		server_call = mcw_call (physics_job, mode == MODE_APART, ++server_serial);
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
	work.sent = work.sends_used = 0;
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

/*
 * A job of the loader's for the worker core, when it has none of the server's (which is so
 * while a level is loaded): 1 if it was given out, and SE_JobEnd() waits for it.
 */
int SE_JobBegin (unsigned (*job) (unsigned, unsigned), unsigned a0, unsigned a1)
{
	setup ();
	if (mode < MODE_WAIT || out == OUT_RUNNING)
		return 0;
	load_call = mcw_call ((mcw_fn)job, a0, a1);
	se_load_jobs++;
	return 1;
}

void SE_JobEnd (void)
{
	mcw_join (load_call);
	load_call = 0;
}

/* Calls of one function given out to idle workers (mcw_call), joined together. */
void SE_GroupAdd (se_group_t *g, unsigned (*fn) (unsigned, unsigned), unsigned a0, unsigned a1)
{
	g->h[g->n++] = mcw_call ((mcw_fn)fn, a0, a1);
}

int SE_GroupReady (const se_group_t *g)
{
	int i;

	for (i = 0; i < g->n; i++)
		if (!mcw_ready (g->h[i]))
			return 0;
	return 1;
}

void SE_GroupJoin (se_group_t *g)
{
	int i;

	for (i = 0; i < g->n; i++)
		mcw_join (g->h[i]);
	g->n = 0;
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
