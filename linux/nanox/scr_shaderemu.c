/*
 * Microwindows screen driver for the ShaderEmu display and GPU: every top-level window is
 * drawn into a buffer of its own and the GPU composes the screen from those
 * (docs/system/nanox.md in the ShaderEmu repository).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include "device.h"
#include "genfont.h"
#include "genmem.h"
#include "fb.h"
#include "mc.h"
#include "sef.h"

/* /dev/gpu maps GPU memory from GPU_PHYS; offsets below are from there */
#define GPU_PHYS	0x80000000u
#define GPU_SIZE	0x01b00000u
#define POOL_SIZE	0x00c00000u	/* window buffers, from offset 0; the worker cores' mailbox page is right above (programs/mc/mc.h) */
#define REGS_OFFSET	0x01000000u	/* the machine's control words (physical 0x81000000) */
#define LAYERS_OFFSET	0x01001000u	/* the display's layer table: count, then x, y, w, h, address */
#define LIST_OFFSET	0x01400000u	/* our command list: 4096 commands of 64 bytes */
#define CURSOR_OFFSET	0x01440000u	/* the cursor's image */
#define FONTS_OFFSET	0x01441000u	/* the built-in fonts' glyph tables and bitmaps, kept for good */
#define CACHE_OFFSET	0x01481000u	/* single glyphs the engine asked for one at a time, kept between lists */
#define DATA_OFFSET	0x01541000u	/* other textures for the commands in the list */
#define DATA_END	0x016b8000u	/* beyond: the network's packets (docs/system/lan.md), what the host fetched (docs/system/fetch.md), then an OpenGL program's */
#define REG_MODE	0		/* word indices from REGS_OFFSET */
#define REG_WIDTH	1
#define REG_HEIGHT	2
#define REG_LAYERS	3		/* display mode 4: address of the layer table */
#define REG_SUBMIT	4		/* what to do with the list; the GPU clears it when done */
#define REG_LIST	5
#define REG_COUNT	6
#define REG_FRAMES	7
#define REG_INTO	20		/* where the picture is copied: address, width, height, row */
#define REG_LOCK	24		/* taken (with an atomic swap) by whoever writes the words above */
#define REG_BUFFERS	25		/* goes up when a window gets another buffer: programs that draw into
					 * their window themselves ask where it is again */
#define REG_COPY	28		/* nonzero: the list drawn last has a copy still to be made */
#define REG_HOST	15		/* the host's flags: bit 0 the desktop at boot, then its largest screen */
#define REG_KEYBOARD	12		/* KEYBOARD_OWNED while a program reads the keyboard device */
#define REG_COPIES	14		/* copies of a drawn picture into RAM made so far */
#define REG_CURSOR	16		/* x, y, on, address of a 32x32 image */
#define REG_POINTER	8		/* the pointer: x, y, buttons (docs/machine/input.md) */
#define REG_CLOCK	13		/* the host's clock, in milliseconds */
/* The pointer's worker core (docs/system/nanox.md, "The pointer on a core of its own"): what it is
 * told and what it answers, a texel each, in the layer table's page. Words from WM_OFFSET. */
#define WM_OFFSET	(LAYERS_OFFSET + 0x800)
/* for the kernel, in the mailbox pages' free part (it has those mapped): left | top << 16,
 * right | bottom << 16, the worker's core, WM_MAGIC */
#define WM_QUIET_OFFSET	(MC_PAGE_GPU_OFFSET + 0x1800)
#define WM_CURSOR	4		/* the cursor: pointer to image corner (x, y), "on", the image */
#define WM_DRAG		8		/* a dragged surface: buffer, pointer to corner (x, y), which drag */
#define WM_AT		12		/* the worker's: where it has it (x, y), of which drag, the drag let go */
#define WM_RUN		16		/* which start of the worker this is; 0: it parks */
#define WM_HERE		20		/* the worker's: the start it runs from */
#define WM_LOOK		24		/* the desktop's look, written by Settings (linux/apps/ui_look.h) */
#define WM_PIXMAPS	28		/* pixmaps given the GPU's memory so far: a program sees whether its own was */
#define WM_MAGIC	0x31514d57u
#define KEYBOARD_OWNED	0x6b657973u
#define MAX_COMMANDS	4096
#define MAX_SURFACES	15		/* the display shows 16 layers; one is the desktop */
#define MAX_BLOCKS	(2 * MAX_SURFACES + 2)
#define CACHE_SLOTS	2048
#define CMD_RECT	2
#define CMD_TEXT	4
#define MAX_MESH	196608		/* vertices the GPU's mesh has for one list */
#define MAX_FONTS	8
#define FRAG_COLOUR	0
#define FRAG_TEXTURE	1
#define FRAG_MASK	3
#define FRAG_RGB24	4
#define FRAG_COVER	7		/* text: four bits of coverage a pixel */
#define FRAG_MSDF	8		/* text: distance fields in a face's atlas */
#define TEXT_MORE	0x100		/* a text command with words 9 to 11 */
#define PASS_OVER	(5u << 16)	/* the pass that blends by alpha and uses no depth */
#define MAX_PIXMAPS	8
#define ROM_PHYS	0x40000000u	/* the ROM the root file system is in */
#define SUBMIT_DRAW	1
#define SUBMIT_INTO	4		/* then copy the picture to the given rectangle of RAM */
/* The pause hint: this machine ends its frame there, which is when the GPU does its work. */
#define next_frame()	mc_next_pass()

/* What a window (and everything inside it) is drawn on: a buffer the size of the window. */
struct surface {
	SCREENDEVICE psd;		/* must be first: the engine draws on this */
	void *owner;			/* the window; NULL for a free slot */
	uint32_t at;			/* the buffer, in GPU memory */
	int x, y, w, h;			/* where the window is on the screen */
	int pixmap;			/* of pixmaps[]: 1 in memory of the server's, 2 in the GPU's */
};

extern int gr_mode;
extern int (*gd_drawpicture)(PSD psd, MWCOORD x, MWCOORD y, MWCOORD width, MWCOORD height, const char *path,
	const MWRECT *within, int flags);
extern int (*gd_measurestring)(PMWCFONT font, const unsigned char *text, int count);
extern int (*gd_drawstring)(PSD psd, PMWCFONT font, MWCOORD x, MWCOORD y, const unsigned char *text, int count,
	MWPIXELVAL fg, MWPIXELVAL bg, int usebg, MWCOORD *endx);
extern void (*gd_hwcursor)(MWCOORD x, MWCOORD y, MWCOORD width, MWCOORD height, int visible,
	const MWIMAGEBITS *image, const MWIMAGEBITS *mask, MWPIXELVAL fg, MWPIXELVAL bg);

static int gpu_fd = -1;
static unsigned char *gpu;		/* GPU memory, mapped */
static volatile uint32_t *regs;
static SUBDRIVER soft;			/* the software drawing routines */
static int accelerate = 1;
static struct surface root, surfaces[MAX_SURFACES];
/* pixmaps: one whose pixels are the GPU's is drawn on and copied from as a window is */
static struct surface pixmaps[MAX_PIXMAPS];
static int blended;			/* the queue has blended text: what follows it is queued in that pass */
static int most_w = 2048, most_h = 2048;	/* the GPU's target as the host has it: it draws nothing past them */
static struct surface *target = &root;	/* the surface the queued commands draw on */
static int commands;			/* queued so far, including the first */
static uint32_t mesh_top;		/* mesh vertices the queued commands take */
static struct { PMWCFONT font; uint32_t table; } fonts[MAX_FONTS];
static uint32_t fonts_top = FONTS_OFFSET;
static uint32_t data_top = DATA_OFFSET;	/* next free byte for textures */
/* the string last measured, whose words are at data_top until something else is put there */
static struct { PMWCFONT font; const unsigned char *text; int count, width; uint32_t at; } measured;
static int dirty_x0 = 1 << 30, dirty_y0 = 1 << 30, dirty_x1, dirty_y1;	/* what the queue covers */
static struct { const unsigned char *lo, *hi; } font_bits[NUMBER_FONTS];
static struct { const unsigned char *src; uint32_t shape, at; } cache[CACHE_SLOTS];
static uint32_t cache_top = CACHE_OFFSET;
static struct { uint32_t at, bytes; int used; } blocks[MAX_BLOCKS];
static int block_count;
static int wm_state;			/* 1: a worker core has the pointer; 0: none now; -1: never */
static uint32_t wm_core, wm_tag;	/* which core, and whose the kernel says it is while it is ours */
static uint32_t wm_offers = ~0u;	/* the kernel's count of cores come free, when one was last asked for */
static uint32_t wm_runs;
static struct surface *wm_dragged;	/* the surface the worker moves with the pointer, */
static uint32_t wm_drag;		/* in which drag: they are numbered */
static struct surface *wm_pinned;	/* a surface the worker lost in a drag stays where it had it */
static int wm_pin_x, wm_pin_y;
static unsigned wm_quiet_seen = ~0u;
static int layers_stale = 1;		/* a surface came, went, moved or has another buffer: the table is made again */
/* the cursor as the window system has it: its image's corner, and how far that is from the pointer */
static struct { MWCOORD x, y, dx, dy; uint32_t on, image; } cursor_now;
static void cursor_tell(void);
static void cursor_check(void);
static uint8_t wm_stack[4096] __attribute__((aligned(16)));

/* ---------------- the pointer's worker core */

/*
 * Runs on a worker core: the cursor where the pointer is and a dragged surface with it, every
 * pass, whatever the first core is busy with. It touches GPU memory and its stack, nothing
 * else of the program, and stores to texels no other core does. Core 0: once through, and back.
 */
uint32_t wm_job(uint32_t core) __attribute__((used));
uint32_t
wm_job(uint32_t core)
{
	volatile uint32_t *r = (volatile uint32_t *)(gpu + REGS_OFFSET);
	volatile uint32_t *w = (volatile uint32_t *)(gpu + WM_OFFSET);
	volatile uint32_t *table = (volatile uint32_t *)(gpu + LAYERS_OFFSET);
	uint32_t seen_x = ~0u, seen_y = 0, seen_b = 0, busy_at = r[REG_CLOCK];

	for (;;) {
		uint32_t x = r[REG_POINTER], y = r[REG_POINTER + 1], b = r[REG_POINTER + 2];
		uint32_t cx = x - w[WM_CURSOR], cy = y - w[WM_CURSOR + 1];
		uint32_t on = w[WM_CURSOR + 2], image = w[WM_CURSOR + 3];
		uint32_t buffer = w[WM_DRAG], drag = w[WM_DRAG + 3], i, n;
		int busy = x != seen_x || y != seen_y || b != seen_b;

		if (r[REG_CURSOR] != cx) r[REG_CURSOR] = cx, busy = 1;
		if (r[REG_CURSOR + 1] != cy) r[REG_CURSOR + 1] = cy, busy = 1;
		if (r[REG_CURSOR + 3] != image) r[REG_CURSOR + 3] = image, busy = 1;
		if (r[REG_CURSOR + 2] != on) r[REG_CURSOR + 2] = on, busy = 1;
		if (buffer && w[WM_AT + 3] != drag) {
			uint32_t ax = x - w[WM_DRAG + 1], ay = y - w[WM_DRAG + 2];

			if (!(b & 1)) {
				w[WM_AT + 3] = drag, busy = 1;	/* let go: where it is, it stays */
			} else {
				if (w[WM_AT] != ax) w[WM_AT] = ax, busy = 1;
				if (w[WM_AT + 1] != ay) w[WM_AT + 1] = ay, busy = 1;
				if (w[WM_AT + 2] != drag) w[WM_AT + 2] = drag, busy = 1;
			}
		}
		if (buffer && w[WM_AT + 2] == drag)
			for (i = 0, n = table[0]; i < n && i <= MAX_SURFACES; i++) {
				volatile uint32_t *layer = table + 4 + 8 * i;

				if (layer[4] != buffer)
					continue;
				if (layer[0] != w[WM_AT]) layer[0] = w[WM_AT], busy = 1;
				if (layer[1] != w[WM_AT + 1]) layer[1] = w[WM_AT + 1], busy = 1;
				break;
			}
		if (!core)
			return 0;
		if (!w[WM_RUN])
			__asm__ volatile(MC_EBREAK);	/* the window system is leaving: parked */
		if (w[WM_HERE] != w[WM_RUN]) w[WM_HERE] = w[WM_RUN], busy = 1;
		seen_x = x, seen_y = y, seen_b = b;
		if (busy)
			busy_at = r[REG_CLOCK];
		/* a tenth of a second after the last change it sleeps, until its job word changes
		 * (the kernel's tick when the pointer moves, wm_poke()); till then a pass, and on */
		if (r[REG_CLOCK] - busy_at < 100)
			mc_end_pass();	/* fence.i: the pass ends here */
		else
			next_frame();
	}
}

/* The worker starts here: the machine gives it a stack and its number, the program's global
 * pointer is ours to set. */
void wm_entry(void);
__asm__(".text\n"
	".globl wm_entry\n"
	"wm_entry:\n"
	".option push\n"
	".option norelax\n"
	"	la gp, __global_pointer$\n"
	".option pop\n"
	"	call wm_job\n"
	"	ebreak\n");

/* The kernel takes no core of ours while this says we write to one (mc.h): 0 if the worker's
 * core is another's by now. wm_ours(0) when the writing is done. */
static int
wm_ours(int writing)
{
	volatile uint32_t *box = (volatile uint32_t *)(gpu + MC_PAGE_GPU_OFFSET);

	box[MC_POSTING_AT(wm_tag) / 4] = writing;
	if (!writing || box[MC_OWNER_AT(wm_core) / 4] == wm_tag)
		return writing;
	box[MC_POSTING_AT(wm_tag) / 4] = 0;
	return 0;
}

/*
 * Wakes the worker: its job word changes. The answer and "taken" words are kept the same as
 * it, which is how the kernel knows a core with no job: one it may have back at once.
 */
static void
wm_poke(void)
{
	volatile uint32_t *job = (volatile uint32_t *)(gpu + MC_PAGE_GPU_OFFSET + MC_JOB_AT(wm_core));
	uint32_t seq;

	if (wm_state <= 0 || !wm_ours(1))
		return;
	/* (one instruction: the kernel's tick adds to it too) */
	__atomic_fetch_add((uint32_t *)job, 1, __ATOMIC_RELAXED);
	do {
		seq = job[0];
		job[4] = seq;
		job[14] = seq;
	} while (job[0] != seq);
	wm_ours(0);
}

/* The worker's core is no longer ours (the kernel had another use for it), or never came:
 * the cursor and a surface being dragged are the window system's own again, as without one. */
static void
wm_lost(int again)
{
	volatile uint32_t *w = (volatile uint32_t *)(gpu + WM_OFFSET);

	wm_state = again ? 0 : -1;
	wm_offers = *(volatile uint32_t *)(gpu + MC_PAGE_GPU_OFFSET + MC_OFFER_AT);
	((volatile uint32_t *)(gpu + WM_QUIET_OFFSET))[3] = 0;
	if (wm_dragged && w[WM_AT + 2] == wm_drag) {
		wm_pinned = wm_dragged;
		wm_pin_x = (int)w[WM_AT];
		wm_pin_y = (int)w[WM_AT + 1];
	}
	wm_dragged = NULL;
	layers_stale = 1;
	if (cursor_now.image) {
		regs[REG_CURSOR] = cursor_now.x;
		regs[REG_CURSOR + 1] = cursor_now.y;
		regs[REG_CURSOR + 3] = cursor_now.image;
	}
	regs[REG_CURSOR + 2] = cursor_now.on;
}

/*
 * A core is asked for and the worker started on it, when the pointer first moves (a machine
 * nobody points at has none) and again when cores have come free. It is only lent: a program
 * that keeps its workers has it back from us at once.
 */
static void
wm_start(void)
{
	static int asked;
	volatile uint32_t *w = (volatile uint32_t *)(gpu + WM_OFFSET);
	volatile uint32_t *box = (volatile uint32_t *)(gpu + MC_PAGE_GPU_OFFSET), *start;
	uint32_t answer[2 + MC_MASK_WORDS] = { 1 }, core;
	int pass;

	if (box[MC_OFFER_AT / 4] == wm_offers)
		return;		/* (none has come free since) */
	wm_offers = box[MC_OFFER_AT / 4];
	if (!asked) {
		const char *off = getenv("NANOX_POINTER_CORE");

		asked = 1;
		if ((off && atoi(off) == 0) || ioctl(gpu_fd, MC_LENT) != 0) {
			wm_state = -1;		/* (the same binary without it, to compare; a kernel without) */
			return;
		}
		/* its code and its stack here and kept: nobody brings a worker a page while we sleep */
		wm_job(0);
		memset(wm_stack, 0, sizeof wm_stack);
		mlock((void *)((uintptr_t)wm_job & ~4095u), 8192);
		mlock(wm_stack, sizeof wm_stack);
	}
	if (ioctl(gpu_fd, MC_WORKERS_ALL, answer) != 0)
		return;
	for (core = 1; core < MC_MAX_CORES && !(answer[2 + core / 32] >> (core % 32) & 1); core++)
		;
	if (core == MC_MAX_CORES)
		return;
	wm_core = core;
	wm_tag = answer[0] >> 12;
	cursor_tell();
	w[WM_RUN] = ++wm_runs;
	start = box + MC_START_AT(core) / 4;
	if (!wm_ours(1))
		return;
	start[1] = (uint32_t)wm_entry;
	start[2] = (uint32_t)(wm_stack + sizeof wm_stack - 16);
	start[3] = answer[1];
	start[0] = MC_START;
	wm_ours(0);
	for (pass = 0; pass < 24 && w[WM_HERE] != wm_runs; pass++)
		next_frame();
	if (w[WM_HERE] != wm_runs || !wm_ours(1)) {
		/* it never ran, or is another's already: back it goes */
		answer[2 + core / 32] = 1u << (core % 32);
		ioctl(gpu_fd, MC_GIVE, answer + 2);
		wm_lost(1);
		return;
	}
	start[0] = 0;
	wm_ours(0);
	wm_state = 1;
	wm_poke();
}

/* Before the server waits: whether the worker is ours still and runs, and the kernel is told
 * where the pointer's moves are nobody's news. */
static void
wm_look(void)
{
	volatile uint32_t *quiet = (volatile uint32_t *)(gpu + WM_QUIET_OFFSET);
	volatile uint32_t *box = (volatile uint32_t *)(gpu + MC_PAGE_GPU_OFFSET);
	volatile uint32_t *fault = box + MC_FAULT_AT(wm_core) / 4, *resume = box + MC_RESUME_AT(wm_core) / 4;
	MWCOORD x0, y0, x1, y1;
	uint32_t from = 0, to = 0;

	if (box[MC_OWNER_AT(wm_core) / 4] != wm_tag) {
		wm_lost(1);
		return;
	}
	if (fault[0] != resume[0] && wm_ours(1)) {
		/* it stopped at a page that is not there (it should never): brought, and on it goes */
		volatile uint8_t *byte = (volatile uint8_t *)fault[2];

		if (fault[1] == 12 || fault[1] == 13) {
			(void)*byte;
		} else if (fault[1] == 15) {
			*byte = *byte;
		} else {
			EPRINTF("nano-X: the pointer's core stopped, cause %u at %08x\n", (unsigned)fault[1],
				(unsigned)fault[3]);
			wm_ours(0);
			wm_lost(0);
			return;
		}
		resume[0] = fault[0];
		wm_ours(0);
	}
	if (wm_quiet_seen == gd_scene_gen && quiet[3] == WM_MAGIC)
		return;
	wm_quiet_seen = gd_scene_gen;
	if (gd_quiet_rect && gd_quiet_rect(&x0, &y0, &x1, &y1)) {
		from = (uint32_t)(x0 < 0 ? 0 : x0) | (uint32_t)(y0 < 0 ? 0 : y0) << 16;
		to = (uint32_t)(x1 > 32767 ? 32767 : x1) | (uint32_t)(y1 > 32767 ? 32767 : y1) << 16;
	}
	if (quiet[0] != from || quiet[1] != to) {
		quiet[1] = 0;	/* (none while the two are not a pair: the tick may come between) */
		quiet[0] = from;
		quiet[1] = to;
	}
	if (quiet[2] != wm_core)
		quiet[2] = wm_core;
	if (quiet[3] != WM_MAGIC)
		quiet[3] = WM_MAGIC;
}

/* ---------------- buffers: first fit in the pool, neighbours merged when freed */

static uint32_t
pool_take(uint32_t bytes)
{
	int i, j;

	bytes = (bytes + 4095) & ~4095u;
	for (i = 0; i < block_count; i++) {
		if (blocks[i].used || blocks[i].bytes < bytes)
			continue;
		if (blocks[i].bytes > bytes && block_count < MAX_BLOCKS) {
			for (j = block_count++; j > i + 1; j--)
				blocks[j] = blocks[j - 1];
			blocks[i + 1].at = blocks[i].at + bytes;
			blocks[i + 1].bytes = blocks[i].bytes - bytes;
			blocks[i + 1].used = 0;
			blocks[i].bytes = bytes;
		}
		blocks[i].used = 1;
		return blocks[i].at;
	}
	return ~0u;
}

static void
pool_give(uint32_t at)
{
	int i, j;

	for (i = 0; i < block_count; i++)
		if (blocks[i].at == at)
			blocks[i].used = 0;
	for (i = 0; i + 1 < block_count; i++)
		while (i + 1 < block_count && !blocks[i].used && !blocks[i + 1].used) {
			blocks[i].bytes += blocks[i + 1].bytes;
			for (j = i + 1; j + 1 < block_count; j++)
				blocks[j] = blocks[j + 1];
			block_count--;
		}
}

/* ---------------- the command queue */

static int
is_surface(PSD psd)
{
	struct surface *s = (struct surface *)psd;

	return psd == &scrdev || (s >= surfaces && s < surfaces + MAX_SURFACES) ||
	       (s >= pixmaps && s < pixmaps + MAX_PIXMAPS && s->pixmap == 2);
}

static struct surface *
surface_of(PSD psd)
{
	return psd == &scrdev ? &root : (struct surface *)psd;
}

/*
 * Programs share the GPU through its submit word: nonzero until the list has been drawn. The
 * lock only covers looking at that word and writing a new list's registers. Returns the value
 * the copy counter will have when this list's picture is in RAM.
 */
static uint32_t
submit(int count, uint32_t address, int width, int height, uint32_t passes)
{
	uint32_t landed;

	for (;;) {
		while (__atomic_exchange_n(&regs[REG_LOCK], 1, __ATOMIC_ACQUIRE))
			sched_yield();
		if (regs[REG_SUBMIT] == 0)
			break;
		__atomic_store_n(&regs[REG_LOCK], 0, __ATOMIC_RELEASE);
		next_frame();
	}
	/* a copy still pending is another list's and is made before ours */
	landed = regs[REG_COPIES] + (regs[REG_COPY] != 0) + 1;
	regs[REG_INTO] = address;
	regs[REG_INTO + 1] = width;
	regs[REG_INTO + 2] = height;
	regs[REG_INTO + 3] = width;
	regs[REG_LIST] = GPU_PHYS + LIST_OFFSET;
	regs[REG_COUNT] = count;
	regs[REG_SUBMIT] = SUBMIT_DRAW | SUBMIT_INTO | passes;
	__atomic_store_n(&regs[REG_LOCK], 0, __ATOMIC_RELEASE);
	return landed;
}

static uint32_t landed;		/* the copy counter once the list submitted last is in its buffer */
static int undrawn;		/* that list may not be drawn yet: its commands and textures are still read */

/* The list submitted last is drawn: the list and texture memory may be written again. */
static void
drawn(void)
{
	if (!undrawn)
		return;
	while (regs[REG_SUBMIT] != 0)
		next_frame();
	undrawn = 0;
}

/* ...and its picture is in the surface's buffer: software may read and draw there. */
static void
settle(void)
{
	drawn();
	while ((int32_t)(regs[REG_COPIES] - landed) < 0)
		next_frame();
}

/*
 * Hands the queued commands to the GPU, which draws them and copies the picture back into
 * the surface's buffer, a pass later. Nothing waits for that here: a wait ends the machine's
 * pass, so it is left to whoever needs the result, drawn() or settle().
 */
static void
flush_last(void)
{
	if (commands == 0)
		return;
	drawn();
	/* (bit 8 + n of the submit word: the list has commands of pass n) */
	landed = submit(commands, GPU_PHYS + target->at, target->w, target->h, blended ? 1u << 13 : 0);
	blended = 0;
	undrawn = 1;
	commands = 0;
	mesh_top = 0;
	data_top = DATA_OFFSET;
	measured.text = NULL;
	dirty_x0 = dirty_y0 = 1 << 30;
	dirty_x1 = dirty_y1 = 0;
}

/* The same when more is drawn right after: the list's memory is free again on return. */
static void
flush(void)
{
	flush_last();
	drawn();
}

/* Writes command `n`: a rectangle, x1 and y1 exclusive, optionally showing a texture. */
static inline __attribute__((always_inline)) void
emit(int n, int x0, int y0, int x1, int y1, uint32_t colour, int mode, uint32_t at,
	int w, int h, int sx, int sy)
{
	volatile uint32_t *c = (volatile uint32_t *)(gpu + LIST_OFFSET) + 16 * n;

	c[0] = CMD_RECT;
	c[1] = colour & 0xffffff;
	c[2] = 0;
	c[3] = mesh_top;
	mesh_top += 6;
	c[4] = x0;
	c[5] = y0;
	c[6] = x1;
	c[7] = y1;
	c[8] = mode | (blended && n ? PASS_OVER : 0);
	if (mode == FRAG_COLOUR)
		return;
	c[9] = GPU_PHYS + at;
	c[10] = w;
	c[11] = h;
	/* 16.16 fractions of the texture; sizes are at most 2048, so 32 bits are enough */
	c[12] = ((uint32_t)sx << 16) / (uint32_t)w;
	c[13] = ((uint32_t)sy << 16) / (uint32_t)h;
	c[14] = ((uint32_t)(sx + x1 - x0) << 16) / (uint32_t)w;
	c[15] = ((uint32_t)(sy + y1 - y0) << 16) / (uint32_t)h;
}

/*
 * Queues a rectangle on the target surface, given in screen coordinates. A list starts by
 * drawing the surface's buffer, so the picture begins as what RAM holds.
 */
static inline __attribute__((always_inline)) void
queue(int x0, int y0, int x1, int y1, uint32_t colour, int mode, uint32_t at, int w, int h, int sx, int sy)
{
	if (commands == MAX_COMMANDS || mesh_top + 6 > MAX_MESH)
		flush();
	if (commands == 0)
		emit(commands++, 0, 0, target->w, target->h, 0xffffff, FRAG_TEXTURE, target->at,
			target->w, target->h, 0, 0);
	x0 -= target->x;
	x1 -= target->x;
	y0 -= target->y;
	y1 -= target->y;
	if (x0 < dirty_x0) dirty_x0 = x0;
	if (y0 < dirty_y0) dirty_y0 = y0;
	if (x1 > dirty_x1) dirty_x1 = x1;
	if (y1 > dirty_y1) dirty_y1 = y1;
	emit(commands++, x0, y0, x1, y1, colour, mode, at, w, h, sx, sy);
}

/* Makes `psd` the surface being drawn on. */
static struct surface *
draw_on(PSD psd)
{
	struct surface *s = surface_of(psd);

	drawn();
	if (s != target) {
		flush();
		target = s;
	}
	return s;
}

/* Whether a plain drawing call goes to the GPU. */
static int
queued(PSD psd)
{
	return accelerate && gr_mode == MWROP_COPY;
}

/* Software is about to draw on a surface: its buffer must be up to date. */
static void
software(PSD psd)
{
	draw_on(psd);
	flush();
	settle();
}

static void
gpu_drawpixel(PSD psd, MWCOORD x, MWCOORD y, MWPIXELVAL c)
{
	/* a lone pixel is not worth a list */
	if (queued(psd) && surface_of(psd) == target && commands > 0) {
		queue(x, y, x + 1, y + 1, c, FRAG_COLOUR, 0, 0, 0, 0, 0);
		return;
	}
	software(psd);
	soft.DrawPixel(psd, x, y, c);
}

static MWPIXELVAL
gpu_readpixel(PSD psd, MWCOORD x, MWCOORD y)
{
	if (surface_of(psd) == target)
		flush();
	settle();
	return soft.ReadPixel(psd, x, y) | 0xff000000;
}

static void
gpu_drawhorzline(PSD psd, MWCOORD x1, MWCOORD x2, MWCOORD y, MWPIXELVAL c)
{
	if (queued(psd)) {
		draw_on(psd);
		queue(x1, y, x2 + 1, y + 1, c, FRAG_COLOUR, 0, 0, 0, 0, 0);
		return;
	}
	software(psd);
	soft.DrawHorzLine(psd, x1, x2, y, c);
}

static void
gpu_drawvertline(PSD psd, MWCOORD x, MWCOORD y1, MWCOORD y2, MWPIXELVAL c)
{
	if (queued(psd)) {
		draw_on(psd);
		queue(x, y1, x + 1, y2 + 1, c, FRAG_COLOUR, 0, 0, 0, 0, 0);
		return;
	}
	software(psd);
	soft.DrawVertLine(psd, x, y1, y2, c);
}

static void
gpu_fillrect(PSD psd, MWCOORD x1, MWCOORD y1, MWCOORD x2, MWCOORD y2, MWPIXELVAL c)
{
	if (queued(psd)) {
		draw_on(psd);
		queue(x1, y1, x2 + 1, y2 + 1, c, FRAG_COLOUR, 0, 0, 0, 0, 0);
		return;
	}
	software(psd);
	soft.FillRect(psd, x1, y1, x2, y2, c);
}

/* Copy from a surface (this one or another): the source's buffer is the texture. */
static void
gpu_frameblit(PSD psd, PMWBLITPARMS gc)
{
	if (accelerate && gc->op == MWROP_COPY && gc->srcpsd && is_surface(gc->srcpsd)) {
		struct surface *s = draw_on(psd), *from = surface_of(gc->srcpsd);
		int sx = gc->srcx - from->x, sy = gc->srcy - from->y;

		/* the source is read from RAM, which lacks what is still queued; and after blended text
		 * a copy would blend by its texels' top bytes (a decoded picture's are 0xff: not drawn) */
		if (blended || (from == s && sx < dirty_x1 && sx + gc->width > dirty_x0 && sy < dirty_y1 && sy + gc->height > dirty_y0))
			flush();
		queue(gc->dstx, gc->dsty, gc->dstx + gc->width, gc->dsty + gc->height, 0xffffff,
			FRAG_TEXTURE, from->at, from->w, from->h, sx, sy);
		return;
	}
	software(psd);
	soft.FrameBlit(psd, gc);
}

/* A copy into a pixmap reads its source from RAM: a surface must be up to date first. */
static void
pixmap_frameblit(PSD psd, PMWBLITPARMS gc)
{
	if (gc->srcpsd && is_surface(gc->srcpsd) && surface_of(gc->srcpsd) == target)
		flush();
	if (gc->srcpsd && is_surface(gc->srcpsd))
		settle();
	psd->orgsubdriver->FrameBlit(psd, gc);
}

static void gpu_fillrect(PSD psd, MWCOORD x1, MWCOORD y1, MWCOORD x2, MWCOORD y2, MWPIXELVAL c);

/* A pixmap's device is one of pixmaps[] while there is one free. */
static PSD
pixmap_new(PSD psd)
{
	struct surface *s;

	for (s = pixmaps; s < pixmaps + MAX_PIXMAPS && s->pixmap; s++)
		;
	if (s == pixmaps + MAX_PIXMAPS)
		return gen_allocatememgc(psd);
	memset(s, 0, sizeof *s);
	s->psd = *psd;
	s->psd.flags = PSF_MEMORY;
	s->psd.portrait = MWPORTRAIT_NONE;
	s->psd.addr = NULL;
	s->psd.Update = NULL;
	s->psd.palette = NULL;
	s->psd.palsize = 0;
	s->psd.transcolor = MWNOCOLOR;
	s->pixmap = 1;
	return &s->psd;
}

/*
 * A pixmap of the screen's kind has its pixels in the GPU's memory when there is room: what is
 * drawn on it the GPU draws, and a copy from it to a window is one rectangle (a page drawn
 * once and scrolled). Another is the server's memory, drawn on in software.
 */
static MWBOOL
pixmap_map(PSD mempsd, MWCOORD w, MWCOORD h, int planes, int bpp, MWIMGDATFMT data_format,
	unsigned int pitch, int size, void *addr)
{
	struct surface *s = (struct surface *)mempsd;
	uint32_t at;

	if (!gen_mapmemgc(mempsd, w, h, planes, bpp, data_format, pitch, size, addr))
		return 0;
	if (mempsd->FrameBlit)
		mempsd->FrameBlit = pixmap_frameblit;
	if (s < pixmaps || s >= pixmaps + MAX_PIXMAPS || !accelerate || bpp != 32 || pitch != (unsigned)w * 4 ||
	    w > most_w || h > most_h || (at = pool_take((uint32_t)w * h * 4)) == ~0u)
		return 1;
	if (mempsd->flags & PSF_ADDRMALLOC)
		free(addr);
	mempsd->flags &= ~PSF_ADDRMALLOC;
	mempsd->addr = gpu + at;
	s->at = at;
	s->w = w;
	s->h = h;
	s->pixmap = 2;
	((volatile uint32_t *)(gpu + WM_OFFSET))[WM_PIXMAPS]++;
#define SCREENS(member) mempsd->member = scrdev.member
	SCREENS(DrawPixel); SCREENS(ReadPixel); SCREENS(DrawHorzLine); SCREENS(DrawVertLine); SCREENS(FillRect);
	SCREENS(BlitFallback); SCREENS(FrameBlit); SCREENS(FrameStretchBlit); SCREENS(BlitCopyMaskMonoByteMSB);
	SCREENS(BlitCopyMaskMonoByteLSB); SCREENS(BlitCopyMaskMonoWordMSB); SCREENS(BlitBlendMaskAlphaByte);
	SCREENS(BlitCopyRGBA8888); SCREENS(BlitSrcOverRGBA8888); SCREENS(BlitCopyRGB888); SCREENS(BlitStretchRGBA8888);
	/* a new pixmap is black: the GPU's fill, where the processor's would be megabytes of stores */
	gpu_fillrect(mempsd, 0, 0, w - 1, h - 1, 0);
	return 1;
}

static void
pixmap_free(PSD mempsd)
{
	struct surface *s = (struct surface *)mempsd;

	if (s < pixmaps || s >= pixmaps + MAX_PIXMAPS) {
		gen_freememgc(mempsd);
		return;
	}
	if (s->pixmap == 2) {
		if (s == target) {
			flush();
			target = &root;
		}
		settle();
		pool_give(s->at);
	} else if (mempsd->addr && (mempsd->flags & PSF_ADDRMALLOC)) {
		free(mempsd->addr);
	}
	if (mempsd->palette)
		free(mempsd->palette);
	s->pixmap = 0;
}

/* Copies bitmap rows into GPU memory at `at`, first pixel in the highest bit of each byte. */
static void
store_mask(uint32_t at, const unsigned char *src, unsigned int bytes, int swap)
{
	unsigned char *dst = gpu + at;
	unsigned int i;

	if (!swap) {
		memcpy(dst, src, bytes);
		return;
	}
	for (i = 0; i + 1 < bytes; i += 2) {
		dst[i] = src[i + 1];
		dst[i + 1] = src[i];
	}
}

/*
 * Where in GPU memory a bitmap of a built-in font is, copying it there the first time.
 * 0 for any other bitmap: only the fonts' tables are known never to change.
 */
static uint32_t
cached_mask(const unsigned char *src, unsigned int pitch, unsigned int height, int swap)
{
	uint32_t shape = pitch << 16 | height << 1 | swap, size = (pitch * height + 3) & ~3u;
	unsigned int f, slot, probe;

	for (f = 0; f < NUMBER_FONTS; f++)
		if (src >= font_bits[f].lo && src < font_bits[f].hi)
			break;
	if (f == NUMBER_FONTS)
		return 0;
	slot = ((uintptr_t)src >> 1) * 2654435761u >> 21;
	for (probe = 0; probe < 8; probe++, slot = (slot + 1) % CACHE_SLOTS) {
		if (cache[slot].src == src && cache[slot].shape == shape)
			return cache[slot].at;
		if (!cache[slot].src)
			break;
	}
	if (probe == 8 || cache_top + size > DATA_OFFSET) {
		flush();	/* queued commands still point into the cache */
		memset(cache, 0, sizeof(cache));
		cache_top = CACHE_OFFSET;
		slot = ((uintptr_t)src >> 1) * 2654435761u >> 21;
	}
	store_mask(cache_top, src, pitch * height, swap);
	cache[slot].src = src;
	cache[slot].shape = shape;
	cache[slot].at = cache_top;
	cache_top += size;
	return cache[slot].at;
}

/*
 * Text and bitmaps: one bit per pixel, drawn as a mask texture from GPU memory. `swap` is
 * set for 16-bit words, whose first pixel is in the second byte.
 */
static int
mask_blit(PSD psd, PMWBLITPARMS gc, int swap)
{
	unsigned int pitch = gc->src_pitch, size = (pitch * gc->height + 3) & ~3u;
	const unsigned char *src = (const unsigned char *)gc->data + gc->srcy * pitch;
	uint32_t at;

	if (!queued(psd) || gc->op != MWROP_COPY || size > DATA_END - DATA_OFFSET)
		return 0;
	draw_on(psd);
	at = cached_mask(src, pitch, gc->height, swap);
	if (!at) {
		if (data_top + size > DATA_END)
			flush();
		at = data_top;
		data_top += size;
		store_mask(at, src, pitch * gc->height, swap);
	}
	if (gc->usebg)
		queue(gc->dstx, gc->dsty, gc->dstx + gc->width, gc->dsty + gc->height, gc->bg_pixelval,
			FRAG_COLOUR, 0, 0, 0, 0, 0);
	queue(gc->dstx, gc->dsty, gc->dstx + gc->width, gc->dsty + gc->height, gc->fg_pixelval,
		FRAG_MASK, at, pitch * 8, gc->height, gc->srcx, 0);
	return 1;
}

/*
 * A built-in font's table in GPU memory, made the first time the font draws a string: a texel
 * a glyph (address of its bitmap, width, height), then the bitmaps, rows of whole bytes with
 * the first pixel in the highest bit. 0 when there is no room for another font.
 */
static uint32_t
font_table(PMWCFONT font)
{
	int f, g, r, b, wide16 = font->offset && ((const uint32_t *)font->offset)[0] >= 0x00010000;
	uint32_t table, at, bytes = 0;
	volatile uint32_t *entry;

	for (f = 0; f < MAX_FONTS && fonts[f].font; f++)
		if (fonts[f].font == font)
			return fonts[f].table;
	for (g = 0; g < font->size; g++)
		bytes += ((((font->width ? font->width[g] : font->maxwidth) + 7) >> 3) * font->height + 3) & ~3u;
	if (f == MAX_FONTS || fonts_top + 16 * font->size + bytes > CACHE_OFFSET)
		return 0;
	table = fonts_top;
	at = table + 16 * font->size;
	entry = (volatile uint32_t *)(gpu + table);
	for (g = 0; g < font->size; g++, entry += 4) {
		int width = font->width ? font->width[g] : font->maxwidth, row = (width + 7) >> 3, words = (width + 15) >> 4;
		const MWIMAGEBITS *bits = font->bits + (!font->offset ? font->height * g :
			wide16 ? ((const unsigned short *)font->offset)[g] : ((const uint32_t *)font->offset)[g]);
		unsigned char *to = gpu + at;

		for (r = 0; r < (int)font->height; r++)
			for (b = 0; b < row; b++)
				*to++ = (b & 1) ? bits[r * words + b / 2] & 0xff : bits[r * words + b / 2] >> 8;
		entry[0] = GPU_PHYS + at;
		entry[1] = width;
		entry[2] = font->height;
		entry[3] = 0;
		at += (row * font->height + 3) & ~3u;
	}
	fonts_top = (at + 15) & ~15u;
	fonts[f].font = font;
	fonts[f].table = table;
	return table;
}

/* One walk over a string: its width, and into `word` (if not NULL) a word a character. */
static int
string_words(PMWCFONT font, const unsigned char *text, int count, uint32_t *word)
{
	const unsigned char *widths = font->width, *end = text + count;
	uint32_t size = font->size, first = font->firstchar;
	int at = 0;

	if (!widths) {
		int fixed = font->maxwidth;

		if (word)
			for (; text < end; text++, at += fixed) {
				uint32_t g = *text - first;

				*word++ = (g < size ? g : 0) | (uint32_t)at << 16;
			}
		return count * fixed;
	}
	if (!word) {
		for (; text < end; text++) {
			uint32_t g = *text - first;

			at += widths[g < size ? g : 0];
		}
		return at;
	}
	for (; text < end; text++) {
		uint32_t g = *text - first;

		if (g >= size)
			g = 0;
		*word++ = g | (uint32_t)at << 16;
		at += widths[g];
	}
	return at;
}

/*
 * The width of a string of a built-in font. The words of its drawing command are written
 * as it is measured, where gpu_drawstring() will want them if it is called next.
 */
static int
gpu_measurestring(PMWCFONT font, const unsigned char *text, int count)
{
	/* (not while the list before may still be read: its words are in the same place) */
	int room = accelerate && !undrawn && count > 0 && count <= 4096 && data_top + 4 * count <= DATA_END;

	measured.text = NULL;
	if (count <= 0)
		return 0;
	measured.width = string_words(font, text, count, room ? (uint32_t *)(gpu + data_top) : NULL);
	if (room) {
		measured.font = font;
		measured.text = text;
		measured.count = count;
		measured.at = data_top;
	}
	return measured.width;
}

/*
 * A whole string of a built-in font as one command (docs/machine/gpu.md): the GPU makes a rectangle
 * of each character from the font's table. The processor's part is a word a character, the
 * glyph's number and where it starts, which for a proportional font only it can add up.
 */
static int
gpu_drawstring(PSD psd, PMWCFONT font, MWCOORD x, MWCOORD y, const unsigned char *text, int count,
	MWPIXELVAL fg, MWPIXELVAL bg, int usebg, MWCOORD *endx)
{
	volatile uint32_t *c;
	uint32_t table;
	int at;

	if (!is_surface(psd) || !queued(psd) || count <= 0 || count > 4096 || (table = font_table(font)) == 0)
		return 0;
	draw_on(psd);
	if (commands + 3 > MAX_COMMANDS || mesh_top + 6 * count + 12 > MAX_MESH || data_top + 4 * count > DATA_END)
		flush();
	if (measured.text == text && measured.count == count && measured.font == font && measured.at == data_top)
		at = measured.width;
	else
		at = string_words(font, text, count, (uint32_t *)(gpu + data_top));
	measured.text = NULL;
	/* through queue(): it starts a list with the surface's picture and keeps the area covered */
	queue(x, y, x + at, y + font->height, bg, FRAG_COLOUR, 0, 0, 0, 0, 0);
	if (!usebg) {
		commands--;
		mesh_top -= 6;
	}
	c = (volatile uint32_t *)(gpu + LIST_OFFSET) + 16 * commands++;
	c[0] = CMD_TEXT;
	c[1] = fg & 0xffffff;
	c[2] = GPU_PHYS + table;
	c[3] = mesh_top;
	c[4] = x - target->x;
	c[5] = y - target->y;
	c[6] = count;
	c[7] = GPU_PHYS + data_top;
	c[8] = blended ? PASS_OVER : 0;
	mesh_top += 6 * count;
	data_top += (4 * count + 15) & ~15u;
	*endx = x + at;
	return 1;
}

static void
gpu_maskwordmsb(PSD psd, PMWBLITPARMS gc)
{
	if (mask_blit(psd, gc, 1))
		return;
	software(psd);
	soft.BlitCopyMaskMonoWordMSB(psd, gc);
}

static void
gpu_maskbytemsb(PSD psd, PMWBLITPARMS gc)
{
	if (mask_blit(psd, gc, 0))
		return;
	software(psd);
	soft.BlitCopyMaskMonoByteMSB(psd, gc);
}

/* The ROM as a device, opened once and kept: every picture and icon is looked for in it. */
static int
rom_device(void)
{
	static int mtd = -2;

	if (mtd == -2)
		mtd = open("/dev/mtd0", O_RDONLY | O_CLOEXEC);
	return mtd;
}

/*
 * The physical address of a file's first byte when the file is in the ROM, else 0. romfs
 * numbers a file by where its header is (16 bytes, then its name, each padded to 16, then the
 * data in one piece), and the overlay on top passes the number on. `begins` is how the file
 * starts: a file that only has such a number by chance does not have that there.
 */
static uint32_t
rom_address(int fd, const char *begins, int count)
{
	struct stat st;
	unsigned char head[16 + 256], data[64];
	uint32_t at;
	int mtd, n;

	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || count > (int)sizeof data || (mtd = rom_device()) < 0)
		return 0;
	n = pread(mtd, head, sizeof head, st.st_ino);
	for (at = 16; (int)at < n && head[at]; at++)
		continue;
	/* the header's size field (big-endian) must be this file's, and the data its start */
	if (n < 32 || (int)at >= n ||
	    ((uint32_t)head[8] << 24 | head[9] << 16 | head[10] << 8 | head[11]) != (uint32_t)st.st_size ||
	    pread(mtd, data, count, st.st_ino + 16 + (at & ~15u)) != count || memcmp(data, begins, count) != 0)
		at = 0;
	return at ? ROM_PHYS + (uint32_t)st.st_ino + 16 + (at & ~15u) : 0;
}

/*
 * A PPM file stretched over a rectangle, scaled by the GPU. A file in the ROM is sampled where
 * it is; another has its rows read into texture memory, as many as fit at a time. 0 leaves
 * the file to the engine's decoder. Only what lies within the given rectangle is drawn.
 * With PICTURE_CELL in the flags it is one cell of a sheet of icons, its key colour left out;
 * with PICTURE_OWN the icon a file carries at its own end (any file: a program's, mostly);
 * with both the file's own icon, or that cell of the sheets when the file has none.
 */
#define PICTURE_CELL	0x80000000u	/* a cell of a sheet 16 cells wide: its size in bits 16-23, its number below */
#define PICTURE_OWN	0x40000000u	/* the file's own icon, of the size in bits 16-23 (32 or 16) */
#define PICTURE_KEY	0xff00ff
/* A file's own icon: 32 x 32 then 16 x 16 pixels of three bytes, then these eight, its last. */
#define ICON_MARK	"NXICON1"
#define ICON_BYTES	(32 * 32 * 3 + 16 * 16 * 3 + 8)

#define PICTURES	64
#define PICTURE_NONE	1u		/* (for `at`) a file of the ROM known to carry no icon */
/* files found in the ROM, with a sum of the name to look by */
static struct { char path[80]; uint32_t at, sum; int pw, ph, own; } pictures[PICTURES];
static int pictures_next;

static uint32_t
picture_sum(const char *path)
{
	uint32_t sum = 2166136261u;

	while (*path)
		sum = (sum ^ (unsigned char)*path++) * 16777619u;
	return sum;
}

static void
picture_keep(const char *path, uint32_t sum, uint32_t at, int pw, int ph, int own)
{
	int j = pictures_next++ & (PICTURES - 1);

	if (strlen(path) >= sizeof pictures[0].path)
		return;
	strcpy(pictures[j].path, path);
	pictures[j].sum = sum;
	pictures[j].at = at;
	pictures[j].pw = pw;
	pictures[j].ph = ph;
	pictures[j].own = own;
}

unsigned nx_look(void);

/* The sheet of icons of a size, for a file that was to show its own and has none: the look's. */
static const char *
picture_sheet(int size)
{
	if ((nx_look() & 0xffff00ffu) == 0x4c4b0001u)
		return size == 16 ? "/usr/share/icons7-16.ppm" : "/usr/share/icons7-32.ppm";
	return size == 16 ? "/usr/share/icons16.ppm" : "/usr/share/icons32.ppm";
}

#define PICTURE_ALPHA	(-1)		/* (for `own`) the table's entry is a sheet's .rgba twin */
#define ALPHA_CLEAR	0xff000000u	/* a clear pixel of such a twin, the key it is drawn with */

/* The twin NAME.rgba of a sheet NAME.ppm (tools/make_icons.py): its words' address in the ROM
 * and the sheet's size, or 0 when there is none there. Looked for once a sheet. */
static uint32_t
picture_alpha(const char *path, uint32_t sum, int *pw, int *ph)
{
	char twin[80], head[16];
	size_t n = strlen(path);
	uint32_t rom = 0, size[2] = { 0, 0 };
	int j, fd;

	for (j = 0; j < PICTURES; j++)
		if (pictures[j].sum == sum && pictures[j].own == PICTURE_ALPHA && !strcmp(pictures[j].path, path)) {
			*pw = pictures[j].pw, *ph = pictures[j].ph;
			return pictures[j].at == PICTURE_NONE ? 0 : pictures[j].at;
		}
	if (n < 5 || n + 2 > sizeof twin || strcmp(path + n - 4, ".ppm") != 0)
		return 0;
	memcpy(twin, path, n - 4);
	strcpy(twin + n - 4, ".rgba");
	if ((fd = open(twin, O_RDONLY)) >= 0) {
		if (read(fd, head, 16) == 16 && !memcmp(head, "SEI1", 4) && (rom = rom_address(fd, head, 16)) != 0) {
			memcpy(size, head + 4, 8);
			rom += 16;
		}
		close(fd);
	}
	*pw = (int)size[0], *ph = (int)size[1];
	picture_keep(path, sum, rom ? rom : PICTURE_NONE, *pw, *ph, PICTURE_ALPHA);
	return rom;
}

/* The desktop's look for the window manager (nxpaintnc.c): 0 until Settings has said one. */
unsigned
nx_look(void)
{
	return gpu ? ((volatile uint32_t *)(gpu + WM_OFFSET))[WM_LOOK] : 0;
}

/*
 * A file's own icon where it lies in the ROM, found without reading the file (a read is a page
 * copied, and a pass ended): the ROM's header at the file's number has its name and size.
 * 0 when that cannot be told; *none when the file is the ROM's and carries no icon.
 */
static uint32_t
rom_icon(const char *path, int own, int *none)
{
	static dev_t rom_dev;		/* what the kernel says a file of the ROM is on, once one was found */
	static int rom_known;
	const char *name = strrchr(path, '/');
	unsigned char head[64], mark[8];
	struct stat st;
	uint32_t size, data = 0;
	unsigned at;
	int mtd, n, same;

	name = name ? name + 1 : path;
	if ((own != 32 && own != 16) || stat(path, &st) || !S_ISREG(st.st_mode) || (mtd = rom_device()) < 0)
		return 0;
	n = pread(mtd, head, sizeof head, st.st_ino);
	for (at = 16; (int)at < n && head[at]; at++)
		continue;
	size = (uint32_t)head[8] << 24 | head[9] << 16 | head[10] << 8 | head[11];
	/* the header's name is the file's; under a link's name, the ROM's device says whose it is */
	same = !strcmp((const char *)head + 16, name);
	if (n >= 32 && (int)at < n && size == (uint32_t)st.st_size && (same || (rom_known && st.st_dev == rom_dev))) {
		if (same) {
			rom_dev = st.st_dev;
			rom_known = 1;
		}
		data = (uint32_t)st.st_ino + 16 + (at & ~15u);
		if (size < ICON_BYTES || pread(mtd, mark, 8, data + size - 8) != 8 || memcmp(mark, ICON_MARK, 8) != 0) {
			*none = 1;
			data = 0;
		}
	}
	return data ? ROM_PHYS + data + size - (own == 32 ? ICON_BYTES : 16 * 16 * 3 + 8) : 0;
}

static int
gpu_picture(PSD psd, MWCOORD x, MWCOORD y, MWCOORD width, MWCOORD height, const char *path,
	const MWRECT *within, int flags)
{
	struct surface *s;
	int pw = 0, ph = 0, most = 0, start = 0, x0, x1, y0, y1, band, rows = 1, j, fd = -1;
	int sx = 0, sy = 0, sw, sh, keyed = ((uint32_t)flags & (PICTURE_CELL | PICTURE_OWN)) != 0;
	int own = (uint32_t)flags & PICTURE_OWN ? (flags >> 16) & 0xff : 0, alpha = 0;
	char head[64];
	volatile uint32_t *c;
	uint32_t rom = 0, sum;

	if (!is_surface(psd))
		return 0;
	sum = accelerate ? picture_sum(path) : 0;
	/* a cell of a sheet with an .rgba twin: that twin's, blended */
	if (keyed && !own && accelerate && (rom = picture_alpha(path, sum, &pw, &ph)) != 0)
		alpha = 1;
	for (j = 0; accelerate && !alpha && j < PICTURES; j++)
		if (pictures[j].sum == sum && pictures[j].at && pictures[j].own == own && !strcmp(pictures[j].path, path)) {
			rom = pictures[j].at;
			pw = pictures[j].pw;
			ph = pictures[j].ph;
		}
	if (!rom && own && accelerate) {
		int none = 0;

		rom = rom_icon(path, own, &none);
		if (none) {
			picture_keep(path, sum, PICTURE_NONE, 0, 0, own);
			rom = PICTURE_NONE;
		} else if (rom) {
			pw = ph = own;
			picture_keep(path, sum, rom, own, own, own);
		}
	}
	/* no icon of its own: the sheet's cell, when one was named with it */
	if (rom == PICTURE_NONE)
		return ((uint32_t)flags & PICTURE_CELL) ? gpu_picture(psd, x, y, width, height, picture_sheet(own), within,
			flags & ~PICTURE_OWN) : 1;
	if (!rom) {
		if ((fd = open(path, O_RDONLY)) < 0)
			return own && ((uint32_t)flags & PICTURE_CELL) ? gpu_picture(psd, x, y, width, height,
				picture_sheet(own), within, flags & ~PICTURE_OWN) : 0;
		j = read(fd, head, sizeof head - 1);
		head[j > 0 ? j : 0] = 0;
		if (own) {
			/* the icon at the file's end: nothing drawn, and no fault, for a file without one */
			struct stat st;
			char mark[8];

			if ((own != 32 && own != 16) || fstat(fd, &st) || st.st_size < ICON_BYTES ||
			    pread(fd, mark, 8, st.st_size - 8) != 8 || memcmp(mark, ICON_MARK, 8) != 0) {
				close(fd);
				return ((uint32_t)flags & PICTURE_CELL) ? gpu_picture(psd, x, y, width, height,
					picture_sheet(own), within, flags & ~PICTURE_OWN) : 1;
			}
			pw = ph = own;
			rows = own;
			start = (int)st.st_size - (own == 32 ? ICON_BYTES : 16 * 16 * 3 + 8);
		} else if (sscanf(head, "P6 %d %d %d%n", &pw, &ph, &most, &start) != 3 || most != 255 || pw < 1 || ph < 1 ||
		    (rows = (DATA_END - DATA_OFFSET) / (3 * pw)) < 1) {
			close(fd);
			return 0;
		} else
			start++;	/* one white space ends the header */
		if (accelerate && (rom = rom_address(fd, head, j)) != 0) {
			rom += start;
			picture_keep(path, sum, rom, pw, ph, own);
		}
	}
	sw = pw;
	sh = ph;
	if (keyed && !own) {
		sw = sh = (flags >> 16) & 0xff;
		sx = (flags & 15) * sw;
		sy = ((flags & 0xffff) >> 4) * sh;
		if (sw < 1 || sx + sw > pw || sy + sh > ph) {	/* no such cell: nothing to draw */
			if (fd >= 0)
				close(fd);
			return 1;
		}
	}
	if (width <= 0) width = sw;
	if (height <= 0) height = sh;
	s = draw_on(psd);
	x0 = x > s->x ? x : s->x;
	x1 = x + width < s->x + s->w ? x + width : s->x + s->w;
	y0 = y > s->y ? y : s->y;
	y1 = y + height < s->y + s->h ? y + height : s->y + s->h;
	if (x0 < within->left) x0 = within->left;
	if (y0 < within->top) y0 = within->top;
	if (x1 > within->right) x1 = within->right;
	if (y1 > within->bottom) y1 = within->bottom;
	if (x0 >= x1 || y0 >= y1) {
		if (fd >= 0)
			close(fd);
		return 1;
	}
/* where screen column or row `p` of the rectangle lies in the file, as a 16.16 fraction of it */
#define PICTURE_U(p) (uint32_t)((((long long)sx * width + (long long)((p) - x) * sw) << 16) / ((long long)width * pw))
#define PICTURE_V(p) (uint32_t)((((long long)sy * height + (long long)((p) - y) * sh) << 16) / ((long long)height * ph))
	if (rom) {
		/* a file in the ROM is the texture where it lies: one rectangle, nothing read */
		if (fd >= 0)
			close(fd);
		if (alpha)
			blended = 1;	/* (this and what follows it in the list: the pass that blends) */
		queue(x0, y0, x1, y1, 0xffffff, alpha ? FRAG_TEXTURE | 0x100 : FRAG_RGB24 | (keyed ? 0x100 : 0), 0, pw, ph, 0, 0);
		c = (volatile uint32_t *)(gpu + LIST_OFFSET) + 16 * (commands - 1);
		c[2] = alpha ? ALPHA_CLEAR : PICTURE_KEY;
		c[9] = rom;
		c[12] = PICTURE_U(x0);
		c[13] = PICTURE_V(y0);
		c[14] = PICTURE_U(x1);
		c[15] = PICTURE_V(y1);
		return 1;
	}
	flush();
	if (!accelerate)
		settle();
	for (; y0 < y1; y0 += band) {
		band = !accelerate ? 1 : y1 - y0 < rows ? y1 - y0 : rows;
		for (j = 0; j < band; j++)
			if (pread(fd, gpu + DATA_OFFSET + j * 3 * pw, 3 * pw,
				  start + (off_t)(sy + (2LL * (y0 + j - y) + 1) * sh / (2 * height)) * 3 * pw) != 3 * pw)
				break;
		if (!accelerate) {
			/* without the GPU: the same texels, a row at a time */
			uint32_t *to = (uint32_t *)(gpu + s->at) + (y0 - s->y) * s->w - s->x;
			const unsigned char *row = gpu + DATA_OFFSET, *p;
			uint32_t step = ((uint32_t)sw << 16) / width;	/* texels a pixel, 16.16 */
			uint32_t at = ((uint32_t)sx << 16) + (uint32_t)(((2LL * (x0 - x) + 1) * sw << 15) / width);
			uint32_t colour;

			for (j = x0; j < x1; j++, at += step) {
				p = row + 3 * (at >> 16 < (uint32_t)(sx + sw) ? at >> 16 : (uint32_t)(sx + sw) - 1);
				colour = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
				if (!keyed || colour != PICTURE_KEY)
					to[j] = 0xff000000u | colour;
			}
			continue;
		}
		queue(x0, y0, x1, y0 + band, 0xffffff, FRAG_RGB24 | (keyed ? 0x100 : 0), DATA_OFFSET, pw, band, 0, 0);
		/* the part of the picture's width this rectangle shows, as 16.16 fractions */
		c = (volatile uint32_t *)(gpu + LIST_OFFSET) + 16 * (commands - 1);
		c[2] = PICTURE_KEY;
		c[12] = PICTURE_U(x0);
		c[13] = 0;
		c[14] = PICTURE_U(x1);
		c[15] = 65536;
		flush();
	}
	close(fd);
	return 1;
}

/* The remaining blits read or blend in software, on a buffer that is up to date. */
#define SOFTWARE_BLIT(name, member) \
static void name(PSD psd, PMWBLITPARMS gc) { software(psd); soft.member(psd, gc); }
SOFTWARE_BLIT(soft_stretchblit, FrameStretchBlit)
SOFTWARE_BLIT(soft_maskbytelsb, BlitCopyMaskMonoByteLSB)
SOFTWARE_BLIT(soft_blendalpha, BlitBlendMaskAlphaByte)
SOFTWARE_BLIT(soft_copyrgba, BlitCopyRGBA8888)
SOFTWARE_BLIT(soft_srcoverrgba, BlitSrcOverRGBA8888)
SOFTWARE_BLIT(soft_copyrgb, BlitCopyRGB888)
SOFTWARE_BLIT(soft_stretchrgba, BlitStretchRGBA8888)

static void
soft_blitfallback(PSD psd, MWCOORD x, MWCOORD y, MWCOORD w, MWCOORD h, PSD src, MWCOORD sx, MWCOORD sy, int op)
{
	software(psd);
	soft.BlitFallback(psd, x, y, w, h, src, sx, sy, op);
}

/* ---------------- fonts in the ROM (docs/system/fonts.md) */

/* A font file of ours: the GPU draws its glyphs from where the file lies. */
struct romfont {
	PMWFONTPROCS fontprocs;		/* an MWFONT's fields */
	MWCOORD fontsize, fontwidth;
	int fontrotation, fontattr;
	struct romfile *file;
	uint32_t scale;			/* 16.16, or 0 for the file's own size */
	uint32_t slant;			/* 256ths of a pixel a row */
};

static struct romfile {
	char path[64];
	const unsigned char *bytes;
	uint32_t size, rom, atlas;	/* atlas: SEF_MSDF's, in the ROM, with its header in `sizes` */
	struct sef_atlas sizes;
} romfiles[64];

extern PMWFONT (*gd_createfont)(const char *name, MWCOORD height, MWCOORD width, int attr) __attribute__((weak));
static MWFONTPROCS romfont_procs;

static int
rom_scaled(const struct romfont *f, int v)
{
	return f->scale ? (int)(((int64_t)v * f->scale + 0x8000) >> 16) : v;
}

/* The glyph a font draws for a code point: its own, or the one it has for what it lacks. */
static int
rom_glyph(const struct romfile *file, unsigned code)
{
	const struct sef_head *h = (const struct sef_head *)file->bytes;
	int g = sef_glyph(file->bytes, code < 32 ? 32 : code);	/* (a line's end inside a run of text is a space) */

	return g >= 0 ? g : h->missing < h->glyphs ? (int)h->missing : 0;
}

/* For a font of distance fields: its atlas (font-NAME.msdf beside font-NAME-N.sef) found in the
 * ROM once, and the name of the face's file whose line is nearest `height`, when not this one's. */
static const char *
msdf_nearest(struct romfile *file, int height)
{
	static char other[64];
	const struct sef_head *h = (const struct sef_head *)file->bytes;
	const struct sef_atlas *a = &file->sizes;
	char *dash, path[64];
	unsigned i, best = 0;
	int fd;

	strcpy(path, file->path);
	if ((dash = strrchr(path, '-')) == NULL)
		return NULL;
	strcpy(dash, ".msdf");
	if (!file->atlas && (fd = open(path, O_RDONLY)) >= 0) {
		if (read(fd, &file->sizes, sizeof *a) == sizeof *a && !memcmp(a->magic, "SEM1", 4) && a->sizes <= 11)
			file->atlas = rom_address(fd, (const char *)a, sizeof *a);
		close(fd);
	}
	if (!file->atlas || height <= 0 || height == h->height)
		return NULL;
	for (i = 1; i < a->sizes; i++)
		if (abs(a->size[i].height - height) <= abs(a->size[best].height - height))
			best = i;
	if (a->sizes == 0 || a->size[best].em == h->em)
		return NULL;
	*dash = 0;
	snprintf(other, sizeof other, "%s-%u.sef", path, a->size[best].em);
	return other;
}

/* A font by its file's name (it ends in .sef), `height` pixels high (0: as the file is) and
 * leaning `width` 256ths of a pixel a row; a face of distance fields takes its nearest size.
 * NULL for a file that is not such a font in the ROM. */
static PMWFONT
romfont_create(const char *name, MWCOORD height, MWCOORD width, int attr)
{
	size_t n = strlen(name);
	struct romfile *file;
	struct romfont *f;
	struct stat st;
	void *bytes;
	int fd;

	if (n < 5 || n >= sizeof romfiles[0].path || strcmp(name + n - 4, ".sef") != 0 || !accelerate)
		return NULL;
	for (file = romfiles; file < romfiles + 64 && file->bytes && strcmp(file->path, name) != 0; file++)
		;
	if (file == romfiles + 64)
		return NULL;
	if (!file->bytes) {
		uint32_t rom;

		if ((fd = open(name, O_RDONLY)) < 0)
			return NULL;
		bytes = fstat(fd, &st) == 0 && st.st_size >= 64 ? mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
		rom = bytes != MAP_FAILED && sef_ok(bytes, st.st_size) ? rom_address(fd, bytes, 64) : 0;
		close(fd);
		if (!rom) {
			if (bytes != MAP_FAILED)
				munmap(bytes, st.st_size);
			return NULL;
		}
		strcpy(file->path, name);
		file->bytes = bytes;
		file->size = st.st_size;
		file->rom = rom;
	}
	if (((const struct sef_head *)file->bytes)->bpp == SEF_MSDF) {
		const char *other = msdf_nearest(file, height);

		if (!file->atlas)
			return NULL;
		if (other)
			return romfont_create(other, height, width, attr);
	}
	if ((f = calloc(1, sizeof *f)) == NULL)
		return NULL;
	f->fontprocs = &romfont_procs;
	f->file = file;
	f->fontattr = attr;
	f->slant = width > 0 ? width : 0;
	f->fontsize = ((const struct sef_head *)file->bytes)->height;
	if (height > 0 && height != f->fontsize) {
		f->scale = ((uint32_t)height << 16) / f->fontsize;
		f->fontsize = height;
	}
	return (PMWFONT)f;
}

static void
romfont_destroy(PMWFONT pfont)
{
	free(pfont);
}

static MWBOOL
romfont_info(PMWFONT pfont, PMWFONTINFO info)
{
	const struct romfont *f = (const struct romfont *)pfont;
	const struct sef_head *h = (const struct sef_head *)f->file->bytes;
	int i;

	memset(info, 0, sizeof *info);
	info->height = rom_scaled(f, h->height);
	info->baseline = rom_scaled(f, h->ascent);
	info->descent = info->height - info->baseline;
	info->maxascent = info->baseline;
	info->maxdescent = info->descent;
	info->linespacing = info->height;
	info->lastchar = 0xffff;
	for (i = 32; i < 256; i++) {
		info->widths[i] = rom_scaled(f, sef_advance(f->file->bytes, rom_glyph(f->file, i)));
		if (info->widths[i] > info->maxwidth)
			info->maxwidth = info->widths[i];
	}
	return TRUE;
}

static void
romfont_size(PMWFONT pfont, const void *text, int cc, MWTEXTFLAGS flags, MWCOORD *pwidth, MWCOORD *pheight,
	MWCOORD *pbase)
{
	const struct romfont *f = (const struct romfont *)pfont;
	const struct sef_head *h = (const struct sef_head *)f->file->bytes;
	const unsigned short *code = text;
	int width = 0;

	while (cc-- > 0)
		width += sef_advance(f->file->bytes, rom_glyph(f->file, *code++));
	*pwidth = rom_scaled(f, width);
	*pheight = rom_scaled(f, h->height);
	*pbase = rom_scaled(f, h->ascent);
}

/* A string as one command: a word a character, and the table and the bitmaps in the ROM.
 * Glyphs of coverage are blended, in a pass of their own that the rest of the list follows. */
static void
romfont_draw(PMWFONT pfont, PSD psd, MWCOORD x, MWCOORD y, const void *text, int cc, MWTEXTFLAGS flags)
{
	const struct romfont *f = (const struct romfont *)pfont;
	const struct sef_head *h = (const struct sef_head *)f->file->bytes;
	const unsigned short *code = text;
	volatile uint32_t *c, *word;
	int at = 0, height = rom_scaled(f, h->height), i, n = 0, clip;

	if (!is_surface(psd) || !queued(psd) || cc <= 0 || cc > 4096)
		return;
	if (flags & MWTF_BASELINE)
		y -= rom_scaled(f, h->ascent);
	else if (flags & MWTF_BOTTOM)
		y -= height - 1;
	if (gr_usebg) {
		MWPIXELVAL fg = gr_foreground;
		MWCOORD w, hh, base;

		romfont_size(pfont, text, cc, flags, &w, &hh, &base);
		gr_foreground = gr_background;
		GdFillRect(psd, x, y, w, height);
		gr_foreground = fg;
	}
	draw_on(psd);
	if (commands + 3 > MAX_COMMANDS || mesh_top + 6 * cc + 12 > MAX_MESH || data_top + 4 * cc > DATA_END)
		flush();
	measured.text = NULL;
	word = (volatile uint32_t *)(gpu + data_top);
	for (i = 0; i < cc; i++)
		at += sef_advance(f->file->bytes, rom_glyph(f->file, code[i]));
	if ((clip = GdClipArea(psd, x, y, x + rom_scaled(f, at) - 1, y + height - 1)) == CLIP_INVISIBLE)
		return;
	/* a string partly out of view keeps the characters wholly in it: the GPU's text has no clip */
	for (i = 0, at = 0; i < cc; i++) {
		int g = rom_glyph(f->file, code[i]), left = rom_scaled(f, at);

		at += sef_advance(f->file->bytes, g);
		if (clip == CLIP_VISIBLE ||
		    GdClipArea(psd, x + left, y, x + rom_scaled(f, at) - 1, y + height - 1) == CLIP_VISIBLE)
			word[n++] = (uint32_t)g | (uint32_t)left << 16;
	}
	at = rom_scaled(f, at);
	if (n == 0)
		return;
	cc = n;
	/* (through queue(): it starts a list with the surface's picture and keeps the area covered) */
	queue(x, y, x + at, y + height, 0, FRAG_COLOUR, 0, 0, 0, 0, 0);
	commands--;
	mesh_top -= 6;
	if (h->bpp == 4 || h->bpp == SEF_MSDF)
		blended = 1;
	c = (volatile uint32_t *)(gpu + LIST_OFFSET) + 16 * commands++;
	c[0] = CMD_TEXT;
	c[1] = gr_foreground & 0xffffff;
	c[2] = f->file->rom + h->table_at;
	c[3] = mesh_top;
	c[4] = x - target->x;
	c[5] = y - target->y;
	c[6] = cc;
	c[7] = GPU_PHYS + data_top;
	c[8] = (h->bpp == SEF_MSDF ? FRAG_MSDF : h->bpp == 4 ? FRAG_COVER : FRAG_MASK) | TEXT_MORE | (blended ? PASS_OVER : 0);
	c[9] = f->scale;
	c[10] = f->slant | (uint32_t)h->ascent << 16;
	c[11] = h->bpp == SEF_MSDF ? f->file->atlas : f->file->rom;
	c[12] = h->range;
	mesh_top += 6 * cc;
	data_top += (4 * cc + 15) & ~15u;
}

static int
romfont_setattr(PMWFONT pfont, int setflags, int clrflags)
{
	int old = pfont->fontattr;

	pfont->fontattr = (old & ~clrflags) | setflags;
	return old;
}

static MWFONTPROCS romfont_procs = {
	0, MWTF_UC16, NULL, NULL, romfont_info, romfont_size, NULL, romfont_destroy, romfont_draw, NULL, NULL,
	romfont_setattr, NULL
};

/* ---------------- surfaces, for the window system */

/* Points a surface's drawing at its buffer, such that screen coordinates land inside it. */
static void
place(struct surface *s, int x, int y)
{
	s->x = x;
	s->y = y;
	layers_stale = 1;
	if (s == wm_pinned)
		wm_pinned = NULL;
	s->psd.addr = gpu + s->at - ((intptr_t)y * (intptr_t)s->psd.pitch + (intptr_t)x * 4);
}

/* The surface of window `owner`, which is at x, y on the screen and this big. */
static PSD
surface_for(void *owner, MWCOORD x, MWCOORD y, MWCOORD width, MWCOORD height)
{
	static struct surface *last;
	struct surface *s, *spare = NULL, *old = NULL;
	uint32_t at;

	if (last && last->owner == owner)
		s = last;
	else
		for (s = surfaces; s < surfaces + MAX_SURFACES && s->owner != owner; s++)
			if (!s->owner && !spare)
				spare = s;
	if (s < surfaces + MAX_SURFACES) {
		last = s;
		if (s->w == width && s->h == height) {
			if (s->x != x || s->y != y) {
				if (s == target)
					flush();
				place(s, x, y);
			}
			return &s->psd;
		}
		/* another size: a new buffer, which takes over what the old one shows */
		if (s == target)
			flush();
		settle();
		old = spare = s;
	}
	if (!spare || width <= 0 || height <= 0 || (at = pool_take((uint32_t)width * height * 4)) == ~0u) {
		if (old) {
			pool_give(old->at);
			old->owner = NULL;
		}
		return &scrdev;		/* nowhere to put it: it draws on the desktop */
	}
	if (old) {
		int rows = old->h < height ? old->h : height, columns = old->w < width ? old->w : width, row;

		for (row = 0; row < rows; row++)
			memcpy(gpu + at + (uint32_t)row * width * 4, gpu + old->at + (uint32_t)row * old->w * 4, columns * 4);
		pool_give(old->at);
	}
	s = spare;
	regs[REG_BUFFERS]++;
	s->psd = scrdev;
	s->psd.pitch = width * 4;
	/* the engine clips to these: a window is drawn past the screen's right and bottom edges */
	s->psd.xvirtres = s->psd.yvirtres = 16384;
	s->owner = owner;
	s->at = at;
	if (s == wm_dragged)
		((volatile uint32_t *)(gpu + WM_OFFSET))[WM_DRAG] = GPU_PHYS + at;
	s->w = width;
	s->h = height;
	place(s, x, y);
	last = s;
	return &s->psd;
}

/* The surface of window `owner` follows the pointer, its corner dx, dy from it, on the
 * pointer's worker core (owner NULL: no longer). 0 where there is no such core. */
static int
surface_drag(void *owner, MWCOORD dx, MWCOORD dy)
{
	volatile uint32_t *w = (volatile uint32_t *)(gpu + WM_OFFSET);
	struct surface *s;

	if (wm_state <= 0)
		return 0;
	wm_dragged = NULL;
	layers_stale = 1;
	for (s = surfaces; owner && s < surfaces + MAX_SURFACES; s++)
		if (s->owner == owner)
			wm_dragged = s;
	if (wm_dragged) {
		w[WM_DRAG + 1] = dx;
		w[WM_DRAG + 2] = dy;
		w[WM_DRAG + 3] = ++wm_drag;
		w[WM_DRAG] = GPU_PHYS + wm_dragged->at;
	} else {
		w[WM_DRAG] = 0;
	}
	wm_poke();
	return wm_dragged != NULL;
}

/* Whether the worker drags a surface (0: none), and where it has it (2) if it took it anywhere. */
static int
surface_dragged(MWCOORD *x, MWCOORD *y)
{
	volatile uint32_t *w = (volatile uint32_t *)(gpu + WM_OFFSET);

	if (!wm_dragged)
		return 0;
	if (w[WM_AT + 2] != wm_drag)
		return 1;
	*x = (int)w[WM_AT];
	*y = (int)w[WM_AT + 1];
	return 2;
}

static void
surface_release(void *owner)
{
	struct surface *s;

	for (s = surfaces; s < surfaces + MAX_SURFACES; s++)
		if (s->owner == owner) {
			layers_stale = 1;
			if (s == wm_dragged)
				surface_drag(NULL, 0, 0);
			if (s == wm_pinned)
				wm_pinned = NULL;
			if (s == target) {
				flush();
				target = &root;
			}
			settle();	/* (a copy into a buffer given back would land in the next one's) */
			pool_give(s->at);
			s->owner = NULL;
		}
}

static void
surface_touch(PSD psd)
{
}

/* Where a surface keeps the pixel for screen position x, y: physical address, row length. */
static void
surface_locate(PSD psd, MWCOORD x, MWCOORD y, uint32_t *address, uint32_t *row)
{
	struct surface *s = surface_of(psd);

	if (!is_surface(psd)) {
		*address = *row = 0;	/* (a pixmap in the server's memory) */
		return;
	}
	/* (no wait for what was drawn: a program that writes the pixels itself reads one first) */
	*address = GPU_PHYS + s->at + ((y - s->y) * s->w + (x - s->x)) * 4;
	*row = s->w;
}

static struct gd_compositor compositor = { surface_for, surface_release, surface_touch, surface_locate, surface_drag,
	surface_dragged };

/* ---------------- the screen: the desktop's buffer, then each window's, lowest first */

static int layer_count, layer_stores;

static void
show(PSD psd)
{
	struct surface *s = surface_of(psd);
	volatile uint32_t *layer = (volatile uint32_t *)(gpu + LAYERS_OFFSET) + 4 + 8 * layer_count;
	MWCOORD x = s->x, y = s->y;

	if ((s == &root && layer_count > 0) || layer_count > MAX_SURFACES)
		return;
	/* a surface the pointer's worker drags is where the worker has it (or had it last) */
	if (s == wm_dragged)
		surface_dragged(&x, &y);
	else if (s == wm_pinned)
		x = wm_pin_x, y = wm_pin_y;
	/* stores only what differs, so an unchanged screen costs no writes */
	if ((int)layer[0] != x) layer[0] = x, layer_stores++;
	if ((int)layer[1] != y) layer[1] = y, layer_stores++;
	if ((int)layer[2] != s->w) layer[2] = s->w, layer_stores++;
	/* (a window rolled up to its caption keeps its buffer: gd_show_height) */
	{
		int h = gd_show_height && gd_show_height < s->h ? gd_show_height : s->h;

		if ((int)layer[3] != h) layer[3] = h, layer_stores++;
	}
	if (layer[4] != GPU_PHYS + s->at) layer[4] = GPU_PHYS + s->at, layer_stores++;
	layer_count++;
}

/*
 * Before the server waits: finish drawing, and tell the display what is where. The display
 * composes the layers itself, so showing a window, moving it or changing the order of
 * windows is a matter of these few words.
 */
static int
gpu_preselect(PSD psd)
{
	volatile uint32_t *table = (volatile uint32_t *)(gpu + LAYERS_OFFSET);

	static unsigned shown = ~0u;	/* gd_scene_gen when the table was last made */
	int again;

	flush_last();
	/* the table is as it was unless a window came, went, moved or changed its place in the
	 * order, or a surface did; a drag's table is looked at every time (below) */
	if (shown == gd_scene_gen && !layers_stale && !wm_dragged)
		goto made;
	shown = gd_scene_gen;
	for (again = 0; ; again++) {
		layers_stale = 0;
		layer_count = layer_stores = 0;
		show(&scrdev);
		if (gd_composite_walk)
			gd_composite_walk(show);
		if ((int)table[0] != layer_count)
			table[0] = layer_count;
		/* during a drag the worker stores to a layer of the table as it was, and of two cores'
		 * stores to one texel one is lost: looked at again when it has seen this table */
		if (!wm_dragged || !layer_stores || again == 3)
			break;
		next_frame();
	}
made:
	if (wm_state > 0)
		wm_look();
	cursor_check();
	return 0;
}

/*
 * The cursor is the display's: a position and an image the display draws over the picture,
 * so moving it costs two stores and drawing never has to step around it.
 */
/* The pointer's worker is told the cursor: how far from the pointer its image's corner is, and what. */
static void
cursor_tell(void)
{
	volatile uint32_t *w = (volatile uint32_t *)(gpu + WM_OFFSET);
	int told = 0;

	if (cursor_now.image) {
		if (w[WM_CURSOR] != (uint32_t)cursor_now.dx) w[WM_CURSOR] = cursor_now.dx, told = 1;
		if (w[WM_CURSOR + 1] != (uint32_t)cursor_now.dy) w[WM_CURSOR + 1] = cursor_now.dy, told = 1;
		if (w[WM_CURSOR + 3] != cursor_now.image) w[WM_CURSOR + 3] = cursor_now.image, told = 1;
	}
	if (w[WM_CURSOR + 2] != cursor_now.on) w[WM_CURSOR + 2] = cursor_now.on, told = 1;
	if (told)
		wm_poke();
}

/* Before the server waits: a program that left (or the kernel at its exit) may have turned the
 * display's cursor off. Whoever writes the cursor's texel puts it back: the worker, else we. */
static void
cursor_check(void)
{
	if (regs[REG_CURSOR + 2] == cursor_now.on)
		return;
	if (wm_state > 0)
		wm_poke();	/* (it stores what differs from what it is told) */
	else
		regs[REG_CURSOR + 2] = cursor_now.on;
}

/* The display's cursor: its image's corner at x, y (address 0: not shown). */
static void
cursor_put(MWCOORD x, MWCOORD y, uint32_t address)
{
	static uint32_t first_x = ~0u, first_y;

	cursor_now.on = 0;
	if (address) {
		cursor_now.x = x;
		cursor_now.y = y;
		cursor_now.dx = gd_cursor_hotx;
		cursor_now.dy = gd_cursor_hoty;
		cursor_now.image = address;
		/* with the hot spot's place in the image, so a host can draw it under its own pointer */
		cursor_now.on = 1 | (gd_cursor_hotx & 31) << 8 | (gd_cursor_hoty & 31) << 16;
	}
	if (first_x == ~0u) {
		first_x = regs[REG_POINTER], first_y = regs[REG_POINTER + 1];
	} else if (!wm_state && (regs[REG_POINTER] != first_x || regs[REG_POINTER + 1] != first_y)) {
		wm_start();
	}
	if (wm_state > 0) {
		cursor_tell();
		return;
	}
	if (address) {
		regs[REG_CURSOR] = x;
		regs[REG_CURSOR + 1] = y;
		regs[REG_CURSOR + 3] = address;
	}
	regs[REG_CURSOR + 2] = cursor_now.on;
}

static void
gpu_cursor(MWCOORD x, MWCOORD y, MWCOORD width, MWCOORD height, int visible,
	const MWIMAGEBITS *image, const MWIMAGEBITS *mask, MWPIXELVAL fg, MWPIXELVAL bg)
{
	static MWIMAGEBITS drawn[2][MWMAX_CURSOR_BUFLEN];
	static MWPIXELVAL drawn_fg, drawn_bg;
	static int drawn_width, drawn_height;
	volatile uint32_t *pixels = (volatile uint32_t *)(gpu + CURSOR_OFFSET);
	int words = (width + 15) >> 4, bytes = words * height * sizeof(MWIMAGEBITS), r, c;

	if (!visible) {
		cursor_put(0, 0, 0);
		return;
	}
	/*
	 * A program's own picture of the pointer: a cursor of 32 x 1 whose mask is this mark and
	 * whose bits are the address of 32 x 32 colours in the GPU's memory, which the display
	 * shows as it is there (the games: linux/tdawn/host.c). No address: no pointer at all.
	 */
	if (width == 32 && height == 1 && mask[0] == 0x5345 && mask[1] == 0x4355) {
		uint32_t address = (uint32_t)image[0] << 16 | image[1];

		cursor_put(x, y, address);
		return;
	}
	if (width != drawn_width || height != drawn_height || fg != drawn_fg || bg != drawn_bg ||
	    memcmp(drawn[0], image, bytes) || memcmp(drawn[1], mask, bytes)) {
		for (r = 0; r < 32; r++)
			for (c = 0; c < 32; c++) {
				int bit = 0x8000 >> (c & 15), at = r * words + (c >> 4);
				int inside = r < height && c < width;

				if (inside && (mask[at] & bit))
					pixels[r * 32 + c] = 0xff000000 | (((image[at] & bit) ? bg : fg) & 0xffffff);
				else
					pixels[r * 32 + c] = 0;
			}
		memcpy(drawn[0], image, bytes);
		memcpy(drawn[1], mask, bytes);
		drawn_width = width;
		drawn_height = height;
		drawn_fg = fg;
		drawn_bg = bg;
	}
	cursor_put(x, y, GPU_PHYS + CURSOR_OFFSET);
}

static PSD
gpu_open(PSD psd)
{
	const char *size = getenv("NANOX_SIZE"), *x;
	int width = SCREEN_WIDTH, height = SCREEN_HEIGHT, f;
	PSUBDRIVER subdriver;

	if (size && (x = strchr(size, 'x')) != NULL) {
		width = atoi(size);
		height = atoi(x + 1);
	}
	if (width < 64 || height < 64 || width > 2048 || (unsigned)(width * height * 4) > POOL_SIZE / 2) {
		/* not a reason to have no desktop: the usual size instead */
		EPRINTF("NANOX_SIZE: %dx%d does not fit the display\n", width, height);
		width = SCREEN_WIDTH;
		height = SCREEN_HEIGHT;
	}
	accelerate = getenv("NANOX_SOFTWARE") == NULL;
	for (f = 0; f < NUMBER_FONTS; f++) {
		PMWCFONT font = gen_fonts[f].cfont;

		/* an upper bound for fonts whose glyphs are packed */
		font_bits[f].lo = (const unsigned char *)font->bits;
		font_bits[f].hi = font_bits[f].lo + font->size * font->height * ((font->maxwidth + 15) >> 4) * 2;
	}

	{
		/* one window system at a time: a second one would take the display, the pointer and
		 * the keyboard from under the first. The lock is this process's while it lives. */
		static int lock = -1;

		if (lock < 0)
			lock = open("/tmp/nano-X.lock", O_RDWR | O_CREAT, 0600);
		if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) {
			EPRINTF("nano-X: a window system is running already (one at a time)\n");
			return NULL;
		}
	}
	gpu_fd = open("/dev/gpu", O_RDWR);
	if (gpu_fd < 0) {
		EPRINTF("Error opening /dev/gpu: %m\n");
		return NULL;
	}
	gpu = mmap(NULL, GPU_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, gpu_fd, 0);
	if (gpu == MAP_FAILED) {
		EPRINTF("Error mapping /dev/gpu: %m\n");
		close(gpu_fd);
		gpu_fd = -1;
		return NULL;
	}
	regs = (volatile uint32_t *)(gpu + REGS_OFFSET);
	/* the host says the largest screen it shows, in 16s of pixels (0: it does not say) */
	f = regs[REG_HOST];
	if ((f >> 8 & 255) && (width > (int)(f >> 8 & 255) * 16 || height > (int)(f >> 16 & 255) * 16)) {
		EPRINTF("NANOX_SIZE: %dx%d is more than this host shows (%dx%d)\n", width, height, (int)(f >> 8 & 255) * 16,
			(int)(f >> 16 & 255) * 16);
		width = SCREEN_WIDTH;
		height = SCREEN_HEIGHT;
	}
	if ((f >> 8 & 255) && (f >> 16 & 255)) {
		most_w = (f >> 8 & 255) * 16;
		most_h = (f >> 16 & 255) * 16;
	}
	blocks[0].at = 0;
	blocks[0].bytes = POOL_SIZE;
	blocks[0].used = 0;
	block_count = 1;

	/* the desktop (the root window) is the first surface; the screen device draws on it */
	root.owner = &root;
	root.at = pool_take(width * height * 4);
	root.w = width;
	root.h = height;
	psd->portrait = MWPORTRAIT_NONE;
	psd->xres = psd->xvirtres = width;
	psd->yres = psd->yvirtres = height;
	psd->planes = 1;
	psd->bpp = 32;
	psd->ncolors = 1 << 24;
	psd->pitch = width * 4;
	psd->size = psd->pitch * height;
	psd->flags = PSF_SCREEN;
	psd->pixtype = MWPF_TRUECOLORARGB;
	psd->data_format = set_data_format(psd);
	psd->addr = gpu + root.at;
	subdriver = select_fb_subdriver(psd);
	if (!subdriver)
		return NULL;
	soft = *subdriver;
	set_subdriver(psd, subdriver);
	psd->DrawPixel = gpu_drawpixel;
	psd->ReadPixel = gpu_readpixel;
	psd->DrawHorzLine = gpu_drawhorzline;
	psd->DrawVertLine = gpu_drawvertline;
	psd->FillRect = gpu_fillrect;
#define WRAP(member, routine) psd->member = soft.member ? routine : NULL
	WRAP(BlitFallback, soft_blitfallback);
	WRAP(FrameBlit, gpu_frameblit);
	WRAP(FrameStretchBlit, soft_stretchblit);
	WRAP(BlitCopyMaskMonoByteMSB, gpu_maskbytemsb);
	WRAP(BlitCopyMaskMonoByteLSB, soft_maskbytelsb);
	WRAP(BlitCopyMaskMonoWordMSB, gpu_maskwordmsb);
	WRAP(BlitBlendMaskAlphaByte, soft_blendalpha);
	WRAP(BlitCopyRGBA8888, soft_copyrgba);
	WRAP(BlitSrcOverRGBA8888, soft_srcoverrgba);
	WRAP(BlitCopyRGB888, soft_copyrgb);
	WRAP(BlitStretchRGBA8888, soft_stretchrgba);

	memset(gpu + root.at, 0, psd->size);
	memset(gpu + LAYERS_OFFSET, 0, 4096);
	regs[REG_CURSOR + 2] = 0;
	gd_hwcursor = gpu_cursor;
	gd_drawstring = gpu_drawstring;
	gd_measurestring = gpu_measurestring;
	gd_drawpicture = gpu_picture;
	if (&gd_createfont)
		gd_createfont = romfont_create;
	gd_compositor = &compositor;
	regs[REG_WIDTH] = width;
	regs[REG_HEIGHT] = height;
	regs[REG_LAYERS] = GPU_PHYS + LAYERS_OFFSET;
	regs[REG_LOCK] = 0;
	regs[REG_MODE] = 4;
	regs[REG_KEYBOARD] = KEYBOARD_OWNED;	/* the kernel clears it when /dev/gpu is closed */
	EPRINTF("ShaderEmu display %dx%d, %s drawing\n", width, height, accelerate ? "GPU" : "software");
	return psd;
}

static void
gpu_close(PSD psd)
{
	if (gpu_fd < 0)
		return;
	flush();
	if (wm_state > 0) {
		/* the worker parks, and the kernel reports every move again */
		((volatile uint32_t *)(gpu + WM_QUIET_OFFSET))[3] = 0;
		((volatile uint32_t *)(gpu + WM_OFFSET))[WM_RUN] = 0;
		wm_poke();
		next_frame();
		next_frame();
	}
	regs[REG_CURSOR + 2] = 0;
	regs[REG_KEYBOARD] = 0;
	munmap(gpu, GPU_SIZE);
	close(gpu_fd);
	gpu_fd = -1;
}

static void
gpu_setpalette(PSD psd, int first, int count, MWPALENTRY *palette)
{
}

SCREENDEVICE scrdev = {
	0, 0, 0, 0, 0, 0, 0, NULL, 0, NULL, 0, 0, 0, 0, 0, 0,
	gen_fonts,
	gpu_open,
	gpu_close,
	gpu_setpalette,
	gen_getscreeninfo,
	pixmap_new,
	pixmap_map,
	pixmap_free,
	NULL,				/* SetPortrait: the screen is not rotated */
	NULL,				/* Update */
	gpu_preselect
};
