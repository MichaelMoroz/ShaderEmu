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
static int count, stack_count;
static int core_of[MC_MAX_CORES];	/* worker k of this program (1 to count) is that core */
static uint32_t seq[MC_MAX_CORES];
static uint8_t *chunk;
static uint32_t chunk_left;
unsigned mcw_faults;		/* pages touched for a worker so far */

static mc_job *job_of(int k) { return (mc_job *)(page + MC_JOB_AT(core_of[k])); }
static mc_answer *answer_of(int k) { return (mc_answer *)(page + MC_ANSWER_AT(core_of[k])); }
static mc_fault *fault_of(int k) { return (mc_fault *)(page + MC_FAULT_AT(core_of[k])); }
static volatile uint32_t *resume_of(int k) { return (volatile uint32_t *)(page + MC_RESUME_AT(core_of[k])); }

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
	mc_job *job = (mc_job *)(page + MC_JOB_AT(core));
	mc_answer *answer = (mc_answer *)(page + MC_ANSWER_AT(core));
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
	uint32_t claim[3], own_tp, i;
	int fd, k, pass, got = 0;

	if (page) return count;
	if (limit > MC_MAX_CORES - 1) limit = MC_MAX_CORES - 1;
	if (limit <= 0 || (fd = (int)call6(56, -100, (long)"/dev/gpu", 2, 0, 0, 0)) < 0) return 0;
	device = fd;
	/* the cores nobody has, as many as are wanted, and this program's page table; from here
	 * the kernel parks them when the program ends */
	claim[0] = (uint32_t)limit;
	claim[1] = claim[2] = 0;
	if (call6(29, fd, MC_WORKERS, (long)claim, 0, 0, 0) != 0 || !claim[1]) goto none;
	for (k = 1; k < MC_MAX_CORES; k++)
		if (claim[1] >> k & 1) core_of[++got] = k;
	if (!(page = map(MC_PAGE_SIZE, fd, MC_PAGE_GPU_OFFSET))) goto none;
	if (!(stacks = map((long)got * STACK_BYTES, -1, 0))) goto none;
	stack_count = got;
	/* the stacks' pages there before a worker steps on one */
	for (i = 0; i < (uint32_t)got * STACK_BYTES; i += 4096) stacks[i] = 0;
	__asm__ volatile("mv %0, tp" : "=r"(own_tp));
	for (k = 1; k <= got; k++) {
		volatile uint32_t *start = (volatile uint32_t *)(page + MC_START_AT(core_of[k]));
		volatile uint32_t *box = (volatile uint32_t *)(page + MC_JOB_AT(core_of[k]));
		uint32_t *top = (uint32_t *)(stacks + (uint32_t)k * STACK_BYTES - 16);
		for (i = 0; i < 16; i++) box[i] = 0;	/* its mailbox as new */
		seq[k] = 0;
		top[0] = own_tp;
		start[1] = (uint32_t)mcw_entry;
		start[2] = (uint32_t)top;
		start[3] = claim[2];
		start[0] = MC_START;
	}
	/* a core that is there says so within a few passes; its first steps may want pages */
	count = got;
	for (pass = 0; pass < 48; pass++) {
		int all = 1;
		for (k = 1; k <= got; k++) {
			serve(k);
			if (answer_of(k)->alive != (MC_ALIVE | (uint32_t)core_of[k])) all = 0;
		}
		if (all) break;
		mc_next_pass();
	}
	count = 0;
	for (k = 1; k <= got && answer_of(k)->alive == (MC_ALIVE | (uint32_t)core_of[k]); k++) count = k;
	for (k = 1; k <= got; k++) *(volatile uint32_t *)(page + MC_START_AT(core_of[k])) = 0;
	mc_next_pass();
	if (count) return count;
none:
	if (stacks) call6(215, (long)stacks, (long)got * STACK_BYTES, 0, 0, 0, 0);
	if (page) call6(215, (long)page, MC_PAGE_SIZE, 0, 0, 0, 0);
	call6(57, device, 0, 0, 0, 0, 0);	/* and the kernel has the cores back */
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
	mc_next_pass();
	call6(215, (long)page, MC_PAGE_SIZE, 0, 0, 0, 0);
	call6(57, device, 0, 0, 0, 0, 0);
	device = -1;
	page = 0;
	count = 0;
	/* (the stacks stay: a worker is on one until the machine has parked it) */
}

void mcw_touch(void *memory, unsigned bytes)
{
	volatile uint8_t *at = (volatile uint8_t *)((uintptr_t)memory & ~4095u);
	volatile uint8_t *end = (volatile uint8_t *)memory + bytes;

	/* a byte stored as it is: the page is there and this program's to write, and nothing changes */
	for (; at < end; at += 4096) *at = *at;
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

	return stacks && &here >= stacks && &here < stacks + (uint32_t)stack_count * STACK_BYTES;
}
