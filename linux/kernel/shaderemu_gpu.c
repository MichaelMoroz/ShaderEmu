// SPDX-License-Identifier: GPL-2.0
/*
 * ShaderEmu GPU device (docs/gpu.md in the ShaderEmu repository): mmap of the GPU's memory,
 * and an ioctl that sleeps with wfi until the submitted list has been drawn.
 */
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

static void __iomem *gpu_regs;
static DEFINE_MUTEX(gpu_lock);

static long shaderemu_gpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
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
	if (!gpu_regs)
		return -ENOMEM;
	pr_info("shaderemu_gpu: /dev/gpu at 0x%lx, %lu KiB\n", GPU_PHYS, GPU_SIZE >> 10);
	return misc_register(&shaderemu_gpu_dev);
}
device_initcall(shaderemu_gpu_init);
