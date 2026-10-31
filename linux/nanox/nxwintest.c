/*
 * nxwintest: what the window manager and the bar are tested with (tools/test_all.py, the wm-
 * cases; docs/system/nanox.md, "The window manager and the bar").
 *
 *	nxwintest list			every framed window, a line: title, place, size and state
 *	nxwintest plain [TITLE]		a window that closes when it is asked to
 *	nxwintest two [TITLE]		two windows of one process, the program ends with the last
 *	nxwintest ignore [TITLE]	takes its events, and does nothing when asked to close
 *	nxwintest hang [TITLE]		takes no event after its window is shown
 *	nxwintest linger [TITLE]	closes its window when asked and goes on running
 *	nxwintest crash [TITLE]		its window, then SIGSEGV 3 s later
 *	nxwintest slow SECONDS		no window for that long, then a plain one
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define MWINCLUDECOLORS
#include "nano-X.h"

static GR_GC_ID gc;

static GR_WINDOW_ID
open_window(const char *title, int x)
{
	GR_WINDOW_ID w = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, title, GR_ROOT_WINDOW_ID, x, 40, 240, 120, MWRGB(230, 236, 244));

	GrSelectEvents(w, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_CLOSE_REQ | GR_EVENT_MASK_UPDATE);
	GrMapWindow(w);
	return w;
}

static void
draw(GR_WINDOW_ID w, const char *title)
{
	GR_WINDOW_INFO info;

	GrGetWindowInfo(w, &info);
	GrSetGCForeground(gc, MWRGB(40, 90, 160));
	GrRect(w, gc, 0, 0, info.width, info.height);
	GrLine(w, gc, 0, 0, info.width - 1, info.height - 1);
	GrSetGCForeground(gc, BLACK);
	GrText(w, gc, 8, 8, (void *)title, -1, GR_TFASCII | GR_TFTOP);
}

static int
list(void)
{
	GR_WINDOW_ID parent, *kids = NULL, *inner;
	GR_COUNT n = 0, m, i;

	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &kids, &n);
	/* (front first) */
	for (i = 0; i < n; i++) {
		GR_WINDOW_INFO info;
		GR_WM_PROPERTIES props;

		GrGetWindowInfo(kids[i], &info);
		if (info.props & GR_WM_PROPS_NODECORATE)
			continue;
		inner = NULL;
		m = 0;
		GrQueryTree(kids[i], &parent, &inner, &m);
		props.title = NULL;
		GrGetWMProperties(m ? inner[0] : kids[i], &props);
		printf("win %d %s %d,%d %dx%d%s%s%s%s%s%s desk %d\n", (int)i, props.title ? (char *)props.title : "?", info.x, info.y, info.width,
			info.height, info.mapped ? " mapped" : " unmapped", info.props & GR_WM_PROPS_MAXED ? " maximised" : "",
			info.props & GR_WM_PROPS_TILED ? " tiled" : "", info.props & GR_WM_PROPS_SHADED ? " shaded" : "",
			info.props & GR_WM_PROPS_ONTOP ? " ontop" : "", GrGetFocus() == (m ? inner[0] : kids[i]) ? " focus" : "",
			(int)((info.props & GR_WM_PROPS_DESK) >> GR_WM_PROPS_DESK_SHIFT));
		free(props.title);
		free(inner);
	}
	free(kids);
	printf("wins %d\n", (int)n);
	fflush(stdout);
	return 0;
}

int
main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "plain", *title = argc > 2 ? argv[2] : mode;
	GR_WINDOW_ID one, other = 0;
	GR_EVENT event;
	int open_count = 1;

	if (!strcmp(mode, "slow")) {
		sleep(atoi(title));
		title = "slow";
	}
	if (GrOpen() < 0)
		return 1;
	if (!strcmp(mode, "list"))
		return list();
	gc = GrNewGC();
	GrSetGCUseBackground(gc, GR_FALSE);
	one = open_window(title, 100);
	if (!strcmp(mode, "two")) {
		other = open_window("second", 200);
		open_count = 2;
	}
	GrFlush();
	if (!strcmp(mode, "hang")) {
		/* its window framed and drawn; nothing is taken from here on */
		while (GrGetNextEventTimeout(&event, 500), event.type != GR_EVENT_TYPE_TIMEOUT)
			if (event.type == GR_EVENT_TYPE_EXPOSURE)
				draw(event.exposure.wid, title);
		GrFlush();
		for (;;)
			sleep(1000);
	}
	for (;;) {
		GrGetNextEventTimeout(&event, !strcmp(mode, "crash") ? 3000 : 100000);
		if (event.type == GR_EVENT_TYPE_TIMEOUT && !strcmp(mode, "crash"))
			raise(SIGSEGV);	/* (without an MMU a store to a low address need not fault) */
		if (event.type == GR_EVENT_TYPE_EXPOSURE)
			draw(event.exposure.wid, event.exposure.wid == one ? title : "second");
		if (event.type == GR_EVENT_TYPE_UPDATE && event.update.utype == GR_UPDATE_SIZE && event.update.wid == one)
			draw(one, title);
		if (event.type != GR_EVENT_TYPE_CLOSE_REQ || !strcmp(mode, "ignore"))
			continue;
		GrDestroyWindow(event.general.wid);
		GrFlush();
		if (--open_count > 0)
			continue;
		if (!strcmp(mode, "linger")) {
			/* gone from the screen, and still here */
			for (;;)
				sleep(1000);
		}
		break;
	}
	GrClose();
	return 0;
}
