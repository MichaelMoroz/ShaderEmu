/* ui_single.h: a program of which one window is enough (Settings, the Monitor). Called after
 * GrOpen and before its window: when a window of the same program is up, that one comes forward,
 * on whichever desktop it is, and this one ends. Costs a question for each window on the screen. */
#ifndef UI_SINGLE_H
#define UI_SINGLE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
ui_single_instance(void)
{
	GR_WINDOW_ID parent, *kids = NULL;
	GR_COUNT n = 0, i;
	char own[96], other[96], path[40];
	int length = (int)readlink("/proc/self/exe", own, sizeof own - 1), found = 0;

	if (length <= 0)
		return 0;
	own[length] = 0;
	GrQueryTree(GR_ROOT_WINDOW_ID, &parent, &kids, &n);
	for (i = 0; i < n && !found; i++) {
		GR_WINDOW_INFO info;
		int m;

		GrGetWindowInfo(kids[i], &info);
		if ((info.props & GR_WM_PROPS_NODECORATE) || info.processid == getpid())
			continue;
		snprintf(path, sizeof path, "/proc/%d/exe", info.processid);
		m = (int)readlink(path, other, sizeof other - 1);
		if (m == length && !memcmp(own, other, length)) {
			GR_WM_PROPERTIES word;

			word.flags = GR_WM_FLAGS_COMMAND;
			word.props = GR_WMC_ACTIVATE;
			word.background = 0;
			word.title = NULL;
			GrSetWMProperties(kids[i], &word);
			GrFlush();
			found = 1;
		}
	}
	free(kids);
	return found;
}
#endif
