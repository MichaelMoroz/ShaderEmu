// SPDX-License-Identifier: GPL-2.0
/*
 * ShaderEmu keyboard and pointer (docs/machine/input.md in the ShaderEmu repository): the machine
 * writes them into plain memory, which is polled here every timer tick into two evdev devices.
 * The tick itself calls the poll (tick_input_hook.py); a kernel timer does only while the
 * machine is idle and its tick may be stopped: one that expired every tick cost 900 instructions.
 */
#include <linux/init.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pm.h>
#include <asm/processor.h>
#include <linux/sched.h>
#include <linux/timer.h>

#define INPUT_PHYS	0x81000000UL
#define INPUT_STATE	0x20
#define INPUT_KEYS	0x80
#define INPUT_RING	32
#define INPUT_WHEEL_UP	0x3fe	/* not keys: a notch of the pointer's wheel */
#define INPUT_WHEEL_DOWN	0x3ff
/* The window system's words for us, which shaderemu_gpu.c has mapped (scr_shaderemu.c,
 * WM_QUIET_OFFSET): left | top << 16, right | bottom << 16, its worker core, WM_MAGIC. */
#define WM_MAGIC	0x31514d57
extern void __iomem *shaderemu_gpu_wm;
extern void shaderemu_gpu_wake(u32 core);	/* shaderemu_gpu.c, which has the cores' mailboxes */

static void __iomem *input_regs;
static unsigned long hushed_at, woken_at;
static bool pointer_quiet;	/* where it is now is in the window system's quiet rectangle, when last asked */
static struct input_dev *keyboard_dev, *pointer_dev;
/* An idle machine that nobody has touched for a second polls every fourth tick; any key, button
 * or move of the pointer brings it back to every tick at once (a tick's whole path for a
 * poll of 30 instructions was most of what an idle machine runs). */
#define IDLE_POLL 4
static u32 seen_x = ~0u, seen_y, seen_buttons;
static unsigned long touched_at;
static struct timer_list input_timer;
static u32 input_tail;
static bool input_started, input_ready;	/* polled once; both devices are registered */
static bool input_ticked;	/* the tick calls the poll (a kernel without the hook: the timer does, always) */
static u32 pointer_x = ~0u, pointer_y, pointer_buttons;
static bool pointer_owed;	/* it is not where it was last reported: a move too soon to report, or a quiet one */
static unsigned long pointer_at;

/*
 * A move inside the window system's rectangle is nobody's news there (docs/system/nanox.md, "The
 * pointer on a core of its own"): no report, and the window system sleeps on.
 */
static noinline bool pointer_hushed(u32 x, u32 y)
{
	u32 from, to;

	pointer_quiet = false;
	if (!shaderemu_gpu_wm || readl(shaderemu_gpu_wm + 12) != WM_MAGIC)
		return false;
	from = readl(shaderemu_gpu_wm);
	to = readl(shaderemu_gpu_wm + 4);
	if (x < (from & 0xffff) || y < from >> 16 || x >= (to & 0xffff) || y >= to >> 16)
		return false;
	hushed_at = jiffies;
	pointer_quiet = true;
	return true;
}

/*
 * The pointer moved or a button changed. The worker core that shows the cursor sleeps a
 * tenth of a second after the last change it saw and wakes when its job word changes: every
 * 40 ms of a pointer that moves is often enough. Its answer and "taken" words go with it,
 * which is a core with no job to shaderemu_gpu.c: one a program may have back at once.
 */
static noinline void pointer_moved(void)
{
	if (!shaderemu_gpu_wm || time_before(jiffies, woken_at + HZ / 25) || readl(shaderemu_gpu_wm + 12) != WM_MAGIC)
		return;
	woken_at = jiffies;
	shaderemu_gpu_wake(readl(shaderemu_gpu_wm + 8));
}

static void shaderemu_input_poll(void)
{
	u32 x = readl(input_regs + INPUT_STATE);
	u32 y = readl(input_regs + INPUT_STATE + 4);
	u32 buttons = readl(input_regs + INPUT_STATE + 8);
	u32 head = readl(input_regs + INPUT_STATE + 12), keys;
	int wheel = 0;

	/* most ticks: nothing new, and nothing of the pointer still to be said (one at rest in the
	 * quiet rectangle is asked about four times a second: the rectangle may change under it) */
	if (head == input_tail && x == seen_x && y == seen_y && buttons == seen_buttons && input_started &&
	    (!pointer_owed || (pointer_quiet && time_before(jiffies, hushed_at + HZ / 4))))
		return;
	/* first poll, a restarted host, or more events than the ring holds: start from now */
	if (!input_started || head - input_tail > INPUT_RING) {
		input_tail = head;
		input_started = true;
	}
	keys = input_tail;
	if (head != input_tail || x != seen_x || y != seen_y || buttons != seen_buttons) {
		if (x != seen_x || y != seen_y || buttons != seen_buttons)
			pointer_moved();
		seen_x = x, seen_y = y, seen_buttons = buttons;
		touched_at = jiffies;
	}
	for (; input_tail != head; input_tail++) {
		u32 event = readl(input_regs + INPUT_KEYS + 4 * (input_tail % INPUT_RING));

		if ((event & 0x3ff) >= INPUT_WHEEL_UP)
			wheel += (event & 0x3ff) == INPUT_WHEEL_UP ? 1 : -1;
		else
			input_report_key(keyboard_dev, event & 0x3ff, event >> 31);
	}
	if (keys != head)
		input_sync(keyboard_dev);
	if (wheel) {
		input_report_rel(pointer_dev, REL_WHEEL, wheel);
		input_sync(pointer_dev);
	}
	/*
	 * Most ticks nothing moved: reporting it anyway costs more than this poll should. And a
	 * pointer that moves is not reported in every tick: a report wakes the window system,
	 * which with a game running is some seventeen thousand instructions of a machine that
	 * has three million a second. So it
	 * is 50 reports a second while the machine has nothing else to do and 10 while a
	 * program is running; what shows the display draws the cursor where the host's pointer
	 * is anyway, and a program that wants every move reads these words itself. A button is
	 * reported in its own tick, with the place.
	 */
	if (buttons != pointer_buttons ||
	    ((x != pointer_x || y != pointer_y) &&
	     time_after_eq(jiffies, pointer_at + (idle_cpu(smp_processor_id()) ? HZ / 50 : HZ / 10)) &&
	     !pointer_hushed(x, y))) {
		pointer_at = jiffies;
		pointer_x = x;
		pointer_y = y;
		pointer_buttons = buttons;
		input_report_abs(pointer_dev, ABS_X, x);
		input_report_abs(pointer_dev, ABS_Y, y);
		input_report_key(pointer_dev, BTN_LEFT, buttons & 1);
		input_report_key(pointer_dev, BTN_RIGHT, (buttons >> 1) & 1);
		input_report_key(pointer_dev, BTN_MIDDLE, (buttons >> 2) & 1);
		input_sync(pointer_dev);
	}
	pointer_owed = x != pointer_x || y != pointer_y;
}

/* Every tick, from the timer interrupt (update_process_times). */
void shaderemu_input_tick(void)
{
	input_ticked = true;
	if (input_ready)
		shaderemu_input_poll();
}

/* The idle loop, before it decides how long to sleep: the timer below wakes it at the next tick. */
void shaderemu_input_idle(void)
{
	if (input_ready && !timer_pending(&input_timer))
		mod_timer(&input_timer, jiffies + (time_before(jiffies, touched_at + HZ) ? 1 : IDLE_POLL));
}

static void shaderemu_input_timer(struct timer_list *t)
{
	unsigned long flags;

	local_irq_save(flags);		/* (the tick's poll must not come in the middle of this one) */
	shaderemu_input_poll();
	local_irq_restore(flags);
	if (!input_ticked || idle_cpu(smp_processor_id()))
		mod_timer(&input_timer, jiffies + (input_ticked && !time_before(jiffies, touched_at + HZ) ? IDLE_POLL : 1));
}

/*
 * Power off. The machine has no firmware to ask (its answer to the call is "not supported",
 * after which Linux carried on with a shell that still ran): it stands still here, waiting
 * for interrupts that are switched off, until it is switched off where it is shown.
 */
static void shaderemu_power_off(void)
{
	local_irq_disable();
	for (;;)
		wait_for_interrupt();
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
	__set_bit(EV_REL, pointer_dev->evbit);
	__set_bit(REL_WHEEL, pointer_dev->relbit);
	__set_bit(BTN_LEFT, pointer_dev->keybit);
	__set_bit(BTN_RIGHT, pointer_dev->keybit);
	__set_bit(BTN_MIDDLE, pointer_dev->keybit);
	input_set_abs_params(pointer_dev, ABS_X, 0, 2047, 0, 0);
	input_set_abs_params(pointer_dev, ABS_Y, 0, 2047, 0, 0);
	ret = input_register_device(pointer_dev);
	if (ret)
		return ret;

	pointer_at = jiffies;	/* (which does not begin at 0) */
	hushed_at = woken_at = jiffies - HZ;
	touched_at = jiffies;
	pm_power_off = shaderemu_power_off;
	timer_setup(&input_timer, shaderemu_input_timer, 0);
	input_ready = true;
	mod_timer(&input_timer, jiffies + 1);
	return 0;
}
device_initcall(shaderemu_input_init);
