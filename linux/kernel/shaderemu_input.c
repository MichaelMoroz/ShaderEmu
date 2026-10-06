// SPDX-License-Identifier: GPL-2.0
/*
 * ShaderEmu keyboard and pointer (docs/input.md in the ShaderEmu repository): the machine
 * writes them into plain memory, which is polled here every timer tick into two evdev devices.
 */
#include <linux/init.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/timer.h>

#define INPUT_PHYS	0x87000000UL
#define INPUT_STATE	0x20
#define INPUT_KEYS	0x80
#define INPUT_RING	32

static void __iomem *input_regs;
static struct input_dev *keyboard_dev, *pointer_dev;
static struct timer_list input_timer;
static u32 input_tail;
static bool input_started;

static void shaderemu_input_poll(struct timer_list *t)
{
	u32 x = readl(input_regs + INPUT_STATE);
	u32 y = readl(input_regs + INPUT_STATE + 4);
	u32 buttons = readl(input_regs + INPUT_STATE + 8);
	u32 head = readl(input_regs + INPUT_STATE + 12);

	/* first poll, a restarted host, or more events than the ring holds: start from now */
	if (!input_started || head - input_tail > INPUT_RING) {
		input_tail = head;
		input_started = true;
	}
	for (; input_tail != head; input_tail++) {
		u32 event = readl(input_regs + INPUT_KEYS + 4 * (input_tail % INPUT_RING));

		input_report_key(keyboard_dev, event & 0x3ff, event >> 31);
	}
	input_sync(keyboard_dev);
	input_report_abs(pointer_dev, ABS_X, x);
	input_report_abs(pointer_dev, ABS_Y, y);
	input_report_key(pointer_dev, BTN_LEFT, buttons & 1);
	input_report_key(pointer_dev, BTN_RIGHT, (buttons >> 1) & 1);
	input_report_key(pointer_dev, BTN_MIDDLE, (buttons >> 2) & 1);
	input_sync(pointer_dev);
	mod_timer(&input_timer, jiffies + 1);
}

static int __init shaderemu_input_init(void)
{
	int key, ret;

	if (pfn_valid(INPUT_PHYS >> PAGE_SHIFT))
		return -ENODEV;	/* this range is RAM here: not our device tree */
	input_regs = ioremap(INPUT_PHYS, PAGE_SIZE);
	keyboard_dev = input_allocate_device();
	pointer_dev = input_allocate_device();
	if (!input_regs || !keyboard_dev || !pointer_dev)
		return -ENOMEM;

	keyboard_dev->name = "ShaderEmu keyboard";
	keyboard_dev->id.bustype = BUS_HOST;
	__set_bit(EV_KEY, keyboard_dev->evbit);
	__set_bit(EV_REP, keyboard_dev->evbit);
	for (key = 1; key < 0x100; key++)
		__set_bit(key, keyboard_dev->keybit);
	ret = input_register_device(keyboard_dev);
	if (ret)
		return ret;

	pointer_dev->name = "ShaderEmu pointer";
	pointer_dev->id.bustype = BUS_HOST;
	__set_bit(EV_KEY, pointer_dev->evbit);
	__set_bit(EV_ABS, pointer_dev->evbit);
	__set_bit(BTN_LEFT, pointer_dev->keybit);
	__set_bit(BTN_RIGHT, pointer_dev->keybit);
	__set_bit(BTN_MIDDLE, pointer_dev->keybit);
	input_set_abs_params(pointer_dev, ABS_X, 0, 2047, 0, 0);
	input_set_abs_params(pointer_dev, ABS_Y, 0, 2047, 0, 0);
	ret = input_register_device(pointer_dev);
	if (ret)
		return ret;

	timer_setup(&input_timer, shaderemu_input_poll, 0);
	mod_timer(&input_timer, jiffies + 1);
	return 0;
}
device_initcall(shaderemu_input_init);
