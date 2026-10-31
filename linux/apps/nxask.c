/*
 * nxask: a question for a script, in a window (ui_ask.h).
 *
 *	nxask [--icon NAME] [--title TITLE] TEXT [ANSWER ...]
 *				exits with the answer's number, the first 0; 255 for none. NAME is an
 *				icon's (error, warning, information: linux/apps/icons.txt)
 *	nxask --closing		asks every program's windows to close, and waits for them
 */
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
	if (!strcmp(argv[1], "--closing")) {
		answer = closing() ? 0 : 1;
		GrClose();
		return answer;
	}
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
