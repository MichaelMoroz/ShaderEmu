/*
 * The worker cores for a Linux program: mcw.h. Nothing of the C library is used here (the
 * system calls are made directly), so that a program with a runtime of its own can link it.
 */
#include "mcw.h"

#define STACK_BYTES 0x10000u
#define CHUNK_BYTES 0x40000u

static uint8_t *page;		/* the mailboxes */
static uint8_t *stacks;
static int device = -1;		/* kept open: the kernel parks the workers when it closes */
static int count;
static uint32_t seq[MC_MAX_CORES];
static uint8_t *chunk;
static uint32_t chunk_left;
unsigned mcw_faults;		/* pages touched for a worker so far */

static mc_job *job_of(int k) { return (mc_job *)(page + MC_JOB_AT(k)); }
static mc_answer *answer_of(int k) { return (mc_answer *)(page + MC_ANSWER_AT(k)); }
static mc_fault *fault_of(int k) { return (mc_fault *)(page + MC_FAULT_AT(k)); }
static volatile uint32_t *resume_of(int k) { return (volatile uint32_t *)(page + MC_RESUME_AT(k)); }

static long call6(long number, long a, long b, long c, long d, long e, long f)
{
	register long a7 __asm__("a7") = number;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	register long a3 __asm__("a3") = d;
	register long a4 __asm__("a4") = e;
	register long a5 __asm__("a5") = f;
	__asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
	return a0;
}
static long own_pid(void) { return call6(172, 0, 0, 0, 0, 0, 0); }
static int alive(long pid) { return pid > 0 && call6(129, pid, 0, 0, 0, 0, 0) == 0; }	/* kill(pid, 0) */
static void *map(long bytes, int fd, long offset)
{
	/* mmap2: shared and of the file, or private and of nothing; the offset is in pages */
	long r = call6(222, 0, bytes, 3, fd >= 0 ? 1 : 0x22, fd, offset >> 12);
	return r < 0 && r > -4096 ? 0 : (void *)r;
}

static void say(const char *text, uint32_t value)
{
	char line[96];
	int n = 0, i;

	while (*text && n < 80) line[n++] = *text++;
	for (i = 28; i >= 0; i -= 4) line[n++] = "0123456789abcdef"[(value >> i) & 15];
	line[n++] = '\n';
	call6(64, 2, (long)line, n, 0, 0, 0);
}

/*
 * A worker starts here, with a stack and its number: the global pointer is the program's, the
 * thread pointer core 0's, which waits at the stack's top (what the C library keeps there is
 * shared, then: a job does not use it).
 */
void mcw_entry(void);
uint32_t mcw_loop(uint32_t core);
__asm__(".text\n"
	".globl mcw_entry\n"
	"mcw_entry:\n"
	".option push\n"
	".option norelax\n"
	"	la gp, __global_pointer$\n"
	".option pop\n"
	"	lw tp, 0(sp)\n"
	"	call mcw_loop\n"
	"	ebreak\n");

uint32_t mcw_loop(uint32_t core)
{
	mc_job *job = job_of((int)core);
	mc_answer *answer = answer_of((int)core);
	uint32_t last = job->seq;

	answer->done = last;
	answer->alive = MC_ALIVE | core;
	for (;;) {
		uint32_t at = job->seq;
		if (at != last) {
			mcw_fn fn = (mcw_fn)job->fn;
			if (!fn) {
				answer->alive = 0;
				answer->done = at;
				__asm__ volatile("ebreak");	/* the machine parks this core */
			}
			answer->result = fn(job->a0, job->a1);
			answer->done = at;
			last = at;
		}
		mc_next_pass();	/* asleep until the job's first word changes */
	}
}

/* A worker that has stopped: the page it wanted is touched, or the program ends. */
static void serve(int k)
{
	mc_fault *fault = fault_of(k);
	uint32_t at = fault->count, cause, address;

	if (at == *resume_of(k)) return;
	cause = fault->cause;
	address = fault->address;
	if (cause == 12 || cause == 13) {
		/* a page not there yet, or not yet used: a read brings it */
		volatile uint8_t byte = *(volatile uint8_t *)address;
		(void)byte;
	} else if (cause == 15) {
		/* a store: a byte stored as it is makes the page ours to write, and changes nothing
		 * (the machine keeps no store that changes nothing, so no other core's is undone) */
		volatile uint8_t *byte = (volatile uint8_t *)address;
		*byte = *byte;
	} else {
		say("mcw: a worker core stopped, cause ", cause);
		say("mcw:   at pc ", fault->pc);
		say("mcw:   address ", address);
		call6(94, 70, 0, 0, 0, 0, 0);	/* exit_group */
	}
	mcw_faults++;
	*resume_of(k) = at;
}

int mcw_count(void) { return count; }

int mcw_open(int limit)
{
	volatile uint32_t *owner;
	uint32_t own_tp;
	long root;
	int fd, k, pass;
	uint32_t i;

	if (page) return count;
	if (limit > MC_MAX_CORES - 1) limit = MC_MAX_CORES - 1;
	if (limit <= 0 || (fd = (int)call6(56, -100, (long)"/dev/gpu", 2, 0, 0, 0)) < 0) return 0;
	page = map(MC_PAGE_SIZE, fd, MC_PAGE_GPU_OFFSET);
	device = fd;
	if (!page) goto none;
	owner = (volatile uint32_t *)(page + MC_OWNER_AT);
	if (*owner && (long)*owner != own_pid() && alive((long)*owner)) goto none;
	/* this program's page table; and from here the kernel parks the workers when it ends */
	if ((root = call6(29, fd, MC_ROOT, 0, 0, 0, 0)) <= 0) goto none;
	if (!(stacks = map((long)limit * STACK_BYTES, -1, 0))) goto none;
	/* the stacks' pages there before a worker steps on one (a zero stored over a zero is no store) */
	for (i = 0; i < (uint32_t)limit * STACK_BYTES; i += 4096) stacks[i] = 0;
	for (i = 0; i < MC_PAGE_SIZE; i += 4) *(volatile uint32_t *)(page + i) = 0;
	*owner = (uint32_t)own_pid();
	for (k = 0; k < MC_MAX_CORES; k++) seq[k] = 0;
	mc_next_pass();
	__asm__ volatile("mv %0, tp" : "=r"(own_tp));
	for (k = 1; k <= limit; k++) {
		volatile uint32_t *start = (volatile uint32_t *)(page + MC_START_AT(k));
		uint32_t *top = (uint32_t *)(stacks + (uint32_t)k * STACK_BYTES - 16);
		top[0] = own_tp;
		start[1] = (uint32_t)mcw_entry;
		start[2] = (uint32_t)top;
		start[3] = (uint32_t)root;
		start[0] = MC_START;
	}
	/* a core that is there says so within a few passes; its first steps may want pages */
	count = limit;
	for (pass = 0; pass < 48; pass++) {
		int all = 1;
		for (k = 1; k <= limit; k++) {
			serve(k);
			if (answer_of(k)->alive != (MC_ALIVE | (uint32_t)k)) all = 0;
		}
		if (all) break;
		mc_next_pass();
	}
	count = 0;
	for (k = 1; k <= limit && answer_of(k)->alive == (MC_ALIVE | (uint32_t)k); k++) count = k;
	for (k = 1; k <= limit; k++) *(volatile uint32_t *)(page + MC_START_AT(k)) = 0;
	mc_next_pass();
	if (count == 0) {
		*owner = 0;
		goto none;
	}
	return count;
none:
	if (stacks) call6(215, (long)stacks, (long)limit * STACK_BYTES, 0, 0, 0, 0);
	if (page) call6(215, (long)page, MC_PAGE_SIZE, 0, 0, 0, 0);
	call6(57, device, 0, 0, 0, 0, 0);
	device = -1;
	page = 0;
	stacks = 0;
	return 0;
}

void mcw_close(void)
{
	int k, passes;

	if (!page) return;
	for (k = 1; k <= count; k++) mcw_post(k, 0, 0, 0);
	for (k = 1; k <= count; k++)
		for (passes = 0; !mcw_done(k) && passes < 1000; passes++) mc_next_pass();
	*(volatile uint32_t *)(page + MC_OWNER_AT) = 0;
	mc_next_pass();
	call6(215, (long)page, MC_PAGE_SIZE, 0, 0, 0, 0);
	call6(57, device, 0, 0, 0, 0, 0);
	device = -1;
	page = 0;
	count = 0;
	/* (the stacks stay: a worker is on one until the machine has parked it) */
}

void *mcw_alloc(unsigned bytes)
{
	void *memory;

	bytes = (bytes + 15) & ~15u;
	if (bytes > CHUNK_BYTES) return map((bytes + 4095) & ~4095u, -1, 0);
	if (bytes > chunk_left) {
		if (!(chunk = map(CHUNK_BYTES, -1, 0))) return 0;
		chunk_left = CHUNK_BYTES;
	}
	memory = chunk;
	chunk += bytes;
	chunk_left -= bytes;
	return memory;
}

void mcw_post(int k, mcw_fn fn, uint32_t a0, uint32_t a1)
{
	mc_job *j = job_of(k);

	j->fn = (uint32_t)fn;
	j->a0 = a0;
	j->a1 = a1;
	j->seq = ++seq[k];
}

int mcw_done(int k)
{
	serve(k);
	return answer_of(k)->done == seq[k];
}

uint32_t mcw_result(int k) { return answer_of(k)->result; }

int mcw_wait(int k)
{
	int passes = 0, other;

	while (!mcw_done(k)) {
		for (other = 1; other <= count; other++) serve(other);
		mc_next_pass();
		passes++;
	}
	return passes;
}

int mcw_on_worker(void)
{
	uint8_t here;

	return stacks && &here >= stacks && &here < stacks + (uint32_t)MC_MAX_CORES * STACK_BYTES;
}
