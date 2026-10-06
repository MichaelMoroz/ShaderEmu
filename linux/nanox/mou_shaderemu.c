/*
 * Microwindows mouse driver for the ShaderEmu pointer: an evdev device that reports absolute
 * position in display pixels and three buttons (docs/input.md in the ShaderEmu repository).
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
#define EV_ABS 3
#define BTN_LEFT 0x110

static int fd = -1;
static int cur_x, cur_y, cur_buttons;

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
	return MWBUTTON_L | MWBUTTON_M | MWBUTTON_R;
}

static void
Ptr_GetDefaultAccel(int *pscale, int *pthresh)
{
	*pscale = 1;
	*pthresh = 0;
}

/* Takes everything waiting and reports where the pointer ended up. */
static int
Ptr_Read(MWCOORD *dx, MWCOORD *dy, MWCOORD *dz, int *bp)
{
	static const int button[3] = { MWBUTTON_L, MWBUTTON_R, MWBUTTON_M };
	struct event ev[32];
	int n, i, changed = 0;

	while ((n = read(fd, ev, sizeof(ev))) >= (int)sizeof(ev[0])) {
		for (i = 0; i < n / (int)sizeof(ev[0]); i++) {
			if (ev[i].type == EV_ABS && ev[i].code == 0)
				cur_x = ev[i].value;
			else if (ev[i].type == EV_ABS && ev[i].code == 1)
				cur_y = ev[i].value;
			else if (ev[i].type == EV_KEY && ev[i].code >= BTN_LEFT && ev[i].code < BTN_LEFT + 3) {
				if (ev[i].value)
					cur_buttons |= button[ev[i].code - BTN_LEFT];
				else
					cur_buttons &= ~button[ev[i].code - BTN_LEFT];
			} else
				continue;
			changed = 1;
		}
	}
	if (!changed)
		return MOUSE_NODATA;
	*dx = cur_x;
	*dy = cur_y;
	*dz = 0;
	*bp = cur_buttons;
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
