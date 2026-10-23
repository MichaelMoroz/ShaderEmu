/*
 * The server's frame on a worker core (docs/games/quake3.md, docs/machine/multicore.md). A frame of the game
 * is the server's (the players' moves, the bots, what everything in the level does, and what
 * each client is told of it) and then the client's (the picture). With bots the first is most
 * of a core by itself. Here it runs on a worker while this core draws, and the two go at
 * their own pace: the client draws from what the server last told it, as it does in a game
 * over a network, which is what the two already are to each other.
 *
 * What the server's frame does on a worker it does to the server's own data, which this core
 * leaves alone meanwhile: a console command of the server's or the game's waits for the frame
 * to end. What it shares with the client it does not touch from there. The packets it is
 * sent wait for its next frame and the ones it sends go when its frame is taken; console
 * lines, commands and cvar changes are kept and done by this core afterwards; an error ends
 * the job and is raised here; and the engine has two of what two cores would both write
 * (quake3.patch: a command's words, the marks of a trace, the box a thing is clipped as, the
 * buffers va() returns, a zone for the server's allocations). The build gives every variable
 * its own 16 bytes, for two cores must not store to the same 16.
 *
 * It is on this core while a level loads, while any client is not yet in the game, and when
 * QUAKE3_SERVER=inline says so or the machine has no worker.
 */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

#include "server/server.h"
#include "botlib/botlib.h"
#include "botlib/be_aas.h"
#include "mcwx.h"
#include "se_q3.h"

enum { MODE_UNSET = -1, MODE_INLINE, MODE_WORKER };
enum { KEPT_PRINT, KEPT_COMMAND, KEPT_CVAR, KEPT_CVAR_FORCED, KEPT_RELIABLE };
#define BOX_BYTES	0x10000		/* of packets waiting for the server's frame, and of those it sent */
#define MOST_MSEC	200		/* of the game's time a frame of the server's catches up, at most */
#define JOB_MSEC	500		/* the same for a job of the worker's: five frames behind are made up, the clients told once */
#define STACK_BYTES	0x80000		/* the server's frame is deeper than a worker's own stack */
#define WARM_BYTES	0x100000	/* of the server's zone made ready for the routes (they were 400 KB) */

unsigned	se_main_stack;		/* where the first core's stack is (q_shared.h, SE_OnWorker) */
unsigned	se_server_jobs, se_server_cycles, se_server_passes;	/* for the q3stat: lines */

typedef struct { unsigned used, unused[3]; unsigned char data[BOX_BYTES]; } box_t;
static box_t	inbox __attribute__ ((aligned (16)));		/* this core's: packets for the server */
static box_t	job_in __attribute__ ((aligned (16)));		/* the job's: those it has to read */
static box_t	outbox __attribute__ ((aligned (16)));		/* the job's: packets the server sent */
/* what else a worker writes while it has a job: the server's, and the client's game's for each of its two lists (client_shaderemu.c) */
static struct {
	unsigned	used, lost, failed;
	int		code;
	char		error[1024];
	char		kept[8192];	/* a kind, then text ending in 0 (a cvar: its name, 0, its value, 0) */
} works[1 + SE_LISTS + SE_THINKERS + 1] __attribute__ ((aligned (16)));	/* then each core of a bot's think, the client's helper */
static char	job_stack[STACK_BYTES] __attribute__ ((aligned (16)));
static char	think_stack[SE_THINKERS][SE_THINK_STACK] __attribute__ ((aligned (16)));
static struct { int (*think) (int, float); float thinktime; int serial, unused; } think_now __attribute__ ((aligned (16)));	/* the server's */
se_abort_t	se_worker_abort[2 + SE_THINKERS];
static int	mode = MODE_UNSET, pending, workers = -1;
static mcw_handle	out;		/* the worker's call (mcwx.h), while it is out */
static unsigned	out_result;	/* what the last one answered: its instructions */
static int	pace_frame, routes_late, routes_all, warming;	/* QUAKE3_SERVER=frame, QUAKE3_ROUTES=late or all; the job out is the routes' */
static int	paced;		/* QUAKE3_SERVER=paced: the worker's jobs, made by this core where they are given out */
static struct { int bytes, items, unused[2]; } warmed __attribute__ ((aligned (16)));	/* the worker's */
int BotWarmLevelItemRoutes (int travelflags, int *items);	/* be_ai_goal.c (quake3.patch) */
extern int se_rand_core;					/* sys_shaderemu.c */
int AAS_WarmAllRoutes (int travelflags);		/* be_aas_route.c */
/* QUAKE3_SERVER_LOG=1: a q3srv: line for every job. The worker's part of it, and this core's */
static struct { unsigned began, ended, frames, bots, game, tell, moves, packets; } job_log __attribute__ ((aligned (16)));
static unsigned	log_on, log_posted, log_msec, log_jobs;
unsigned SE_WindowClock (void);
extern unsigned se_server_parts[3], se_server_frames;	/* sv_main.c (quake3.patch) */

/* fn(arg) on another stack */
void CL_AddReliableCommand (const char *cmd, qboolean isDisconnectCmd);
uint32_t se_call_on_stack (uint32_t (*fn) (uint32_t), uint32_t arg, void *stack_top);
#ifdef __gpuvm__
__asm__ (".text\n.globl se_call_on_stack\n.type se_call_on_stack, @function\n.p2align 4\nse_call_on_stack:\n"
	"	addi a2, a2, -16\n	sw ra, 12(a2)\n	sw sp, 8(a2)\n	mv sp, a2\n	mv t0, a0\n	mv a0, a1\n	callr t0\n"
	"	lw ra, 12(sp)\n	lw sp, 8(sp)\n	ret\n");
#else
__asm__ (".text\n.globl se_call_on_stack\n.type se_call_on_stack, @function\nse_call_on_stack:\n"
	"	addi a2, a2, -16\n	sw ra, 12(a2)\n	sw sp, 8(a2)\n	mv sp, a2\n	mv t0, a0\n	mv a0, a1\n	jalr t0\n"
	"	lw ra, 12(sp)\n	lw sp, 8(sp)\n	ret\n");
#endif

/* The program's worker cores: the server's, the client's game's, the scene's, the bots' thinks', and
   QUAKE3_DECODERS more (24) for the pictures a level loads (tr_shaderemu.c). How many there are. */
int	se_game_workers;	/* of them the game's own */

int SE_Workers (void)
{
	if (workers < 0) {
		const char	*scene = getenv ("QUAKE3_SCENE"), *decoders = getenv ("QUAKE3_DECODERS");

		se_game_workers = (scene && !strcmp (scene, "inline") ? 3 : 4) + SE_THINKERS;
		workers = mcw_open (se_game_workers + (decoders ? atoi (decoders) : 24));
		if (workers > 0)
			atexit (mcw_close);
	}
	return workers;
}

static void setup (void)
{
	const char	*how;
	unsigned	i;

	if (mode != MODE_UNSET)
		return;
	how = getenv ("QUAKE3_SERVER");	/* (asked once: this is called every frame) */
	log_on = getenv ("QUAKE3_SERVER_LOG") != NULL;
	mode = MODE_INLINE;
	pace_frame = how && !strcmp (how, "frame");
	routes_late = getenv ("QUAKE3_ROUTES") && !strcmp (getenv ("QUAKE3_ROUTES"), "late");
	routes_all = getenv ("QUAKE3_ROUTES") && !strcmp (getenv ("QUAKE3_ROUTES"), "all");
	if (getenv ("QUAKE3_TRACES") && !strcmp (getenv ("QUAKE3_TRACES"), "all"))
		se_trace_keep = 0;
	paced = how && !strcmp (how, "paced");
	if (paced || (!(how && !strcmp (how, "inline")) && SE_Workers () > 0)) {
		mode = MODE_WORKER;
		/* (the stack's top is there before the worker steps on it) */
		for (i = 4096; i <= 0x20000; i += 4096)
			job_stack[STACK_BYTES - i] = 0;
		for (i = 0; i < SE_THINKERS; i++)
			mcw_touch (think_stack[i] + SE_THINK_STACK - 0x10000, 0x10000);
	}
	think_now.serial = getenv ("QUAKE3_BOTS") && !strcmp (getenv ("QUAKE3_BOTS"), "serial");
	printf ("quake3: the server's frame is on %s\n", paced ? "this core, as the worker's" : mode == MODE_WORKER ? "a worker core" : "this core");
}

/* ---- what the worker keeps for this core ---- */

/* Whose kept lines a core's are: the server's, a list of the client's game's, a bot's think's */
static int kept_by (int core)
{
	return core == 2 ? se_client_list : core >= 3 ? 1 + SE_LISTS + core - 3 : 0;
}

static void keep (int kind, const char *a, const char *b)
{
	unsigned	la = strlen (a) + 1, lb = b ? strlen (b) + 1 : 0;
	int		w = kept_by (SE_OnWorker ());

	if (works[w].used + 1 + la + lb > sizeof works[w].kept) {
		works[w].lost++;
		return;
	}
	works[w].kept[works[w].used++] = (char)kind;
	memcpy (works[w].kept + works[w].used, a, la);
	works[w].used += la;
	if (b) {
		memcpy (works[w].kept + works[w].used, b, lb);
		works[w].used += lb;
	}
}

void SE_ServerKeepPrint (const char *text) { keep (KEPT_PRINT, text, NULL); }
void SE_ServerKeepCommand (const char *text) { keep (KEPT_COMMAND, text, NULL); }
void SE_ServerKeepCvar (const char *name, const char *value, int force) { keep (force ? KEPT_CVAR_FORCED : KEPT_CVAR, name, value ? value : ""); }
void SE_ClientKeepReliable (const char *text) { keep (KEPT_RELIABLE, text, NULL); }

void SE_ServerWorkerError (int code, const char *text)
{
	int	core = SE_OnWorker (), w = kept_by (core);

	Q_strncpyz (works[w].error, text, sizeof works[w].error);
	works[w].code = code;
	works[w].failed = 1;
	longjmp (se_worker_abort[core >= 3 ? core - 1 : w != 0].at, 1);
}

/* What a worker kept (0: the server's; 1, 2: the client's game's, by its list) is done by this core; 1 when it ended in an error (SE_WorkerRaise). */
int SE_WorkerKept (int w)
{
	unsigned	at;

	for (at = 0; at < works[w].used; ) {
		int	kind = works[w].kept[at++];
		char	*a = works[w].kept + at;

		at += strlen (a) + 1;
		if (kind == KEPT_PRINT) {
			Com_Printf ("%s", a);
		} else if (kind == KEPT_COMMAND) {
			Cbuf_AddText (a);
		} else if (kind == KEPT_RELIABLE) {
			CL_AddReliableCommand (a, qfalse);
		} else {
			Cvar_Set2 (a, works[w].kept + at, kind == KEPT_CVAR_FORCED);
			at += strlen (works[w].kept + at) + 1;
		}
	}
	works[w].used = 0;
	if (works[w].lost) {
		Com_Printf ("(%u of the %s lines lost)\n", works[w].lost, w ? "client's game's" : "server's");
		works[w].lost = 0;
	}
	at = works[w].failed;
	works[w].failed = 0;
	return (int)at;
}

void SE_WorkerRaise (int w)
{
	Com_Error (works[w].code, "%s", works[w].error);
}

/*
 * QUAKE3_SUM=1 with the server on this core (inline, paced): a q3srvsum: line a frame of the game,
 * over every linked entity's state and every player's, to compare two builds (=2: every word too).
 * On standard output, not the console: its lines are drawn, and the lists' sums would have them.
 */
void SE_ServerSum (void)
{
	static int	on = -1;
	unsigned	h = 2166136261u, i, k;
	playerState_t	ps;

	if (on < 0)
		on = getenv ("QUAKE3_SUM") ? atoi (getenv ("QUAKE3_SUM")) : 0;
	if (!on || (SE_OnWorker () && !paced))
		return;
	for (i = 0; i < (unsigned)sv.num_entities + sv_maxclients->integer; i++) {
		const unsigned char	*p;
		unsigned		bytes;

		if (i < (unsigned)sv.num_entities) {
			sharedEntity_t	*e = SV_GentityNum (i);

			if (!e->r.linked)
				continue;
			p = (const unsigned char *)&e->s, bytes = sizeof e->s;
		} else if (svs.clients[i - sv.num_entities].state == CS_ACTIVE) {
			ps = *SV_GameClientNum (i - sv.num_entities);
			ps.ping = 0;	/* (the clock's: SV_CalcPings) */
			p = (const unsigned char *)&ps, bytes = sizeof ps;
		} else
			continue;
		for (k = 0; k < bytes; k++)
			h = (h ^ p[k]) * 16777619u;
		if (on == 2) {
			printf ("q3srvpart: %d %u:", sv.time, i);
			for (k = 0; k < bytes / 4; k++)
				printf (" %x", ((const unsigned *)p)[k]);
			printf ("\n");
		}
	}
	printf ("q3srvsum: %d %08x\n", sv.time, h);
}

/* The bots' library as the game calls it directly (g_syscalls.c in quake3.patch), as SV_GameSystemCalls would */
extern botlib_export_t	*botlib_export;
int SE_GameBotLibUpdateEntity (int ent, void *bue) { return botlib_export->BotLibUpdateEntity (ent, bue); }
int AAS_SE_SeenAgain (int entnum);	/* be_aas_entity.c */
int SE_GameBotLibSeenAgain (int ent) { return AAS_SE_SeenAgain (ent); }
float SE_GameAASTime (void) { return botlib_export->aas.AAS_Time (); }
int SE_GameAASPointContents (vec3_t point) { return botlib_export->aas.AAS_PointContents (point); }
void SE_GameEAGetInput (int client, float thinktime, void *input) { botlib_export->ea.EA_GetInput (client, thinktime, input); }
float SE_GameCharacteristicBFloat (int character, int index, float min, float max)
{
	return botlib_export->ai.Characteristic_BFloat (character, index, min, max);
}
void SE_GameBotUserCommand (int clientNum, usercmd_t *ucmd)
{
	if (clientNum >= 0 && clientNum < sv_maxclients->integer)
		SV_ClientThink (&svs.clients[clientNum], ucmd);
}

/* With the server on this core (inline, paced) a command of the game's (addbot) draws from the server's numbers, not the client's; was it so */
int SE_ServerCommandDraws (int on)
{
	int	was = se_rand_core >= 0;

	setup ();
	if (mode != MODE_WORKER || paced)
		se_rand_core = on ? 1 : -1;
	return was;
}

/* ---- the bots' thinks ---- */

/*
 * The bots that think in a frame think at once (ai_main.c): each draws its own random numbers,
 * and what it says or asks as a client is kept and done after all of them, in their order, so
 * that one think does not see another's: the same whether they are made one after another or
 * on several cores.
 */
#define SAID_BYTES	4096
static struct { uint64_t seed; unsigned made, unused; } bot_rand[MAX_CLIENTS] __attribute__ ((aligned (16)));
static struct { int thinking, used, unused[2]; char text[SAID_BYTES]; } said[SE_CORES] __attribute__ ((aligned (16)));
void SE_RandUse (uint64_t *seed);	/* sys_shaderemu.c */
void SE_BotFreesDone (void);		/* be_ai_chat.c (quake3.patch) */
void AAS_SE_ThinkPools (void);		/* be_aas_route.c */

static void think_one (int (*think) (int, float), int client, float thinktime, int together)
{
	int	core = SE_OnWorker ();

	if (!together) {	/* (a bot that thinks alone draws and says as it always did) */
		think (client, thinktime);
		return;
	}
	if (!bot_rand[client].made)
		bot_rand[client].seed = 0x9e3779b97f4a7c15ull * (unsigned)(client + 1), bot_rand[client].made = 1;
	SE_RandUse (&bot_rand[client].seed);
	said[core].thinking = client + 1;
	think (client, thinktime);
	said[core].thinking = 0;
	SE_RandUse (NULL);
}

/* A command a bot gives as a client: kept when it is said in a think (1). */
int SE_BotCommandKept (int client, const char *command)
{
	int		core = SE_OnWorker ();
	unsigned	bytes = strlen (command) + 2;

	if (!said[core].thinking)
		return 0;
	if (said[core].used + bytes <= SAID_BYTES) {
		said[core].text[said[core].used] = (char)client;
		memcpy (said[core].text + said[core].used + 1, command, bytes - 1);
		said[core].used += bytes;
	}
	return 1;
}

/*
 * Where the bots think: one after another here, and the rest on cores of their own, each on a
 * stack of SE_THINK_STACK (SE_OnWorker says 3 and up there). From the first core they are calls
 * (mcwx.h); from the server's worker they are its helpers, jobs the first core gave out with it.
 */
unsigned	se_think_stacks = (unsigned)(uintptr_t)think_stack;
struct se_think_counts	se_think_counts __attribute__ ((aligned (16)));	/* bots thought, of them on helpers: the server's */
static struct { unsigned seq, count; int clients[MAX_CLIENTS]; } think_task[SE_THINKERS] __attribute__ ((aligned (16)));	/* the server's */
static struct { unsigned seq, unused[3]; } think_done[SE_THINKERS] __attribute__ ((aligned (16)));	/* each helper's */
static struct { unsigned quit, count, unused[2]; } helpers __attribute__ ((aligned (16)));	/* this core's */
static mcw_handle	helper_out[SE_THINKERS];

static uint32_t think_body (uint32_t k)
{
	unsigned	i;

	if (!setjmp (se_worker_abort[2 + k].at))
		for (i = 0; i < think_task[k].count; i++)
			think_one (think_now.think, think_task[k].clients[i], think_now.thinktime, 1);
	said[3 + k].thinking = 0;
	SE_RandUse (NULL);
	return 0;
}

static uint32_t think_job (uint32_t k, uint32_t unused)
{
	return se_call_on_stack (think_body, k, think_stack[k] + SE_THINK_STACK);
}

/* A helper of the server's job: the thinks it is given, until the job has ended. */
static uint32_t helper_job (uint32_t k, uint32_t unused)
{
	unsigned	seen = think_done[k].seq;

	for (;;) {
		while (think_task[k].seq == seen && !helpers.quit)
			mc_end_pass ();
		if (think_task[k].seq == seen)
			return 0;
		seen = think_task[k].seq;
		think_job (k, 0);
		think_done[k].seq = seen;
	}
}

/* The first core gives the server's job helpers when bots are there to think at once. */
static void helpers_give (void)
{
	int	i, bots = 0;

	/* (they stay from job to job, waiting a pass at a time: a job's end does not wait for them) */
	if (!helpers.count)
		helpers.quit = 0;
	if (think_now.serial || paced)
		return;
	for (i = 0; i < sv_maxclients->integer; i++)
		bots += svs.clients[i].state == CS_ACTIVE && svs.clients[i].netchan.remoteAddress.type == NA_BOT;
	for (i = helpers.count; i < bots - 1 && i < SE_THINKERS; i++) {
		if (!(helper_out[i] = mcw_try_call (helper_job, i, 0)))
			break;
		helpers.count++;
	}
}

static void helpers_join (void)
{
	unsigned	i;

	helpers.quit = 1;
	for (i = 0; i < helpers.count; i++)
		mcw_join (helper_out[i]);
	helpers.count = 0;
}

static void helpers_poll (void)
{
	unsigned	i;

	for (i = 0; i < helpers.count; i++)
		mcw_ready (helper_out[i]);	/* (a page a helper stopped at is brought) */
}

void SE_BotThinks (int (*think) (int, float), const int *clients, int n, float thinktime)
{
	/* (calls from the first core, the server's own or a job of it made there: QUAKE3_SERVER=paced) */
	int		i, core, k, h, first = mode != MODE_WORKER || paced || SE_OnWorker () == 0, failed = -1;
	unsigned	at;
	mcw_handle	call[SE_THINKERS];

	h = think_now.serial ? 0 : first ? SE_THINKERS : (int)helpers.count;
	if (h > n - 1)
		h = n - 1;
	se_think_counts.n[0] += (unsigned)n, se_think_counts.n[1] += (unsigned)(h > 0 ? h : 0);
	if (h > 0) {
		AAS_SE_ThinkPools ();	/* (room for the routes a core beside makes for itself) */
		think_now.think = think, think_now.thinktime = thinktime;
		for (k = 0; k < h; k++)
			think_task[k].count = 0;
		for (i = 0; i < n; i++)		/* (the server's core takes the first, the others one each in turn) */
			if (i % (h + 1))
				k = i % (h + 1) - 1, think_task[k].clients[think_task[k].count++] = clients[i];
		for (k = 0; k < h; k++) {
			think_task[k].seq++;
			if (first)
				call[k] = mcw_call (think_job, k, 0);
		}
	}
	for (i = 0; i < n; i += h + 1)
		think_one (think, clients[i], thinktime, n > 1);
	for (k = 0; k < h; k++) {
		if (first)
			mcw_join (call[k]);
		else
			while (think_done[k].seq != think_task[k].seq)
				mc_end_pass ();
		/* what the think kept, as this core's own; an error raised here */
		if (SE_OnWorker () == 0)
			failed = SE_WorkerKept (1 + SE_LISTS + k) ? 1 + SE_LISTS + k : failed;
		else {
			int		w = 1 + SE_LISTS + k;
			unsigned	at2;

			for (at2 = 0; at2 < works[w].used; ) {
				int		kind = works[w].kept[at2];
				const char	*a = works[w].kept + at2 + 1, *b = a + strlen (a) + 1;

				keep (kind, a, kind == KEPT_CVAR || kind == KEPT_CVAR_FORCED ? b : NULL);
				at2 += 1 + strlen (a) + 1 + (kind == KEPT_CVAR || kind == KEPT_CVAR_FORCED ? strlen (b) + 1 : 0);
			}
			works[w].used = 0, works[w].lost = 0;
			if (works[w].failed)
				failed = w, works[w].failed = 0;
		}
	}
	SE_BotFreesDone ();
	if (failed >= 0)
		Com_Error (works[failed].code, "%s", works[failed].error);
	for (i = 0; i < n; i++)
		for (core = 0; core < SE_CORES; core++)
			for (at = 0; at < (unsigned)said[core].used; at += strlen (said[core].text + at + 1) + 2)
				if (said[core].text[at] == clients[i])
					SV_ExecuteClientCommand (&svs.clients[clients[i]], said[core].text + at + 1, qtrue);
	for (core = 0; core < SE_CORES; core++)
		said[core].used = 0;
}

/* ---- packets ---- */

typedef struct { netadr_t adr; int length, unused; } packet_t;

static int box_add (box_t *box, netadr_t adr, int length, const void *data)
{
	unsigned	bytes = sizeof (packet_t) + (((unsigned)length + 15) & ~15u);
	packet_t	*p = (packet_t *)(box->data + box->used);

	if (length < 0 || box->used + bytes > BOX_BYTES)
		return 0;
	p->adr = adr;
	p->length = length;
	memcpy (p + 1, data, length);
	box->used += bytes;
	return 1;
}

/* A packet for the server: kept for its next frame, or read now when the server is this core's. */
int SE_ServerInbox (netadr_t from, msg_t *msg)
{
	setup ();
	if (mode != MODE_WORKER) {
		se_rand_core = 1;
		SV_PacketEvent (from, msg);
		se_rand_core = -1;
		return 1;
	}
	box_add (&inbox, from, msg->cursize, msg->data);	/* (one there is no room for is lost, as on a network) */
	return 1;
}

/* One the server sends from the worker: it goes when this core takes the frame. */
void SE_ServerOutbox (netadr_t to, int length, const void *data)
{
	box_add (&outbox, to, length, data);
}

/* The server reads the packets of a box. */
static void box_read (box_t *box)
{
	unsigned	at;

	job_log.packets = 0;
	for (at = 0; at < box->used; ) {
		packet_t	*p = (packet_t *)(box->data + at);
		msg_t		msg;

		job_log.packets++;

		at += sizeof (packet_t) + (((unsigned)p->length + 15) & ~15u);
		MSG_Init (&msg, (byte *)(p + 1), p->length);
		msg.cursize = p->length;
		if (com_sv_running->integer)
			SV_PacketEvent (p->adr, &msg);
	}
	box->used = 0;
}

/* ---- the job ---- */

static uint32_t job_body (uint32_t msec)
{
	if (!setjmp (se_worker_abort[0].at)) {
		int	left = (int)msec, step = sv_fps->integer > 0 ? 1000 / sv_fps->integer : 100;

		job_log.moves = SE_Cycles ();
		box_read (&job_in);
		job_log.moves = SE_Cycles () - job_log.moves;
		/* (a frame of the game at a time: the bots look where they are before each, not once for several) */
		do {
			/* (of several at once the last tells the clients: they would have the others in the same moment) */
			se_server_quiet = left > step && !pace_frame;
			SV_Frame (left < step ? left : step);
			left -= step;
		} while (left > 0 && com_sv_running->integer);
	}
	se_server_quiet = 0;
	return 0;
}

static uint32_t server_job (uint32_t msec, uint32_t unused)
{
	unsigned	began = SE_Cycles (), frames = se_server_frames, *p = se_server_parts, p0 = p[0], p1 = p[1], p2 = p[2];

	job_log.began = SE_WindowClock ();
	se_call_on_stack (job_body, msec, job_stack + STACK_BYTES);
	job_log.frames = se_server_frames - frames, job_log.bots = p[0] - p0, job_log.game = p[1] - p1, job_log.tell = p[2] - p2;
	job_log.ended = SE_WindowClock ();
	return SE_Cycles () - began;
}

/* A frame the worker had has ended (out_result its answer): what it kept and sent is done here. */
static void done (void)
{
	unsigned	at;
	int		failed;

	if (warming) {
		warming = 0;
		Com_Printf ("quake3: the bots' routes to %d items made ahead: %d KB, %u thousand instructions of the worker core\n",
			warmed.items, warmed.bytes / 1024, out_result / 1000);
		if (SE_WorkerKept (0))
			SE_WorkerRaise (0);
		return;
	}
	se_server_cycles += out_result;
	se_server_jobs++;
	if (log_on)
		printf ("q3srv: job %u: posted %u began %u ended %u taken %u ms; %d ms of the game in %u frames, %u instructions: the bots %u, the game %u, telling %u, the moves %u in %u\n",
			++log_jobs, log_posted, job_log.began, job_log.ended, SE_WindowClock (), log_msec, job_log.frames, out_result,
			job_log.bots, job_log.game, job_log.tell, job_log.moves, job_log.packets);
	failed = SE_WorkerKept (0);
	for (at = 0; at < outbox.used; ) {
		packet_t *p = (packet_t *)(outbox.data + at);

		at += sizeof (packet_t) + (((unsigned)p->length + 15) & ~15u);
		NET_SendPacket (NS_SERVER, p->length, p + 1, p->adr);
	}
	outbox.used = 0;
	if (failed)
		SE_WorkerRaise (0);
}

static void finish (void)
{
	out_result = mcw_join (out);
	out = 0;
	done ();
}

/* Before anything on this core that reads or changes the server's data. */
void SE_ServerSettle (void)
{
	if (!out)
		return;
	while (!mcw_ready (out)) {
		mcw_handle	on[1 + SE_THINKERS + 3];
		unsigned	i;
		int		n;

		on[0] = out;
		for (i = 0; i < helpers.count && i < SE_THINKERS; i++)
			on[1 + i] = helper_out[i];
		n = 1 + (int)i;
		n += SE_ClientOut (on + n);
		n += SE_SceneOut (on + n);
		mcw_rest_on (on, n);	/* (gpu_vm: those and the client's frame have the whole pass) */
		se_server_passes++;
		helpers_poll ();	/* (the job may wait for a helper stopped at a page) */
	}
	finish ();
}

/* Whether the server's next frame is this core's: a level loading, somebody joining or leaving. */
static int slow (void)
{
	int	i;

	if (sv.state != SS_GAME || sv.restarting || !SE_ClientPlaying ())
		return 1;
	for (i = 0; i < sv_maxclients->integer; i++)
		if (svs.clients[i].state != CS_FREE && svs.clients[i].state != CS_ACTIVE)
			return 1;
	return 0;
}

/* The server's next frame is given to the worker, if one is due and the worker's to make. */
static void give (void)
{
	int	frame_msec;

	if (slow () || (sv_paused->integer && cl_paused->integer))
		return;
	/* the infostrings, if a variable of them has changed: this core sets those flags too */
	if (cvar_modifiedFlags & CVAR_SERVERINFO) {
		SV_SetConfigstring (CS_SERVERINFO, Cvar_InfoString (CVAR_SERVERINFO));
		cvar_modifiedFlags &= ~CVAR_SERVERINFO;
	}
	if (cvar_modifiedFlags & CVAR_SYSTEMINFO) {
		SV_SetConfigstring (CS_SYSTEMINFO, Cvar_InfoString_Big (CVAR_SYSTEMINFO));
		cvar_modifiedFlags &= ~CVAR_SYSTEMINFO;
	}
	/* not before a frame of the game is due: a job that only reads the client's moves is a core woken for little */
	frame_msec = sv_fps->integer > 0 ? (int)(1000 / sv_fps->integer * com_timescale->value) : 100;
	if (sv.timeResidual + pending < frame_msec && inbox.used < BOX_BYTES / 2)
		return;
	memcpy (job_in.data, inbox.data, inbox.used);
	job_in.used = inbox.used;
	inbox.used = 0;
	log_posted = SE_WindowClock (), log_msec = pending;
	if (paced) {
		pending = 0;
		out_result = server_job ((uint32_t)(log_msec > MOST_MSEC ? MOST_MSEC : log_msec), 0);
		done ();
		return;
	}
	helpers_give ();
	out = mcw_call (server_job, (uint32_t)(pending > JOB_MSEC ? JOB_MSEC : pending), 0);
	pending = 0;
}

/*
 * Called a few times in a frame of the client's and in every pass it waits: a worker stopped
 * at a page nobody has touched is helped on, and a frame of the server's that kept nothing
 * for this core is taken where it is found ended and the next given out.
 */
void SE_ServerPoll (void)
{
	if (out && mcw_ready (out) && !warming && !pace_frame && !works[0].used && !works[0].lost && !works[0].failed) {
		finish ();
		if (com_sv_running->integer && !sv_killserver->integer)
			give ();
	}
	helpers_poll ();
	SE_ClientPoll ();
}

static uint32_t warm_body (uint32_t unused)
{
	/* (a bot that has the rocket launcher asks for the routes a rocket jump opens: another set) */
	if (setjmp (se_worker_abort[0].at))
		return 0;
	if (routes_all)
		warmed.bytes = AAS_WarmAllRoutes (TFL_DEFAULT) + AAS_WarmAllRoutes (TFL_DEFAULT | TFL_ROCKETJUMP), warmed.items = -1;
	else
		warmed.bytes = BotWarmLevelItemRoutes (TFL_DEFAULT, &warmed.items) + BotWarmLevelItemRoutes (TFL_DEFAULT | TFL_ROCKETJUMP, &warmed.items);
	return 0;
}

static uint32_t warm_job (uint32_t a, uint32_t b)
{
	unsigned	began = SE_Cycles ();

	se_call_on_stack (warm_body, 0, job_stack + STACK_BYTES);
	return SE_Cycles () - began;
}

/* The client is about to load the level and asks nothing of the server until it has: the bots' routes are made meanwhile. */
void SE_ServerWarm (void)
{
	int	i;

	setup ();
	if (routes_late || out || !com_sv_running->integer || sv.state != SS_GAME)
		return;
	/* (only when a bot is there to ask for them: one added later makes its own, in its first frames) */
	for (i = 0; i < sv_maxclients->integer; i++)
		if (svs.clients[i].state != CS_FREE && svs.clients[i].netchan.remoteAddress.type == NA_BOT)
			break;
	if (i == sv_maxclients->integer)
		return;
	if (mode != MODE_WORKER) {	/* (here too, so that a frame is the same work as on the worker) */
		warm_body (0);
		Com_Printf ("quake3: the bots' routes to %d items made ahead: %d KB\n", warmed.items, warmed.bytes / 1024);
		return;
	}
	warming = 1;
	if (paced) {
		out_result = warm_job (0, 0);
		done ();
		return;
	}
	SE_ServerZoneTouch (mcw_touch, WARM_BYTES);
	out = mcw_call (warm_job, 0, 0);
}

/* In place of SV_Frame(): so much time has passed. */
void SE_ServerFrame (int msec)
{
	setup ();
	if (!com_sv_running->integer || sv_killserver->integer) {
		SE_ServerSettle ();
		helpers_join ();
		SV_Frame (msec);
		return;
	}
	if (mode != MODE_WORKER) {
		/* (a frame of the game at a time here too, when this core's frame was several long) */
		int	step = sv_fps->integer > 0 ? 1000 / sv_fps->integer : 100;

		if (msec > MOST_MSEC)
			msec = MOST_MSEC;
		se_rand_core = 1;
		do {
			SV_Frame (msec < step ? msec : step);
			msec -= step;
		} while (msec > 0 && com_sv_running->integer);
		se_rand_core = -1;
		return;
	}
	pending += msec;
	if (out) {
		if (!mcw_ready (out))
			return;
		finish ();
		if (!com_sv_running->integer)
			return;
	}
	if (slow () || (sv_paused->integer && cl_paused->integer)) {
		int	step = sv_fps->integer > 0 ? 1000 / sv_fps->integer : 100;

		se_rand_core = paced ? 1 : -1;	/* (paced: the server's draws are its own all along, for two builds to compare) */
		helpers_join ();	/* (the thinks here are this core's calls: a helper left waiting would make them too) */
		box_read (&inbox);
		if (pending > MOST_MSEC)
			pending = MOST_MSEC;
		do {
			SV_Frame (pending < step ? pending : step);
			pending -= step;
		} while (pending > 0 && com_sv_running->integer);
		pending = 0;
		se_rand_core = -1;
		return;
	}
	give ();
}
