/*
 * A desktop program's controller (docs/machine/input.md, "More of a mouse and the hands"): the
 * window asks for the extra buttons (the wheel tilted, the left hand's stick as a second wheel,
 * a mouse's side buttons), which come as Nano-X button events, and the machine's buttons word is
 * read for the hands' own buttons, which do not. A game polls it where it already wakes (each
 * frame of play) and maps what went down and up to its moves.
 *
 *   ui_pad_ask(window);                 once, after the window is made
 *   ui_pad_poll(&pad);                  pad.down, pad.up: the word's bits that went down, up
 *   ui_pad_scroll(&event, &x, &y, &which)   a button event of a wheel: notches across and up,
 *                                       which 0 the pointer's wheel (or the right stick), 1 the left stick
 */
#ifndef UI_PAD_H
#define UI_PAD_H

#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include "nano-X.h"
#include "input_words.h"

struct ui_pad {
	uint32_t held, down, up;	/* the buttons word now; what went down and up at the last poll */
};

static void
ui_pad_ask(GR_WINDOW_ID window)
{
	GR_WM_PROPERTIES props;

	props.flags = GR_WM_FLAGS_COMMAND;
	props.props = GR_WMC_BUTTONS;
	props.background = 1;
	GrSetWMProperties(window, &props);
}

/* The machine's input words (0x81000000's page through /dev/gpu), or NULL without them. */
static const volatile uint32_t *
ui_pad_words(void)
{
	static const volatile uint32_t *words;
	static int tried;

	if (!tried) {
		int fd = open("/dev/gpu", O_RDWR);
		void *at = fd < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x01000000);

		tried = 1;
		if (at != MAP_FAILED)
			words = at;
		else if (fd >= 0)
			close(fd);
	}
	return words;
}

static uint32_t
ui_pad_buttons(void)
{
	const volatile uint32_t *w = ui_pad_words();

	return w ? w[0x28 / 4] : 0;
}

static void
ui_pad_poll(struct ui_pad *pad)
{
	uint32_t now = ui_pad_buttons();

	pad->down = now & ~pad->held;
	pad->up = pad->held & ~now;
	pad->held = now;
}

/* A button event of a wheel: 1 with its notch across (x, right positive) and up (y), and which wheel. */
static int
ui_pad_scroll(const GR_EVENT *event, int *x, int *y, int *which)
{
	GR_BUTTON b;

	if (event->type != GR_EVENT_TYPE_BUTTON_DOWN)
		return 0;
	b = event->button.changebuttons;
	*x = *y = 0;
	*which = (b & (GR_BUTTON_SCROLL2UP | GR_BUTTON_SCROLL2DN | GR_BUTTON_SCROLL2LEFT | GR_BUTTON_SCROLL2RIGHT)) != 0;
	if (b & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLL2UP))
		*y = 1;
	else if (b & (GR_BUTTON_SCROLLDN | GR_BUTTON_SCROLL2DN))
		*y = -1;
	else if (b & (GR_BUTTON_SCROLLRIGHT | GR_BUTTON_SCROLL2RIGHT))
		*x = 1;
	else if (b & (GR_BUTTON_SCROLLLEFT | GR_BUTTON_SCROLL2LEFT))
		*x = -1;
	else if (!b && (event->button.buttons & (GR_BUTTON_SCROLLUP | GR_BUTTON_SCROLLDN)))	/* (the server's repeat of a wheel held) */
		*y = event->button.buttons & GR_BUTTON_SCROLLUP ? 1 : -1;
	return *x || *y;
}

/* The left stick's notch as an arrow key (0: no such event): a program's keyboard cursor moves by it. */
static int
ui_pad_arrow(const GR_EVENT *event)
{
	int x, y, which;

	if (!ui_pad_scroll(event, &x, &y, &which) || !which)
		return 0;
	return x < 0 ? MWKEY_LEFT : x > 0 ? MWKEY_RIGHT : y > 0 ? MWKEY_UP : MWKEY_DOWN;
}

/* After ui_pad_poll: the key of the first of A (the jump button), B and Y that went down, 0 for none. */
static int
ui_pad_key(const struct ui_pad *pad, int a, int b, int y)
{
	if ((pad->down & INPUT_JUMP) && a)
		return a;
	if ((pad->down & INPUT_RIGHT_UPPER) && b)
		return b;
	if ((pad->down & INPUT_LEFT_UPPER) && y)
		return y;
	return 0;
}

/* How long to wait for an event (-1: for ever): at most a tenth of a second while a headset's hands are told,
 * whose buttons are no event. */
static int
ui_pad_wait(const struct ui_pad *pad, int ms)
{
	return pad->held & INPUT_HANDS && (ms < 0 || ms > 100) ? 100 : ms;
}

#endif
