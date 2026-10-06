/*
 * Microwindows screen driver for the ShaderEmu display and GPU: every top-level window is
 * drawn into a buffer of its own and the GPU composes the screen from those
 * (docs/nanox.md in the ShaderEmu repository).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include "device.h"
#include "genfont.h"
#include "genmem.h"
#include "fb.h"

/* /dev/gpu maps GPU memory from GPU_PHYS; offsets below are from there */
#define GPU_PHYS	0x86000000u
#define GPU_SIZE	0x01b00000u
#define POOL_SIZE	0x01000000u	/* window buffers, from offset 0 */
#define REGS_OFFSET	0x01000000u	/* the machine's control words (physical 0x87000000) */
#define LAYERS_OFFSET	0x01001000u	/* the display's layer table: count, then x, y, w, h, address */
#define LIST_OFFSET	0x01400000u	/* our command list: 4096 commands of 64 bytes */
#define CURSOR_OFFSET	0x01440000u	/* the cursor's image */
#define CACHE_OFFSET	0x01441000u	/* glyphs of the built-in fonts, kept between lists */
#define DATA_OFFSET	0x01541000u	/* other textures for the commands in the list */
#define DATA_END	0x01700000u	/* beyond: left to an OpenGL program */
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
#define REG_KEYBOARD	12		/* KEYBOARD_OWNED while a program reads the keyboard device */
#define REG_COPIES	14		/* copies of a drawn picture into RAM made so far */
#define REG_CURSOR	16		/* x, y, on, address of a 32x32 image */
#define KEYBOARD_OWNED	0x6b657973u
#define MAX_COMMANDS	4096
#define MAX_SURFACES	15		/* the display shows 16 layers; one is the desktop */
#define MAX_BLOCKS	(2 * MAX_SURFACES + 2)
#define CACHE_SLOTS	2048
#define CMD_RECT	2
#define FRAG_COLOUR	0
#define FRAG_TEXTURE	1
#define FRAG_MASK	3
#define FRAG_RGB24	4
#define SUBMIT_DRAW	1
#define SUBMIT_INTO	4		/* then copy the picture to the given rectangle of RAM */
/* The pause hint: this machine ends its frame there, which is when the GPU does its work. */
#define next_frame()	__asm__ volatile(".word 0x0100000f")

/* What a window (and everything inside it) is drawn on: a buffer the size of the window. */
struct surface {
	SCREENDEVICE psd;		/* must be first: the engine draws on this */
	void *owner;			/* the window; NULL for a free slot */
	uint32_t at;			/* the buffer, in GPU memory */
	int x, y, w, h;			/* where the window is on the screen */
};

extern int gr_mode;
extern int (*gd_drawpicture)(PSD psd, MWCOORD x, MWCOORD y, MWCOORD width, MWCOORD height, const char *path);
extern void (*gd_hwcursor)(MWCOORD x, MWCOORD y, MWCOORD width, MWCOORD height, int visible,
	const MWIMAGEBITS *image, const MWIMAGEBITS *mask, MWPIXELVAL fg, MWPIXELVAL bg);

static int gpu_fd = -1;
static unsigned char *gpu;		/* GPU memory, mapped */
static volatile uint32_t *regs;
static SUBDRIVER soft;			/* the software drawing routines */
static int accelerate = 1;
static struct surface root, surfaces[MAX_SURFACES];
static struct surface *target = &root;	/* the surface the queued commands draw on */
static int commands;			/* queued so far, including the first */
static uint32_t data_top = DATA_OFFSET;	/* next free byte for textures */
static int dirty_x0 = 1 << 30, dirty_y0 = 1 << 30, dirty_x1, dirty_y1;	/* what the queue covers */
static struct { const unsigned char *lo, *hi; } font_bits[NUMBER_FONTS];
static struct { const unsigned char *src; uint32_t shape, at; } cache[CACHE_SLOTS];
static uint32_t cache_top = CACHE_OFFSET;
static struct { uint32_t at, bytes; int used; } blocks[MAX_BLOCKS];
static int block_count;

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
	return psd == &scrdev || ((struct surface *)psd >= surfaces && (struct surface *)psd < surfaces + MAX_SURFACES);
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
submit(int count, uint32_t address, int width, int height)
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
	regs[REG_SUBMIT] = SUBMIT_DRAW | SUBMIT_INTO;
	__atomic_store_n(&regs[REG_LOCK], 0, __ATOMIC_RELEASE);
	return landed;
}

/* Draws the queued commands and copies the picture back into the surface's buffer. */
static void
flush(void)
{
	uint32_t landed;

	if (commands == 0)
		return;
	landed = submit(commands, GPU_PHYS + target->at, target->w, target->h);
	/* software may read the buffer next, and the list memory is reused: wait for the copy */
	while ((int32_t)(regs[REG_COPIES] - landed) < 0)
		next_frame();
	commands = 0;
	data_top = DATA_OFFSET;
	dirty_x0 = dirty_y0 = 1 << 30;
	dirty_x1 = dirty_y1 = 0;
}

/* Writes command `n`: a rectangle, x1 and y1 exclusive, optionally showing a texture. */
static void
emit(int n, int x0, int y0, int x1, int y1, uint32_t colour, int mode, uint32_t at,
	int w, int h, int sx, int sy)
{
	volatile uint32_t *c = (volatile uint32_t *)(gpu + LIST_OFFSET) + 16 * n;

	c[0] = CMD_RECT;
	c[1] = colour & 0xffffff;
	c[2] = 0;
	c[3] = 6 * n;
	c[4] = x0;
	c[5] = y0;
	c[6] = x1;
	c[7] = y1;
	c[8] = mode;
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
static void
queue(int x0, int y0, int x1, int y1, uint32_t colour, int mode, uint32_t at, int w, int h, int sx, int sy)
{
	if (commands == MAX_COMMANDS)
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

		/* the source is read from RAM, which lacks what is still queued */
		if (from == s && sx < dirty_x1 && sx + gc->width > dirty_x0 && sy < dirty_y1 && sy + gc->height > dirty_y0)
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
	psd->orgsubdriver->FrameBlit(psd, gc);
}

static MWBOOL
pixmap_map(PSD mempsd, MWCOORD w, MWCOORD h, int planes, int bpp, MWIMGDATFMT data_format,
	unsigned int pitch, int size, void *addr)
{
	if (!gen_mapmemgc(mempsd, w, h, planes, bpp, data_format, pitch, size, addr))
		return 0;
	if (mempsd->FrameBlit)
		mempsd->FrameBlit = pixmap_frameblit;
	return 1;
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

/*
 * A PPM file stretched over a rectangle: its rows are read straight into texture memory, as
 * many as fit at a time, and the GPU scales them. 0 leaves the file to the engine's decoder.
 */
static int
gpu_picture(PSD psd, MWCOORD x, MWCOORD y, MWCOORD width, MWCOORD height, const char *path)
{
	struct surface *s;
	int pw = 0, ph = 0, most = 0, start = 0, x0, x1, y0, y1, band, rows, j, fd;
	char head[64];
	volatile uint32_t *c;

	if (!is_surface(psd) || (fd = open(path, O_RDONLY)) < 0)
		return 0;
	j = read(fd, head, sizeof head - 1);
	head[j > 0 ? j : 0] = 0;
	if (sscanf(head, "P6 %d %d %d%n", &pw, &ph, &most, &start) != 3 || most != 255 || pw < 1 || ph < 1 ||
	    (rows = (DATA_END - DATA_OFFSET) / (3 * pw)) < 1) {
		close(fd);
		return 0;
	}
	start++;	/* one white space ends the header */
	if (width <= 0) width = pw;
	if (height <= 0) height = ph;
	s = draw_on(psd);
	flush();
	x0 = x > s->x ? x : s->x;
	x1 = x + width < s->x + s->w ? x + width : s->x + s->w;
	y1 = y + height < s->y + s->h ? y + height : s->y + s->h;
	for (y0 = y > s->y ? y : s->y; x0 < x1 && y0 < y1; y0 += band) {
		band = !accelerate ? 1 : y1 - y0 < rows ? y1 - y0 : rows;
		for (j = 0; j < band; j++)
			if (pread(fd, gpu + DATA_OFFSET + j * 3 * pw, 3 * pw,
				  start + (off_t)((long long)(y0 + j - y) * ph / height) * 3 * pw) != 3 * pw)
				break;
		if (!accelerate) {
			/* without the GPU: the same texels, a row at a time */
			uint32_t *to = (uint32_t *)(gpu + s->at) + (y0 - s->y) * s->w - s->x;
			const unsigned char *row = gpu + DATA_OFFSET, *p;
			uint32_t step = ((uint32_t)pw << 16) / width;	/* texels a pixel, 16.16 */
			uint32_t at = (uint32_t)(((2LL * (x0 - x) + 1) * pw << 15) / width);

			for (j = x0; j < x1; j++, at += step) {
				p = row + 3 * (at >> 16 < (uint32_t)pw ? at >> 16 : (uint32_t)pw - 1);
				to[j] = 0xff000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
			}
			continue;
		}
		queue(x0, y0, x1, y0 + band, 0xffffff, FRAG_RGB24, DATA_OFFSET, pw, band, 0, 0);
		/* the part of the picture's width this rectangle shows, as 16.16 fractions */
		c = (volatile uint32_t *)(gpu + LIST_OFFSET) + 16 * (commands - 1);
		c[12] = (uint32_t)(((long long)(x0 - x) << 16) / width);
		c[13] = 0;
		c[14] = (uint32_t)(((long long)(x1 - x) << 16) / width);
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

/* ---------------- surfaces, for the window system */

/* Points a surface's drawing at its buffer, such that screen coordinates land inside it. */
static void
place(struct surface *s, int x, int y)
{
	s->x = x;
	s->y = y;
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
	s->w = width;
	s->h = height;
	place(s, x, y);
	last = s;
	return &s->psd;
}

static void
surface_release(void *owner)
{
	struct surface *s;

	for (s = surfaces; s < surfaces + MAX_SURFACES; s++)
		if (s->owner == owner) {
			if (s == target) {
				flush();
				target = &root;
			}
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

	*address = GPU_PHYS + s->at + ((y - s->y) * s->w + (x - s->x)) * 4;
	*row = s->w;
}

static struct gd_compositor compositor = { surface_for, surface_release, surface_touch, surface_locate };

/* ---------------- the screen: the desktop's buffer, then each window's, lowest first */

static int layer_count;

static void
show(PSD psd)
{
	struct surface *s = surface_of(psd);
	volatile uint32_t *layer = (volatile uint32_t *)(gpu + LAYERS_OFFSET) + 4 + 8 * layer_count;

	if ((s == &root && layer_count > 0) || layer_count > MAX_SURFACES)
		return;
	/* stores only what differs, so an unchanged screen costs no writes */
	if ((int)layer[0] != s->x) layer[0] = s->x;
	if ((int)layer[1] != s->y) layer[1] = s->y;
	if ((int)layer[2] != s->w) layer[2] = s->w;
	if ((int)layer[3] != s->h) layer[3] = s->h;
	if (layer[4] != GPU_PHYS + s->at) layer[4] = GPU_PHYS + s->at;
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

	flush();
	layer_count = 0;
	show(&scrdev);
	if (gd_composite_walk)
		gd_composite_walk(show);
	if ((int)table[0] != layer_count)
		table[0] = layer_count;
	return 0;
}

/*
 * The cursor is the display's: a position and an image the display draws over the picture,
 * so moving it costs two stores and drawing never has to step around it.
 */
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
		regs[REG_CURSOR + 2] = 0;
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
	regs[REG_CURSOR] = x;
	regs[REG_CURSOR + 1] = y;
	regs[REG_CURSOR + 3] = GPU_PHYS + CURSOR_OFFSET;
	regs[REG_CURSOR + 2] = 1;
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
		EPRINTF("NANOX_SIZE: %dx%d does not fit the display\n", width, height);
		return NULL;
	}
	accelerate = getenv("NANOX_SOFTWARE") == NULL;
	for (f = 0; f < NUMBER_FONTS; f++) {
		PMWCFONT font = gen_fonts[f].cfont;

		/* an upper bound for fonts whose glyphs are packed */
		font_bits[f].lo = (const unsigned char *)font->bits;
		font_bits[f].hi = font_bits[f].lo + font->size * font->height * ((font->maxwidth + 15) >> 4) * 2;
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
	gd_drawpicture = gpu_picture;
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
	gen_allocatememgc,
	pixmap_map,
	gen_freememgc,
	NULL,				/* SetPortrait: the screen is not rotated */
	NULL,				/* Update */
	gpu_preselect
};
