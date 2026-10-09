/* The worker cores for a Linux program: mcw.h. */
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "mcw.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#define OWNER_AT 0x3f0u		/* the owner's process number, in the arena's first page */

static uint8_t *arena;		/* MC_ARENA_PHYS, here as on the workers */
static int count;
static uint32_t seq[MC_MAX_CORES];
static uint32_t top = MC_DATA_AT;

static mc_job *job_of(int k) { return (mc_job *)(arena + MC_JOB_AT(k)); }
static mc_answer *answer_of(int k) { return (mc_answer *)(arena + MC_ANSWER_AT(k)); }

/* (system calls made here, so that a program with a runtime of its own needs nothing more) */
static long call2(long number, long a, long b)
{
	register long a7 __asm__("a7") = number;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	__asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1) : "memory");
	return a0;
}
static long own_pid(void) { return call2(172, 0, 0); }
static int alive(long pid) { return pid > 0 && call2(129, pid, 0) == 0; }	/* kill(pid, 0) */

int mcw_count(void) { return count; }

int mcw_open(const unsigned char *code, unsigned bytes, int limit)
{
	volatile uint32_t *owner;
	int fd, k;

	if (arena) return count;
	if (limit > MC_MAX_CORES - 1) limit = MC_MAX_CORES - 1;
	if (limit <= 0 || bytes > MC_STACK_TOP(1) - 0x10000u - MC_CODE_AT || (fd = open("/dev/gpu", O_RDWR)) < 0) return 0;
	arena = mmap((void *)MC_ARENA_PHYS, MC_ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd,
		     MC_ARENA_GPU_OFFSET);
	close(fd);
	if (arena != (uint8_t *)MC_ARENA_PHYS) {
		if (arena != MAP_FAILED) call2(215, (long)arena, MC_ARENA_SIZE);
		arena = NULL;
		return 0;
	}
	owner = (volatile uint32_t *)(arena + OWNER_AT);
	if (*owner && (long)*owner != own_pid() && alive((long)*owner)) {
		call2(215, (long)arena, MC_ARENA_SIZE);
		arena = NULL;
		return 0;
	}
	/* workers an earlier program left running park first: they start again below */
	for (k = 1; k <= limit; k++) {
		mc_job *j = job_of(k);
		j->fn = MC_FN_PARK;
		j->seq = j->seq + 1;
	}
	for (k = 0; k < 3; k++) mc_next_pass();
	/* the code, and mailboxes with nothing in them; a pass, so that every core sees them */
	memset(arena, 0, MC_CODE_AT);
	*owner = (uint32_t)own_pid();
	memcpy(arena + MC_CODE_AT, code, bytes);
	memset(seq, 0, sizeof seq);
	top = MC_DATA_AT;
	mc_next_pass();
	for (k = 1; k <= limit; k++) {
		volatile uint32_t *start = (volatile uint32_t *)(arena + MC_START_AT(k));
		start[1] = MC_ARENA_PHYS + MC_CODE_AT;
		start[2] = MC_ARENA_PHYS + MC_STACK_TOP(k);
		start[3] = 0;
		start[0] = MC_START;
	}
	/* a core that is there says so within a few passes */
	for (k = 0; k < 8; k++) mc_next_pass();
	count = 0;
	for (k = 1; k <= limit && answer_of(k)->alive == (MC_ALIVE | (uint32_t)k); k++) count = k;
	for (k = 1; k <= limit; k++) *(volatile uint32_t *)(arena + MC_START_AT(k)) = 0;
	return count;
}

void mcw_close(void)
{
	int k;

	if (!arena) return;
	for (k = 1; k <= count; k++) mcw_post(k, MC_FN_PARK, 0, 0);
	for (k = 1; k <= count; k++) {
		int passes = 0;
		while (!mcw_done(k) && ++passes < 1000) mc_next_pass();
	}
	*(volatile uint32_t *)(arena + OWNER_AT) = 0;
	mc_next_pass();
	call2(215, (long)arena, MC_ARENA_SIZE);
	arena = NULL;
	count = 0;
}

void *mcw_alloc(unsigned bytes)
{
	void *memory;

	bytes = (bytes + 15) & ~15u;
	if (!arena || count == 0 || top + bytes > MC_ARENA_SIZE) return NULL;
	memory = arena + top;
	top += bytes;
	return memory;
}

void mcw_free_all(void) { top = MC_DATA_AT; }

int mcw_owns(const void *memory)
{
	return arena && (const uint8_t *)memory >= arena && (const uint8_t *)memory < arena + MC_ARENA_SIZE;
}

void mcw_post(int k, uint32_t fn, uint32_t a0, uint32_t a1)
{
	mc_job *j = job_of(k);

	j->fn = fn;
	j->a0 = a0;
	j->a1 = a1;
	j->seq = ++seq[k];
}

int mcw_done(int k) { return answer_of(k)->done == seq[k]; }
uint32_t mcw_result(int k) { return answer_of(k)->result; }

int mcw_wait(int k)
{
	int passes = 0;

	while (!mcw_done(k)) {
		mc_next_pass();
		passes++;
	}
	return passes;
}
