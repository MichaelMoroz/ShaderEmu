/*
 * Microwindows mouse driver for the ShaderEmu pointer: an evdev device that reports absolute
 * position in display pixels, three buttons, a mouse's side buttons and two wheels of two axes
 * (docs/machine/input.md in the ShaderEmu repository, "More of a mouse and the hands").
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "device.h"

#define POINTER_DEV "/dev/input/event1"

/* the kernel's input_event on a 32-bit machine with 64-bit time: C libraries disagree on it */
struct event { uint32_t sec, usec; uint16_t type, code; int32_t value; };
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define REL_HWHEEL 6
#define REL_DIAL 7
#define REL_WHEEL 8
#define REL_MISC 9
#define BTN_LEFT 0x110
#define BTN_SIDE 0x113
#define BTN_EXTRA 0x114
/* nano-X.h's GR_BUTTON_* past the usual ones (a window asks for them: GR_WMC_BUTTONS) */
#define SE_SCROLLLEFT 0x0100
#define SE_SCROLLRIGHT 0x0200
#define SE_BACK 0x0400
#define SE_FORWARD 0x0800
#define SE_SCROLL2UP 0x1000
#define SE_SCROLL2DN 0x2000
#define SE_SCROLL2LEFT 0x4000
#define SE_SCROLL2RIGHT 0x8000

static int fd = -1;
static int cur_x, cur_y, cur_buttons;
/* notches still to report, up or right positive: the wheel, tilted, the second wheel, across;
 * and the button of the one being reported */
static int wheel[4], wheel_shown;
static const int wheel_button[4][2] = {
	{ MWBUTTON_SCROLLUP, MWBUTTON_SCROLLDN }, { SE_SCROLLRIGHT, SE_SCROLLLEFT },
	{ SE_SCROLL2UP, SE_SCROLL2DN }, { SE_SCROLL2RIGHT, SE_SCROLL2LEFT },
};

static int
Ptr_Open(MOUSEDEVICE *pmd)
{
	const char *dev = getenv("MOUSE_PORT");

	fd = open(dev ? dev : POINTER_DEV, O_RDONLY | O_NONBLOCK);
	return fd < 0 ? MOUSE_FAIL : fd;
}

static void
Ptr_Close(void)
{
	if (fd >= 0)
		close(fd);
	fd = -1;
}

static int
Ptr_GetButtonInfo(void)
{
	return MWBUTTON_L | MWBUTTON_M | MWBUTTON_R | MWBUTTON_SCROLLUP | MWBUTTON_SCROLLDN | 0xff00;
}

static void
Ptr_GetDefaultAccel(int *pscale, int *pthresh)
{
	*pscale = 1;
	*pthresh = 0;
}

/*
 * Takes what is waiting and reports where the pointer ended up, but no further than a report
 * in which a button changed: a press and its release that both wait (the server was busy
 * drawing) are two answers, or the click is never seen. The server asks until there is none.
 */
static int
Ptr_Read(MWCOORD *dx, MWCOORD *dy, MWCOORD *dz, int *bp)
{
	static const int button[3] = { MWBUTTON_L, MWBUTTON_R, MWBUTTON_M };
	static struct event ev[32];
	static int have, at;	/* events read, and how many of them are dealt with */
	/* a read that did not fill the room took all there was: no second one to hear so */
	static int emptied;
	int n, w, changed = 0, pressed = 0;

	for (;;) {
		if (at == have) {
			at = have = 0;
			if (emptied)
				break;
			n = read(fd, ev, sizeof(ev));
			emptied = n > 0 && n < (int)sizeof(ev);
			have = n >= (int)sizeof(ev[0]) ? n / (int)sizeof(ev[0]) : 0;
			if (!have)
				break;
		}
		if (ev[at].type == EV_ABS && ev[at].code == 0) {
			cur_x = ev[at].value;
			changed = 1;
		} else if (ev[at].type == EV_ABS && ev[at].code == 1) {
			cur_y = ev[at].value;
			changed = 1;
		} else if (ev[at].type == EV_KEY && ev[at].code >= BTN_LEFT && ev[at].code < BTN_LEFT + 3) {
			if (ev[at].value)
				cur_buttons |= button[ev[at].code - BTN_LEFT];
			else
				cur_buttons &= ~button[ev[at].code - BTN_LEFT];
			changed = pressed = 1;
		} else if (ev[at].type == EV_KEY && (ev[at].code == BTN_SIDE || ev[at].code == BTN_EXTRA)) {
			int b = ev[at].code == BTN_SIDE ? SE_BACK : SE_FORWARD;

			cur_buttons = ev[at].value ? cur_buttons | b : cur_buttons & ~b;
			changed = pressed = 1;
		} else if (ev[at].type == EV_REL && ev[at].code == REL_WHEEL) {
			wheel[0] += ev[at].value;
		} else if (ev[at].type == EV_REL && ev[at].code == REL_HWHEEL) {
			wheel[1] += ev[at].value;
		} else if (ev[at].type == EV_REL && ev[at].code == REL_DIAL) {
			wheel[2] += ev[at].value;
		} else if (ev[at].type == EV_REL && ev[at].code == REL_MISC) {
			wheel[3] += ev[at].value;
		} else if (ev[at].type == 0 && pressed) {	/* the end of a report with a button in it */
			at++;
			break;
		}
		at++;
	}
	/* a notch of a wheel is a scroll button seen down in one report and up in the next */
	if (wheel_shown) {
		wheel_shown = 0;
		changed = 1;
	} else
		for (w = 0; w < 4; w++)
			if (wheel[w]) {
				wheel_shown = wheel_button[w][wheel[w] > 0 ? 0 : 1];
				wheel[w] += wheel[w] > 0 ? -1 : 1;
				changed = 1;
				break;
			}
	if (!changed) {
		emptied = 0;
		return MOUSE_NODATA;
	}
	*dx = cur_x;
	*dy = cur_y;
	*dz = 0;
	*bp = cur_buttons | wheel_shown;
	return MOUSE_ABSPOS;
}

MOUSEDEVICE mousedev = {
	Ptr_Open,
	Ptr_Close,
	Ptr_GetButtonInfo,
	Ptr_GetDefaultAccel,
	Ptr_Read,
	NULL,
	MOUSE_NORMAL
};
