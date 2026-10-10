// SPDX-License-Identifier: GPL-2.0
/*
 * ShaderEmu GPU device (docs/gpu.md in the ShaderEmu repository): mmap of the GPU's memory,
 * and an ioctl that sleeps with wfi until the submitted list has been drawn.
 */
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <asm/processor.h>

#define GPU_PHYS	0x86000000UL	/* buffers; the registers are 16 MiB in */
#define GPU_SIZE	0x01b00000UL
#define GPU_REGS	0x87000000UL
#define GPU_SUBMIT	0x10
#define GPU_LIST	0x14
#define GPU_COUNT	0x18
#define GPU_FRAMES	0x1c	/* lists drawn so far */
#define GPU_INTO	0x50	/* where a copy goes: address, width, height, row length */
#define KEYBOARD_OWNER	0x30	/* set by a program that takes the keyboard (docs/input.md) */
#define DISPLAY_CURSOR	0x48	/* 1 while the display shows a cursor (docs/display.md) */
#define VOLUME_LIST	0x300	/* the frame the volume display shows (docs/volume.md) */

/* arg: the frame counter's value before submitting */
#define SHADEREMU_GPU_WAIT	_IO('G', 1)

/* arg: a list and what to do with it; returns when it has been drawn and copied */
struct shaderemu_gpu_submit {
	__u32 list, count, how;
	__u32 address, width, height, row;
};
#define SHADEREMU_GPU_SUBMIT	_IOW('G', 2, struct shaderemu_gpu_submit)

/*
 * A program asks for worker cores (docs/multicore.md): up to `want` of those nobody has are
 * its own from here on, a bit each in `mask`, and `root` is the number of the first page of
 * its page table, which it hands them to run in its memory. They are parked and free again
 * when the program closes this file, or ends.
 */
struct shaderemu_gpu_workers {
	__u32 want, mask, root;
};
#define SHADEREMU_GPU_WORKERS	_IOWR('G', 4, struct shaderemu_gpu_workers)
#define WORKERS_CORES	0x3c0	/* control word: how many cores the machine has */
#define WORKERS_MOST_NOW	0x3cc	/* and how many it could have (0: a machine whose cores are as they are) */
/*
 * The geometry (docs/multicore.md): how many worker cores there are and how large each one's
 * write cache is, four bits a worker (the tables' 2^bits buckets, 3 to 6; 0 ends the list),
 * workers 1 to 8 in the first word and 9 to 15 in the second. The workers share 64 rows of the
 * machine's state, and a worker's rows are what its cache needs, so a program chooses
 * between few cores that store much in a pass and many that store little. It can only be
 * set while no program has a worker: EBUSY otherwise.
 */
#define SHADEREMU_GPU_SHAPE	_IOW('G', 6, __u32[2])
#define WORKERS_SHAPE	0x3d0	/* control words: bit 0 while it is laid out anew; the two words */
#define WORKERS_ROWS	64
/*
 * One program at a time draws with the GPU's memory for programs (its textures, vertices and
 * lists are at fixed places there): the program asks for it with this, and has it until it
 * closes the file or ends. While another has it the answer is EBUSY, and that program's
 * number is in the argument.
 */
#define SHADEREMU_GPU_DRAW	_IOR('G', 5, __u32)

#define WORKERS_PHYS	0x86C00000UL	/* the worker cores' mailboxes */
#define WORKERS_STOP	0x5453434d	/* in a core's start word: park it */
#define WORKERS_MOST	16

static void __iomem *gpu_regs;
static void __iomem *workers;
static struct file *worker_owner[WORKERS_MOST];
static DEFINE_MUTEX(workers_lock);
static struct file *draw_owner;
static pid_t draw_pid;
static DEFINE_MUTEX(gpu_lock);

static u32 workers_rows(u32 bits)
{
	return bits ? (44 + 8 + (2u << bits) * 4 + 63) / 64 : 0;
}

/* Lays the workers out: called with workers_lock held and no worker anyone's. */
static long workers_shape(u32 first, u32 second)
{
	u32 most = readl(gpu_regs + WORKERS_MOST_NOW), k, rows = 0, ended = 0;

	if (most < 2)
		return -ENODEV;
	if (most > WORKERS_MOST)
		most = WORKERS_MOST;
	for (k = 1; k < WORKERS_MOST; k++) {
		u32 bits = (k <= 8 ? first >> (4 * (k - 1)) : second >> (4 * (k - 9))) & 15;

		if (!bits) {
			ended = 1;
			continue;
		}
		if (ended || bits < 3 || bits > 6 || k >= most)
			return -EINVAL;
		rows += workers_rows(bits);
	}
	if (rows > WORKERS_ROWS)
		return -EINVAL;
	/*
	 * Every worker parked, then the strip is nothing but zeros for some passes (a worker
	 * with no state is a parked one), and then it is the new workers' rows.
	 */
	for (k = 1; k < WORKERS_MOST; k++)
		writel(WORKERS_STOP, workers + 16 * k);
	writel(1, gpu_regs + WORKERS_SHAPE);
	msleep(60);
	writel(first, gpu_regs + WORKERS_SHAPE + 4);
	writel(second, gpu_regs + WORKERS_SHAPE + 8);
	msleep(60);
	writel(0, gpu_regs + WORKERS_SHAPE);
	msleep(60);
	return 0;
}

/* The shape a machine has until a program asks for another: three workers of the full size,
 * and as many of the smallest kept for work that stores little as there is room for. */
static void workers_default_shape(void)
{
	u32 most = readl(gpu_regs + WORKERS_MOST_NOW), shape[2] = {0, 0}, k, rows = 0;

	if (most > WORKERS_MOST)
		most = WORKERS_MOST;
	for (k = 1; k < most; k++) {
		u32 bits = k <= 3 ? 6 : 4;

		if (rows + workers_rows(bits) > WORKERS_ROWS)
			break;
		rows += workers_rows(bits);
		shape[k > 8] |= bits << (4 * (k <= 8 ? k - 1 : k - 9));
	}
	if (shape[0])
		workers_shape(shape[0], shape[1]);
}

static long shaderemu_gpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (cmd == SHADEREMU_GPU_SHAPE) {
		u32 shape[2], k;
		long answer = 0;

		if (copy_from_user(shape, (void __user *)arg, sizeof(shape)))
			return -EFAULT;
		mutex_lock(&workers_lock);
		for (k = 1; k < WORKERS_MOST; k++)
			if (worker_owner[k])
				answer = -EBUSY;
		if (!answer)
			answer = workers_shape(shape[0], shape[1]);
		mutex_unlock(&workers_lock);
		return answer;
	}
	if (cmd == SHADEREMU_GPU_DRAW) {
		u32 other = 0;
		long answer = 0;

		mutex_lock(&workers_lock);
		if (!draw_owner || draw_owner == file) {
			draw_owner = file;
			draw_pid = task_tgid_vnr(current);
		} else {
			other = (u32)draw_pid;
			answer = -EBUSY;
		}
		mutex_unlock(&workers_lock);
		if (answer && put_user(other, (u32 __user *)arg))
			return -EFAULT;
		return answer;
	}
	if (cmd == SHADEREMU_GPU_WORKERS) {
		struct shaderemu_gpu_workers w;
		u32 cores, k, got = 0;

		if (copy_from_user(&w, (void __user *)arg, sizeof(w)))
			return -EFAULT;
		w.mask = 0;
		w.root = csr_read(CSR_SATP) & 0x3fffff;
		mutex_lock(&workers_lock);
		/* a machine that lays its workers out and has not been told how yet */
		if (readl(gpu_regs + WORKERS_MOST_NOW) > 1 && !readl(gpu_regs + WORKERS_SHAPE + 4))
			workers_default_shape();
		cores = readl(gpu_regs + WORKERS_CORES);
		if (cores > WORKERS_MOST)
			cores = WORKERS_MOST;
		for (k = 1; k < cores && got < w.want; k++)
			if (!worker_owner[k]) {
				worker_owner[k] = file;
				w.mask |= 1u << k;
				got++;
			}
		mutex_unlock(&workers_lock);
		return copy_to_user((void __user *)arg, &w, sizeof(w)) ? -EFAULT : 0;
	}
	if (cmd == SHADEREMU_GPU_SUBMIT) {
		struct shaderemu_gpu_submit s;
		u32 frames;

		if (copy_from_user(&s, (void __user *)arg, sizeof(s)))
			return -EFAULT;
		/* one list at a time, whichever program it is from */
		mutex_lock(&gpu_lock);
		frames = readl(gpu_regs + GPU_FRAMES);
		writel(s.address, gpu_regs + GPU_INTO);
		writel(s.width, gpu_regs + GPU_INTO + 4);
		writel(s.height, gpu_regs + GPU_INTO + 8);
		writel(s.row, gpu_regs + GPU_INTO + 12);
		writel(s.list, gpu_regs + GPU_LIST);
		writel(s.count, gpu_regs + GPU_COUNT);
		writel(s.how, gpu_regs + GPU_SUBMIT);
		while (readl(gpu_regs + GPU_FRAMES) == frames) {
			wait_for_interrupt();
			cond_resched();
		}
		mutex_unlock(&gpu_lock);
		return 0;
	}
	if (cmd != SHADEREMU_GPU_WAIT)
		return -ENOTTY;
	while (readl(gpu_regs + GPU_FRAMES) == (u32)arg) {
		if (signal_pending(current))
			return -ERESTARTSYS;
		wait_for_interrupt();
		cond_resched();
	}
	return 0;
}

static int shaderemu_gpu_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	if (offset > GPU_SIZE || size > GPU_SIZE - offset)
		return -EINVAL;
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
	return remap_pfn_range(vma, vma->vm_start, (GPU_PHYS + offset) >> PAGE_SHIFT, size,
			       vma->vm_page_prot);
}

/* A program that dies leaves the keyboard and the cursor behind: give them back. */
static int shaderemu_gpu_release(struct inode *inode, struct file *file)
{
	writel(0, gpu_regs + KEYBOARD_OWNER);
	writel(0, gpu_regs + DISPLAY_CURSOR);
	writel(0, gpu_regs + VOLUME_LIST + 4);
	writel(0, gpu_regs + VOLUME_LIST);
	/* and its worker cores run in memory that is going away: park them */
	{
		int k;

		mutex_lock(&workers_lock);
		if (draw_owner == file)
			draw_owner = NULL;
		for (k = 1; k < WORKERS_MOST; k++)
			if (worker_owner[k] == file) {
				writel(WORKERS_STOP, workers + 16 * k);
				worker_owner[k] = NULL;
			}
		mutex_unlock(&workers_lock);
	}
	return 0;
}

static const struct file_operations shaderemu_gpu_fops = {
	.owner		= THIS_MODULE,
	.release	= shaderemu_gpu_release,
	.unlocked_ioctl	= shaderemu_gpu_ioctl,
	.mmap		= shaderemu_gpu_mmap,
	.llseek		= noop_llseek,
};

static struct miscdevice shaderemu_gpu_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "gpu",
	.fops	= &shaderemu_gpu_fops,
};

static int __init shaderemu_gpu_init(void)
{
	/* With a device tree that still counts this range as RAM, the kernel is using it. */
	if (pfn_valid(GPU_PHYS >> PAGE_SHIFT)) {
		pr_warn("shaderemu_gpu: 0x%lx is RAM here; not registering\n", GPU_PHYS);
		return -ENODEV;
	}
	gpu_regs = ioremap(GPU_REGS, PAGE_SIZE);
	workers = ioremap(WORKERS_PHYS, PAGE_SIZE);
	if (!gpu_regs || !workers)
		return -ENOMEM;
	pr_info("shaderemu_gpu: /dev/gpu at 0x%lx, %lu KiB\n", GPU_PHYS, GPU_SIZE >> 10);
	return misc_register(&shaderemu_gpu_dev);
}
device_initcall(shaderemu_gpu_init);
