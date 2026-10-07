/*
 * Microwindows keyboard driver for the ShaderEmu keyboard: an evdev device that reports Linux
 * key codes (docs/input.md in the ShaderEmu repository). US layout.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "device.h"
#include "keymap_standard.h"

#define KEYBOARD_DEV "/dev/input/event0"

/* the kernel's input_event on a 32-bit machine with 64-bit time: C libraries disagree on it */
struct event { uint32_t sec, usec; uint16_t type, code; int32_t value; };
#define EV_KEY 1

static int fd = -1;
static MWKEYMOD modstate;
static struct event queue[32];
static int queued, taken;

static int
Kbd_Open(KBDDEVICE *pkd)
{
	const char *dev = getenv("KEYBOARD_PORT");

	fd = open(dev ? dev : KEYBOARD_DEV, O_RDONLY | O_NONBLOCK);
	modstate = MWKMOD_NONE;
	return fd < 0 ? DRIVER_FAIL : fd;
}

static void
Kbd_Close(void)
{
	if (fd >= 0)
		close(fd);
	fd = -1;
}

static void
Kbd_GetModifierInfo(MWKEYMOD *modifiers, MWKEYMOD *curmodifiers)
{
	if (modifiers)
		*modifiers = MWKMOD_CTRL | MWKMOD_SHIFT | MWKMOD_ALT | MWKMOD_META | MWKMOD_CAPS | MWKMOD_NUM;
	if (curmodifiers)
		*curmodifiers = modstate;
}

/* The modifier bit a key holds down, or 0. */
static MWKEYMOD
held_modifier(MWKEY key)
{
	switch (key) {
	case MWKEY_LSHIFT: return MWKMOD_LSHIFT;
	case MWKEY_RSHIFT: return MWKMOD_RSHIFT;
	case MWKEY_LCTRL: return MWKMOD_LCTRL;
	case MWKEY_RCTRL: return MWKMOD_RCTRL;
	case MWKEY_LALT: return MWKMOD_LALT;
	case MWKEY_RALT: return MWKMOD_RALT;
	case MWKEY_LMETA: return MWKMOD_LMETA;
	case MWKEY_RMETA: return MWKMOD_RMETA;
	}
	return 0;
}

/* What a key means with the current modifiers. */
static MWKEY
translate(MWKEY key)
{
	static const char plain[] = "`1234567890-=[]\\;',./";
	static const char shifted[] = "~!@#$%^&*()_+{}|:\"<>?";
	static const MWKEY keypad[] = { MWKEY_KP0, MWKEY_KP1, MWKEY_KP2, MWKEY_KP3, MWKEY_KP4,
		MWKEY_KP5, MWKEY_KP6, MWKEY_KP7, MWKEY_KP8, MWKEY_KP9 };
	int shift = (modstate & MWKMOD_SHIFT) != 0, i;
	const char *p;

	if (modstate & MWKMOD_NUM)
		for (i = 0; i < 10; i++)
			if (key == keypad[i])
				return '0' + i;
	switch (key) {
	case MWKEY_KP_PERIOD: return (modstate & MWKMOD_NUM) ? '.' : MWKEY_DELETE;
	case MWKEY_KP_DIVIDE: return '/';
	case MWKEY_KP_MULTIPLY: return '*';
	case MWKEY_KP_MINUS: return '-';
	case MWKEY_KP_PLUS: return '+';
	case MWKEY_KP_ENTER: return MWKEY_ENTER;
	}
	if (key >= 0x80)
		return key;
	if (key >= 'a' && key <= 'z') {
		if (modstate & MWKMOD_CTRL)
			return key & 0x1f;
		return (shift != ((modstate & MWKMOD_CAPS) != 0)) ? key - 'a' + 'A' : key;
	}
	if (shift && key && (p = strchr(plain, key)) != NULL)
		key = shifted[p - plain];
	if ((modstate & MWKMOD_CTRL) && key >= '@' && key <= '_')
		return key & 0x1f;
	return key;
}

/* One key event per call: 1 on a press, 2 on a release, 0 with nothing waiting. */
static int
Kbd_Read(MWKEY *kbuf, MWKEYMOD *modifiers, MWSCANCODE *pscancode)
{
	for (;;) {
		struct event *ev;
		MWKEY key;
		MWKEYMOD mod;
		int pressed;

		if (taken == queued) {
			int n = read(fd, queue, sizeof(queue));

			if (n < (int)sizeof(queue[0]))
				return (n < 0 && errno != EINTR && errno != EAGAIN) ? KBD_FAIL : KBD_NODATA;
			queued = n / sizeof(queue[0]);
			taken = 0;
		}
		ev = &queue[taken++];
		if (ev->type != EV_KEY || ev->code >= 128)
			continue;
		pressed = ev->value != 0;	/* 2 is the kernel's auto-repeat */
		key = ev->code == 25 ? 'p' : keymap[ev->code];	/* the table has a second 'o' there */
		if (key == MWKEY_UNKNOWN)
			continue;

		mod = held_modifier(key);
		/* a modifier held down is not pressed again and again: the Windows key would open
		 * and close the Start menu thirty times a second for as long as it is down */
		if (mod && ev->value == 2)
			continue;
		if (mod) {
			if (pressed)
				modstate |= mod;
			else
				modstate &= ~mod;
		} else if (key == MWKEY_CAPSLOCK || key == MWKEY_NUMLOCK) {
			if (ev->value == 1)
				modstate ^= key == MWKEY_CAPSLOCK ? MWKMOD_CAPS : MWKMOD_NUM;
		} else
			key = translate(key);

		*kbuf = key;
		*modifiers = modstate;
		*pscancode = ev->code;
		return pressed ? KBD_KEYPRESS : KBD_KEYRELEASE;
	}
}

KBDDEVICE kbddev = {
	Kbd_Open,
	Kbd_Close,
	Kbd_GetModifierInfo,
	Kbd_Read,
	NULL
};
