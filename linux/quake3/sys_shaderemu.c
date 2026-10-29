/*
 * Quake III Arena's system layer on the ShaderEmu machine's Linux (docs/games/quake3.md): in place
 * of ioquake3's sys_main.c and its terminal console. The game, the client's game and the
 * menus are not libraries found at run time but part of the program.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "qcommon/q_shared.h"
#include "qcommon/qcommon.h"
#include "sys/sys_local.h"
#include "se_data.h"
#include "se_q3.h"
#include "mcw.h"

unsigned	se_started;

/* The C library's rand() as it is, but a core its own state (SE_OnWorker): the game and the server's both draw */
static struct { uint64_t seed; unsigned unused[2]; } rand_of[SE_CORES] __attribute__ ((aligned (16)));
int	se_rand_core = -1;	/* the server's frame on this core draws as on its worker (server_shaderemu.c) */

static struct { uint64_t *seed; unsigned unused[3]; } use_of[SE_CORES] __attribute__ ((aligned (16)));	/* a thinking bot's */

static int rand_core (void)
{
	int	core = SE_OnWorker ();

	return core == 0 && se_rand_core >= 0 ? se_rand_core : core;
}

/* This core draws from that state until it is told 0 (a bot's think: server_shaderemu.c) */
void SE_RandUse (uint64_t *seed)
{
	use_of[SE_OnWorker ()].seed = seed;
}

void srand (unsigned s)
{
	rand_of[rand_core ()].seed = s - 1;
}

int rand (void)
{
	int		core = rand_core ();
	uint64_t	*seed = use_of[SE_OnWorker ()].seed ? use_of[SE_OnWorker ()].seed : &rand_of[core].seed;

	*seed = 6364136223846793005ULL * *seed + 1;
	return (int)(*seed >> 33);
}

unsigned SE_Cycles (void)
{
	unsigned v;

	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

static unsigned	phase, phase_since, phase_cycles[6];
#ifdef SE_PARTS
unsigned	se_part, se_part_since, se_part_cycles[24];
#endif

int SE_Phase (int next)
{
	unsigned	now = SE_Cycles ();
	int		was = (int)phase;

	phase_cycles[phase] += now - phase_since;
	phase = (unsigned)next;
	phase_since = now;
	SE_ServerPoll ();	/* a worker core with the server's frame may want a page */
	return was;
}

static unsigned	frames, hold;

/* The console command "hold": the frame after next stays, as with QUAKE3_HOLD. */
static void hold_command (void)
{
	hold = frames + 2;
}

/*
 * Every QUAKE3_STATS_FRAMES frames (200; 0 never): the machine's instructions a frame and
 * where they went. QUAKE3_HOLD=N keeps frame N on the screen and stops there.
 */
void SE_FrameDone (void)
{
	static unsigned	since_frames, since_cycles, since_ms, every = 200;
	static int	read_settings;
	unsigned	now, n, *c = phase_cycles;

	if (!read_settings) {
		const char *s;

		read_settings = 1;
		if ((s = getenv ("QUAKE3_STATS_FRAMES")) != NULL)
			every = (unsigned)atoi (s);
		if ((s = getenv ("QUAKE3_HOLD")) != NULL)
			hold = (unsigned)atoi (s);
		Cmd_AddCommand ("hold", hold_command);
		since_cycles = SE_Cycles (), since_ms = (unsigned)Sys_Milliseconds ();
	}
	frames++;
	if (hold && frames >= hold) {
		SE_Settle ();	/* (the frame is on the screen when this says so) */
		printf ("quake3: holding frame %u\n", frames);
		fflush (stdout);
		for (;;)
			__asm__ volatile (".word 0x0100000f");	/* pause: the machine's pass ends */
	}
	n = frames - since_frames;
	if (!every || n < every)
		return;
	SE_Phase ((int)phase);
#ifdef SE_MEMPROF
	{
		void SE_MemProfile (void);

		SE_MemProfile ();
	}
#endif
	now = (unsigned)Sys_Milliseconds ();
	printf ("q3stat: frame %u: %u frames in %u ms, %u instructions a frame; in thousands: server %u, client %u, scene %u, swap %u, sound %u, rest %u\n",
		frames, n, now - since_ms, (SE_Cycles () - since_cycles) / n, c[1] / n / 1000, c[2] / n / 1000, c[3] / n / 1000,
		c[4] / n / 1000, c[5] / n / 1000, c[0] / n / 1000);
	{
		extern unsigned se_counts[6];	/* tr_shaderemu.c: the drawing core's to write, read here and never cleared */
		static unsigned was[6];
		unsigned k[6], i;

		for (i = 0; i < 6; i++)
			k[i] = se_counts[i] - was[i], was[i] = se_counts[i];
		printf ("q3stat: a frame drew %u.%u models in %u.%u draws, %u.%u of them lit, %u.%u sprites and beams, %u.%u polygons, %u flat pictures\n",
			k[0] / n, k[0] * 10 / n % 10, k[1] / n, k[1] * 10 / n % 10, k[2] / n, k[2] * 10 / n % 10, k[3] / n, k[3] * 10 / n % 10,
			k[4] / n, k[4] * 10 / n % 10, k[5] / n);
	}
	if (se_scene_jobs) {
		printf ("q3stat: the scene on a worker core: %u frames of it to these %u, %u thousand instructions each, %u.%u passes waited for one\n",
			se_scene_jobs, n, se_scene_cycles / se_scene_jobs / 1000, se_scene_passes / se_scene_jobs, se_scene_passes * 10 / se_scene_jobs % 10);
		se_scene_jobs = se_scene_cycles = se_scene_passes = 0;
	}
	if (mcw_count ()) {
		extern unsigned mcw_faults, mcw_calls;	/* mcw.c */

		printf ("q3stat: for the worker cores so far: %u pages brought, %u system calls made\n", mcw_faults, mcw_calls);
	}
	if (se_server_jobs) {
		printf ("q3stat: the server on a worker core: %u frames of its own to these %u, %u thousand instructions each, %u.%u passes waited for one\n",
			se_server_jobs, n, se_server_cycles / se_server_jobs / 1000, se_server_passes / se_server_jobs, se_server_passes * 10 / se_server_jobs % 10);
		se_server_jobs = se_server_cycles = se_server_passes = 0;
	}
	if (se_client_jobs) {
		printf ("q3stat: the client's game on a worker core: %u frames of its own to these %u, %u thousand instructions each, %u.%u passes waited for one\n",
			se_client_jobs, n, se_client_cycles / se_client_jobs / 1000, se_client_passes / se_client_jobs, se_client_passes * 10 / se_client_jobs % 10);
		se_client_jobs = se_client_cycles = se_client_passes = 0;
	}
	{
		static unsigned	was[4];
		unsigned	f = se_side_worker.n[0] - was[0];

		if (f)
			printf ("q3stat: the level's items on the client's helper core: %u frames, %u items each, %u thousand instructions each, "
				"%u.%u passes waited for one\n", f, (se_side_worker.n[1] - was[1]) / f, (se_side_helper.n[0] - was[3]) / f / 1000,
				(se_side_worker.n[2] - was[2]) / f, (se_side_worker.n[2] - was[2]) * 10 / f % 10);
		was[0] = se_side_worker.n[0], was[1] = se_side_worker.n[1], was[2] = se_side_worker.n[2], was[3] = se_side_helper.n[0];
	}
	{
		extern unsigned se_server_parts[3], se_server_frames;	/* sv_main.c (quake3.patch) */
		static unsigned	was[4];		/* (the counts are the server's core's to write: read here, never cleared) */
		static unsigned	thinks[2];
		unsigned	now[4] = {se_server_parts[0], se_server_parts[1], se_server_parts[2], se_server_frames};
		unsigned	made = now[3] - was[3], f = made ? made : 1;

		printf ("q3stat: the server made %u frames of the game, in thousands each: the bots %u, the game %u, telling the clients %u; "
			"%u bots' thinks, %u of them on helpers\n", made, (now[0] - was[0]) / f / 1000, (now[1] - was[1]) / f / 1000,
			(now[2] - was[2]) / f / 1000, se_think_counts.n[0] - thinks[0], se_think_counts.n[1] - thinks[1]);
		memcpy (was, now, sizeof was);
		thinks[0] = se_think_counts.n[0], thinks[1] = se_think_counts.n[1];
	}
#ifdef SE_PARTS
	{
		static const char *const names[] = {"rest", "packets", "moves", "snapshots", "prediction", "entities", "effects", "weapon",
			"scene", "flat", "screen", "sound", "swap", "server", "view"};
		unsigned i;

		SE_Part ((int)se_part);
		printf ("q3parts:");
		for (i = 0; i < ARRAY_LEN (names); i++)
			printf (" %s %u.%u", names[i], se_part_cycles[i] / n / 1000, se_part_cycles[i] / n / 100 % 10);
		printf ("\n");
		memset (se_part_cycles, 0, sizeof se_part_cycles);
	}
#endif
	fflush (stdout);
	memset (phase_cycles, 0, sizeof phase_cycles);
	since_frames = frames, since_cycles = SE_Cycles (), since_ms = now;
}

/*
 * Where an open file's first byte lies in the ROM, as the GPU and the sound card address it;
 * 0 when it is not a file of the ROM that begins with those bytes. (romfs: a file's inode
 * number is where its header is, and its bytes follow its name; linux/userland/shaderemu_sound.h.)
 */
unsigned SE_RomAddress (int fd, const char *begins, int count)
{
	struct stat	st;
	unsigned char	head[16 + 256], data[64];
	unsigned	at;
	int		mtd, n;

	if (fstat (fd, &st) || !S_ISREG (st.st_mode) || count > (int)sizeof data || (mtd = open ("/dev/mtd0", O_RDONLY)) < 0)
		return 0;
	n = pread (mtd, head, sizeof head, st.st_ino);
	for (at = 16; (int)at < n && head[at]; at++)
		continue;
	if (n < 32 || (int)at >= n ||
	    ((unsigned)head[8] << 24 | head[9] << 16 | head[10] << 8 | head[11]) != (unsigned)st.st_size ||
	    pread (mtd, data, count, st.st_ino + 16 + (at & ~15u)) != count || memcmp (data, begins, count) != 0)
		at = 0;
	close (mtd);
	return at ? 0x40000000u + (unsigned)st.st_ino + 16 + (at & ~15u) : 0;
}

const unsigned char *SE_Data (unsigned *rom)
{
	static const unsigned char	*data;
	static unsigned			at;
	static int			tried;
	struct stat			st;
	int				fd;

	if (!tried) {
		tried = 1;
		if ((fd = open (SE_DATA_FILE, O_RDONLY)) >= 0 && !fstat (fd, &st)) {
			data = mmap (NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
			if (data == MAP_FAILED || ((const se_header_t *)data)->magic != SE_MAGIC)
				data = NULL;
			else
				at = SE_RomAddress (fd, "SEQ3", 4);
			close (fd);
		}
	}
	if (rom)
		*rom = at;
	return data;
}

const void *SE_DataSection (const char *tag, unsigned each, int *count)
{
	const se_header_t	*h = (const se_header_t *)SE_Data (NULL);
	unsigned		i;

	for (i = 0; h && i < h->sections && i < SE_SECTIONS; i++)
		if (!memcmp (h->section[i].tag, tag, 4)) {
			if (count)
				*count = (int)(h->section[i].bytes / each);
			return (const unsigned char *)h + h->section[i].at;
		}
	return NULL;
}

int	se_wanted;

void SE_Wanted (const char *kind, const char *name)
{
	if (se_wanted)
		printf ("wanted: %s %s\n", kind, name);
}

/* ---- where the program and its data are ---- */

static char	binary_path[MAX_OSPATH], install_path[MAX_OSPATH];

void Sys_SetBinaryPath (const char *path) { Q_strncpyz (binary_path, path, sizeof binary_path); }
char *Sys_BinaryPath (void) { return binary_path; }
void Sys_SetDefaultInstallPath (const char *path) { Q_strncpyz (install_path, path, sizeof install_path); }
char *Sys_DefaultInstallPath (void) { return *install_path ? install_path : Sys_Cwd (); }
char *Sys_DefaultAppPath (void) { return Sys_BinaryPath (); }

char *Sys_GetClipboardData (void) { return NULL; }
void Sys_RemovePIDFile (const char *gamedir) {}
void Sys_InitPIDFile (const char *gamedir) {}
qboolean Sys_OpenFolderInFileManager (const char *path, qboolean create) { return qfalse; }
cpuFeatures_t Sys_GetProcessorFeatures (void) { return 0; }
void Sys_UnloadDll (void *dllHandle) {}
void *Sys_LoadDll (const char *name, qboolean useSystemLib) { return NULL; }

void Sys_Init (void)
{
	Cvar_Set ("arch", "linux riscv32");
	Cvar_Set ("username", "player");
}

/* ---- the three modules, linked in (build.sh) ---- */

typedef intptr_t (QDECL *syscall_t) (intptr_t, ...);
#define MODULE(m) 	intptr_t m##_vmMain (int, int, int, int, int, int, int, int, int, int, int, int, int); 	void m##_dllEntry (syscall_t); 	extern char __start_##m##_data[], __stop_##m##_data[], __start_##m##_bss[], __stop_##m##_bss[];
MODULE (game) MODULE (cgame) MODULE (ui)
#define PARTS(m) m##_vmMain, m##_dllEntry, __start_##m##_data, __stop_##m##_data, __start_##m##_bss, __stop_##m##_bss

/*
 * The module of that name ("qagame", "cgame", "ui"): its entry, told where the engine's calls
 * go. Its variables are as the program started with them each time, as a library's are each
 * time it is loaded: the game counts on that between levels.
 */
void *Sys_LoadGameDll (const char *name, vmMainProc *entryPoint, intptr_t (*systemcalls) (intptr_t, ...))
{
	static struct { const char *name; void *entry; void (*told) (syscall_t); char *data, *data_end, *bss, *bss_end, *first; } modules[] = {
		{"qagame", PARTS (game)}, {"cgame", PARTS (cgame)}, {"ui", PARTS (ui)},
	};
	unsigned i;

	for (i = 0; i < ARRAY_LEN (modules); i++)
		if (!strcmp (name, modules[i].name)) {
			size_t bytes = modules[i].data_end - modules[i].data;

			if (!modules[i].first) {
				if ((modules[i].first = malloc (bytes + 1)) == NULL)
					return NULL;
				memcpy (modules[i].first, modules[i].data, bytes);
			} else {
				memcpy (modules[i].data, modules[i].first, bytes);
				memset (modules[i].bss, 0, modules[i].bss_end - modules[i].bss);
			}
			*entryPoint = (vmMainProc)modules[i].entry;
			modules[i].told ((syscall_t)systemcalls);
			return (void *)&modules[i];
		}
	return NULL;
}

/* ---- the clock ---- */

/* The machine's clock word once the window is open (a load, where the system's is a call), going on from the system's. */
int Sys_Milliseconds (void)
{
	static unsigned	ahead, base;
	static int	ours, based;
	struct timespec	ts;
	unsigned	ms;

	if (ours)
		return (int)(SE_WindowClock () + ahead);
	clock_gettime (CLOCK_MONOTONIC, &ts);
	ms = (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
	if (!based)
		base = ms, based = 1;
	ms -= base;
	if (SE_WindowClock ()) {
		ahead = ms - SE_WindowClock ();
		ours = 1;
	}
	return (int)ms;
}

/* ---- the terminal ---- */

/* The game's lines without its colour marks (^ and a digit). */
void Sys_Print (const char *msg)
{
	char		out[1024];
	unsigned	n = 0;

	for (; *msg; msg++) {
		if (msg[0] == '^' && msg[1] && msg[1] != '^') {
			msg++;
			continue;
		}
		out[n++] = *msg;
		if (n == sizeof out)
			fwrite (out, 1, n, stdout), n = 0;
	}
	fwrite (out, 1, n, stdout);
	fflush (stdout);
}

void Sys_AnsiColorPrint (const char *msg) { Sys_Print (msg); }

static Q_NO_RETURN void leave (int code)
{
	NET_Shutdown ();
	Sys_PlatformExit ();
	exit (code);
}

void Sys_Quit (void)
{
	leave (0);
}

void Sys_Error (const char *error, ...)
{
	va_list	argptr;
	char	string[1024];

	va_start (argptr, error);
	vsnprintf (string, sizeof string, error, argptr);
	va_end (argptr);
	fprintf (stderr, "quake3: error: %s\n", string);
	leave (3);
}

void Sys_ErrorDialog (const char *error) {}

int Sys_FileTime (char *path)
{
	struct stat buf;

	return stat (path, &buf) == -1 ? -1 : (int)buf.st_mtime;
}

void Sys_SigHandler (int signal)
{
	static int	caught;

	fprintf (stderr, "quake3: signal %d\n", signal);
	if (!caught) {
		caught = 1;
		CL_Shutdown (va ("Received signal %d", signal), qtrue, qtrue);
		SV_Shutdown (va ("Received signal %d", signal));
	}
	leave (signal == SIGTERM || signal == SIGINT ? 1 : 2);
}

/* A fault says where it was: the address and the instruction (riscv's first register of the context is the pc) */
static void fault (int signal, siginfo_t *info, void *context)
{
	fprintf (stderr, "quake3: fault at %p, pc %08lx\n", info->si_addr, (unsigned long)((ucontext_t *)context)->uc_mcontext.__gregs[0]);
	Sys_SigHandler (signal);
}

/*
 * Lines typed at the terminal the game was started from are console commands: the way to
 * drive it without its window's keyboard (a test does, through the machine's serial console).
 * Asking costs a system call, tens of thousands of instructions: asked four times a second.
 */
char *Sys_ConsoleInput (void)
{
	static char	line[256], given[256];
	static int	length, closed;
	static unsigned	asked;
	struct timeval	none = {0, 0};
	fd_set		set;
	char		c;
	unsigned	now = (unsigned)Sys_Milliseconds ();

	if (closed || now - asked < 250)
		return NULL;
	for (;;) {
		FD_ZERO (&set);
		FD_SET (0, &set);
		if (select (1, &set, NULL, NULL, &none) <= 0) {
			asked = now;
			return NULL;
		}
		if (read (0, &c, 1) != 1) {
			closed = 1;
			return NULL;
		}
		if (c == '\n' || c == '\r') {
			if (!length)
				continue;
			line[length] = 0;
			length = 0;
			strcpy (given, line);
			return given;
		}
		if (length < (int)sizeof line - 1)
			line[length++] = c;
	}
}

int main (int argc, char **argv)
{
	char	command[MAX_STRING_CHARS] = {0};
	int	i;

	se_started = SE_Cycles ();
	se_main_stack = (unsigned)(uintptr_t)&i;
	se_wanted = getenv ("QUAKE3_WANTED") != NULL;
	Sys_PlatformInit ();
	Sys_Milliseconds ();
	Sys_SetBinaryPath (Sys_Dirname (argv[0]));
	Sys_SetDefaultInstallPath (".");
	for (i = 1; i < argc; i++) {
		int spaces = strchr (argv[i], ' ') != NULL;

		if (spaces)
			Q_strcat (command, sizeof command, "\"");
		Q_strcat (command, sizeof command, argv[i]);
		if (spaces)
			Q_strcat (command, sizeof command, "\"");
		Q_strcat (command, sizeof command, " ");
	}
	Com_Init (command);
	NET_Init ();
	signal (SIGILL, Sys_SigHandler);
	signal (SIGFPE, Sys_SigHandler);
	{
		struct sigaction	act;

		memset (&act, 0, sizeof act);
		act.sa_sigaction = fault;
		act.sa_flags = SA_SIGINFO;
		sigaction (SIGSEGV, &act, NULL);
	}
	signal (SIGTERM, Sys_SigHandler);
	signal (SIGINT, Sys_SigHandler);
	for (;;) {
		SE_Input ();
		Com_Frame ();
	}
}

#ifdef SE_MEMPROF
/*
 * QUAKE3_DEFINES=-DSE_MEMPROF QUAKE3_LINK=-Wl,--wrap=memset,--wrap=memcpy to build.sh: who
 * fills and copies how much, by the address that called (quake3.nm names it), printed with the
 * first q3stat: line.
 */
void *__real_memset (void *, int, size_t);
void *__real_memcpy (void *, const void *, size_t);
static struct { void *from; unsigned bytes, calls; } se_fills[2][512];

static void se_tally (int which, void *from, size_t n)
{
	unsigned i, h = ((unsigned)(uintptr_t)from >> 2) % 512;

	for (i = 0; i < 512; i++, h = (h + 1) % 512)
		if (se_fills[which][h].from == from || !se_fills[which][h].from) {
			se_fills[which][h].from = from;
			se_fills[which][h].bytes += n >> 4, se_fills[which][h].calls++;
			return;
		}
}

void *__wrap_memset (void *to, int c, size_t n)
{
	se_tally (0, __builtin_return_address (0), n);
	return __real_memset (to, c, n);
}

void *__wrap_memcpy (void *to, const void *from, size_t n)
{
	se_tally (1, __builtin_return_address (0), n);
	return __real_memcpy (to, from, n);
}

void SE_MemProfile (void)
{
	int which, i, k;

	for (which = 0; which < 2; which++)
		for (k = 0; k < 14; k++) {
			int best = -1;

			for (i = 0; i < 512; i++)
				if (se_fills[which][i].from && (best < 0 || se_fills[which][i].bytes > se_fills[which][best].bytes))
					best = i;
			if (best < 0 || !se_fills[which][best].bytes)
				break;
			printf ("memprof: %s from %p: %u KB in %u calls\n", which ? "memcpy" : "memset", se_fills[which][best].from,
				se_fills[which][best].bytes >> 6, se_fills[which][best].calls);
			se_fills[which][best].bytes = 0;
		}
	memset (se_fills, 0, sizeof se_fills);	/* (each line's since the last) */
}
#endif
