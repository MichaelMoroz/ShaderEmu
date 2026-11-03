/*
 * The machine's input words past a pointer's three buttons and one wheel, for a program that reads
 * them itself (docs/machine/input.md, "More of a mouse and the hands"). The words are at 0x81000020:
 * x, y, the buttons, the key events' count; the ring of key events at 0x81000080. Nano-X has the
 * same as button events of their own for a window that asks (GR_WMC_BUTTONS, nano-X.h's
 * GR_BUTTON_SCROLLLEFT ... GR_BUTTON_SCROLL2RIGHT), and Linux as evdev events of /dev/input/event1.
 */
#ifndef SHADEREMU_INPUT_WORDS_H
#define SHADEREMU_INPUT_WORDS_H

/* the buttons word (0x81000028) */
#define INPUT_LEFT		0x00001	/* the pointer's: a trigger or a click */
#define INPUT_RIGHT		0x00002	/* a grip or the right click */
#define INPUT_MIDDLE		0x00004
#define INPUT_BACK		0x00008	/* a mouse's side buttons */
#define INPUT_FORWARD		0x00010
#define INPUT_HANDS		0x00080	/* the bits below are told: a headset's hands */
#define INPUT_LEFT_TRIGGER	0x00100
#define INPUT_LEFT_GRIP		0x00200
#define INPUT_RIGHT_TRIGGER	0x00400
#define INPUT_RIGHT_GRIP	0x00800
#define INPUT_LEFT_STICK	0x01000	/* a stick pressed in */
#define INPUT_RIGHT_STICK	0x02000
#define INPUT_LEFT_UPPER	0x04000	/* the upper face button: Y on the left, B on the right */
#define INPUT_RIGHT_UPPER	0x08000
#define INPUT_JUMP		0x10000	/* the jump button (A) */

/* a wheel's notch in the key ring: a "press" of a code that is no key, bits 12-13 whose wheel */
#define INPUT_WHEEL_LEFT	0x3fc
#define INPUT_WHEEL_RIGHT	0x3fd
#define INPUT_WHEEL_UP		0x3fe
#define INPUT_WHEEL_DOWN	0x3ff
#define INPUT_WHEEL_FROM(event)	(((event) >> 12) & 3)	/* 0 the pointer's own, 1 the left hand's stick, 2 the right hand's */

/* the evdev codes of /dev/input/event1 for them (linux/kernel/shaderemu_input.c) */
#define INPUT_REL_LEFT_HAND_UP		0x07	/* REL_DIAL: the left hand's stick, up and down */
#define INPUT_REL_LEFT_HAND_ACROSS	0x09	/* REL_MISC: and across */
#define INPUT_BTN_HAND(bit)		(0x2c0 + (bit) - 8)	/* BTN_TRIGGER_HAPPY1 for bit 8, the left trigger */

#endif
