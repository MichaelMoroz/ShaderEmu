/*
 * nxask: a question for a script, in a window (ui_ask.h).
 *
 *	nxask [--icon NAME] [--title TITLE] TEXT [ANSWER ...]
 *				exits with the answer's number, the first 0; 255 for none. NAME is an
 *				icon's (error, warning, information: linux/apps/icons.txt)
 *	nxask --closing		asks every program's windows to close, and waits for them
 *	nxask --gpu PID CMD...	CMD was refused the GPU, which draws for process PID: brings that one
 *				forward when it is the same program, or offers to close it and start CMD
 */
#include <fcntl.h>
#include <signal.h>
#include "ui.h"
#include "ui_ask.h"

/* How many programs still have windows (not this one, not the bar or a menu); with `close`
 * each window is asked to go, as its caption's box asks. */
static int
windows(int close)
{
	GR_WINDOW_ID parent, *children = NULL, *inside;
	GR_COUNT n = 0, m, i;
	GR_WINDOW_INFO info;
	int count = 0, seen[32], k;

	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &children, &n);
	for (i = 0; i < n; i++) {
		GrGetWindowInfo(children[i], &info);
		if (info.props & GR_WM_PROPS_NODECORATE)
			continue;
		/* a frame's first window is the program's */
		inside = NULL, m = 0;
		GrQueryTree(children[i], &parent, &inside, &m);
		if (m > 0)
			GrGetWindowInfo(inside[0], &info);
		if (m > 0 && info.processid != getpid()) {
			for (k = 0; k < count && seen[k] != info.processid; k++)
				continue;
			if (k == count && count < 32)
				seen[count++] = info.processid;
			if (close)
				GrCloseWindow(inside[0]);
		}
		free(inside);
	}
	free(children);
	return count;
}

static int
all_closed(void)
{
	return windows(0) == 0;
}

/* Asks the programs to close and waits: one with changes asks about them first. True when
 * all have gone, or the answer was not to wait for the rest. */
static int
closing(void)
{
	static const char *const answers[] = { "Don't wait", "Cancel" };
	char text[160];
	int waited, left = windows(1), answer;

	for (waited = 0; left && waited < 6; waited++) {
		usleep(250000);
		left = windows(0);
	}
	if (!left)
		return 1;
	snprintf(text, sizeof text, "%d program%s still open: one may be asking about its changes.\nWhat is not saved will be lost.",
		 left, left == 1 ? " is" : "s are");
	ui_ask_over = all_closed;
	ui_ask_aside = 1;	/* (the program that asks about its changes has the keyboard) */
	answer = ui_ask("Closing the programs", text, answers, 2, NULL);
	return answer == 0 || answer == -2;
}

/* The name the process goes by; true if it is there and not a zombie. */
static int
process_named(int pid, char *name, int room)
{
	char path[40], text[128], *open_at, *close_at;
	int fd, n;

	name[0] = 0;
	snprintf(path, sizeof path, "/proc/%d/stat", pid);
	if ((fd = open(path, O_RDONLY)) < 0)
		return 0;
	n = (int)read(fd, text, sizeof text - 1);
	close(fd);
	text[n > 0 ? n : 0] = 0;
	if (!(open_at = strchr(text, '(')) || !(close_at = strrchr(text, ')')))
		return 0;
	*close_at = 0;
	snprintf(name, room, "%s", open_at + 1);
	return close_at[2] != 'Z';
}

/* Every framed window of a process: closed (1) or one brought forward (0). How many it has. */
static int
windows_of(int pid, int closing)
{
	GR_WINDOW_ID parent, *kids = NULL, *inner;
	GR_COUNT n = 0, m, i;
	int found = 0;

	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &kids, &n);
	for (i = 0; i < n; i++) {
		GR_WINDOW_INFO info;

		GrGetWindowInfo(kids[i], &info);
		if (info.processid != pid || (info.props & GR_WM_PROPS_NODECORATE))
			continue;
		if (closing) {
			inner = NULL;
			m = 0;
			GrQueryTree(kids[i], &parent, &inner, &m);
			if (m)
				GrCloseWindow(inner[0]);
			free(inner);
		} else if (!found) {
			GR_WM_PROPERTIES word;

			word.flags = GR_WM_FLAGS_COMMAND;
			word.props = GR_WMC_ACTIVATE;
			word.background = 0;
			word.title = NULL;
			GrSetWMProperties(kids[i], &word);
		}
		found++;
	}
	free(kids);
	GrFlush();
	return found;
}

/* The GPU draws for `other`, and argv would have it. */
static int
gpu_taken(int other, char **argv)
{
	char holder[32] = "another program", wanted[64], text[200], close_it[96], forward[64];
	const char *slash = strrchr(argv[0], '/');
	const char *answers[3];
	int i, answer;

	snprintf(wanted, sizeof wanted, "%s", slash ? slash + 1 : argv[0]);
	if (!process_named(other, holder, sizeof holder))
		answer = 0;		/* (gone meanwhile: nothing to ask) */
	else if (!strncmp(holder, wanted, 15)) {
		/* the program is running: that one is brought forward, as a second click does elsewhere */
		windows_of(other, 0);
		printf("nxask: %s is running: brought forward\n", holder);
		fflush(stdout);
		return 0;
	} else {
		snprintf(text, sizeof text, "The GPU is drawing for %s (process %d).\nOne program at a time draws with it.", holder, other);
		snprintf(close_it, sizeof close_it, "Close %s, start %s", holder, wanted);
		snprintf(forward, sizeof forward, "Go to %s", holder);
		answers[0] = close_it;
		answers[1] = forward;
		answers[2] = "Cancel";
		ui_ask_icon = "warning";
		answer = ui_ask("The GPU is in use", text, answers, 3, NULL);
	}
	if (answer == 1)
		windows_of(other, 0);
	if (answer != 0)
		return 0;
	windows_of(other, 1);
	/* its answer: what it asks about a text not saved takes its time, a program that does not answer is ended */
	for (i = 0; i < 300 && process_named(other, holder, sizeof holder); i++) {
		usleep(100000);
		if (i == 80)
			kill(other, SIGTERM);
		if (i == 120)
			kill(other, SIGKILL);
	}
	GrClose();
	execvp(argv[0], argv);
	fprintf(stderr, "nxask: %s: cannot be started\n", argv[0]);
	return 1;
}

int
main(int argc, char **argv)
{
	char text[400];
	const char *title = NULL;
	int i, o = 0, answer;

	/* What this program's starter had open is not this program's. Its window system's socket, kept, held the window of a
	 * game that had gone until this ended (a program refused the GPU, and its Notice; a link: docs/system/fuzz.md). */
	for (i = 3; i < 64; i++)
		close(i);
	while (argc > 3 && (!strcmp(argv[1], "--icon") || !strcmp(argv[1], "--title"))) {
		if (argv[1][2] == 'i')
			ui_ask_icon = argv[2];
		else
			title = argv[2];
		argc -= 2, argv += 2;
	}
	if (argc < 2) {
		fprintf(stderr, "usage: nxask [--icon NAME] [--title TITLE] TEXT [ANSWER ...] | nxask --closing\n");
		return 2;
	}
	if (GrOpen() < 0)
		return 255;
	ui_init();
	if (!strcmp(argv[1], "--gpu") && argc > 3)
		return gpu_taken(atoi(argv[2]), argv + 3);
	if (!strcmp(argv[1], "--closing")) {
		answer = closing() ? 0 : 1;
		GrClose();
		return answer;
	}
	/* a program the GPU refused, started by a launcher: the launcher asks (ui_launch_gpu) */
	if (getenv("UI_LAUNCHED") && !strncmp(argv[1], "The GPU is drawing for", 22))
		return 0;
	/* (a script writes a new line as \n) */
	for (i = 0; argv[1][i] && o < (int)sizeof text - 1; i++)
		if (argv[1][i] == '\\' && argv[1][i + 1] == 'n')
			text[o++] = '\n', i++;
		else
			text[o++] = argv[1][i];
	text[o] = 0;
	if (argc > 2)
		answer = ui_ask(title ? title : "Question", text, (const char *const *)argv + 2, argc - 2, NULL);
	else {
		static const char *const ok[] = { "OK" };

		answer = ui_ask(title ? title : "Notice", text, ok, 1, NULL);
	}
	GrClose();
	return answer < 0 ? 255 : answer;
}
