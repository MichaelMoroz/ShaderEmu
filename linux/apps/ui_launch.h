/* ui_launch.h: what a click that starts a program shows until the program's first window is
 * there: the pointer with an hourglass and a note by it, "Starting NAME" (docs/system/nanox.md, "A
 * program starting"). Ends at another program's window shown, the program gone, or 30 s. A
 * program that could not be started, ended before its window came or crashed later is reported
 * in a window of nxask's (ui_launch_watch). Needs nano-X.h, ui_icons.h and real_vfork.h;
 * UI_LAUNCH=0 shows no note, UI_LAUNCH=print says when it ends. */
#ifndef UI_LAUNCH_H
#define UI_LAUNCH_H
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include "ui_icons.h"
#include "ui_exec.h"

#define UI_LAUNCH_MOST 30000	/* ms */
#define UI_LAUNCH_HIGH 40

static GR_WINDOW_ID ui_launch_note;
static GR_CURSOR_ID ui_launch_busy;
static GR_GC_ID ui_launch_gc;
static GR_EVENT_MASK ui_launch_root;	/* the root's events as this program has them otherwise */
static GR_EVENT_MASK ui_launch_root_asked;	/* and as the last start said, the note's or not */
static pid_t ui_launch_pid;
static unsigned ui_launch_began, ui_launch_shown_s, ui_launch_looked;
static int ui_launch_w, ui_launch_fresh;	/* fresh: drawn at its map, its first exposure is no news */
static char ui_launch_line[48], ui_launch_name[33];
static struct ui_icon_ref ui_launch_icon;

/* The programs started for clicks, while they run: a name each, and whether a window came. */
#define UI_WATCHED 16
static struct { pid_t pid; int shown; char name[33], command[64]; } ui_watched[UI_WATCHED];
static int ui_watched_next, ui_watching;
static char ui_launch_cmd[64];		/* the next program's command, to start it again (ui_launch_will_run) */
/* the questions that may start a program again: nxask's process, and what it would start */
static struct { pid_t pid; char name[33], command[64]; } ui_again[4];
/* children reaped by the handler, for the program's loop to look at (ui_launch_tick) */
static volatile pid_t ui_reaped_pid[32];
static volatile int ui_reaped_status[32];
static volatile unsigned ui_reaped_in;
static unsigned ui_reaped_out;

static void ui_launch_again(const char *name, const char *command);
static void ui_launch_begin(const char *name, const struct ui_icon_ref *icon, GR_EVENT_MASK root_events);
static void ui_launch_started(pid_t pid);

static void
ui_children_reap(int sig)
{
	int status, saved = errno;
	pid_t pid;

	(void)sig;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		unsigned n = ui_reaped_in;

		if (n - ui_reaped_out < 32) {
			ui_reaped_pid[n & 31] = pid;
			ui_reaped_status[n & 31] = status;
			ui_reaped_in = n + 1;
		}
	}
	errno = saved;
}

/* In place of ignoring SIGCHLD: children are reaped as they end, and how each ended is known.
 * (An interrupted wait for events comes back as an event of type none.) */
static void
ui_launch_watch(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = ui_children_reap;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGCHLD, &sa, NULL);
	ui_watching = 1;
	/* what is started from here is watched: a program the GPU refuses leaves the question to this
	 * launcher, which can offer to close the holder (ui_launch_gpu), not a notice of its own (nxask.c) */
	setenv("UI_LAUNCHED", "1", 1);
}

/* The command the next ui_launch_begin's program is started by, words parted by spaces. */
static void
ui_launch_will_run(const char *command)
{
	snprintf(ui_launch_cmd, sizeof ui_launch_cmd, "%s", command);
}

/* Holds the handler off (1) while the program waits for a child of its own, and lets it go (0). */
static void
ui_children_hold(int hold)
{
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGCHLD);
	sigprocmask(hold ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
}

/* A window of nxask's that says what went wrong; the launcher waits for nothing. */
static void
ui_launch_report(const char *title, const char *text)
{
	if (getenv("UI_LAUNCH") && !strcmp(getenv("UI_LAUNCH"), "print"))
		fprintf(stderr, "launch: %s: %s\n", title, text);
	if (real_vfork() == 0) {
		signal(SIGCHLD, SIG_DFL);
		execlp("nxask", "nxask", "--icon", "error", "--title", title, text, "OK", (char *)NULL);
		_exit(127);
	}
}

/* The same, with a button that starts the command again; the answer is read as nxask ends. */
static void
ui_launch_report_again(const char *title, const char *text, const char *name, const char *command)
{
	unsigned k;
	pid_t pid;

	if (getenv("UI_LAUNCH") && !strcmp(getenv("UI_LAUNCH"), "print"))
		fprintf(stderr, "launch: %s: %s\n", title, text);
	for (k = 0; k < 4 && ui_again[k].pid; k++)
		;
	if (k == 4 || !command[0]) {
		ui_launch_report(title, text);
		return;
	}
	if ((pid = real_vfork()) == 0) {
		signal(SIGCHLD, SIG_DFL);
		execlp("nxask", "nxask", "--icon", "error", "--title", title, text, "Start again", "OK", (char *)NULL);
		_exit(127);
	}
	ui_again[k].pid = pid;
	snprintf(ui_again[k].name, sizeof ui_again[k].name, "%s", name);
	snprintf(ui_again[k].command, sizeof ui_again[k].command, "%s", command);
}

/* The process the GPU is drawing for, when another has it: 0 when nobody has (the kernel's claim,
 * given back at the close). */
static unsigned
ui_launch_gpu_holder(void)
{
	unsigned other = 0;
	int fd = open("/dev/gpu", O_RDWR | O_CLOEXEC), busy;

	if (fd < 0)
		return 0;
	busy = ioctl(fd, 0x80044705u, &other) < 0 && errno == EBUSY;	/* SEGL_DRAW_CLAIM */
	close(fd);
	return busy ? other : 0;
}

/* A program that ended with no window while the GPU draws for another: nxask --gpu offers what
 * can be done. True if it does. */
static int
ui_launch_gpu(int k)
{
	char text[64], *words[12];
	unsigned other;
	int n = 3;
	char number[16];

	if (!ui_watched[k].command[0] || !(other = ui_launch_gpu_holder()))
		return 0;
	snprintf(text, sizeof text, "%s", ui_watched[k].command);
	snprintf(number, sizeof number, "%u", other);
	words[0] = "nxask";
	words[1] = "--gpu";
	words[2] = number;
	for (words[n] = strtok(text, " "); words[n] && n < 10; words[n] = strtok(NULL, " "))
		n++;
	words[n] = NULL;
	if (getenv("UI_LAUNCH") && !strcmp(getenv("UI_LAUNCH"), "print"))
		fprintf(stderr, "launch: the GPU draws for process %u\n", other);
	if (real_vfork() == 0) {
		signal(SIGCHLD, SIG_DFL);
		execvp("nxask", words);
		_exit(127);
	}
	return 1;
}

/* What the report says of a program that could not become one: the exec's error. */
static void
ui_launch_exec_report(const char *name, int error)
{
	char text[200];

	if (error == ENOMEM)
		snprintf(text, sizeof text, "There is not enough memory to start %s.\nClose a program and try again.", name);
	else if (error == ENOENT)
		snprintf(text, sizeof text, "%s could not be started:\nits program was not found.", name);
	else if (error == EACCES)
		snprintf(text, sizeof text, "%s could not be started:\nits program may not be run.", name);
	else if (error == ENOEXEC)
		snprintf(text, sizeof text, "%s could not be started:\nits file is no program this machine runs.", name);
	else
		snprintf(text, sizeof text, "%s could not be started:\n%s.", name, strerror(error));
	ui_launch_report("Could not start", text);
}

/* A watched program has ended: reported when it never showed a window and did not end well,
 * or when a fault's signal ended it later. */
static void
ui_launch_ended(int k, int status)
{
	char text[200];
	int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;

	if (!ui_watched[k].shown && WIFEXITED(status) && WEXITSTATUS(status) == 1 && ui_launch_gpu(k))
		;
	else if (!ui_watched[k].shown && WIFEXITED(status) && WEXITSTATUS(status) != 0) {
		int code = WEXITSTATUS(status);

		snprintf(text, sizeof text, "%s stopped before its window opened.\n%s (status %d).", ui_watched[k].name,
			code == 127 ? "A program it needs was not found" : code == 126 ? "A program it needs could not be run" :
			"It said something went wrong", code);
		ui_launch_report("Could not start", text);
	} else if (sig && (!ui_watched[k].shown || sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE ||
		   sig == SIGABRT || sig == SIGSYS)) {
		snprintf(text, sizeof text, ui_watched[k].shown ? "%s stopped working:\n%s (signal %d).\nWhat was not saved in it is lost." :
			"%s stopped before its window opened:\n%s (signal %d).", ui_watched[k].name, strsignal(sig), sig);
		ui_launch_report_again(ui_watched[k].shown ? "Stopped working" : "Could not start", text, ui_watched[k].name, ui_watched[k].command);
	}
	ui_watched[k].pid = 0;
}

static unsigned
ui_launch_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)now.tv_sec * 1000u + (unsigned)(now.tv_nsec / 1000000);
}

/* The pointer with an hourglass beside it: '#' black, 'o' white. */
static GR_CURSOR_ID
ui_launch_cursor(void)
{
	static const char *art[23] = {
		"#",
		"##",
		"#o#",
		"#oo#",
		"#ooo#",
		"#oooo#",
		"#ooooo#",
		"#oooooo#",
		"#ooooooo#  ###########",
		"#oooooooo# #ooooooooo#",
		"#ooooooooo############",
		"#oooooo######ooooooo#",
		"#ooo#oo#    #o#####o#",
		"#oo##oo#     #o###o#",
		"#o#  #oo#     #o#o#",
		"##   #oo#      #o#",
		"#     #oo#    #ooo#",
		"      #oo#   #oo#oo#",
		"       ##   #oo###oo#",
		"            #o#####o#",
		"           ###########",
		"           #ooooooooo#",
		"           ###########",
	};
	GR_BITMAP image[23 * 2], mask[23 * 2];
	int r, c;

	memset(image, 0, sizeof image);
	memset(mask, 0, sizeof mask);
	for (r = 0; r < 23; r++)
		for (c = 0; art[r][c]; c++) {
			GR_BITMAP bit = (GR_BITMAP)(0x8000 >> (c & 15));

			if (art[r][c] == '#' || art[r][c] == 'o')
				mask[r * 2 + (c >> 4)] |= bit;
			if (art[r][c] == '#')
				image[r * 2 + (c >> 4)] |= bit;
		}
	return GrNewCursor(22, 23, 0, 0, WHITE, BLACK, image, mask);
}

static void
ui_launch_draw(void)
{
	unsigned s = (ui_launch_ms() - ui_launch_began) / 1000;
	char line[64];

	ui_launch_shown_s = s;
	GrSetGCForeground(ui_launch_gc, MWRGB(192, 192, 192));
	GrFillRect(ui_launch_note, ui_launch_gc, 0, 0, ui_launch_w, UI_LAUNCH_HIGH);
	GrSetGCForeground(ui_launch_gc, BLACK);
	GrRect(ui_launch_note, ui_launch_gc, 0, 0, ui_launch_w, UI_LAUNCH_HIGH);
	GrSetGCForeground(ui_launch_gc, MWRGB(128, 128, 128));
	GrLine(ui_launch_note, ui_launch_gc, 1, 1, ui_launch_w - 2, 1);
	GrLine(ui_launch_note, ui_launch_gc, 1, 1, 1, UI_LAUNCH_HIGH - 2);
	ui_icon_draw(ui_launch_note, ui_launch_gc, 6, 4, &ui_launch_icon, 32);
	GrSetGCForeground(ui_launch_gc, BLACK);
	GrText(ui_launch_note, ui_launch_gc, 44, 7, ui_launch_line, -1, GR_TFASCII | GR_TFTOP);
	snprintf(line, sizeof line, s ? "%u s" : "", s);
	GrSetGCForeground(ui_launch_gc, MWRGB(64, 64, 64));
	GrText(ui_launch_note, ui_launch_gc, 44, 22, line, -1, GR_TFASCII | GR_TFTOP);
	GrFlush();
}

static void
ui_launch_end(const char *why)
{
	const char *say = getenv("UI_LAUNCH");

	if (!ui_launch_pid)
		return;
	if (say && !strcmp(say, "print"))
		fprintf(stderr, "launch: %s after %u ms\n", why, ui_launch_ms() - ui_launch_began);
	if (!strncmp(why, "a window", 8)) {
		int k;

		for (k = 0; k < UI_WATCHED; k++)
			if (ui_watched[k].pid == ui_launch_pid)
				ui_watched[k].shown = 1;
	}
	ui_launch_pid = 0;
	if (!ui_launch_note)
		return;
	GrDestroyWindow(ui_launch_note);
	ui_launch_note = 0;
	GrSetWindowCursor(GR_ROOT_WINDOW_ID, 0);
	GrSelectEvents(GR_ROOT_WINDOW_ID, ui_launch_root);
	GrFlush();
}

/* A program is about to be started for a click: `name` and its icon by the pointer, before it
 * is (ui_launch_started), until it shows a window. `root_events`: the root's events this
 * program selects of its own. */
static void
ui_launch_begin(const char *name, const struct ui_icon_ref *icon, GR_EVENT_MASK root_events)
{
	const char *off = getenv("UI_LAUNCH");
	static GR_SCREEN_INFO si;
	GR_WINDOW_ID under;
	GR_COORD px, py, x, y;
	GR_BUTTON buttons;
	GR_SIZE tw, th, tb;

	ui_launch_end("another started");
	ui_launch_root_asked = root_events;
	snprintf(ui_launch_name, sizeof ui_launch_name, "%s", name);
	ui_exec_errno = 0;
	ui_launch_pid = -1;	/* (not known yet) */
	ui_launch_note = 0;
	if (off && !strcmp(off, "0"))
		return;		/* (no note: the program is still watched) */
	if (!ui_launch_gc) {
		ui_launch_gc = GrNewGC();
		GrSetGCUseBackground(ui_launch_gc, GR_FALSE);
		GrSetGCFont(ui_launch_gc, GrCreateFontEx(GR_FONT_SYSTEM_VAR, 0, 0, NULL));
		ui_launch_busy = ui_launch_cursor();
		GrGetScreenInfo(&si);
	}
	ui_launch_pid = -1;	/* (not known yet) */
	ui_launch_began = ui_launch_ms();
	if (off && !strcmp(off, "print"))
		fprintf(stderr, "launch: %s\n", name);
	ui_launch_root = root_events;
	ui_launch_icon = *icon;
	snprintf(ui_launch_line, sizeof ui_launch_line, "Starting %.32s", name);
	GrGetGCTextSize(ui_launch_gc, ui_launch_line, -1, GR_TFASCII, &tw, &th, &tb);
	ui_launch_w = 44 + tw + 12;
	GrQueryPointer(&under, &px, &py, &buttons);
	/* below and right of the pointer's point, on the screen and above the bar */
	x = px + 14 + ui_launch_w > si.cols ? si.cols - ui_launch_w : px + 14;
	y = py + 22 + UI_LAUNCH_HIGH > si.rows - 30 ? py - UI_LAUNCH_HIGH - 8 : py + 22;
	ui_launch_note = GrNewWindowEx(GR_WM_PROPS_NODECORATE | GR_WM_PROPS_NOMOVE | GR_WM_PROPS_NOAUTOMOVE |
		GR_WM_PROPS_NOFOCUS, "starting", GR_ROOT_WINDOW_ID, x < 0 ? 0 : x, y < 0 ? 0 : y,
		ui_launch_w, UI_LAUNCH_HIGH, MWRGB(192, 192, 192));
	GrSelectEvents(ui_launch_note, GR_EVENT_MASK_EXPOSURE);
	GrSelectEvents(GR_ROOT_WINDOW_ID, root_events | GR_EVENT_MASK_CHLD_UPDATE);
	GrSetWindowCursor(GR_ROOT_WINDOW_ID, ui_launch_busy);
	GrMapWindow(ui_launch_note);
	ui_launch_fresh = 1;
	ui_launch_draw();	/* (at once, not at its exposure: a pass sooner) */
}

/* The program's number once it is started, or 0 or less when it could not be. */
static void
ui_launch_started(pid_t pid)
{
	int error = pid > 0 ? ui_exec_errno : pid < 0 ? errno : 0, k;

	if (ui_launch_pid != -1)
		return;
	if (pid > 0 && !error) {
		ui_launch_pid = ui_launch_note ? pid : 0;	/* (UI_LAUNCH=0: no note to take away) */
		k = ui_watched_next++ % UI_WATCHED;
		ui_watched[k].pid = pid;
		ui_watched[k].shown = 0;
		snprintf(ui_watched[k].name, sizeof ui_watched[k].name, "%s", ui_launch_name);
		snprintf(ui_watched[k].command, sizeof ui_watched[k].command, "%s", ui_launch_cmd);
		ui_launch_cmd[0] = 0;
#ifndef UI_LAUNCH_IS_BAR
		{
			/* the bar shows what is starting (linux/nanox/nxbar.c) */
			GR_WM_PROPERTIES word;

			word.flags = GR_WM_FLAGS_COMMAND;
			word.props = GR_WMC_STARTING;
			word.background = pid;
			word.title = NULL;
			GrSetWMProperties(GR_ROOT_WINDOW_ID, &word);
		}
#endif
		return;
	}
	ui_launch_cmd[0] = 0;
	ui_launch_end("not started");
	if (error)
		ui_launch_exec_report(ui_launch_name, error);
}

/* Every event, before the program looks at it: true if it was the note's own. */
static int
ui_launch_event(const GR_EVENT *event)
{
	GR_WINDOW_INFO info;

	if (!ui_launch_pid || !ui_launch_note)
		return 0;
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->exposure.wid == ui_launch_note) {
		if (!ui_launch_fresh)
			ui_launch_draw();
		ui_launch_fresh = 0;
		return 1;
	}
	/* a window shown, or framed and made active (a framed window's map is not told the root's
	 * listeners), by the program started or one it started: a process numbered since */
	if (event->type == GR_EVENT_TYPE_CHLD_UPDATE && (event->update.utype == GR_UPDATE_MAP ||
	    event->update.utype == GR_UPDATE_ACTIVATE) && event->update.subwid != ui_launch_note) {
		GrGetWindowInfo(event->update.subwid, &info);
		if (info.processid >= ui_launch_pid)
			ui_launch_end("a window shown");
	}
	return 0;
}

/* Whether a window without a frame of the program started, or of one it started, is shown:
 * the root is told nothing of it. A program's own window numbers are bit 30, its process and a
 * count (nx_new_id in the client library). */
static int
ui_launch_bare_shown(void)
{
	GR_WINDOW_ID parent, *kids = NULL;
	GR_WINDOW_INFO info;
	GR_COUNT n = 0, i;
	int shown = 0;

	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &kids, &n);
	for (i = 0; i < n && !shown; i++) {
		if (!(kids[i] & 0x40000000) || (pid_t)((kids[i] >> 15) & 0x7fff) < ui_launch_pid || kids[i] == ui_launch_note)
			continue;
		GrGetWindowInfo(kids[i], &info);
		shown = info.mapped;
	}
	free(kids);
	return shown;
}

/* "Start again" was the answer: the command, words parted by spaces, with the note as at a click. */
static void
ui_launch_again(const char *name, const char *command)
{
	char text[64], *words[8];
	struct ui_icon_ref icon;
	int n = 0;
	pid_t child;

	snprintf(text, sizeof text, "%s", command);
	for (words[n] = strtok(text, " "); words[n] && n < 6; words[n] = strtok(NULL, " "))
		n++;
	words[n] = NULL;
	if (!n)
		return;
	ui_icon_of_command(command, &icon);
	ui_launch_will_run(command);
	ui_launch_begin(name, &icon, ui_launch_root_asked);
	if ((child = real_vfork()) == 0) {
		signal(SIGCHLD, SIG_DFL);
		execvp(words[0], words);
		ui_exec_failed();
	}
	ui_launch_started(child);
}

/* How long the program's event loop may wait now: a quarter second while a program starts. */
static int
ui_launch_wait(int otherwise)
{
	return ui_launch_pid ? 250 : otherwise;
}

/* Every wake: the seconds it has waited, or its end when a window without a frame is shown,
 * the program went, or 30 s did. */
static void
ui_launch_tick(void)
{
	unsigned now;

	/* the children the handler reaped: a watched one may be reported */
	while (ui_reaped_out != ui_reaped_in) {
		pid_t pid = ui_reaped_pid[ui_reaped_out & 31];
		int status = ui_reaped_status[ui_reaped_out & 31], k;

		ui_reaped_out++;
#ifdef UI_LAUNCH_REAPED
		UI_LAUNCH_REAPED(pid, status);
#endif
		for (k = 0; k < 4; k++)
			if (ui_again[k].pid == pid) {
				ui_again[k].pid = 0;
				if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
					ui_launch_again(ui_again[k].name, ui_again[k].command);
			}
		if (ui_launch_pid > 0 && pid == ui_launch_pid)
			ui_launch_end("the program gone");
		for (k = 0; k < UI_WATCHED; k++)
			if (ui_watched[k].pid == pid)
				ui_launch_ended(k, status);
	}
	if (ui_launch_pid <= 0)
		return;
	now = ui_launch_ms();
	if (now - ui_launch_looked < 250)
		return;
	ui_launch_looked = now;
	if (now - ui_launch_began > UI_LAUNCH_MOST)
		ui_launch_end("30 s");
	else if (!ui_watching && kill(ui_launch_pid, 0) < 0 && errno == ESRCH)
		ui_launch_end("the program gone");
	else if (ui_launch_pid > 0 && ui_launch_bare_shown())
		ui_launch_end("a window without a frame shown");
	else if ((now - ui_launch_began) / 1000 != ui_launch_shown_s)
		ui_launch_draw();
}
#endif
