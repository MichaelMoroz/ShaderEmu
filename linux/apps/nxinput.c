/*
 * nxinput: what the machine's input comes to, three ways at once, for a test (docs/machine/input.md,
 * "More of a mouse and the hands"). A window over the whole screen that asks for the extra buttons
 * (GR_WMC_BUTTONS) prints each button event Nano-X gives it, the pointer's evdev device prints
 * what the kernel reports, and the buttons word prints when it changes:
 *
 *   nx down 0x100 at 12,40         a button event: what went down or up, where
 *   ev rel 6 -1                    an evdev event: EV_REL's code and value, or "ev key CODE 0|1"
 *   word 0x400                     the buttons word at 0x81000028
 *
 *   nxinput [SECONDS [plain]]      (default 10), then "nxinput done"; plain: the window does not
 *                                  ask for the extra buttons, as most programs do not
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "nano-X.h"
#include "input_words.h"

/* the kernel's input_event on a 32-bit machine with 64-bit time */
struct event { uint32_t sec, usec; uint16_t type, code; int32_t value; };

static double
now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

int
main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 10, end;
	const volatile uint32_t *words = NULL;
	uint32_t buttons = 0;
	GR_SCREEN_INFO si;
	GR_WINDOW_ID window;
	GR_WM_PROPERTIES props;
	int fd, gpu;

	if (GrOpen() < 0) {
		fprintf(stderr, "nxinput: no window system\n");
		return 1;
	}
	GrGetScreenInfo(&si);
	window = GrNewWindowEx(GR_WM_PROPS_NODECORATE, "nxinput", GR_ROOT_WINDOW_ID, 0, 0, si.cols, si.rows, GR_RGB(32, 32, 48));
	GrSelectEvents(window, GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_CLOSE_REQ);
	if (argc < 3 || strcmp(argv[2], "plain")) {
		props.flags = GR_WM_FLAGS_COMMAND;
		props.props = GR_WMC_BUTTONS;
		props.background = 1;
		GrSetWMProperties(window, &props);
	}
	GrMapWindow(window);
	GrFlush();
	fd = open("/dev/input/event1", O_RDONLY | O_NONBLOCK);
	gpu = open("/dev/gpu", O_RDWR);
	if (gpu >= 0) {
		void *at = mmap(NULL, 4096, PROT_READ, MAP_SHARED, gpu, 0x01000000);

		if (at != MAP_FAILED)
			words = at;
	}
	printf("nxinput: a window %dx%d, evdev %s, words %s\n", si.cols, si.rows, fd >= 0 ? "open" : "none", words ? "mapped" : "none");
	fflush(stdout);
	end = now() + seconds;
	while (now() < end) {
		GR_EVENT e;
		struct event ev[16];
		int n;

		GrGetNextEventTimeout(&e, 20);
		if (e.type == GR_EVENT_TYPE_BUTTON_DOWN || e.type == GR_EVENT_TYPE_BUTTON_UP)
			printf("nx %s 0x%x at %d,%d buttons 0x%x\n", e.type == GR_EVENT_TYPE_BUTTON_DOWN ? "down" : "up",
			       e.button.changebuttons, e.button.rootx, e.button.rooty, e.button.buttons);
		else if (e.type == GR_EVENT_TYPE_CLOSE_REQ)
			break;
		while (fd >= 0 && (n = read(fd, ev, sizeof ev)) > 0) {
			int i;

			for (i = 0; i < n / (int)sizeof ev[0]; i++) {
				if (ev[i].type == 2)
					printf("ev rel %d %d\n", ev[i].code, ev[i].value);
				else if (ev[i].type == 1)
					printf("ev key 0x%x %d\n", ev[i].code, ev[i].value);
			}
		}
		if (words && words[0x28 / 4] != buttons) {
			buttons = words[0x28 / 4];
			printf("word 0x%x\n", buttons);
		}
		fflush(stdout);
	}
	printf("nxinput done\n");
	GrClose();
	return 0;
}
