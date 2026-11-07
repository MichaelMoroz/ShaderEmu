/*
 * Asking the host with an address of the fetch's (docs/system/fetch.md) that no network has: "persist:",
 * "host:", "clip:" (docs/system/clipboard.md). A header and nothing to link; nxpersist has its own copy.
 *
 *   if (host_open()) status = host_ask("clip:show", bytes, 30);   (bytes left at host_data first)
 *
 * The answer's bytes are at host_data, host_answered() of them.
 */
#ifndef HOST_ASK_H
#define HOST_ASK_H

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define HOST_DATA_MOST 0x40000	/* the fetch's 256 KB: the answer, and what the host reads of ours */
#define HOST_REG_ASKED 64
#define HOST_REG_REPLY 68
#define HOST_REG_ADDRESS 72
#define HOST_KIND_FILE 3

static volatile uint32_t *host_regs;
static unsigned char *host_data;

/* The device's words and the fetch's memory mapped; 0 where the machine has no way to ask. */
static int
host_open(void)
{
	int fd;

	if (host_regs)
		return 1;
	fd = open("/dev/gpu", O_RDWR);
	if (fd < 0)
		return 0;
	host_regs = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01000000);
	host_data = mmap(NULL, HOST_DATA_MOST, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x016c0000);
	close(fd);
	if (host_regs == MAP_FAILED || host_data == MAP_FAILED) {
		host_regs = NULL, host_data = NULL;
		return 0;
	}
	return 1;
}

/* Waits for another program's request to be answered (its answer lands in the same memory): before
 * leaving bytes at host_data. False if it was not in 10 seconds. */
static int
host_free(void)
{
	int waited;

	for (waited = 0; host_regs[HOST_REG_ASKED] != host_regs[HOST_REG_REPLY] && waited < 200; waited++)
		usleep(50000);
	return host_regs[HOST_REG_ASKED] == host_regs[HOST_REG_REPLY];
}

/* Asks; the answer's status, or 0 for none in `seconds`. `offset` is the request's fourth word: for an
 * address that hands the host bytes, how many lie at host_data. */
static int
host_ask(const char *address, uint32_t offset, int seconds)
{
	uint32_t mine;
	int waited;
	size_t n = strlen(address);

	if (!host_open() || n > 255)
		return 0;
	host_free();
	memset((void *)&host_regs[HOST_REG_ADDRESS], 0, 256);
	memcpy((void *)&host_regs[HOST_REG_ADDRESS], address, n);
	host_regs[HOST_REG_ASKED + 1] = n;
	host_regs[HOST_REG_ASKED + 2] = HOST_KIND_FILE;
	host_regs[HOST_REG_ASKED + 3] = offset;
	mine = host_regs[HOST_REG_ASKED] + 1;
	host_regs[HOST_REG_ASKED] = mine;
	for (waited = 0; host_regs[HOST_REG_REPLY] != mine; waited++) {
		if (waited > seconds * 20)
			return 0;
		usleep(50000);
	}
	return host_regs[HOST_REG_REPLY + 2];
}

/* How many bytes the last answer put at host_data. */
static uint32_t
host_answered(void)
{
	uint32_t n = host_regs ? host_regs[HOST_REG_REPLY + 1] : 0;

	return n < HOST_DATA_MOST ? n : HOST_DATA_MOST;
}

#endif
