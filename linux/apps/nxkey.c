/* nxkey: presses keys through the keyboard's evdev device, for a test without a keyboard.
 * An argument is a key (enter, tab, up, f4, meta, ... or a character), with ctrl+, alt+ or
 * shift+ before it for keys held meanwhile, a number of milliseconds to wait, or X,Y for a
 * click of the left button there:   nxkey meta down down enter 500 alt+tab 60,200 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the kernel's input_event on this machine, as kbd_shaderemu.c reads it */
struct event { uint32_t sec, usec; uint16_t type, code; int32_t value; };

static const struct { const char *name; int code; } names[] = {
	{ "escape", 1 }, { "backspace", 14 }, { "tab", 15 }, { "enter", 28 }, { "ctrl", 29 },
	{ "shift", 42 }, { "alt", 56 }, { "space", 57 }, { "f1", 59 }, { "f2", 60 }, { "f3", 61 },
	{ "f4", 62 }, { "f5", 63 }, { "f6", 64 }, { "f7", 65 }, { "f8", 66 }, { "f9", 67 },
	{ "f10", 68 }, { "home", 102 }, { "up", 103 }, { "pageup", 104 }, { "left", 105 },
	{ "right", 106 }, { "end", 107 }, { "down", 108 }, { "pagedown", 109 }, { "delete", 111 },
	{ "meta", 125 },
};
/* the characters of the keyboard's rows, in key code order from 2 ("1"), 16 ("q"), 30 and 44 */
static const char *rows[] = { "1234567890-=", "qwertyuiop[]", "asdfghjkl;'`", "zxcvbnm,./" };
static const int row_code[] = { 2, 16, 30, 44 };

static int fd;

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
send(int code, int down)
{
	struct event events[2] = { { 0, 0, 1, (uint16_t)code, down }, { 0, 0, 0, 0, 0 } };	/* the key, and a report's end */

	if (write(fd, events, sizeof events) != sizeof events)
		perror("nxkey");
	usleep(20000);
}

/* A press of the left button at x, y, through the pointer's device. The kernel puts the
 * pointer back where the host has it at its next poll, which lets the button go as well. */
static void
click(int x, int y)
{
	struct event events[4] = { { 0, 0, 3, 0, x }, { 0, 0, 3, 1, y }, { 0, 0, 1, 0x110, 1 }, { 0, 0, 0, 0, 0 } };
	int pointer = open("/dev/input/event1", O_WRONLY);

	if (pointer < 0 || write(pointer, events, sizeof events) != sizeof events)
		perror("nxkey: /dev/input/event1");
	if (pointer >= 0)
		close(pointer);
}

int
main(int argc, char **argv)
{
	int i;

	fd = open("/dev/input/event0", O_WRONLY);
	if (fd < 0) {
		perror("nxkey: /dev/input/event0");
		return 1;
	}
	for (i = 1; i < argc; i++) {
		char text[64], *key = text, *plus;
		int held[4], n = 0, code;

		if (argv[i][0] >= '0' && argv[i][0] <= '9' && argv[i][1]) {
			if (strchr(argv[i], ','))
				click(atoi(argv[i]), atoi(strchr(argv[i], ',') + 1));
			usleep(strchr(argv[i], ',') ? 150000 : atoi(argv[i]) * 1000);
			continue;
		}
		snprintf(text, sizeof text, "%s", argv[i]);
		while ((plus = strchr(key, '+')) != NULL && plus[1] && n < 4) {
			*plus = 0;
			if ((held[n] = code_of(key)) != 0)
				send(held[n++], 1);
			key = plus + 1;
		}
		if ((code = code_of(key)) == 0)
			fprintf(stderr, "nxkey: no key called %s\n", key);
		else {
			send(code, 1);
			send(code, 0);
		}
		while (n > 0)
			send(held[--n], 0);
		usleep(150000);	/* time for what the key did to be drawn */
	}
	return 0;
}
