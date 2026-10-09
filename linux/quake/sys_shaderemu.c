/*
 * Quake's system layer on the ShaderEmu machine's Linux (docs/quake.md): sys_linux.c without
 * the terminal handling, and with the machine's clock.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "quakedef.h"

qboolean	isDedicated;

unsigned VID_Milliseconds (void);

void Sys_DebugNumber (int y, int val) {}
void Sys_Init (void) {}
void Sys_HighFPPrecision (void) {}
void Sys_LowFPPrecision (void) {}
void Sys_MakeCodeWriteable (unsigned long startaddr, unsigned long length) {}
void Sys_DebugLog (char *file, char *fmt, ...) {}
char *Sys_ConsoleInput (void) { return NULL; }

void Sys_Printf (char *fmt, ...)
{
	va_list	argptr;
	char	text[1024];
	unsigned char *p;

	va_start (argptr, fmt);
	vsnprintf (text, sizeof text, fmt, argptr);
	va_end (argptr);
	/* Quake's coloured letters are the plain ones with the high bit set */
	for (p = (unsigned char *)text; *p; p++) {
		*p &= 0x7f;
		if (*p >= 32 || *p == '\n' || *p == '\t')
			putc (*p, stdout);
	}
	fflush (stdout);
}

void Sys_Quit (void)
{
	Host_Shutdown ();
	exit (0);
}

void Sys_Error (char *error, ...)
{
	va_list	argptr;
	char	string[1024];

	va_start (argptr, error);
	vsnprintf (string, sizeof string, error, argptr);
	va_end (argptr);
	if (SE_ServerOnWorker ())
		SE_ServerWorkerError (string);	/* the server's frame on a worker core: this core raises it */
	SE_ServerSettle ();
	fprintf (stderr, "quake: error: %s\n", string);
	Host_Shutdown ();
	exit (1);
}

int Sys_FileTime (char *path)
{
	struct stat buf;

	return stat (path, &buf) == -1 ? -1 : (int)buf.st_mtime;
}

void Sys_mkdir (char *path)
{
	mkdir (path, 0777);
}

int Sys_FileOpenRead (char *path, int *handle)
{
	struct stat info;
	int h = open (path, O_RDONLY, 0666);

	*handle = h;
	if (h == -1)
		return -1;
	if (fstat (h, &info) == -1)
		Sys_Error ("Error fstating %s", path);
	return info.st_size;
}

int Sys_FileOpenWrite (char *path)
{
	int handle = open (path, O_RDWR | O_CREAT | O_TRUNC, 0666);

	if (handle == -1)
		Sys_Error ("Error opening %s: %s", path, strerror (errno));
	return handle;
}

int Sys_FileWrite (int handle, void *src, int count) { return write (handle, src, count); }
void Sys_FileClose (int handle) { close (handle); }
void Sys_FileSeek (int handle, int position) { lseek (handle, position, SEEK_SET); }
int Sys_FileRead (int handle, void *dest, int count) { return read (handle, dest, count); }

/* The machine's clock word once the window is open, going on from the system's clock before. */
double Sys_FloatTime (void)
{
	static unsigned	ahead, last, turns;
	static int	ours;
	struct timespec	ts;
	unsigned	ms;

	if (ours) {
		ms = VID_Milliseconds () + ahead;
	} else {
		clock_gettime (CLOCK_MONOTONIC, &ts);
		ms = (unsigned)ts.tv_sec * 1000u + (unsigned)ts.tv_nsec / 1000000u;
		if (VID_Milliseconds ()) {
			ahead = ms - VID_Milliseconds ();
			ours = 1;
		}
	}
	if (ms < last)
		turns++;
	last = ms;
	return (turns * 4294967296.0 + ms) * 0.001;
}

/*
 * Lines typed at the terminal Quake was started from are console commands: the way to drive
 * it without its window's keyboard (a test does, through the machine's serial console).
 */
static void terminal_commands (void)
{
	static char	line[256];
	static int	length, closed;
	struct timeval	none = {0, 0};
	fd_set		set;
	char		c;

	while (!closed) {
		FD_ZERO (&set);
		FD_SET (0, &set);
		if (select (1, &set, NULL, NULL, &none) <= 0)
			return;
		if (read (0, &c, 1) != 1) {
			closed = 1;
		} else if (c == '\n' || c == '\r') {
			line[length] = 0;
			if (length) {
				Cbuf_AddText (line);
				Cbuf_AddText ("\n");
			}
			length = 0;
		} else if (length < (int)sizeof line - 1) {
			line[length++] = c;
		}
	}
}

int main (int c, char **v)
{
	double		time, oldtime, newtime;
	quakeparms_t	parms;
	int		j;

	memset (&parms, 0, sizeof (parms));
	COM_InitArgv (c, v);
	parms.argc = com_argc;
	parms.argv = com_argv;
	parms.memsize = 16 * 1024 * 1024;
	j = COM_CheckParm ("-mem");
	if (j)
		parms.memsize = (int)(Q_atof (com_argv[j + 1]) * 1024 * 1024);
	parms.membase = malloc (parms.memsize);
	if (!parms.membase)
		Sys_Error ("no memory for a heap of %d bytes", parms.memsize);
	parms.basedir = ".";

	Host_Init (&parms);

	oldtime = Sys_FloatTime () - 0.1;
	for (;;) {
		newtime = Sys_FloatTime ();
		time = newtime - oldtime;
		/* the clock moves once a machine frame: end the frame instead of asking again */
		if (time < 0.001) {
			__asm__ volatile (".word 0x0100000f");	/* pause */
			continue;
		}
		oldtime = newtime;
		terminal_commands ();
		Host_Frame (time);
	}
}
