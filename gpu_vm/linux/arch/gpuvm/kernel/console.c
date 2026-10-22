// SPDX-License-Identifier: GPL-2.0
/*
 * The machine's console (gpu_vm/linux/README.md, "Console"): the RISC-V machine's words in memory
 * (docs/machine/console.md, "Words in memory"), moved down into the device window with the rest of it
 * (0x876b8280 - 0x86000000). The kernel reads and writes them with loads and stores; the host reads them after every
 * commit and writes its own between passes.
 *
 *   +0x00  characters the host has delivered so far (the host's)
 *   +0x04  the last delivery, up to four characters, the first in the low byte (the host's)
 *   +0x10  characters the guest has taken so far
 *   +0x14  bytes the guest has written so far
 *   +0x20  the output ring: 256 bytes, byte n of the output at n mod 256
 *
 * The host delivers again only once the guest has taken everything. The guest puts at most 255 bytes in a pass
 * (TIME is the same for a whole pass: a new value is a new pass); a full ring ends the pass with sync. No interrupt:
 * input is polled from the timer's tick and by hvc's own poll.
 */
#include <linux/console.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include <asm/gvm.h>

#include "hvc_console.h"

#define GVM_CON_BASE	0x016b8280UL
#define GVM_CON_HOST	((volatile u32 *)(GVM_CON_BASE + 0x00))
#define GVM_CON_GUEST	((volatile u32 *)(GVM_CON_BASE + 0x10))
#define GVM_CON_RING	((volatile u8 *)(GVM_CON_BASE + 0x20))
#define GVM_CON_SIZE	256

static unsigned long gvm_con_pass;	/* TIME when the count began */
static int gvm_con_put_n;		/* bytes put in that pass */
static int gvm_con_waits;		/* the last write left bytes behind */
static int gvm_hvc_ready;		/* hvc's thread is there to wake */

/* As many of buf's bytes as the ring takes in this pass */
static int gvm_con_put(const char *buf, int count)
{
	unsigned long now = gvm_mfsr(time_lo);
	u32 n = GVM_CON_GUEST[1];
	int i;

	if (now != gvm_con_pass) {
		gvm_con_pass = now;
		gvm_con_put_n = 0;
	}
	for (i = 0; i < count && gvm_con_put_n < GVM_CON_SIZE - 1; i++, gvm_con_put_n++)
		GVM_CON_RING[(n + i) & (GVM_CON_SIZE - 1)] = buf[i];
	GVM_CON_GUEST[1] = n + i;
	return i;
}

/* The characters delivered and not taken: the last ones of the host's group of up to four */
static int gvm_con_get(char *buf, int count)
{
	u32 delivered = GVM_CON_HOST[0], group = GVM_CON_HOST[1], taken = GVM_CON_GUEST[0];
	u32 have = (group >> 24) ? 4 : (group >> 16) ? 3 : (group >> 8) ? 2 : 1, left = delivered - taken;
	int i = 0;

	if (left == 0)
		return 0;
	if (left > have)
		left = have;
	for (; left && i < count; left--) {
		char c = group >> (8 * (have - left));

		if (c)
			buf[i++] = c;
	}
	GVM_CON_GUEST[0] = delivered - left;
	return i;
}

/* Every byte, ending passes while the ring is full: for the kernel's own messages */
static void gvm_con_write_all(const char *s, unsigned int n)
{
	while (n) {
		int put = gvm_con_put(s, n);

		s += put;
		n -= put;
		if (n)
			gvm_sync();
	}
}

static void gvm_boot_console_write(struct console *con, const char *s, unsigned int n)
{
	gvm_con_write_all(s, n);
}

static struct console gvm_boot_console = {
	.name	= "gvmboot",
	.write	= gvm_boot_console_write,
	.flags	= CON_PRINTBUFFER | CON_BOOT | CON_ANYTIME,
	.index	= -1,
};

/* setup_arch's first line: printk from the start */
void __init gvm_early_console_init(void)
{
	register_console(&gvm_boot_console);
}

static int gvm_hvc_put(uint32_t vtermno, const char *buf, int count)
{
	int i = gvm_con_put(buf, count);

	gvm_con_waits = i < count;
	return i ? i : -EAGAIN;
}

static int gvm_hvc_get(uint32_t vtermno, char *buf, int count)
{
	return gvm_con_get(buf, count);
}

/* The kernel's own messages are not put off (wait false): the pass ends, the next has room */
static int gvm_hvc_flush(uint32_t vtermno, bool wait)
{
	if (!wait && gvm_con_waits)
		gvm_sync();
	return 0;
}

static const struct hv_ops gvm_hvc_ops = {
	.get_chars	= gvm_hvc_get,
	.put_chars	= gvm_hvc_put,
	.flush		= gvm_hvc_flush,
};

/* From the timer's interrupt: input waiting, or output left behind, wakes hvc's thread */
void gvm_console_tick(void)
{
	if (gvm_hvc_ready && (GVM_CON_HOST[0] != GVM_CON_GUEST[0] || gvm_con_waits))
		hvc_kick();
}

static int __init gvm_hvc_console_init(void)
{
	hvc_instantiate(0, 0, &gvm_hvc_ops);
	return 0;
}
console_initcall(gvm_hvc_console_init);

static int __init gvm_hvc_init(void)
{
	struct hvc_struct *hp = hvc_alloc(0, 0, &gvm_hvc_ops, 1024);

	if (IS_ERR(hp))
		return PTR_ERR(hp);
	gvm_hvc_ready = 1;
	return 0;
}
device_initcall(gvm_hvc_init);
