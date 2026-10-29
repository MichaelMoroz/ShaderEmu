/*
 * The worker cores for a Linux program: mcw.h. Nothing of the C library is used here (the
 * system calls are made directly), so that a program with a runtime of its own can link it.
 */
#include "mcw.h"

#define STACK_BYTES 0x10000u
#define STACK_TOUCHED 0x3000u	/* of a stack's top, here before its worker: the rest when it is stepped on (nxphys steps on three) */
#define CHUNK_BYTES 0x40000u

static uint8_t *page;		/* the mailboxes */
static uint8_t *stacks;		/* stack_count of them: worker k's is the k-th */
static int device = -1;		/* kept open: the kernel parks the workers when it closes */
static int count, stack_count;
/* Worker k of this program (1 to count) is that core; 0: it has none now (the kernel shares
 * the cores out, mc.h), and what is posted to it is run by the core that posts it. */
static int core_of[MC_MAX_CORES];
static int starting[MC_MAX_CORES];	/* the core being started for worker k, not there yet */
static uint32_t seq[MC_MAX_CORES];
static uint32_t result_of[MC_MAX_CORES];	/* worker k's last job's answer, */
static uint8_t settled[MC_MAX_CORES];	/* which is here when this is set: nothing of k's is out */
static uint32_t root, own_tp, offer_seen, my_tag;
static int lent, lost, share;
static uint8_t *chunk;
static uint32_t chunk_left;
unsigned mcw_faults;		/* pages touched for a worker so far */
unsigned mcw_calls;		/* and system calls made for one */

static mc_job *job_of(int k) { return (mc_job *)(page + MC_JOB_AT(core_of[k])); }
static mc_answer *answer_of(int k) { return (mc_answer *)(page + MC_ANSWER_AT(core_of[k])); }
/* (a core that is being started may stop at a page too: it is served as one that runs) */
static int core_now(int k) { return core_of[k] ? core_of[k] : starting[k]; }
static mc_fault *fault_of(int k) { return (mc_fault *)(page + MC_FAULT_AT(core_now(k))); }
static volatile uint32_t *resume_of(int k) { return (volatile uint32_t *)(page + MC_RESUME_AT(core_now(k))); }
static int writing(int core);
static void written(void);
/* Worker k's answer is taken: its mailbox says so (MC_TAKEN_AT), and only then is the core one
 * the kernel may take back. */
static void took(int k)
{
	settled[k] = 1;
	if (core_of[k]) *(volatile uint32_t *)(page + MC_TAKEN_AT(core_of[k])) = seq[k];
}

static long call6(long number, long a, long b, long c, long d, long e, long f)
{
	register long a7 __asm__("a7") = number;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	register long a3 __asm__("a3") = d;
	register long a4 __asm__("a4") = e;
	register long a5 __asm__("a5") = f;
	__asm__ volatile(MC_ECALL : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5) : "memory");
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
uint32_t mcw_loop(uint32_t core, uint32_t tag) __attribute__((used));	/* (called from the lines below only) */
__asm__(".text\n"
	".globl mcw_entry\n"
	"mcw_entry:\n"
	".option push\n"
	".option norelax\n"
	"	la gp, __global_pointer$\n"
	".option pop\n"
	"	lw tp, 0(sp)\n"
	"	lw a1, 4(sp)\n"
	"	call mcw_loop\n"
	"	ebreak\n");

uint32_t mcw_loop(uint32_t core, uint32_t tag)
{
	mc_job *job = (mc_job *)(page + MC_JOB_AT(core));
	mc_answer *answer = (mc_answer *)(page + MC_ANSWER_AT(core));
	uint32_t last = job->seq;

	answer->done = last;
	/* (the program's tag with it: a core the kernel parked when its last program ended still says it is
	 * alive, and the next program to start it must not take that for its own worker there) */
	answer->pad = tag;
	answer->alive = MC_ALIVE | core;
	for (;;) {
		uint32_t at = job->seq;
		if (at != last) {
			mcw_fn fn = (mcw_fn)job->fn;
			if (!fn) {
				answer->alive = 0;
				answer->done = at;
				__asm__ volatile(MC_EBREAK);	/* the machine parks this core */
			}
			answer->result = fn(job->a0, job->a1);
			answer->done = at;
			last = at;
			/* (looked at again first: a job posted while this one ran is no change to sleep on) */
			continue;
		}
		mc_next_pass();	/* asleep until the job's first word changes */
	}
}

/* A worker that has stopped: the page it wanted is touched, or the program ends. */
static void serve(int k)
{
	mc_fault *fault;
	uint32_t at, cause, address;

	if (!core_now(k)) return;
	/* (looked at first: most passes a worker has asked nothing, and taking its core costs two stores) */
	if (fault_of(k)->count == *resume_of(k)) return;
	/* (only a core that is this program's still: another's stops at pages of its own) */
	if (!writing(core_now(k))) return;
	fault = fault_of(k);
	at = fault->count;
	if (at == *resume_of(k)) {
		written();
		return;
	}
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
	} else if (cause == 8) {
		/* a system call: made here for it, in the program and the files they share. (Not
		 * the ones that are about which thread calls: a new thread, a new program.) */
		const volatile uint32_t *c = (const volatile uint32_t *)(page + MC_CALL_AT(core_now(k)));
		long number = (long)c[0], result = -38;	/* ENOSYS */

		if (number != 220 && number != 221 && number != 435)	/* clone, execve, clone3 */
			result = call6(number, (long)c[1], (long)c[2], (long)c[3], (long)c[4], (long)c[5], (long)c[6]);
		resume_of(k)[1] = (uint32_t)result;
		mcw_calls++;
	} else {
		say("mcw: a worker core stopped, cause ", cause);
		say("mcw:   at pc ", fault->pc);
		say("mcw:   address ", address);
		say("mcw:   its count ", at);
		say("mcw:   core 0 had answered ", *resume_of(k));
		say("mcw:   worker ", (uint32_t)core_now(k));
		call6(94, 70, 0, 0, 0, 0, 0);	/* exit_group */
	}
	mcw_faults++;
	*resume_of(k) = at;
	written();
}

int mcw_count(void) { return count; }

void mcw_rest(void)
{
	static int none;	/* a kernel from before the rest: the pause hint, as it was */

	if (device >= 0 && !none && call6(29, device, MC_REST, 0, 0, 0, 0) == 0) return;
	none = device >= 0;
	mc_next_pass();
}

void mcw_rest_for(const int *workers, int n)
{
	(void)workers, (void)n;
	mcw_rest();
}

int mcw_most(void)
{
	static int most = -1;
	volatile uint32_t *words;
	int fd;

	if (most >= 0) return most;
	most = 0;
	if ((fd = (int)call6(56, -100, (long)"/dev/gpu", 2, 0, 0, 0)) < 0) return 0;
	/* the machine's control words: how many cores it could have, or (a machine whose cores are as they are) has */
	if ((words = map(4096, fd, 0x01000000)) != 0) {
		uint32_t cores = words[0x3cc / 4] ? words[0x3cc / 4] : words[0x3c0 / 4];

		most = cores > MC_MAX_CORES ? MC_MAX_CORES - 1 : cores > 0 ? (int)cores - 1 : 0;
		call6(215, (long)words, 4096, 0, 0, 0, 0);
	}
	call6(57, fd, 0, 0, 0, 0, 0);
	return most;
}

void mcw_lend(void) { lent = 1; }

int mcw_asked(void)
{
	int k, asked = lost;

	if (!page) return 0;
	for (k = 1; k <= count; k++)
		if (core_of[k] && (((volatile uint32_t *)(page + MC_RECALL_AT))[core_of[k] / 32] >> (core_of[k] % 32) & 1)) asked = 1;
	return asked;
}

int mcw_recalled(int k)
{
	int core = core_of[k];

	return core && page && (((volatile uint32_t *)(page + MC_RECALL_AT))[core / 32] >> (core % 32) & 1);
}

int mcw_bound(int k) { return k >= 1 && k <= count && core_of[k] != 0; }

int mcw_core(int k) { return k >= 1 && k <= count ? core_of[k] : 0; }

/* RISC-V's hybrid geometry (docs/machine/multicore.md, "Packing the small workers"): cores 1 to 16 have the larger
 * write cache (geotune's BIG_COUNT, 2^5 buckets against the others' 2^3). Not read from the geometry's words: a map
 * and unmap of the control page at every open moved what came after and cost nxvideo's AVI 2% on the card (27.4
 * frames a second to 26.8). Where every core is alike, the first 16 are as good as any, and a call still goes to
 * the lowest free worker. */
#define MC_BIG_CORES 16

int mcw_fast(int k) { return k >= 1 && k <= count && core_of[k] && core_of[k] <= MC_BIG_CORES; }

static volatile uint32_t *owner_of_core(int core) { return (volatile uint32_t *)(page + MC_OWNER_AT(core)); }

/* This program is about to write to a core's mailbox: none of its cores is taken meanwhile. */
static int writing(int core)
{
	*(volatile uint32_t *)(page + MC_POSTING_AT(my_tag)) = 1;
	if (*owner_of_core(core) == my_tag) return 1;
	*(volatile uint32_t *)(page + MC_POSTING_AT(my_tag)) = 0;
	return 0;
}
static void written(void) { *(volatile uint32_t *)(page + MC_POSTING_AT(my_tag)) = 0; }

/* Worker k's core is given its start: it says it is there some passes later (arrived()). */
static void start(int k, int core)
{
	volatile uint32_t *at = (volatile uint32_t *)(page + MC_START_AT(core));
	uint32_t *top = (uint32_t *)(stacks + (uint32_t)k * STACK_BYTES - 16);

	top[0] = own_tp;
	top[1] = my_tag;
	if (!writing(core)) return;	/* (taken back already) */
	starting[k] = core;
	at[1] = (uint32_t)mcw_entry;
	at[2] = (uint32_t)top;
	at[3] = root;
	at[0] = MC_START;
	written();
}

/* Whether the core started for worker k is there: it is k's from then on. */
static int arrived(int k)
{
	int core = starting[k];
	mc_answer *answer = (mc_answer *)(page + MC_ANSWER_AT(core));

	if (!core) return 0;
	if (*owner_of_core(core) != my_tag) {
		starting[k] = 0;	/* (taken back before it was there) */
		return 0;
	}
	if (answer->alive != (MC_ALIVE | (uint32_t)core) || answer->pad != my_tag) return 0;
	if (!writing(core)) {
		starting[k] = 0;
		return 0;
	}
	*(volatile uint32_t *)(page + MC_START_AT(core)) = 0;
	written();
	starting[k] = 0;
	core_of[k] = core;
	seq[k] = 0;
	return 1;
}

/* Stacks for workers 1 to n, made at an open (no worker is on them): those of an earlier
 * open serve again when they are enough. 0 where there is no memory for them. */
static int stacks_for(int n)
{
	uint32_t i;
	int k;

	if (n <= stack_count) return 1;
	if (stacks) call6(215, (long)stacks, (long)stack_count * STACK_BYTES, 0, 0, 0, 0);
	stack_count = 0;
	if (!(stacks = map((long)n * STACK_BYTES, -1, 0))) return 0;
	stack_count = n;
	/* the top of each stack there before its worker steps on it */
	for (k = 1; k <= n; k++)
		for (i = 4096; i <= STACK_TOUCHED; i += 4096) stacks[(uint32_t)k * STACK_BYTES - i] = 0;
	return 1;
}

/* The kernel is asked for this program's share again: the cores that are new to it are
 * started for the workers that have none. How many it has or is starting. */
static int claim(int want)
{
	uint32_t answer[2 + MC_MASK_WORDS] = {(uint32_t)want};
	uint8_t has[MC_MAX_CORES] = {0};	/* the cores a worker has or is being started on */
	int core, k, n = 0, free_k = 1;

	offer_seen = *(volatile uint32_t *)(page + MC_OFFER_AT);
	if (call6(29, device, MC_WORKERS_ALL, (long)answer, 0, 0, 0) != 0) return 0;
	root = answer[1];
	share = (int)(answer[0] & 0xfff);
	my_tag = answer[0] >> 12;
	if (!stacks_for(want < share ? want : share)) return 0;
	for (k = 1; k < MC_MAX_CORES; k++) {
		/* (a core taken while idle and handed back by this very call is parked, its mailbox
		 * cleared: the worker that thought it had it has none, and it is started anew) */
		if (core_of[k] && (answer[2 + core_of[k] / 32] >> (core_of[k] % 32) & 1) &&
		    answer_of(k)->alive != (MC_ALIVE | (uint32_t)core_of[k])) core_of[k] = 0;
		if (starting[k] && (answer[2 + starting[k] / 32] >> (starting[k] % 32) & 1) &&
		    *(volatile uint32_t *)(page + MC_START_AT(starting[k])) != MC_START) starting[k] = 0;
		has[core_of[k]] = has[starting[k]] = 1;
	}
	for (core = 1; core < MC_MAX_CORES; core++) {
		if (!(answer[2 + core / 32] >> (core % 32) & 1)) continue;
		n++;
		if (has[core]) continue;		/* one it has */
		/* the first worker without a core: none before it loses one in here */
		for (; free_k <= want && (core_of[free_k] || starting[free_k]); free_k++) {}
		k = free_k;
		if (k > want || k > stack_count) {
			/* (more than it has workers for: back it goes) */
			uint32_t back[MC_MASK_WORDS] = {0};

			back[core / 32] = 1u << (core % 32);
			call6(29, device, MC_GIVE, (long)back, 0, 0, 0);
			n--;
			continue;
		}
		start(k, core);
	}
	return n;
}

/* Worker k has no core any more: its last answer is kept here. */
static void unbind(int k, int give)
{
	int core = core_of[k];

	if (!core) return;
	if (!settled[k]) {
		result_of[k] = answer_of(k)->result;
		took(k);
	}
	core_of[k] = 0;
	lost = 1;
	if (give) {
		uint32_t back[MC_MASK_WORDS] = {0};

		back[core / 32] = 1u << (core % 32);
		call6(29, device, MC_GIVE, (long)back, 0, 0, 0);
	}
}

/*
 * What has become of worker k's core, looked at before anything is done with it. Only between
 * jobs: the kernel takes no core whose job is out, and neither does this give one up.
 */
static void look(int k)
{
	int core = core_of[k];

	if (starting[k]) {
		/* (its first steps may stop at pages: left unserved, it never arrived, and what was
		 * posted to it ran on the posting core for good) */
		serve(k);
		arrived(k);
	}
	core = core_of[k];
	if (core) {
		/* (whose it is first: a core taken and given to another has that one's mailbox, whose
		 * numbers say nothing of this program's jobs; the kernel takes none at a job) */
		if (*owner_of_core(core) != my_tag) core_of[k] = 0, settled[k] = 1, lost = 1;	/* the kernel took it back */
		else if (answer_of(k)->done != seq[k]) return;	/* at a job */
		else if (((volatile uint32_t *)(page + MC_RECALL_AT))[core / 32] >> (core % 32) & 1) unbind(k, 1);
	} else if (!starting[k] && *(volatile uint32_t *)(page + MC_OFFER_AT) != offer_seen) {
		claim(count);	/* cores have come free: this program's share again */
	}
}

int mcw_open(int limit)
{
	int fd, k, pass;

	if (page) return count;
	if (limit > MC_MAX_CORES - 1) limit = MC_MAX_CORES - 1;
	/* (O_RDWR | O_CLOEXEC: a program that runs another in its place gives its workers back) */
	if (limit <= 0 || (fd = (int)call6(56, -100, (long)"/dev/gpu", 2 | 0x80000, 0, 0, 0)) < 0) return 0;
	device = fd;
	for (k = 0; k < MC_MAX_CORES; k++) {
		core_of[k] = starting[k] = 0;
		settled[k] = 1;
		result_of[k] = seq[k] = 0;
	}
	lost = share = 0;
	if (lent) call6(29, fd, MC_LENT, 0, 0, 0, 0);
	if (!(page = map(MC_PAGE_SIZE, fd, MC_PAGE_GPU_OFFSET))) goto none;
	/* (the mailboxes' pages here now, and this program's to write: a worker's first store to its
	 * own, which is its first step, would stop it until this core had stored there) */
	mcw_touch(page, MC_PAGE_SIZE);
	__asm__ volatile("mv %0, tp" : "=r"(own_tp));
	/* the code a worker starts in, read here: a page of it no core has run stops it (cause 12) */
	mcw_touch_code((const void *)mcw_entry, 64);
	mcw_touch_code((const void *)mcw_loop, 512);
	/* this program's share of the cores (the kernel waits a while for those other programs
	 * have over theirs); from here the kernel parks them when the program ends */
	claim(limit);
	/* as many workers as its share, whether or not their cores are here yet: one whose core
	 * is another program's until that one's job ends has it some passes later (look()) */
	count = share < limit ? share : limit;
	if (count > stack_count) count = stack_count;	/* (no memory for more stacks) */
	if (count <= 0) goto none;
	/* a core that is there says so within a few passes; its first steps may want pages */
	for (pass = 0; pass < 48; pass++) {
		int all = 1;
		for (k = 1; k <= count; k++) {
			if (starting[k] && !arrived(k)) all = 0;
			serve(k);
		}
		if (all) break;
		mcw_rest();
	}
	mc_next_pass();
	return count;
none:
	if (stacks) call6(215, (long)stacks, (long)stack_count * STACK_BYTES, 0, 0, 0, 0);
	stack_count = 0;
	if (page) call6(215, (long)page, MC_PAGE_SIZE, 0, 0, 0, 0);
	call6(57, device, 0, 0, 0, 0, 0);	/* and the kernel has the cores back */
	device = -1;
	page = 0;
	stacks = 0;
	count = 0;
	return 0;
}

void mcw_close(void)
{
	int k, passes, step;

	/* (the field's job too: its lanes read and write this program's memory) */
	mcw_field_wait();
	if (!page) return;
	/* The workers leave their loops, in two steps: the jobs that are out end, then the leaving
	 * is posted. A worker with a job out is never posted to: it would sleep on the new job's
	 * word and never wake for it. All are waited for at once, their pages brought meanwhile. */
	for (step = 0; step < 2; step++) {
		if (step == 1)
			for (k = 1; k <= count; k++)
				if (core_of[k] && answer_of(k)->done == seq[k] && writing(core_of[k])) {
					mc_job *j = job_of(k);

					j->fn = 0;
					j->seq = ++seq[k];
					settled[k] = 0;
					written();
				} else {
					core_of[k] = 0;
				}
		for (passes = 0; passes < 1500; passes++) {
			int out = 0;

			for (k = 1; k <= count; k++) {
				if (!core_of[k]) continue;
				serve(k);
				if (answer_of(k)->done != seq[k]) out++;
			}
			if (!out) break;
			mcw_rest();
		}
	}
	mc_next_pass();
	call6(215, (long)page, MC_PAGE_SIZE, 0, 0, 0, 0);
	call6(57, device, 0, 0, 0, 0, 0);
	device = -1;
	page = 0;
	count = 0;
	/* (the stacks stay, a worker being on one until the machine has parked it; without an MMU,
	 * where they are memory taken whole, they are given back, every worker parked now) */
	if (!root && stacks) {
		call6(215, (long)stacks, (long)stack_count * STACK_BYTES, 0, 0, 0, 0);
		stacks = 0;
		stack_count = 0;
	}
}

void mcw_touch_code(const void *code, unsigned bytes)
{
	const volatile uint8_t *at = (const volatile uint8_t *)((uintptr_t)code & ~4095u);
	const volatile uint8_t *end = (const volatile uint8_t *)code + bytes;

	for (; at < end; at += 4096) (void)*at;
}

void mcw_touch(void *memory, unsigned bytes)
{
	volatile uint8_t *at = (volatile uint8_t *)((uintptr_t)memory & ~4095u);
	volatile uint8_t *end = (volatile uint8_t *)memory + bytes;

	/* a word or-ed with nothing: the page is there and this program's to write, and nothing
	 * changes. One instruction that stores, so one fault: a load and then a store were two. */
	for (; at < end; at += 4096) __asm__ volatile("amoor.w zero, zero, (%0)" : : "r"(at) : "memory");
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
	mc_job *j;

	/* (what the program stored for the job is stored before the job: a call orders it only where this file is
	 * compiled apart, and the NES links it into its program under LTO) */
	mc_publish();
	look(k);
	if (core_of[k]) {
		if (writing(core_of[k])) {
			j = job_of(k);
			j->fn = (uint32_t)fn;
			j->a0 = a0;
			j->a1 = a1;
			mc_publish();
			j->seq = ++seq[k];
			settled[k] = 0;
			written();
			return;
		}
		/* (the kernel took the core between the look and now) */
		core_of[k] = 0;
		lost = 1;
	}
	/* no core: the job is done here and now */
	result_of[k] = fn ? fn(a0, a1) : 0;
	settled[k] = 1;
}

int mcw_done(int k)
{
	if (settled[k]) {
		look(k);
		return 1;
	}
	serve(k);
	if (answer_of(k)->done != seq[k]) return 0;
	result_of[k] = answer_of(k)->result;
	took(k);
	look(k);
	return 1;
}

uint32_t mcw_result(int k) { return settled[k] || !core_of[k] ? result_of[k] : answer_of(k)->result; }

int mcw_wait(int k)
{
	int passes = 0, other;

	while (!mcw_done(k)) {
		for (other = 1; other <= count; other++) serve(other);
		mcw_rest();
		passes++;
	}
	return passes;
}

int mcw_self(void)
{
	uint8_t here;

	if (!mcw_on_worker()) return 0;
	return (int)((uint32_t)(&here - stacks) / STACK_BYTES) + 1;
}

int mcw_slot(void) { return page ? (int)(my_tag & 15) : -1; }

void *mcw_map(uint32_t offset, uint32_t bytes) { return device >= 0 ? map((long)bytes, device, (long)offset) : 0; }

int mcw_on_worker(void)
{
	uint8_t here;

	return stacks && &here >= stacks && &here < stacks + (uint32_t)stack_count * STACK_BYTES;
}

/* ---- lanes (docs/machine/lanes.md) ---- */

#define LANES_MOST 64
/* What a lane's index returns to: the machine ends the index there. */
void mcw_lane_stub(void);
__asm__(".text\n"
	".p2align 4\n"
	".globl mcw_lane_stub\n"
	"mcw_lane_stub:\n"
	"	" MC_EBREAK "\n");

/* CSR 0x0b8: bit 31 where the machine has lanes, this core's in bits 8-15, how its last job
 * ended in bits 0-7 (1 whole, 2 a lane stuck). A machine without lanes reads it as 0. */
static inline uint32_t lanes_word(void)
{
	uint32_t v;

	__asm__ volatile("csrr %0, 0x0b8" : "=r"(v));
	return v;
}

int mcw_lanes_here(void)
{
	uint32_t v;

	if (!mcw_on_worker()) return 0;
	v = lanes_word();
	return v >> 31 ? (int)((v >> 8) & 0xff) : 0;
}

/* What a lane could not make: its K + 1 stuck texels, each with the job's word `tag` first (one of
 * another job is empty): K of a bit for each index of its order it skipped, 96 a texel, and one that
 * says where it stopped (bit 31 of its second word). Its order is first, first + n, ... for `left`
 * indices. Returns how many there were. */
static unsigned stuck_made(volatile uint32_t (*s)[4], uint32_t K, uint32_t tag, mcw_lane_fn fn, void *ctx, uint32_t first, uint32_t n, uint32_t left)
{
	uint32_t k, w, j, made = 0, bits, most = (left + n - 1) / n;

	for (k = 0; k < K && 96 * k < most; k++) {
		if (s[k][0] != tag) continue;
		for (w = 0; w < 3; w++)
			for (bits = s[k][1 + w]; bits; bits &= bits - 1) {
				j = 96 * k + 32 * w + (uint32_t)__builtin_ctz(bits);
				if (n * j < left)
					fn(first + n * j, ctx), made++;
			}
	}
	if (s[K][0] == tag && s[K][1] >> 31)
		for (j = s[K][1] & 0x7fffffff; n * j < left; j++) fn(first + n * j, ctx), made++;
	return made;
}

/* The job on this worker's lanes; *how: how it ended (lanes_word's bits 0-7: 4 an index not through),
 * the indices made here after it in bits 8 up; 0 for plain calls. */
static int lanes_run(mcw_lane_fn fn, void *ctx, uint32_t first, uint32_t count, void *out, uint32_t texels, int steps, uint32_t *how)
{
	/* the job as the machine reads it, three texels on this core's stack; then a lane's stuck texel
	 * (stuck_made) */
	struct {
		uint32_t fn, first, count, out, texels, sp, ctx, stub, status, pad[3];
		uint32_t stuck[2 * LANES_MOST][4];
	} __attribute__((aligned(16))) job;
	uint32_t lanes = (uint32_t)mcw_lanes_here(), i, j, L, sp, v;

	*how = 0;
	if (!count) return 0;
	if (!lanes || lanes > LANES_MOST || texels < 1 || texels > 6 || ((uint32_t)out & 15)) {
		for (i = 0; i < count; i++) fn(first + i, ctx);
		return 0;
	}
	/* (the function's first page read here: a page of code no core has run stops a lane, and the
	 * index is then this core's) */
	mcw_touch_code((const void *)fn, 4);
	__asm__ volatile("mv %0, sp" : "=r"(sp));
	job.fn = (uint32_t)fn;
	job.first = first;
	job.count = count;
	job.out = (uint32_t)out;
	job.texels = texels;
	job.sp = (sp - 64) & ~15u;	/* every lane's stack: under this core's, which nothing else uses */
	job.ctx = (uint32_t)ctx;
	job.stub = (uint32_t)mcw_lane_stub;
	job.status = (uint32_t)job.stuck | (steps ? 1u : 0u);	/* (bit 0: a job in steps, its indices' answers looked at) */
	job.pad[0] = job.pad[1] = 0;
	job.pad[2] = 1;	/* (K: a texel of skipped indices a lane, then where it stopped) */
	for (L = 0; L < 2 * lanes; L++) job.stuck[L][0] = 0;
	/* the job: this core's passes are its lanes' until all are through (the stores above are in
	 * memory by then: this pass ends here) */
	__asm__ volatile("csrw 0x0b8, %0" : : "r"(&job) : "memory");
	v = lanes_word();
	*how = v & 0xff;
	if (v & 2)
		for (L = 0; L < lanes; L++)
			*how += stuck_made((volatile uint32_t (*)[4])job.stuck[2 * L], 1, 0x80000000u, fn, ctx, first + L, lanes, count - L) << 8;
	(void)j;
	return (int)lanes;
}

int mcw_lanes(mcw_lane_fn fn, void *ctx, uint32_t first, uint32_t count, void *out, uint32_t texels)
{
	uint32_t how;

	return lanes_run(fn, ctx, first, count, out, texels, 0, &how);
}

/* ---- the lanes' field (docs/machine/lanes.md, "The field") ---- */

#define FIELD_JOB_AT 0x1900u	/* in the mailboxes' page: the job's descriptor, its number, how many lanes */
#define FIELD_STATUS_AT 0x1A00u	/* a texel a group of 64: the number of the job it is at, busy | lanes that left some, its count, more lanes */
#define FIELD_MOST 3968u
#define FIELD_GROUPS 62u
#define FIELD_K 3u	/* a lane's texels of skipped indices: the first 288 of its order */

static volatile uint32_t *field_page;	/* the mailboxes' page, the field's words in it */
static uint32_t field_seq, field_lanes_n, field_out;
static struct {
	uint32_t fn, first, count, out, texels, sp, ctx, stub, stuck, gp, tp, K;
} __attribute__((aligned(16))) field_job;
static uint32_t (*field_stuck[2])[4];	/* a lane's stuck texels (stuck_made), an array for every other job */
static int field_set;			/* the array of the job out, or of the next */
static int field_apart;			/* mcw_field_apart(): the skips kept for the workers, not made here */
/* what a job's lanes left: its groups' words of which lanes skipped some, and the job's own */
struct mcw_field_skips {
	volatile uint32_t (*stuck)[4];
	mcw_lane_fn fn;
	void *ctx;
	uint32_t first, count, lanes_n, tag, any;
	uint32_t lanes[FIELD_GROUPS][2];
};
static struct mcw_field_skips field_skips[2];
static mcw_lane_fn field_fn;
unsigned mcw_field_stuck;	/* indices the field's lanes could not make, made here so far */
static void *field_ctx;
/* a job in steps (mcw_field_steps): what its lanes call, read by them as the round found it */
struct steps_desc {
	mcw_step_fn fn;
	void *ctx;
	uint32_t round, pad;
} __attribute__((aligned(16)));
static struct steps_desc field_steps_desc;
static int field_steps_on;
static uint32_t field_steps_most;	/* (rounds a job at most: mcw_field_steps_rounds) */
static struct mcw_steps_report field_steps_rep;

int mcw_field_lanes(void)
{
	uint32_t v;

	if (mcw_on_worker()) return 0;
	__asm__ volatile("csrr %0, 0x0b9" : "=r"(v));
	return v > FIELD_MOST ? 0 : (int)v;
}

/* The page, mapped here if the program has not opened its workers: 0 where it cannot be. */
static int field_map(void)
{
	int fd;

	if (field_page) return 1;
	if (page) {
		field_page = (volatile uint32_t *)page;
	} else {
		if ((fd = (int)call6(56, -100, (long)"/dev/gpu", 2 | 0x80000, 0, 0, 0)) < 0) return 0;
		field_page = map(MC_PAGE_SIZE, fd, MC_PAGE_GPU_OFFSET);
		call6(57, fd, 0, 0, 0, 0, 0);
		if (!field_page) return 0;
		mcw_touch((void *)field_page, MC_PAGE_SIZE);
	}
	if (!field_stuck[0]) {
		if (!(field_stuck[0] = map(2 * FIELD_MOST * (FIELD_K + 1) * 16, -1, 0))) {
			field_page = 0;
			return 0;
		}
		field_stuck[1] = field_stuck[0] + FIELD_MOST * (FIELD_K + 1);
		mcw_touch(field_stuck[0], 2 * FIELD_MOST * (FIELD_K + 1) * 16);
	}
	mcw_touch(&field_job, sizeof field_job);
	return 1;
}

/* The job (again: a round of steps) under a new number, its stuck texels the set's (the descriptor is in
 * memory by the next pass, as the job's texel is: both are this pass's stores) */
static void field_post(void)
{
	volatile uint32_t *job = field_page + FIELD_JOB_AT / 4;

	field_skips[field_set].any = 0;
	field_job.stuck = (uint32_t)field_stuck[field_set] | (field_steps_on ? 1u : 0u);	/* (bit 0: a job in steps) */
	field_seq = job[1] + 1;
	mc_publish();
	job[1] = field_seq;
}

void mcw_field_begin(mcw_lane_fn fn, void *ctx, uint32_t first, uint32_t count, void *out, uint32_t texels)
{
	volatile uint32_t *job;
	uint32_t i, sp, gp, tp, lanes = (uint32_t)mcw_field_lanes();

	if (field_out) mcw_field_wait();
	field_skips[field_set].any = 0;
	if (!count) return;
	if (!lanes || texels < 1 || texels > 6 || ((uint32_t)out & 15) || !field_map()) {
		for (i = 0; i < count; i++) fn(first + i, ctx);
		return;
	}
	job = field_page + FIELD_JOB_AT / 4;
	/* (another program's job: its end is waited for) */
	while (job[0] != 0 && job[0] != (uint32_t)&field_job) mcw_rest();
	/* (the function's first page read here: a page of code no core has run stops a lane, and the
	 * index is then this core's) */
	mcw_touch_code((const void *)fn, 4);
	__asm__ volatile("mv %0, sp" : "=r"(sp));
	__asm__ volatile("mv %0, gp" : "=r"(gp));
	__asm__ volatile("mv %0, tp" : "=r"(tp));
	field_job.fn = (uint32_t)fn;
	field_job.first = first;
	field_job.count = count;
	field_job.out = (uint32_t)out;
	field_job.texels = texels;
	field_job.sp = (sp - 256) & ~15u;	/* every lane's stack, under this core's: nothing of it is stored */
	field_job.ctx = (uint32_t)ctx;
	field_job.stub = (uint32_t)mcw_lane_stub;
	field_job.gp = gp;
	field_job.tp = tp;
	field_job.K = FIELD_K;
	field_fn = fn, field_ctx = ctx;
	/* every lane drawn for every job: a lane's state says which job it is at, by the number's last byte */
	field_lanes_n = lanes - (lanes + 63) / 64;
	job[2] = lanes;
	job[3] = (uint32_t)call6(172, 0, 0, 0, 0, 0, 0);	/* getpid: the kernel takes the job back if this program dies with it out */
	field_post();
	job[0] = (uint32_t)&field_job;
	field_out = 1;
}

int mcw_field_ready(void)
{
	volatile uint32_t *status;
	struct mcw_field_skips *k = &field_skips[field_set];
	uint32_t g, stuck = 0, more = 0;

	if (!field_out) return 1;
	status = field_page + FIELD_STATUS_AT / 4;
	for (g = 0; g < FIELD_GROUPS; g++) {
		if (status[4 * g] != field_seq || (status[4 * g + 1] & 1)) return 0;
		stuck |= (status[4 * g + 1] & ~1u) | status[4 * g + 3];
		more |= status[4 * g + 2] >> 31;
	}
	/* the lanes each group says left indices (lanes 0 to 30 in bits 1 to 31 of its second word, 31 to 62
	 * in its fourth), and the job's own: for the workers (mcw_field_apart), or made here */
	k->stuck = field_stuck[field_set], k->fn = field_fn, k->ctx = field_ctx;
	k->first = field_job.first, k->count = field_job.count, k->lanes_n = field_lanes_n;
	k->tag = 0x80000000u | field_seq, k->any = stuck != 0;
	for (g = 0; g < FIELD_GROUPS && stuck; g++)
		k->lanes[g][0] = status[4 * g + 1] >> 1, k->lanes[g][1] = status[4 * g + 3];
	field_set ^= 1;
	if (field_steps_on) {
		/* (what the lanes skipped made here first: the next round's lanes read those outputs) */
		if (stuck) field_steps_rep.too_long += mcw_field_skips_make(k, 0, 1);
		k->any = 0;
		if (more && !(field_steps_most && field_steps_rep.rounds >= field_steps_most)) {
			field_steps_desc.round++, field_steps_rep.rounds++;
			field_post();
			return 0;
		}
		field_steps_rep.left = more, field_steps_on = 0;
	}
	field_page[FIELD_JOB_AT / 4] = 0;	/* (the machine draws the field no more) */
	field_out = 0;
	if (stuck && !field_apart) mcw_field_stuck += mcw_field_skips_make(k, 0, 1);
	return 1;
}

void mcw_field_apart(int on) { field_apart = on; }

const struct mcw_field_skips *mcw_field_skips(void)
{
	const struct mcw_field_skips *k = &field_skips[field_set ^ 1];

	return field_apart && k->any ? k : 0;
}

unsigned mcw_field_skips_make(const struct mcw_field_skips *k, int part, int parts)
{
	uint32_t g, h, m, l, c = 0;
	unsigned made = 0;

	if (!k || !k->any) return 0;
	/* (the lanes that left some dealt out in turn: such lanes lie together, where the work is long) */
	for (g = 0; g < FIELD_GROUPS; g++)
		for (h = 0; h < 2; h++)
			for (m = k->lanes[g][h]; m; m &= m - 1) {
				l = 63 * g + 31 * h + (uint32_t)__builtin_ctz(m);
				if (c++ % (uint32_t)parts == (uint32_t)part && l < k->lanes_n && l < k->count)
					made += stuck_made(k->stuck + (FIELD_K + 1) * l, FIELD_K, k->tag, k->fn, k->ctx, k->first + l, k->lanes_n, k->count - l);
			}
	return made;
}

void mcw_field_wait(void)
{
	while (!mcw_field_ready()) mcw_rest();
}

/* ---- steps (docs/machine/lanes.md, "Steps") ---- */

static int probe_on;
static uint32_t probe_t0, probe_budget;

static inline uint32_t clock_now(void)
{
	return mc_cycles();
}

/* (0 too is no end: a machine from before the CSR) */
uint32_t mcw_steps_left(void)
{
	uint32_t v, d;

	__asm__ volatile("csrr %0, 0x0ba" : "=r"(v));
	if (v != 0 && v != 0xffffffffu) return v;
	if (!probe_on) return 0xffffffffu;
	d = clock_now() - probe_t0;
	return d < probe_budget ? probe_budget - d : 0;
}

/* What a lane calls: a step of the index; a plain call (on a core) goes on until the index is through. */
static uint32_t steps_lane(uint32_t i, void *p)
{
	const struct steps_desc *d = p;
	uint32_t r = d->round, more = d->fn(i, d->ctx, r);

	while (more && !probe_on && mcw_steps_left() == 0xffffffffu) more = d->fn(i, d->ctx, ++r);
	return more;
}

void mcw_field_steps(mcw_step_fn fn, void *ctx, uint32_t first, uint32_t count, void *out, uint32_t texels)
{
	if (field_out) mcw_field_wait();
	field_steps_desc.fn = fn, field_steps_desc.ctx = ctx, field_steps_desc.round = 0;
	field_steps_rep.rounds = 1, field_steps_rep.too_long = 0, field_steps_rep.left = 0;
	field_steps_on = 1;
	mcw_field_begin((mcw_lane_fn)steps_lane, &field_steps_desc, first, count, out, texels);
	if (!field_out) field_steps_on = 0;	/* (no field: made here, every index through) */
}

struct mcw_steps_report mcw_field_steps_report(void) { return field_steps_rep; }
void mcw_field_steps_rounds(uint32_t most) { field_steps_most = most; }

int mcw_lanes_steps(mcw_step_fn fn, void *ctx, uint32_t first, uint32_t count, void *out, uint32_t texels, struct mcw_steps_report *report)
{
	struct steps_desc d;
	uint32_t how, made = 0, rounds = 0;
	int lanes;

	d.fn = fn, d.ctx = ctx, d.round = 0;
	do {
		lanes = lanes_run((mcw_lane_fn)steps_lane, &d, first, count, out, texels, 1, &how);
		made += how >> 8, rounds++, d.round++;
	} while (lanes && (how & 4));
	if (report) report->rounds = rounds, report->too_long = made, report->left = 0;
	return lanes;
}

struct mcw_steps_probe mcw_steps_probe(mcw_step_fn fn, void *ctx, uint32_t first, uint32_t count, uint32_t budget)
{
	struct mcw_steps_probe p = {0, 0, 0, 0, 0};
	uint64_t sum = 0;
	uint32_t i, r, d, more;

	probe_budget = budget ? budget : 8192;
	probe_on = 1;
	for (i = first; i < first + count; i++) {
		r = 0;
		do {
			probe_t0 = clock_now();
			more = fn(i, ctx, r);
			d = clock_now() - probe_t0;
			p.steps++, sum += d, r++;
			if (d > p.longest) p.longest = d;
			if (d > probe_budget) p.over++;
		} while (more && r < 100000);
		if (r > p.rounds) p.rounds = r;
	}
	probe_on = 0;
	p.mean = p.steps ? (uint32_t)(sum / p.steps) : 0;
	return p;
}

/* The first page of the table workers run on: 0 on a kernel without an MMU. */
uint32_t mcw_mmu_root(void) { return root; }
