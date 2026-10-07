// SPDX-License-Identifier: GPL-2.0
/*
 * ShaderEmu sound card (docs/sound.md in the ShaderEmu repository). Programs map the voices'
 * registers and write them themselves; this hands out voices and sample memory, takes both
 * back when a program goes, and plays PCM written to the device through a stream voice.
 */
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <asm/processor.h>

#define SND_REGS	0x87000000UL	/* the machine's control words: one page */
#define SND_MEM		0x87b00000UL	/* samples, rings, tracks */
#define SND_MEM_SIZE	0x00300000UL
#define SND_CHUNK	0x10000UL	/* what memory is handed out in */
#define SND_CHUNKS	(SND_MEM_SIZE / SND_CHUNK)
#define SND_ENABLE	0x220
#define SND_MASTER	0x224
#define SND_COUNT	0x228
#define SND_CLOCK	0x230	/* samples played so far */
#define SND_RATE	0x234	/* output rate; the device writes it once it mixes */
#define SND_VOICE(n)	(0x800 + 32 * (n))	/* key, kind, address, length, loop, step, volume, aux */
#define SND_STATE(n)	(0xc00 + 32 * (n))	/* seen, state, position, ... */
#define SND_VOICES	32
#define KIND_STREAM	3
#define KIND_SMOOTH	0x200
#define KIND_STEREO	0x400

/* out: a voice that is the caller's until it frees it or closes the device */
#define SHADEREMU_SOUND_CLAIM	_IOR('S', 1, __u32)
#define SHADEREMU_SOUND_FREE	_IOW('S', 2, __u32)
/* in: bytes wanted; out: their physical address (mmap offset: address - 0x87000000) */
struct shaderemu_sound_memory {
	__u32 bytes, address;
};
#define SHADEREMU_SOUND_MEMORY	_IOWR('S', 3, struct shaderemu_sound_memory)
/* what write() takes: 16-bit samples at this rate, 1 or 2 channels (default 22050, 1) */
struct shaderemu_sound_format {
	__u32 rate, channels;
};
#define SHADEREMU_SOUND_FORMAT	_IOW('S', 4, struct shaderemu_sound_format)
/* arg: a value of the sample clock; returns once the clock has another */
#define SHADEREMU_SOUND_WAIT	_IO('S', 5)

struct snd_file {
	u32 voices;		/* one bit each */
	int stream;		/* the voice write() plays through, or -1 */
	u32 ring, frames;	/* its ring: offset in sound memory, length in sample frames */
	u32 written;
	u32 rate, channels;
};

static void __iomem *snd_regs;
static void *snd_mem;
static DEFINE_MUTEX(snd_lock);
static u32 snd_claimed;
static struct snd_file *snd_owner[SND_CHUNKS];
static int snd_opens;

/* One frame of the machine: nothing the device does can be seen before it ends. */
static void snd_frame(void)
{
	wait_for_interrupt();
	cond_resched();
}

static int snd_claim(struct snd_file *f)
{
	int n;

	mutex_lock(&snd_lock);
	if (snd_claimed == ~0u) {
		mutex_unlock(&snd_lock);
		return -EBUSY;
	}
	n = ffz(snd_claimed);
	snd_claimed |= 1u << n;
	f->voices |= 1u << n;
	mutex_unlock(&snd_lock);
	return n;
}

/* A new key with bit 0 clear stops the voice. */
static void snd_free(struct snd_file *f, u32 voices)
{
	int n;

	mutex_lock(&snd_lock);
	voices &= f->voices;
	for (n = 0; n < SND_VOICES; n++)
		if (voices & (1u << n))
			writel(((readl(snd_regs + SND_VOICE(n)) >> 1) + 1) << 1, snd_regs + SND_VOICE(n));
	f->voices &= ~voices;
	snd_claimed &= ~voices;
	mutex_unlock(&snd_lock);
}

/* The offset in sound memory of `bytes` nobody has, or -ENOMEM. */
static long snd_memory(struct snd_file *f, u32 bytes)
{
	unsigned int want = DIV_ROUND_UP(bytes, SND_CHUNK), at, run = 0;
	long found = -ENOMEM;

	if (!want || want > SND_CHUNKS)
		return -EINVAL;
	mutex_lock(&snd_lock);
	for (at = 0; at < SND_CHUNKS; at++) {
		run = snd_owner[at] ? 0 : run + 1;
		if (run == want) {
			for (found = at + 1 - want; want; want--)
				snd_owner[at--] = f;
			found *= SND_CHUNK;
			break;
		}
	}
	mutex_unlock(&snd_lock);
	return found;
}

static long shaderemu_sound_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct snd_file *f = file->private_data;
	struct shaderemu_sound_memory m;
	struct shaderemu_sound_format fmt;
	u32 n;
	long at;

	switch (cmd) {
	case SHADEREMU_SOUND_CLAIM:
		at = snd_claim(f);
		if (at < 0)
			return at;
		n = at;
		return put_user(n, (__u32 __user *)arg);
	case SHADEREMU_SOUND_FREE:
		if (get_user(n, (__u32 __user *)arg))
			return -EFAULT;
		if (n >= SND_VOICES || (int)n == f->stream)
			return -EINVAL;
		snd_free(f, 1u << n);
		return 0;
	case SHADEREMU_SOUND_MEMORY:
		if (copy_from_user(&m, (void __user *)arg, sizeof(m)))
			return -EFAULT;
		at = snd_memory(f, m.bytes);
		if (at < 0)
			return at;
		m.address = SND_MEM + at;
		return copy_to_user((void __user *)arg, &m, sizeof(m)) ? -EFAULT : 0;
	case SHADEREMU_SOUND_FORMAT:
		if (copy_from_user(&fmt, (void __user *)arg, sizeof(fmt)))
			return -EFAULT;
		if (f->stream >= 0 || !fmt.rate || fmt.rate > 192000 || fmt.channels < 1 || fmt.channels > 2)
			return -EINVAL;
		f->rate = fmt.rate;
		f->channels = fmt.channels;
		return 0;
	case SHADEREMU_SOUND_WAIT:
		while (readl(snd_regs + SND_CLOCK) == (u32)arg) {
			if (signal_pending(current))
				return -ERESTARTSYS;
			snd_frame();
		}
		return 0;
	}
	return -ENOTTY;
}

/* Where the device is in the stream; nothing before it has seen the voice's key. */
static u32 snd_stream_at(struct snd_file *f)
{
	if (readl(snd_regs + SND_STATE(f->stream)) != readl(snd_regs + SND_VOICE(f->stream)))
		return 0;
	return readl(snd_regs + SND_STATE(f->stream) + 8);
}

static int snd_stream_start(struct snd_file *f)
{
	int v = snd_claim(f);
	long at;
	void __iomem *voice;

	if (v < 0)
		return v;
	at = snd_memory(f, SND_CHUNK);
	if (at < 0) {
		snd_free(f, 1u << v);
		return at;
	}
	f->ring = at;
	f->frames = SND_CHUNK / (2 * f->channels);
	f->written = 0;
	voice = snd_regs + SND_VOICE(v);
	writel(KIND_STREAM | KIND_SMOOTH | (f->channels == 2 ? KIND_STEREO : 0), voice + 4);
	writel(SND_MEM + at, voice + 8);
	writel(f->frames, voice + 12);
	writel(0, voice + 16);
	writel(div_u64((u64)f->rate << 16, readl(snd_regs + SND_RATE)), voice + 20);
	writel(256 | 256 << 16, voice + 24);
	writel(((readl(voice) >> 1) + 1) << 1 | 1, voice);
	f->stream = v;
	return 0;
}

/* PCM in the format set: into the ring as far as it has room, waiting for the device while full. */
static ssize_t shaderemu_sound_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	struct snd_file *f = file->private_data;
	u32 frame = 2 * f->channels;
	size_t left = count / frame;
	int err;

	if (f->stream < 0) {
		err = snd_stream_start(f);
		if (err)
			return err;
	}
	while (left) {
		u32 room = f->frames - (f->written - snd_stream_at(f));
		u32 at = f->written & (f->frames - 1);
		u32 n = min3(room, (u32)left, f->frames - at);

		if (!n) {
			if (signal_pending(current))
				return count - left * frame ? : -ERESTARTSYS;
			snd_frame();
			continue;
		}
		if (copy_from_user(snd_mem + f->ring + at * frame, buf, n * frame))
			return -EFAULT;
		buf += n * frame;
		left -= n;
		f->written += n;
		writel(f->written, snd_regs + SND_VOICE(f->stream) + 16);
	}
	return count;
}

static int shaderemu_sound_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned long mem = SND_MEM - SND_REGS;

	/* the registers' page, or sound memory */
	if (!(offset == 0 && size <= PAGE_SIZE) &&
	    !(offset >= mem && offset - mem <= SND_MEM_SIZE && size <= SND_MEM_SIZE - (offset - mem)))
		return -EINVAL;
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
	return remap_pfn_range(vma, vma->vm_start, (SND_REGS + offset) >> PAGE_SHIFT, size, vma->vm_page_prot);
}

static int shaderemu_sound_open(struct inode *inode, struct file *file)
{
	struct snd_file *f = kzalloc(sizeof(*f), GFP_KERNEL);
	unsigned long until = jiffies + HZ / 2;
	int first;

	if (!f)
		return -ENOMEM;
	f->stream = -1;
	f->rate = 22050;
	f->channels = 1;
	mutex_lock(&snd_lock);
	first = snd_opens++ == 0;
	mutex_unlock(&snd_lock);
	if (first) {
		writel(256, snd_regs + SND_MASTER);
		writel(SND_VOICES, snd_regs + SND_COUNT);
		writel(1, snd_regs + SND_ENABLE);
	}
	/* a host with a sound card says so within a few frames */
	while (!readl(snd_regs + SND_RATE) && time_before(jiffies, until))
		snd_frame();
	if (!readl(snd_regs + SND_RATE)) {
		mutex_lock(&snd_lock);
		if (--snd_opens == 0)
			writel(0, snd_regs + SND_ENABLE);
		mutex_unlock(&snd_lock);
		kfree(f);
		return -ENODEV;
	}
	file->private_data = f;
	return 0;
}

/* What was written is played to its end; then the program's voices stop and its memory is free. */
static int shaderemu_sound_release(struct inode *inode, struct file *file)
{
	struct snd_file *f = file->private_data;
	unsigned long until = jiffies + 30 * HZ;
	unsigned int at;

	while (f->stream >= 0 && snd_stream_at(f) != f->written && !signal_pending(current) && time_before(jiffies, until))
		snd_frame();
	snd_free(f, f->voices);
	mutex_lock(&snd_lock);
	for (at = 0; at < SND_CHUNKS; at++)
		if (snd_owner[at] == f)
			snd_owner[at] = NULL;
	if (--snd_opens == 0)
		writel(0, snd_regs + SND_ENABLE);
	mutex_unlock(&snd_lock);
	kfree(f);
	return 0;
}

static const struct file_operations shaderemu_sound_fops = {
	.owner		= THIS_MODULE,
	.open		= shaderemu_sound_open,
	.release	= shaderemu_sound_release,
	.write		= shaderemu_sound_write,
	.unlocked_ioctl	= shaderemu_sound_ioctl,
	.mmap		= shaderemu_sound_mmap,
	.llseek		= noop_llseek,
};

static struct miscdevice shaderemu_sound_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "sound",
	.fops	= &shaderemu_sound_fops,
};

static int __init shaderemu_sound_init(void)
{
	/* With a device tree that still counts this range as RAM, the kernel is using it. */
	if (pfn_valid(SND_REGS >> PAGE_SHIFT)) {
		pr_warn("shaderemu_sound: 0x%lx is RAM here; not registering\n", SND_REGS);
		return -ENODEV;
	}
	snd_regs = ioremap(SND_REGS, PAGE_SIZE);
	snd_mem = (void __force *)ioremap(SND_MEM, SND_MEM_SIZE);
	if (!snd_regs || !snd_mem)
		return -ENOMEM;
	pr_info("shaderemu_sound: /dev/sound, %lu KiB of sample memory at 0x%lx\n", SND_MEM_SIZE >> 10, SND_MEM);
	return misc_register(&shaderemu_sound_dev);
}
device_initcall(shaderemu_sound_init);
