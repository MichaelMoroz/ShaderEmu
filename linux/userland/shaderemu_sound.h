/*
 * The sound card from a Linux program (docs/sound.md): /dev/sound hands out voices and sample
 * memory, and maps the registers that programs/common/sound.h describes. The device plays
 * what is at a physical address: sound memory (snd_memory) or a file in the ROM (snd_rom).
 */
#pragma once

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static char *snd_page;   /* the machine's control words, once snd_open() has mapped them */
#define SND_BASE ((uintptr_t)snd_page)
#include "../../programs/common/sound.h"

struct snd_memory_request {
	uint32_t bytes, address;
};
struct snd_format {
	uint32_t rate, channels;
};
#define SND_IOCTL_CLAIM  _IOR('S', 1, uint32_t)
#define SND_IOCTL_FREE   _IOW('S', 2, uint32_t)
#define SND_IOCTL_MEMORY _IOWR('S', 3, struct snd_memory_request)
#define SND_IOCTL_FORMAT _IOW('S', 4, struct snd_format)   /* what write() takes: 16-bit, default 22050 Hz mono */
#define SND_IOCTL_WAIT   _IO('S', 5)

static int snd_fd = -1;

/* 0, or -1 when the machine or its host has no sound card. */
static inline int snd_open(void)
{
	void *page;

	if (snd_fd >= 0)
		return 0;
	if ((snd_fd = open("/dev/sound", O_RDWR | O_CLOEXEC)) < 0)
		return -1;
	page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, snd_fd, 0);
	if (page == MAP_FAILED) {
		close(snd_fd);
		snd_fd = -1;
		return -1;
	}
	snd_page = (char *)page;
	return 0;
}

/* A voice that is this program's until it ends, or -1. */
static inline int snd_claim(void)
{
	uint32_t v;

	return ioctl(snd_fd, SND_IOCTL_CLAIM, &v) ? -1 : (int)v;
}

/* Memory the device can play from: where the program sees it, and its physical address. */
static inline void *snd_memory(uint32_t bytes, uint32_t *address)
{
	struct snd_memory_request m = { bytes, 0 };
	void *at;

	if (ioctl(snd_fd, SND_IOCTL_MEMORY, &m))
		return NULL;
	at = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, snd_fd, m.address - 0x87000000u);
	*address = m.address;
	return at == MAP_FAILED ? NULL : at;
}

/* Sleeps until the device has mixed again: its clock and its voices' words have moved. */
static inline void snd_wait(void)
{
	ioctl(snd_fd, SND_IOCTL_WAIT, SND_CLOCK);
}

/*
 * The physical address of an open file's first byte when the file is in the ROM, else 0.
 * romfs numbers a file by where its header is: 16 bytes, its name, each padded to 16, then
 * the data in one piece. `begins` is how the file starts, to tell a chance number apart.
 */
static inline uint32_t snd_rom(int fd, const char *begins, int count)
{
	struct stat st;
	unsigned char head[16 + 256], data[64];
	uint32_t at;
	int mtd, n;

	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || count > (int)sizeof data || (mtd = open("/dev/mtd0", O_RDONLY)) < 0)
		return 0;
	n = pread(mtd, head, sizeof head, st.st_ino);
	for (at = 16; (int)at < n && head[at]; at++)
		continue;
	if (n < 32 || (int)at >= n ||
	    ((uint32_t)head[8] << 24 | head[9] << 16 | head[10] << 8 | head[11]) != (uint32_t)st.st_size ||
	    pread(mtd, data, count, st.st_ino + 16 + (at & ~15u)) != count || memcmp(data, begins, count) != 0)
		at = 0;
	close(mtd);
	return at ? 0x40000000u + (uint32_t)st.st_ino + 16 + (at & ~15u) : 0;
}
