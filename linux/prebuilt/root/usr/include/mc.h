// Worker cores (docs/machine/multicore.md): the mailboxes, which are what the machine, the library
// (mcw.h) and a worker agree on. They are one page of the GPU device's memory, which /dev/gpu
// maps, at a physical address the machine knows; everything else a worker touches is the
// program's own memory, which it has as the program has it.
//
// A core reads memory as the last pass left it, plus its own writes: what one core stores,
// another sees one pass later. Two cores must not store to the same 16 bytes (one texel of
// the machine's memory: addresses that differ only in their last four bits) in one pass.
#ifndef MC_H
#define MC_H
#include <stdint.h>

#define MC_PAGE_PHYS 0x80C00000u
#define MC_PAGE_SIZE 0x8000u   // (eight pages: a word, a mailbox and a system call's words for 256 cores)
#define MC_PAGE_GPU_OFFSET (MC_PAGE_PHYS - 0x80000000u)   // where it is in /dev/gpu
#define MC_MAX_CORES 256
#define MC_MASK_WORDS (MC_MAX_CORES / 32)   // a bit a core

// { MC_START, pc, sp, root } starts core k there in user mode with k in a0, on the page table
// whose first page is number `root`. A program gets its cores and its root from the kernel:
// MC_WORKERS on /dev/gpu, with { how many it wants, a bit a core it got, its root }
#define MC_START 0x5553434du
#define MC_STOP 0x5453434du   // there instead: the core parks (the kernel's, when the program is gone)
#define MC_START_AT(k) (16u * (k))
#define MC_WORKERS 0xc00c4704u   // _IOWR('G', 4, three words)
// { how many, root, a bit a core in eight words } for the workers of any machine
#define MC_WORKERS_ALL 0xc0284708u   // _IOWR('G', 8, ten words)
// The kernel shares the cores out between the programs that ask (docs/machine/multicore.md, "Shared
// between programs"). MC_WORKERS_ALL may be called again and again: it answers with the
// program's share in place of the number asked for. A program whose workers are only lent
// (MC_LENT, before it asks) has what the others leave. Three things in the mailbox page are
// the kernel's to write: a word a core at MC_OWNER_AT, its program's tag (0: nobody's, the
// kernel took it back, which it does to a core with no job); a bit a core at MC_RECALL_AT, a
// core the kernel wants back when its job has ended (MC_GIVE: eight words, a bit a core); and
// a count at MC_OFFER_AT that goes up when cores come free.
#define MC_LENT 0x4709u              // _IO('G', 9)
#define MC_GIVE 0x4020470au          // _IOW('G', 10, eight words)
// The first core with nothing to do until the next pass says so to the kernel (mcw_rest()):
// another program runs, or the pass ends there as in the idle loop. No argument.
#define MC_REST 0x470bu              // _IO('G', 11)
#define MC_RECALL_AT 0x1000u
#define MC_OFFER_AT 0x1020u
#define MC_OWNER_AT(k) (0x1400u + 4u * (k))
// MC_WORKERS_ALL's first word comes back as the share and, from bit 12, the program's tag. A
// program sets its word here (its tag's low four bits say which) while it writes to a core's
// mailbox or start, and looks at the core's owner word after setting it: the kernel takes no
// core of a program whose word is set, so what is written is never another program's.
#define MC_POSTING_AT(tag) (0x1040u + 4u * ((tag) & 15u))
// gpu_vm (gpu_vm/platform.md, "Resting"): eight words, a bit a core, the workers the first core
// waits for while it rests (mcw_rest_for: set before the pass ends, 0 again after), which the
// tick gives their whole pass; then a word, the pid of the program that says what it waits for
// (set at its first mcw_rest_for, 0 again when it closes /dev/gpu); and the kernel's two words
// { MC_CROWD_MARK, other tasks are runnable }: with the mark there and 0 a rest ends the pass
// by itself, with no system call.
#define MC_WAITING_AT 0x1100u
#define MC_DECLARED_AT 0x1120u
#define MC_CROWD_AT 0x1130u
#define MC_CROWD_MARK 0x54534552u   // "REST"
// (the texel at 0x1800 is the window system's for the kernel's pointer driver: linux/nanox/scr_shaderemu.c)
// core k's 64 bytes: a job (core 0 writes it), its answer (the worker's), a fault (the
// worker's, written by the machine) and the word that lets it go on (core 0's)
#define MC_JOB_AT(k) (0x4000u + 64u * (k))
#define MC_ANSWER_AT(k) (MC_JOB_AT(k) + 16u)
#define MC_FAULT_AT(k) (MC_JOB_AT(k) + 32u)
#define MC_RESUME_AT(k) (MC_JOB_AT(k) + 48u)
// the resume texel's third word is core 0's too: the job whose answer the program has taken. The
// kernel takes back no core whose last answer is still in its mailbox.
#define MC_TAKEN_AT(k) (MC_JOB_AT(k) + 56u)
// a worker's system call, for core 0 to make: a7 and a0 to a5 (the fault's cause is 8); the
// answer goes in the resume texel's second word before its first
#define MC_CALL_AT(k) (0x2000u + 32u * (k))

#define MC_ALIVE 0x600d0000u

// Counters (mcwx.h): 1024 words, 64 for each program's slot (its tag's low four bits). A worker
// adds into a region of its own (core 0 into region 0) and sets the head's first word; some
// passes later the machine makes each total the sum of all regions, and counts it in the head.
#define MC_COUNT_PHYS 0x80C10000u
#define MC_COUNT_GPU_OFFSET (MC_COUNT_PHYS - 0x80000000u)
#define MC_COUNT_BYTES 0x101000u                         // the totals, then the regions
#define MC_COUNT_REGION_AT(k) (0x1000u + 0x1000u * (k))
#define MC_COUNT_HEAD_PHYS 0x81000320u                   // a control word: { added, MC_COUNT_MERGED, merges, 0 }
#define MC_COUNT_MERGED 0x4d475245u                      // "ERGM": a machine that merges
#define MC_COUNTERS 64

typedef struct { volatile uint32_t seq, fn, a0, a1; } mc_job;                  // seq changes: a new job
typedef struct { volatile uint32_t done, result, alive, pad; } mc_answer;      // done == seq: finished
typedef struct { volatile uint32_t count, cause, address, pc; } mc_fault;      // count != resume: stopped

#ifdef __gpuvm__
// gpu_vm (gpu_vm/GPU_VM.md, "Workers"; gpu_vm/userland/apps/README.md): a user's `wait` is the pause hint (the
// core's pass ends; a worker sleeps until the first word of its job changes), `sync` is fence.i (the pass ends
// here, and the commit's operations are made before it goes on), `break` is ebreak (a worker parks), `syscall` is
// ecall, the special register `cycle` (bundles run) the cycle counter and `time_lo` the clock (the machine's
// 100 kHz). The mailboxes are where they were in /dev/gpu (the device window moved down by 0x80000000, so
// MC_PAGE_GPU_OFFSET is the same).
#define mc_next_pass() __asm__ volatile("wait" ::: "memory")
#define mc_end_pass() __asm__ volatile("sync" ::: "memory")
#define MC_ECALL "syscall"
#define MC_EBREAK "break"
static inline uint32_t mc_cycles(void)
{
	uint32_t v;
	__asm__ volatile("mfsr %0, cycle" : "=r"(v));
	return v;
}
static inline uint32_t mc_time(void)
{
	uint32_t v;
	__asm__ volatile("mfsr %0, time_lo" : "=r"(v));
	return v;
}
#else
// The pause hint ends this core's pass: its stores reach the others, and theirs reach it. On a
// worker it is a sleep until the first word of its job changes.
#define mc_next_pass() __asm__ volatile(".word 0x0100000f")
// For a job that waits for another core's stores: its pass ends here and it goes on in the next
// (fence.i ends a pass and does no more). Turning in place costs the machine the pass's instructions.
#define mc_end_pass() __asm__ volatile(".word 0x0000100f")
#define MC_ECALL "ecall"
#define MC_EBREAK "ebreak"
static inline uint32_t mc_cycles(void)
{
	uint32_t v;
	__asm__ volatile("rdcycle %0" : "=r"(v));
	return v;
}
static inline uint32_t mc_time(void)
{
	uint32_t v;
	__asm__ volatile("rdtime %0" : "=r"(v));
	return v;
}
#endif

// Between what another core reads and the word that tells it the data is there: the compiler keeps every store before
// it before every store after. A pass ends at any instruction and its commit publishes what the pass stored, so a word
// that says "ready" stored ahead of its data (the compiler may: C orders no plain stores) reaches the other core first
// (GPU-VM's handoff.c: 293 of 20,000 frames on its card; Quake III's n < cg.latestSnapshotNum). A call to mcw_post
// orders what was stored before it (another file's function, or its own barrier); a volatile word does not hold back
// plain stores, so a flag of a program's own wants this.
#define mc_publish() __asm__ volatile("" ::: "memory")

#endif
