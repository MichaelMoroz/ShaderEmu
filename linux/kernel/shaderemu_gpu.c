// SPDX-License-Identifier: GPL-2.0
/*
 * ShaderEmu GPU device (docs/machine/gpu.md in the ShaderEmu repository): mmap of the GPU's memory,
 * and an ioctl that sleeps with wfi until the submitted list has been drawn.
 */
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/fdtable.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/stat.h>
#include <asm/processor.h>

#define GPU_PHYS	0x80000000UL	/* buffers; the registers are 16 MiB in */
#define GPU_SIZE	0x01b00000UL
#define GPU_REGS	0x81000000UL
#define GPU_SUBMIT	0x10
#define GPU_LIST	0x14
#define GPU_COUNT	0x18
#define GPU_FRAMES	0x1c	/* lists drawn so far */
#define GPU_INTO	0x50	/* where a copy goes: address, width, height, row length */
#define KEYBOARD_OWNER	0x30	/* set by a program that takes the keyboard (docs/machine/input.md) */
#define DISPLAY_CURSOR	0x48	/* 1 while the display shows a cursor (docs/machine/display.md) */
#define VOLUME_LIST	0x300	/* the frame the volume display shows (docs/world/volume.md) */

/* arg: the frame counter's value before submitting */
#define SHADEREMU_GPU_WAIT	_IO('G', 1)

/* arg: a list and what to do with it; returns when it has been drawn and copied */
struct shaderemu_gpu_submit {
	__u32 list, count, how;
	__u32 address, width, height, row;
};
#define SHADEREMU_GPU_SUBMIT	_IOW('G', 2, struct shaderemu_gpu_submit)

/*
 * A program asks for worker cores (docs/machine/multicore.md): up to `want` of those nobody has are
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
 * The workers' layout is the kernel's (workers_layout): these two calls, with which a program
 * once laid them out its own way, do nothing now.
 */
#define SHADEREMU_GPU_SHAPE	_IOW('G', 6, __u32[2])
#define SHADEREMU_GPU_SHAPE_ALL	_IOW('G', 7, __u32[8])
/* { how many it wants, its root, a bit a core it has in eight words } */
#define SHADEREMU_GPU_WORKERS_ALL	_IOWR('G', 8, __u32[10])
#define WORKERS_SHAPE	0x3d0	/* control words: bit 0 while it is laid out anew; how many workers; their size */
#define WORKERS_SIZE	5	/* a worker's write cache: tables of 2^5 buckets, 3 KB of stores a pass */
#define WORKERS_WORDS	8	/* a bit a core */
/*
 * One program at a time draws with the GPU's memory for programs (its textures, vertices and
 * lists are at fixed places there): the program asks for it with this, and has it until it
 * closes the file or ends. While another has it the answer is EBUSY, and that program's
 * number is in the argument.
 */
#define SHADEREMU_GPU_DRAW	_IOR('G', 5, __u32)

/*
 * The worker cores are shared out between the programs that ask (docs/machine/multicore.md, "Shared
 * between programs"). A program says how many it wants with a WORKERS call, as often as it
 * likes, and has its share: the cores divided evenly among the programs that ask, nobody
 * more than it wants, the small wants filled first; a program that only borrows (LENT, said
 * before it asks: a monitor that draws with them) has what the others leave. A core over its
 * program's share is taken back. One with no job (its mailbox's job and answer numbers are
 * the same) is taken at once: it is parked and its word at WORKERS_OWNER, which was its
 * program's tag, is 0, which is how the program's library knows before it posts the next job
 * there (and a program sets its word at WORKERS_POSTING while it writes to a mailbox, during
 * which none of its cores is taken: so it never writes to one that has become another's).
 * One at a job is asked for: its bit is set at WORKERS_RECALL, and the library gives
 * it up when the job has ended (GIVE: eight words, a bit a core). A job is never cut short.
 * The count at WORKERS_OFFER goes up whenever cores come free or the shares change: a
 * program that has fewer than its share asks again then. The WORKERS calls answer with the
 * share in place of the number asked for.
 */
#define SHADEREMU_GPU_LENT	_IO('G', 9)
#define SHADEREMU_GPU_GIVE	_IOW('G', 10, __u32[8])
/* A program with nothing to do until the next pass: shaderemu_gpu_rest(). */
#define SHADEREMU_GPU_REST	_IO('G', 11)
#define WORKERS_RECALL	0x1000
#define WORKERS_OFFER	0x1020
#define WORKERS_POSTING	0x1040	/* a word a program (its tag's low four bits): not 0 while it writes to a mailbox */
#define WORKERS_OWNER	0x1400	/* a word a core: its program's tag, 0 for a core that is nobody's */
#define WORKERS_BOX(k)	(0x4000 + 64 * (k))	/* a core's mailbox: its job, then its answer */
#define WORKERS_PAGES	0x8000	/* the mailboxes: programs/mc/mc.h */
#define WORKERS_CLIENTS	16
#define WORKERS_PHYS	0x80C00000UL	/* the worker cores' mailboxes */
#define WORKERS_STOP	0x5453434d	/* in a core's start word: park it */
#define WORKERS_MOST	256
/* in a free part of the mailbox pages: the window system's words for the pointer's driver
 * (shaderemu_input.c), of which the third is its worker core and the fourth a mark */
#define WORKERS_WM	0x1800
#define WM_MAGIC	0x31514d57
/* the lanes' field's job (docs/machine/lanes.md): its descriptor's address, its number, its lanes, its poster's pid */
#define FIELD_JOB	0x1900

static void __iomem *gpu_regs;
static void __iomem *workers;
void __iomem *shaderemu_gpu_wm;
static struct file *worker_owner[WORKERS_MOST];
/* the programs that have asked for workers: how many each wants, and its share of them now */
static struct worker_client {
	struct file *file;
	u32 want, share, tag;
	bool lent, settled;
} worker_clients[WORKERS_CLIENTS];
static u32 worker_tags;		/* tags given so far: no two programs ever have the same */
static DEFINE_MUTEX(workers_lock);
static struct file *draw_owner;
static pid_t draw_pid;
/* who has /dev/gpu open and how many lists each has had drawn: /proc/gpu, for a monitor */
#define GPU_USERS	32
static struct gpu_user {
	struct file *file;
	pid_t pid;
	u32 lists;
} gpu_users[GPU_USERS];
static DEFINE_MUTEX(gpu_lock);

/*
 * The workers' layout, set once, at boot (below) or the first time a program asks: as many as the
 * machine has cores for, all alike, so that any program can have any of them (the cores are
 * shared out between programs, below). Every worker parked, then the machine's strip is
 * nothing but zeros for some passes, and then it is the workers'.
 */
static void workers_layout(void)
{
	u32 most = readl(gpu_regs + WORKERS_MOST_NOW), k;

	if (most > WORKERS_MOST)
		most = WORKERS_MOST;
	if (most < 2)
		return;
	for (k = 1; k < WORKERS_MOST; k++)
		writel(WORKERS_STOP, workers + 16 * k);
	writel(1, gpu_regs + WORKERS_SHAPE);
	msleep(60);
	writel(most - 1, gpu_regs + WORKERS_SHAPE + 4);
	writel(WORKERS_SIZE, gpu_regs + WORKERS_SHAPE + 8);
	msleep(60);
	writel(0, gpu_regs + WORKERS_SHAPE);
	msleep(60);
}

/*
 * The layout made at boot, in the background, not when the first program asks: its three sleeps
 * of 60 ms were paid by the first program with workers and by the window server at the
 * pointer's first move. Tried again every 100 ms, a few times, while the host
 * has not yet said how many cores there could be; a program that asks first lays them out itself.
 */
static void workers_layout_boot(struct work_struct *work);
static DECLARE_DELAYED_WORK(workers_layout_work, workers_layout_boot);

static void workers_layout_boot(struct work_struct *work)
{
	static int tries;
	bool again;

	mutex_lock(&workers_lock);
	again = readl(gpu_regs + WORKERS_MOST_NOW) < 2;
	if (!again && !readl(gpu_regs + WORKERS_SHAPE + 4))
		workers_layout();
	mutex_unlock(&workers_lock);
	if (again && ++tries < 20)
		schedule_delayed_work(&workers_layout_work, HZ / 10);
}

/* Everything below is called with workers_lock held. */

static u32 workers_cores(void)
{
	u32 cores = readl(gpu_regs + WORKERS_CORES);

	return cores > WORKERS_MOST ? WORKERS_MOST : cores;
}

static struct worker_client *workers_client(struct file *file, bool make)
{
	int i;

	for (i = 0; i < WORKERS_CLIENTS; i++)
		if (worker_clients[i].file == file)
			return &worker_clients[i];
	for (i = 0; make && i < WORKERS_CLIENTS; i++)
		if (!worker_clients[i].file) {
			memset(&worker_clients[i], 0, sizeof(worker_clients[i]));
			worker_clients[i].file = file;
			/* (no two programs ever have the same; its low four bits are its place here) */
			worker_clients[i].tag = (++worker_tags & 0xffff) << 4 | (u32)i;
			writel(0, workers + WORKERS_POSTING + 4 * i);
			return &worker_clients[i];
		}
	return NULL;
}

static u32 workers_of(struct file *file)
{
	u32 k, n = 0;

	for (k = 1; k < WORKERS_MOST; k++)
		n += worker_owner[k] == file;
	return n;
}

/* A core that is somebody's and has no job: what its program posted last, it has answered,
 * and the program has taken the answer (the third word of the mailbox's last texel). */
static bool workers_idle(u32 k)
{
	u32 job = readl(workers + WORKERS_BOX(k));

	return job == readl(workers + WORKERS_BOX(k) + 16) && job == readl(workers + WORKERS_BOX(k) + 56);
}

static void workers_park(u32 k)
{
	/* the window system's core for the pointer: every move of the pointer is reported again */
	if (shaderemu_gpu_wm && readl(shaderemu_gpu_wm + 8) == k && readl(shaderemu_gpu_wm + 12) == WM_MAGIC)
		writel(0, shaderemu_gpu_wm + 12);
	writel(WORKERS_STOP, workers + 16 * k);
	writel(0, workers + WORKERS_OWNER + 4 * k);
	worker_owner[k] = NULL;
}

/*
 * For the pointer's driver, from the tick: core k's job word changes, which wakes a worker that
 * sleeps on it, and its answer and "taken" words with it: a core with no job (workers_idle),
 * as the window system's for the pointer is to everybody else.
 */
void shaderemu_gpu_wake(u32 k)
{
	u32 seq;

	if (!workers || k - 1 >= WORKERS_MOST - 1)
		return;
	seq = readl(workers + WORKERS_BOX(k)) + 1;
	writel(seq, workers + WORKERS_BOX(k));
	writel(seq, workers + WORKERS_BOX(k) + 16);
	writel(seq, workers + WORKERS_BOX(k) + 56);
}

static void workers_offer(void)
{
	writel(readl(workers + WORKERS_OFFER) + 1, workers + WORKERS_OFFER);
}

/*
 * Every program's share, and the cores that are over one asked back. The programs that keep
 * their workers divide the cores first, then the borrowers what is left: evenly, except that
 * who wants less than an even part has what it wants and the others divide the rest.
 */
static void workers_balance(void)
{
	u32 cores = workers_cores(), left = cores > 1 ? cores - 1 : 0, recall[WORKERS_WORDS] = {0}, k;
	struct worker_client *c;
	bool freed = false;
	int lent, i;

	for (i = 0; i < WORKERS_CLIENTS; i++) {
		worker_clients[i].share = 0;
		worker_clients[i].settled = !worker_clients[i].file || !worker_clients[i].want;
	}
	for (lent = 0; lent < 2; lent++) {
		for (;;) {
			u32 askers = 0, each, done = 0;

			for (i = 0; i < WORKERS_CLIENTS; i++)
				askers += !worker_clients[i].settled && worker_clients[i].lent == lent;
			if (!askers)
				break;
			each = left / askers;
			for (i = 0; i < WORKERS_CLIENTS; i++) {
				c = &worker_clients[i];
				if (c->settled || c->lent != lent || c->want > each)
					continue;
				c->share = c->want;
				c->settled = true;
				left -= c->share;
				done = 1;
			}
			if (done)
				continue;
			/* they all want more than an even part: that part each, the odd cores to the first */
			for (i = 0; i < WORKERS_CLIENTS; i++) {
				c = &worker_clients[i];
				if (c->settled || c->lent != lent)
					continue;
				c->share = each + (left % askers ? 1 : 0);
				c->settled = true;
				if (left % askers)
					left--;
				left -= each;
				askers--;
			}
			break;
		}
	}
	/* of a program with more than its share, the last cores it has are the ones to come
	 * back: at once if they have no job, when their job ends if they have */
	for (i = 0; i < WORKERS_CLIENTS; i++) {
		u32 over;

		c = &worker_clients[i];
		if (!c->file)
			continue;
		over = workers_of(c->file);
		over = over > c->share ? over - c->share : 0;
		for (k = WORKERS_MOST - 1; k >= 1 && over; k--) {
			if (worker_owner[k] != c->file)
				continue;
			if (workers_idle(k) && !readl(workers + WORKERS_POSTING + 4 * i)) {
				workers_park(k);
				freed = true;
			} else {
				recall[k / 32] |= 1u << (k % 32);
			}
			over--;
		}
	}
	if (freed)
		workers_offer();
	for (k = 0; k < WORKERS_WORDS; k++)
		writel(recall[k], workers + WORKERS_RECALL + 4 * k);
}

/* Whether any core is still on its way back. */
static bool workers_recalled(void)
{
	u32 k, any = 0;

	for (k = 0; k < WORKERS_WORDS; k++)
		any |= readl(workers + WORKERS_RECALL + 4 * k);
	return any != 0;
}

/*
 * `file` wants `want` workers (of the cores below `below`): its share of them, waited for half
 * a second at most while cores over the others' shares come back. A bit a core it has, after;
 * the share is what it returns.
 */
static u32 workers_take(struct file *file, u32 want, u32 below, u32 *mask)
{
	struct worker_client *c = workers_client(file, true);
	u32 cores, k;
	int tries;

	memset(mask, 0, 4 * WORKERS_WORDS);
	if (c) {
		c->want = want;
		if (readl(gpu_regs + WORKERS_MOST_NOW) > 1 && !readl(gpu_regs + WORKERS_SHAPE + 4))
			workers_layout();
		workers_balance();
		for (tries = 0; tries < 50; tries++) {
			u32 have = workers_of(file);

			cores = workers_cores();
			if (cores > below)
				cores = below;
			for (k = 1; k < cores && have < c->share; k++)
				if (!worker_owner[k]) {
					u32 box = WORKERS_BOX(k), i;

					/* its mailbox as new, and whose it is */
					for (i = 0; i < 64; i += 4)
						writel(0, workers + box + i);
					writel(0, workers + 16 * k);
					writel(c->tag, workers + WORKERS_OWNER + 4 * k);
					worker_owner[k] = file;
					have++;
				}
			if (have >= c->share || !workers_recalled())
				break;
			mutex_unlock(&workers_lock);
			msleep(10);
			mutex_lock(&workers_lock);
			workers_balance();
		}
	}
	for (k = 1; k < WORKERS_MOST; k++)
		if (worker_owner[k] == file)
			mask[k / 32] |= 1u << (k % 32);
	return c ? c->share | c->tag << 12 : 0;
}

/* The cores of `mask` that are `file`'s (all of its, for a file that is closed) are nobody's again. */
static bool workers_give(struct file *file, const u32 *mask)
{
	bool had = false;
	u32 k;

	for (k = 1; k < WORKERS_MOST; k++)
		if (worker_owner[k] == file && (!mask || (mask[k / 32] >> (k % 32) & 1))) {
			workers_park(k);
			had = true;
		}
	return had;
}

/*
 * The core is given away for what is left of this pass (docs/machine/multicore.md, "A core that
 * rests"): to anything else that can run, and with nothing to run the pass ends here, as it
 * would in the idle loop, whose work is done here too. It returns in a later pass.
 */
static noinline void shaderemu_gpu_rest(void)
{
	extern void zeroed_idle(void);	/* mm/page_alloc.c, the zero pool */
	unsigned long pass = csr_read(CSR_TIME);	/* (one value a pass) */

	if (!single_task_running())
		yield();
	if (csr_read(CSR_TIME) == pass)
		zeroed_idle();	/* (a block cleared ahead ends the pass too) */
	if (csr_read(CSR_TIME) == pass)
		__asm__ __volatile__(".word 0x0100000f");	/* pause: the machine's pass ends */
	arch_cpu_idle_exit();
}

static long shaderemu_gpu_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	if (cmd == SHADEREMU_GPU_REST) {
		shaderemu_gpu_rest();
		return 0;
	}
	if (cmd == SHADEREMU_GPU_LENT) {
		struct worker_client *c;

		mutex_lock(&workers_lock);
		c = workers_client(file, true);
		if (c)
			c->lent = true;
		mutex_unlock(&workers_lock);
		return c ? 0 : -ENOMEM;
	}
	if (cmd == SHADEREMU_GPU_GIVE) {
		u32 mask[WORKERS_WORDS];

		if (copy_from_user(mask, (void __user *)arg, sizeof(mask)))
			return -EFAULT;
		mutex_lock(&workers_lock);
		if (workers_give(file, mask)) {
			workers_balance();
			workers_offer();
		}
		mutex_unlock(&workers_lock);
		return 0;
	}
	/* (a program built when the workers could be laid out its own way is told that they are) */
	if (cmd == SHADEREMU_GPU_SHAPE || cmd == SHADEREMU_GPU_SHAPE_ALL)
		return 0;
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
	if (cmd == SHADEREMU_GPU_WORKERS_ALL) {
		u32 w[2 + WORKERS_WORDS];

		if (copy_from_user(w, (void __user *)arg, sizeof(w)))
			return -EFAULT;
		w[1] = csr_read(CSR_SATP) & 0x3fffff;
		mutex_lock(&workers_lock);
		w[0] = workers_take(file, w[0], WORKERS_MOST, &w[2]);
		mutex_unlock(&workers_lock);
		return copy_to_user((void __user *)arg, w, sizeof(w)) ? -EFAULT : 0;
	}
	if (cmd == SHADEREMU_GPU_WORKERS) {
		struct shaderemu_gpu_workers w;
		u32 mask[WORKERS_WORDS];

		if (copy_from_user(&w, (void __user *)arg, sizeof(w)))
			return -EFAULT;
		w.root = csr_read(CSR_SATP) & 0x3fffff;
		mutex_lock(&workers_lock);
		w.want = workers_take(file, w.want, 16, mask) & 0xfff;
		mutex_unlock(&workers_lock);
		w.mask = mask[0] & 0xffff;
		return copy_to_user((void __user *)arg, &w, sizeof(w)) ? -EFAULT : 0;
	}
	if (cmd == SHADEREMU_GPU_SUBMIT) {
		struct shaderemu_gpu_submit s;
		u32 frames;
		int u;

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
		for (u = 0; u < GPU_USERS; u++)
			if (gpu_users[u].file == file)
				gpu_users[u].lists++;
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
	/* accessed and written from the start: the machine's walk faults on a clear bit, and a
	 * worker's first store to a page stopped it until the first core served it */
	return remap_pfn_range(vma, vma->vm_start, (GPU_PHYS + offset) >> PAGE_SHIFT, size,
			       __pgprot(pgprot_val(vma->vm_page_prot) | _PAGE_ACCESSED |
					((vma->vm_flags & VM_WRITE) ? _PAGE_DIRTY : 0)));
}

/* Which of the keyboard, the cursor and the volume list were nobody's when this file was
 * opened: only those can be its program's, and only those it gives back. */
static int shaderemu_gpu_open(struct inode *inode, struct file *file)
{
	int i;

	mutex_lock(&workers_lock);
	for (i = 0; i < GPU_USERS; i++)
		if (!gpu_users[i].file) {
			gpu_users[i].file = file;
			gpu_users[i].pid = task_tgid_vnr(current);
			gpu_users[i].lists = 0;
			break;
		}
	mutex_unlock(&workers_lock);
	file->private_data = (void *)(uintptr_t)((readl(gpu_regs + KEYBOARD_OWNER) ? 0 : 1) | (readl(gpu_regs + DISPLAY_CURSOR) ? 0 : 2) |
						 (readl(gpu_regs + VOLUME_LIST) ? 0 : 4));
	return 0;
}

/* A program that dies leaves the keyboard and the cursor behind: give them back. */
static int shaderemu_gpu_release(struct inode *inode, struct file *file)
{
	uintptr_t free_at_open = (uintptr_t)file->private_data;
	bool others = false;

	if (free_at_open & 1)
		writel(0, gpu_regs + KEYBOARD_OWNER);
	if (free_at_open & 4) {
		writel(0, gpu_regs + VOLUME_LIST + 4);
		writel(0, gpu_regs + VOLUME_LIST);
	}
	/* and its worker cores run in memory that is going away: park them */
	{
		int i;

		mutex_lock(&workers_lock);
		for (i = 0; i < GPU_USERS; i++)
			if (gpu_users[i].file == file)
				gpu_users[i].file = NULL;
			else if (gpu_users[i].file)
				others = true;
		/* the window system's pointer core writes the cursor's texel in the same pass:
		 * only the last program out turns the cursor off */
		if ((free_at_open & 2) && !others)
			writel(0, gpu_regs + DISPLAY_CURSOR);
		if (draw_owner == file)
			draw_owner = NULL;
		/* the field's job of a program that dies with it out: its lanes read and write that memory */
		if ((current->flags & PF_EXITING) && readl(workers + FIELD_JOB) &&
		    readl(workers + FIELD_JOB + 12) == (u32)task_tgid_vnr(current))
			writel(0, workers + FIELD_JOB);
		{
			struct worker_client *c = workers_client(file, false);
			bool had = workers_give(file, NULL);

			if (c)
				c->file = NULL;
			if (had || c) {
				workers_balance();
				workers_offer();
			}
		}
		mutex_unlock(&workers_lock);
	}
	return 0;
}

/* A line an open of /dev/gpu: its process, worker cores held and wanted, whether it only
 * borrows them, its tag (a core's word at WORKERS_OWNER), whether it has the GPU's memory
 * for programs, and lists drawn so far. */
static int shaderemu_gpu_show(struct seq_file *m, void *v)
{
	int i;

	seq_puts(m, "pid workers wanted lent tag draws lists\n");
	mutex_lock(&workers_lock);
	for (i = 0; i < GPU_USERS; i++) {
		struct gpu_user *u = &gpu_users[i];
		struct worker_client *c;

		if (!u->file)
			continue;
		c = workers_client(u->file, false);
		seq_printf(m, "%d %u %u %d %u %d %u\n", u->pid, workers_of(u->file), c ? c->want : 0, c ? c->lent : 0, c ? c->tag : 0,
			   draw_owner == u->file, u->lists);
	}
	mutex_unlock(&workers_lock);
	return 0;
}

static const struct file_operations shaderemu_gpu_fops = {
	.owner		= THIS_MODULE,
	.open		= shaderemu_gpu_open,
	.release	= shaderemu_gpu_release,
	.unlocked_ioctl	= shaderemu_gpu_ioctl,
	.mmap		= shaderemu_gpu_mmap,
	.llseek		= noop_llseek,
};

/*
 * A program's memory is about to go (do_exit, before exit_mm: linux/nommu/patch_exitpark.py), and its
 * worker cores run in it. A worker reads its start word as its pass begins: one that began this pass
 * runs it whole and its stores are committed when the pass ends, into what was the program's. Its
 * cores are parked here and the pass is ended, so that they have stored all they will before the
 * memory is freed, and the next pass finds them parked. (Parked at release, after exit_mm, they ran
 * on in freed memory through the passes between.)
 */
static int shaderemu_gpu_exiting_one(const void *parked, struct file *file, unsigned int fd)
{
	u32 k;

	if (file->f_op != &shaderemu_gpu_fops)
		return 0;
	for (k = 1; k < WORKERS_MOST; k++)
		if (worker_owner[k] == file) {
			workers_park(k);
			*(bool *)parked = true;
		}
	/* the lanes' field's job of this program: its lanes read and write that memory too */
	if (readl(workers + FIELD_JOB) && readl(workers + FIELD_JOB + 12) == (u32)task_tgid_vnr(current)) {
		writel(0, workers + FIELD_JOB);
		*(bool *)parked = true;
	}
	return 0;
}

void shaderemu_gpu_exiting(void)
{
	bool parked = false;
	unsigned long pass;
	u32 k;

	if (!workers || !current->mm || atomic_read(&current->mm->mm_users) != 1 || !current->files)
		return;
	mutex_lock(&workers_lock);
	iterate_fd(current->files, 0, shaderemu_gpu_exiting_one, &parked);
	mutex_unlock(&workers_lock);
	if (!parked)
		return;
	pass = csr_read(CSR_TIME);	/* (one value a pass) */
	while (csr_read(CSR_TIME) == pass)
		__asm__ __volatile__(".word 0x0100000f" ::: "memory");	/* pause: the machine's pass ends */
	/* Parked now, they store nothing more: what they last said is taken back. A parked core's answer
	 * still said MC_ALIVE, which the next program to start it took for its own worker there at once
	 * (programs/mc/mcw.c, arrived()): the start word taken away before the core had seen it, or a job
	 * "done" by the old answer's number. */
	mutex_lock(&workers_lock);
	for (k = 1; k < WORKERS_MOST; k++)
		if (readl(workers + 16 * k) == WORKERS_STOP)
			writel(0, workers + WORKERS_BOX(k) + 24);
	mutex_unlock(&workers_lock);
}

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
	workers = ioremap(WORKERS_PHYS, WORKERS_PAGES);
	if (!gpu_regs || !workers)
		return -ENOMEM;
	shaderemu_gpu_wm = workers + WORKERS_WM;
	pr_info("shaderemu_gpu: /dev/gpu at 0x%lx, %lu KiB\n", GPU_PHYS, GPU_SIZE >> 10);
	proc_create_single("gpu", 0444, NULL, shaderemu_gpu_show);
	schedule_delayed_work(&workers_layout_work, 0);
	return misc_register(&shaderemu_gpu_dev);
}
device_initcall(shaderemu_gpu_init);
