/* nxfuzz: one plan of tools/app_fuzz.py run on the desktop, its operations given as arguments
 * (docs/system/fuzz.md). What it says goes to the console, a line "FZ ..." each, "FZ-BAD WHAT ..." for each
 * thing an oracle holds against the plan, and "FZ-DONE" at the end. It presses keys and the pointer through
 * the kernel's input devices (as nxkey does) and sees the desktop as the bar does.
 *
 *   start:TAG:COMMAND    run COMMAND (',' for a space) with sh; a window that comes is TAG's
 *   wait:TAG:SECONDS     until TAG has a window: BAD no-window if it has not
 *   close:TAG:HOW        x (the frame's button), alt (Alt+F4), bar (as the taskbar's Close), term, kill,
 *                        twice (x, x again at once); then until it has gone, answering what it asks
 *   tell:TAG:HOW         the same way of closing, not waited for (what it asks can then be answered by a click)
 *   gone:TAG             until it has gone, as close: does
 *   tell:TAG:HOW         the same way of closing, not waited for (what it asks can then be answered by a click)
 *   gone:TAG             until it has gone, as close: does
 *   front:TAG            a click on its caption
 *   key:TAG:K,K          keys, as nxkey's names
 *   click:TAG:X,Y        the left button at X, Y of its window; rclick the right one
 *   drag:TAG:X,Y,X,Y     the left button held between two places of its window
 *   rand:TAG:N:SEED      N random keys, clicks, drags and wheel turns over its window
 *   move:TAG:X,Y         its frame dragged by the caption to X, Y of the screen
 *   sleep:MS             guest's time
 *   check                the oracles now, with what is open allowed for
 *   end                  every tag closed (x, alt, then SIGKILL as each fails), and the oracles against the start
 *   wins                 the desktop's windows, for a person
 *   ask:SECONDS          how long close: waits for a program with its window gone, or one that asks (default 40, 12)
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "nano-X.h"

#define IS_SLEEP(op) (!strncmp(op, "sleep:", 6))
#define MAX_TAGS 16
#define MAX_PROCS 128
#define CXBORDER 3
#define CYCAPTION 22
#define CXCLOSEBOX 18

struct event { uint32_t sec, usec; uint16_t type, code; int32_t value; };

static struct tag {
	char name[16], command[200];
	int child;		/* the shell or program started */
	GR_WINDOW_ID frame, client;
	int pid;		/* its window's process */
	int closed;
} tags[MAX_TAGS];
static int tag_count;
static GR_WINDOW_ID known[64];	/* frames that were there before, or are a tag's */
static int known_count;
static int base_pids[MAX_PROCS], base_count;
static long base_free;
static int key_fd = -1, pointer_fd = -1;
static int bads;
static int gone_wait = 40, ask_wait = 12;
static unsigned rng_state = 1;
static const char *current = "";

static void
say(const char *format, ...)
{
	char line[512];
	va_list a;
	int n;
	static int console = -1;

	if (console < 0)
		console = open("/dev/console", O_WRONLY | O_NOCTTY);
	va_start(a, format);
	n = vsnprintf(line, sizeof line - 1, format, a);
	va_end(a);
	if (n > (int)sizeof line - 2)
		n = sizeof line - 2;
	line[n++] = '\n';
	if (write(console >= 0 ? console : 1, line, n) < 0)
		return;
}

#define BAD(...) (bads++, say("FZ-BAD " __VA_ARGS__))

static void
on_alarm(int s)
{
	(void)s;
	say("FZ-BAD hang in %s", current);
	say("FZ-DONE %d", bads + 1);
	_exit(3);
}

static void
ms_sleep(int ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };

	while (nanosleep(&t, &t) < 0 && errno == EINTR)
		;
}

static long
now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static unsigned
rnd(unsigned n)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return (rng_state >> 8) % (n ? n : 1);
}

/* ---- input ---- */

static void
point(int x, int y, int button, int down)
{
	struct event e[4] = { { 0, 0, 3, 0, x }, { 0, 0, 3, 1, y }, { 0, 0, 1, (uint16_t)button, down }, { 0, 0, 0, 0, 0 } };

	if (pointer_fd < 0)
		pointer_fd = open("/dev/input/event1", O_WRONLY);
	if (pointer_fd < 0 || write(pointer_fd, e, sizeof e) != sizeof e)
		say("FZ pointer: %s", strerror(errno));
	ms_sleep(60);
}

static void
click_at(int x, int y, int button)
{
	point(x, y, button, 1);
	point(x, y, button, 0);
}

static void
drag_at(int x0, int y0, int x1, int y1)
{
	int i;

	point(x0, y0, 0x110, 1);
	for (i = 1; i <= 8; i++)
		point(x0 + (x1 - x0) * i / 8, y0 + (y1 - y0) * i / 8, 0x110, 1);
	point(x1, y1, 0x110, 0);
}

static void
wheel_at(int x, int y, int notches)
{
	struct event e[4] = { { 0, 0, 3, 0, x }, { 0, 0, 3, 1, y }, { 0, 0, 2, 8, notches }, { 0, 0, 0, 0, 0 } };

	point(x, y, 0x110, 0);
	if (pointer_fd >= 0 && write(pointer_fd, e, sizeof e) != sizeof e)
		say("FZ wheel: %s", strerror(errno));
	ms_sleep(100);
}

static const struct { const char *name; int code; } names[] = {
	{ "escape", 1 }, { "backspace", 14 }, { "tab", 15 }, { "enter", 28 }, { "ctrl", 29 },
	{ "shift", 42 }, { "alt", 56 }, { "space", 57 }, { "f1", 59 }, { "f2", 60 }, { "f3", 61 },
	{ "f4", 62 }, { "f5", 63 }, { "f6", 64 }, { "f7", 65 }, { "f8", 66 }, { "f9", 67 },
	{ "f10", 68 }, { "home", 102 }, { "up", 103 }, { "pageup", 104 }, { "left", 105 },
	{ "right", 106 }, { "end", 107 }, { "down", 108 }, { "pagedown", 109 }, { "delete", 111 },
	{ "f11", 87 }, { "f12", 88 },
};
static const char *rows[] = { "1234567890-=", "qwertyuiop[]", "asdfghjkl;'`", "zxcvbnm,./" };
static const int row_code[] = { 2, 16, 30, 44 };

static int
code_of(const char *key)
{
	unsigned i;
	const char *at;

	for (i = 0; i < sizeof names / sizeof names[0]; i++)
		if (!strcmp(key, names[i].name))
			return names[i].code;
	for (i = 0; i < 4 && key[0] && !key[1]; i++)
		if ((at = strchr(rows[i], key[0])) != NULL)
			return row_code[i] + (int)(at - rows[i]);
	return 0;
}

static void
key_event(int code, int down)
{
	struct event e[2] = { { 0, 0, 1, (uint16_t)code, down }, { 0, 0, 0, 0, 0 } };

	if (key_fd < 0)
		key_fd = open("/dev/input/event0", O_WRONLY);
	if (key_fd < 0 || write(key_fd, e, sizeof e) != sizeof e)
		say("FZ keyboard: %s", strerror(errno));
	ms_sleep(20);
}

/* "alt+f4", "ctrl+shift+a", "x" */
static void
press(const char *spec)
{
	char text[64], *key = text, *plus;
	int held[4], n = 0, code;

	snprintf(text, sizeof text, "%s", spec);
	while ((plus = strchr(key, '+')) != NULL && plus[1] && n < 4) {
		*plus = 0;
		if ((code = code_of(key)) != 0)
			key_event(held[n++] = code, 1);
		key = plus + 1;
	}
	if ((code = code_of(key)) != 0) {
		key_event(code, 1);
		key_event(code, 0);
	} else
		say("FZ no key %s", key);
	while (n > 0)
		key_event(held[--n], 0);
}

/* ---- the desktop ---- */

/* The window where the program draws, its frame's first child; a window with no frame is no task's. */
static int
is_task(GR_WINDOW_ID w, GR_WINDOW_INFO *info)
{
	GrGetWindowInfo(w, info);
	/* (a program's own window id has this bit: at the root, it is one the window manager has not framed yet) */
	return info->wid == w && !(w & 0x40000000) && !(info->props & GR_WM_PROPS_NODECORATE);
}

static void
title_of(GR_WINDOW_ID w, char *to, int size)
{
	GR_WM_PROPERTIES p;

	p.title = NULL;
	GrGetWMProperties(w, &p);
	snprintf(to, size, "%s", p.title ? (char *)p.title : "");
	free(p.title);
}

static GR_WINDOW_ID
client_of(GR_WINDOW_ID frame)
{
	GR_WINDOW_ID parent, *inside = NULL, client;
	GR_COUNT m = 0;

	GrQueryTree(frame, &parent, &inside, &m);
	client = m > 0 ? inside[0] : frame;
	free(inside);
	return client;
}

static void
list_windows(void)
{
	GR_WINDOW_ID parent, *children = NULL;
	GR_COUNT n = 0, i;

	say("FZ focus %d", GrGetFocus());
	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &children, &n);
	for (i = 0; i < n; i++) {
		GR_WINDOW_INFO in;
		char title[64];

		if (!is_task(children[i], &in))
			continue;
		title_of(client_of(children[i]), title, sizeof title);
		say("FZ win %d client %d process %d %s at %d,%d %dx%d \"%s\"", children[i], client_of(children[i]), in.processid,
		    in.mapped ? "mapped" : "hidden", in.x, in.y, in.width, in.height, title);
	}
	free(children);
}

static int
is_known(GR_WINDOW_ID w)
{
	int i;

	for (i = 0; i < known_count; i++)
		if (known[i] == w)
			return 1;
	return 0;
}

static int
frame_alive(GR_WINDOW_ID frame)
{
	GR_WINDOW_INFO in;

	GrGetWindowInfo(frame, &in);
	return in.wid == frame;
}

/* What of the root's frames is new gets to be noted for `t`; only a mapped one with its client. */
static int
process_is(int pid, const char *name)
{
	char path[32], text[32];
	int fd, n;

	snprintf(path, sizeof path, "/proc/%d/comm", pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, text, sizeof text - 1);
	close(fd);
	if (n <= 0)
		return 0;
	text[n] = 0;
	text[strcspn(text, "\n")] = 0;
	return !strcmp(text, name);
}

static int
session_of(int pid)
{
	char path[32], text[256], *c;
	int fd, n, ppid, group, session = 0;
	char state;

	snprintf(path, sizeof path, "/proc/%d/stat", pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, text, sizeof text - 1);
	close(fd);
	if (n <= 0)
		return 0;
	text[n] = 0;
	c = strrchr(text, ')');
	if (c)
		sscanf(c + 1, " %c %d %d %d", &state, &ppid, &group, &session);
	return session;
}

static int
find_window(struct tag *t)
{
	GR_WINDOW_ID parent, *children = NULL;
	GR_COUNT n = 0, i;
	int found = 0;

	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &children, &n);
	for (i = 0; i < n && !found; i++) {
		GR_WINDOW_INFO in;

		if (is_known(children[i]) || !is_task(children[i], &in) || !in.mapped)
			continue;
		/* (the tag's program is in the session its start made: another's window, or a dialog of another's, is not its) */
		if (session_of(in.processid) != t->child && t->child > 0)
			continue;
		if (process_is(in.processid, "nxask")) {
			/* (what a program that could not start says, in a program of its own: not its window) */
			char title[64];

			static int said;

			title_of(client_of(children[i]), title, sizeof title);
			if (said != in.processid)
				say("FZ a question, process %d: \"%s\"", in.processid, title);
			said = in.processid;
			continue;
		}
		t->frame = children[i];
		t->client = client_of(children[i]);
		t->pid = in.processid;
		if (known_count < 64)
			known[known_count++] = children[i];
		found = 1;
	}
	free(children);
	return found;
}

static struct tag *
tag_named(const char *name, size_t length)
{
	int i;

	for (i = 0; i < tag_count; i++)
		if (strlen(tags[i].name) == length && !strncmp(tags[i].name, name, length))
			return &tags[i];
	return NULL;
}

static int
pid_alive(int pid)
{
	char path[32], text[256], *c;
	int fd, n;

	if (pid <= 0)
		return 0;
	snprintf(path, sizeof path, "/proc/%d/stat", pid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	n = read(fd, text, sizeof text - 1);
	close(fd);
	if (n <= 0)
		return 0;
	text[n] = 0;
	c = strrchr(text, ')');
	return c && c[1] && c[2] != 'Z';	/* (a zombie has gone, but for its parent's wait) */
}

static void
reap(void)
{
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

/* Every live process's pid, and its name in the line below. */
static int
processes(int *pids, char names_of[][24], int most)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	int n = 0;

	while (d && (e = readdir(d)) != NULL && n < most) {
		int pid = atoi(e->d_name);
		char path[32], text[128], *a, *b;
		int fd, got;

		if (pid <= 0)
			continue;
		snprintf(path, sizeof path, "/proc/%d/stat", pid);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		got = read(fd, text, sizeof text - 1);
		close(fd);
		if (got <= 0)
			continue;
		text[got] = 0;
		a = strchr(text, '(');
		b = strrchr(text, ')');
		if (!a || !b || (b[1] && b[2] == 'Z') || (b[1] && b[2] == 'X'))
			continue;
		*b = 0;
		pids[n] = pid;
		if (names_of)
			snprintf(names_of[n], 24, "%s", a + 1);
		n++;
	}
	if (d)
		closedir(d);
	return n;
}

static long
meminfo(const char *key)
{
	static char text[2048];
	int fd = open("/proc/meminfo", O_RDONLY), n;
	char *at;

	if (fd < 0)
		return -1;
	n = read(fd, text, sizeof text - 1);
	close(fd);
	if (n <= 0)
		return -1;
	text[n] = 0;
	at = strstr(text, key);
	return at ? atol(at + strlen(key) + 1) : -1;
}

static long
free_kb(void)
{
	return meminfo("MemFree") + meminfo("Cached") + meminfo("Buffers");
}

/* Lines of /proc/gpu: each open of the GPU by a process, and its workers. `allow`: a process other
 * than the baseline's may hold it (programs still open). */
static void
check_gpu(int allow)
{
	static char text[2048];
	int fd = open("/proc/gpu", O_RDONLY), n, i;
	char *line;

	if (fd < 0)
		return;
	n = read(fd, text, sizeof text - 1);
	close(fd);
	if (n <= 0)
		return;
	text[n] = 0;
	for (line = strchr(text, '\n'); line && line[1]; line = strchr(line + 1, '\n')) {
		unsigned pid, held, wanted, lent, tag, draws, lists;

		if (sscanf(line + 1, "%u %u %u %u %u %u %u", &pid, &held, &wanted, &lent, &tag, &draws, &lists) != 7)
			continue;
		if (!pid_alive((int)pid))
			BAD("gpu-of-dead process %u holds %u workers", pid, held);
		for (i = 0; !allow && i < base_count; i++)
			if (base_pids[i] == (int)pid)
				break;
		if (!allow && i == base_count)
			BAD("gpu-holder process %u (%u workers, draws %u) after everything closed", pid, held, draws);
	}
}

static void
print_gpu(void)
{
	static char text[2048];
	int fd = open("/proc/gpu", O_RDONLY), n;

	if (fd < 0)
		return;
	n = read(fd, text, sizeof text - 1);
	close(fd);
	if (n > 0) {
		char *line, *next;

		text[n] = 0;
		for (line = text; line && *line; line = next) {
			next = strchr(line, '\n');
			if (next)
				*next++ = 0;
			say("FZ gpu: %s", line);
		}
	}
}

/* A program run and waited for, through the kernel and the shell's start: the machine goes on. */
static void
canary(void)
{
	long from = now_ms();
	int child = vfork(), status = 0;

	if (child == 0) {
		execl("/bin/true", "true", (char *)0);
		_exit(9);
	}
	if (child < 0) {
		BAD("canary no process: %s", strerror(errno));
		return;
	}
	waitpid(child, &status, 0);
	if (status)
		BAD("canary exit %x", status);
	{
		GR_SCREEN_INFO s;

		GrGetScreenInfo(&s);	/* (the window system answers) */
	}
	if (now_ms() - from > 2000)
		say("FZ canary slow, %ld ms", now_ms() - from);
}

/* ---- closing ---- */

/* A program can make its window again (a film's size, a mode): the tag goes on with the window its process has now. */
static void
refresh(struct tag *t)
{
	GR_WINDOW_ID parent, *children = NULL;
	GR_COUNT n = 0, i;

	if (frame_alive(t->frame) || !t->pid || !pid_alive(t->pid))
		return;
	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &children, &n);
	for (i = 0; i < n; i++) {
		GR_WINDOW_INFO in;

		if (is_task(children[i], &in) && in.mapped && (int)in.processid == t->pid) {
			say("FZ %s has a new window", t->name);
			t->frame = children[i];
			t->client = client_of(children[i]);
			if (known_count < 64)
				known[known_count++] = children[i];
			break;
		}
	}
	free(children);
}

static void
caption_click(struct tag *t, int frame_x, int button)
{
	GR_WINDOW_INFO in;

	GrGetWindowInfo(t->frame, &in);
	click_at(in.x + frame_x, in.y + CXBORDER + CYCAPTION / 2, button);
}

static void
front(struct tag *t)
{
	GR_WINDOW_INFO in;

	GrGetWindowInfo(t->frame, &in);
	if (in.wid != t->frame)
		return;
	if (!in.mapped)
		GrMapWindow(t->frame);
	caption_click(t, in.width / 2, 0x110);
	ms_sleep(150);
	if (GrGetFocus() != t->client && GrGetFocus() != t->frame)
		say("FZ focus: %d, not %s's %d (frame %d)", GrGetFocus(), t->name, t->client, t->frame);
}

static void
close_button(struct tag *t)
{
	GR_WINDOW_INFO in;

	GrGetWindowInfo(t->frame, &in);
	click_at(in.x + in.width - CXBORDER - 2 - CXCLOSEBOX / 2, in.y + CXBORDER + CYCAPTION / 2, 0x110);
}

static void
do_close(struct tag *t, const char *how)
{
	refresh(t);
	if (!strcmp(how, "x"))
		close_button(t);
	else if (!strcmp(how, "twice")) {
		close_button(t);
		close_button(t);
	} else if (!strcmp(how, "alt")) {
		front(t);
		press("alt+f4");
	} else if (!strcmp(how, "bar"))
		GrCloseWindow(t->client);
	else if (!strcmp(how, "term"))
		kill(t->pid, SIGTERM);
	else if (!strcmp(how, "kill"))
		kill(t->pid, SIGKILL);
	else
		say("FZ no way to close: %s", how);
	GrFlush();
}

/* Other windows of the same process, that came after its own: it asks something. */
static GR_WINDOW_ID
question_of(struct tag *t, int *x, int *y, int *w, int *h)
{
	GR_WINDOW_ID parent, *children = NULL, found = 0;
	GR_COUNT n = 0, i;

	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &children, &n);
	for (i = 0; i < n && !found; i++) {
		GR_WINDOW_INFO in;

		if (children[i] != t->frame && is_task(children[i], &in) && in.mapped && !is_known(children[i])) {
			found = children[i];
			*x = in.x, *y = in.y, *w = in.width, *h = in.height;
		}
	}
	free(children);
	return found;
}

/* Until the program of `t` has gone. A window that stays is a program waiting for an answer, which
 * is no fault: it is given Enter, then the next way of closing; a process with its window gone that
 * goes on is one (the OpenTTD report that started this). */
static void
wait_gone(struct tag *t, const char *how)
{
	long from = now_ms(), asked_at = 0;
	int answers = 0, x, y, w, h;
	long window_gone_at = 0;

	for (;;) {
		int alive, shown;

		reap();
		refresh(t);
		alive = t->pid > 0 && pid_alive(t->pid);
		shown = frame_alive(t->frame);
		if (!alive && !shown) {
			say("FZ closed %s (%s) in %ld ms%s", t->name, how, now_ms() - from, answers ? ", answered" : "");
			t->closed = 1;
			return;
		}
		if (!alive && shown && now_ms() - from > 3000) {
			BAD("window-of-dead %s: process %d has gone, its window has not", t->name, t->pid);
			t->closed = 1;
			return;
		}
		if (alive && !shown) {
			if (!window_gone_at)
				window_gone_at = now_ms();
			if (now_ms() - window_gone_at > gone_wait * 1000) {
				say("FZ stuck: %s", t->name);
				BAD("left-running %s: process %d goes on %d s after its window went (%s)", t->name, t->pid, gone_wait, how);
				print_gpu();
				t->closed = 2;
				return;
			}
		}
		if (alive && shown && now_ms() - (asked_at ? asked_at : from) > ask_wait * 1000) {
			/* it has not gone for this way: what it asks is answered */
			asked_at = now_ms();
			answers++;
			if (question_of(t, &x, &y, &w, &h))
				say("FZ %s asks in a window of %dx%d", t->name, w, h);
			if (answers == 1) {
				front(t);
				say("FZ %s stays after %s: Enter", t->name, how);
				press("enter");
			} else if (answers == 2) {
				say("FZ %s still there: Alt+F4 and Enter", t->name);
				front(t);
				press("alt+f4");
				ms_sleep(500);
				press("enter");
			} else if (answers == 3) {
				say("FZ %s still there: SIGTERM", t->name);
				kill(t->pid, SIGTERM);
			} else {
				BAD("no-close %s by %s: SIGTERM did nothing either", t->name, how);
				kill(t->pid, SIGKILL);
				t->closed = 2;
				return;
			}
		}
		ms_sleep(100);
	}
}

static void
start_tag(const char *name, size_t length, const char *command)
{
	struct tag *t = &tags[tag_count];
	char cmd[sizeof t->command];
	int i, child;

	if (tag_count >= MAX_TAGS)
		return;
	tag_count++;
	memset(t, 0, sizeof *t);
	snprintf(t->name, sizeof t->name, "%.*s", (int)length, name);
	snprintf(cmd, sizeof cmd, "%s", command);
	for (i = 0; cmd[i]; i++)
		if (cmd[i] == ',')
			cmd[i] = ' ';
	snprintf(t->command, sizeof t->command, "%s", cmd);
	child = vfork();
	if (child == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", t->command, (char *)0);
		_exit(127);
	}
	t->child = child;
	say("FZ started %s: process %d: %s", t->name, child, t->command);
}

static int
wait_window(struct tag *t, int seconds)
{
	long until = now_ms() + seconds * 1000L;

	while (now_ms() < until) {
		if (find_window(t)) {
			char title[64];

			title_of(t->client, title, sizeof title);
			say("FZ window %s: process %d \"%s\"", t->name, t->pid, title);
			return 1;
		}
		reap();
		ms_sleep(100);
	}
	return 0;
}

static void
window_area(struct tag *t, int *x, int *y, int *w, int *h)
{
	GR_WINDOW_INFO in, fr;

	GrGetWindowInfo(t->frame, &fr);
	GrGetWindowInfo(t->client, &in);
	*x = fr.x + in.x;
	*y = fr.y + in.y;
	*w = in.width;
	*h = in.height;
}

static void
random_use(struct tag *t, int count, unsigned seed)
{
	/* (keys that play and do not leave: no Alt, no Escape's quitting is held against it) */
	static const char *keys[] = { "up", "down", "left", "right", "enter", "tab", "space", "a", "s", "d", "w", "x", "1", "2",
		"pageup", "pagedown", "home", "end", "backspace", "delete", "ctrl+a", "ctrl+s", "ctrl+o", "ctrl+z", "f1", "f2", "f5", "escape" };
	int i;

	rng_state = seed * 2654435761u + 7;
	for (i = 0; i < count && (refresh(t), frame_alive(t->frame)); i++) {
		int x, y, w, h, kind;

		window_area(t, &x, &y, &w, &h);
		if (w < 4 || h < 4)
			return;
		/* (each action said, to tell which of a plan's did it) */
		kind = rnd(8);
		if (kind <= 2) {
			const char *k = keys[rnd(sizeof keys / sizeof keys[0])];

			say("FZ r key %s", k);
			press(k);
		} else if (kind <= 5) {
			int ay = y + rnd(h), ax = x + rnd(w);

			say("FZ r %s %d,%d", kind == 5 ? "rclick" : "click", ax - x, ay - y);
			click_at(ax, ay, kind == 5 ? 0x111 : 0x110);
		} else if (kind == 6) {
			int by = y + rnd(h), bx = x + rnd(w), ay = y + rnd(h), ax = x + rnd(w);

			say("FZ r drag %d,%d %d,%d", ax - x, ay - y, bx - x, by - y);
			drag_at(ax, ay, bx, by);
		} else {
			int n = rnd(2) ? 1 : -1, ay = y + rnd(h), ax = x + rnd(w);

			say("FZ r wheel %d,%d", ax - x, ay - y);
			wheel_at(ax, ay, n);
		}
		ms_sleep(30 + rnd(200));
	}
}

static void
check(int final)
{
	static int pids[MAX_PROCS];
	static char pnames[MAX_PROCS][24];
	int n, i, j;

	canary();
	if (!final) {
		check_gpu(1);
		return;
	}
	n = processes(pids, pnames, MAX_PROCS);
	for (i = 0; i < n; i++) {
		for (j = 0; j < base_count && base_pids[j] != pids[i]; j++)
			;
		/* (a question nobody here has answered is no program left behind) */
		if (j == base_count && !strcmp(pnames[i], "nxask")) {
			kill(pids[i], SIGKILL);
			continue;
		}
		/* (`open`, started for a link, waits for a host that rvc_cpu has not: 40 s) */
		if (j == base_count && !strcmp(pnames[i], "open")) {
			say("FZ open goes on, killed");
			kill(pids[i], SIGKILL);
			continue;
		}
		if (j == base_count && pids[i] != getpid() && !strchr(pnames[i], '/'))	/* (a kernel worker has a slash) */
			BAD("left-process %d %s after everything closed", pids[i], pnames[i]);
	}
	ms_sleep(200);
	reap();
	check_gpu(0);
	{
		long now = free_kb();

		say("FZ memory: free+cached %ld KB at the start, %ld KB now", base_free, now);
	}
}

/* ---- a plan ---- */

static const char *
after(const char *s, int colons)
{
	while (colons-- > 0 && s)
		if ((s = strchr(s, ':')) != NULL)
			s++;
	return s;
}

int
main(int argc, char **argv)
{
	int i;

	signal(SIGALRM, on_alarm);
	signal(SIGCHLD, SIG_DFL);
	{
		/* (random keys in a terminal can bring this line back from the shell's history: one plan at a time) */
		static int pids[MAX_PROCS];
		static char pnames[MAX_PROCS][24];
		int n = processes(pids, pnames, MAX_PROCS);

		for (i = 0; i < n; i++)
			if (pids[i] != getpid() && !strcmp(pnames[i], "nxfuzz"))
				return 0;
	}
	if (GrOpen() < 0) {
		say("FZ-BAD no window system");
		say("FZ-DONE 1");
		return 2;
	}
	/* what is there is the baseline */
	{
		GR_WINDOW_ID parent, *children = NULL;
		GR_COUNT n = 0, k;

		GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &children, &n);
		for (k = 0; k < n && known_count < 64; k++)
			known[known_count++] = children[k];
		free(children);
	}
	base_count = processes(base_pids, NULL, MAX_PROCS);
	base_free = free_kb();
	for (i = 1; i < argc; i++) {
		const char *op = argv[i], *colon = strchr(op, ':'), *b = colon ? colon + 1 : "", *c = strchr(b, ':');
		size_t oplen = colon ? (size_t)(colon - op) : strlen(op);
		struct tag *t = colon ? tag_named(b, c ? (size_t)(c - b) : strlen(b)) : NULL;
		const char *rest = c ? c + 1 : "";

		current = op;
		alarm(240 + (IS_SLEEP(op) ? atoi(b) / 1000 : 0));	/* (the guest's seconds: what no operation should take) */
		say("FZ op %s", op);
#define IS(word) (oplen == sizeof word - 1 && !strncmp(op, word, oplen))
		if (IS("start"))
			start_tag(b, c ? (size_t)(c - b) : 0, rest);
		else if (IS("sleep"))
			ms_sleep(atoi(b));
		else if (IS("ask")) {
			gone_wait = atoi(b);
			if (c)
				ask_wait = atoi(rest);
		} else if (IS("wins"))
			list_windows();
		else if (IS("check"))
			check(0);
		else if (IS("end")) {
			int k;

			for (k = 0; k < tag_count; k++) {
				struct tag *e = &tags[k];

				if (e->closed || !e->frame)
					continue;
				if (!pid_alive(e->pid) && !frame_alive(e->frame))
					continue;
				do_close(e, "x");
				wait_gone(e, "x at the end");
			}
			/* what nobody here started: a question (nxask), or a program started from the desktop by random keys */
			{
				GR_WINDOW_ID parent, *children = NULL;
				GR_COUNT n = 0, w;
				static int strays[32];
				int stray_count = 0;

				GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &children, &n);
				for (w = 0; w < n; w++) {
					GR_WINDOW_INFO in;

					if (is_known(children[w]) || !is_task(children[w], &in))
						continue;
					say("FZ a window of nobody's: process %d: closed", in.processid);
					GrCloseWindow(client_of(children[w]));
					if (stray_count < 32)
						strays[stray_count++] = in.processid;
				}
				free(children);
				GrFlush();
				ms_sleep(2000);
				while (stray_count > 0)
					if (pid_alive(strays[--stray_count]))
						kill(strays[stray_count], SIGKILL);
			}
			/* a program with no window yet: its process's group */
			for (k = 0; k < tag_count; k++) {
				if (tags[k].child > 0 && !tags[k].frame && pid_alive(tags[k].child)) {
					say("FZ %s never had a window: killed", tags[k].name);
					kill(-tags[k].child, SIGTERM);
				}
			}
			ms_sleep(1500);
			reap();
			check(1);
		} else if (!t && colon)
			say("FZ no tag in %s", op);
		else if (IS("wait")) {
			if (!wait_window(t, atoi(rest)))
				BAD("no-window %s in %s s", t->name, rest);
		} else if (IS("close")) {
			if (!t->frame) {
				if (!wait_window(t, 2)) {
					say("FZ %s closed before its window: SIGTERM", t->name);
					kill(-t->child, SIGTERM);
					continue;
				}
			}
			do_close(t, rest);
			wait_gone(t, rest);
		} else if (IS("tell"))
			do_close(t, rest);
		else if (IS("gone"))
			wait_gone(t, "gone");
		else if (IS("front"))
			front(t);
		else if (IS("key")) {
			char k[128], *p, *save = NULL;

			snprintf(k, sizeof k, "%s", rest);
			for (p = strtok_r(k, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
				press(p);
				ms_sleep(60);
			}
		} else if (IS("click") || IS("rclick") || IS("drag")) {
			int a[4] = { 0 }, x, y, w, h;

			sscanf(rest, "%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3]);
			window_area(t, &x, &y, &w, &h);
			if (IS("drag"))
				drag_at(x + a[0], y + a[1], x + a[2], y + a[3]);
			else
				click_at(x + a[0], y + a[1], IS("click") ? 0x110 : 0x111);
		} else if (IS("rand")) {
			int count = 0, seed = 0;

			sscanf(rest, "%d:%d", &count, &seed);
			random_use(t, count, (unsigned)seed);
		} else if (IS("move")) {
			int a[2] = { 0 };
			GR_WINDOW_INFO in;

			sscanf(rest, "%d,%d", &a[0], &a[1]);
			GrGetWindowInfo(t->frame, &in);
			drag_at(in.x + in.width / 2, in.y + CXBORDER + CYCAPTION / 2, a[0] + in.width / 2, a[1] + CXBORDER + CYCAPTION / 2);
		} else
			say("FZ no operation %s", op);
		alarm(0);
		GrFlush();
		reap();
	}
	(void)after;
	say("FZ-DONE %d", bads);
	GrClose();
	return bads != 0;
}
