/*
 * nxpersist load | save | clear | auto 0|1 | state: the folder /persistence, which the host
 * keeps between sessions (docs/system/persist.md). "load!" loads though loading at start is off.
 */
#include <fcntl.h>
#include <ftw.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include "zlib.h"
#include "lzma.h"
#include "ui.h"
#include "ui_ask.h"
#include "mcw.h"
#include "mcwx.h"

#define FOLDER "/persistence"
#define TAR "/tmp/nxpersist.tar"
#define LOCK "/tmp/nxpersist.lock"
#define MOST 96000		/* bytes handed to the host: a player's data is 100,000 */
#define RAW_MOST 0x800000	/* and what they may unpack to */
#define FETCH_MOST 0x40000	/* the host's answer, and what it reads of ours (docs/system/fetch.md) */
#define REG_ASKED 64
#define REG_REPLY 68
#define REG_ADDRESS 72
#define KIND_FILE 3
#define HEADER 16		/* "SEPD", a version (1 gzip, 2 xz, 3 blocks), the packed bytes' count, their CRC-32 */
#define DICT_MOST 0x200000	/* LZMA's dictionary, and so what a load must find in one piece */
#define DICT_LEAST 0x40000
#define WIDTH 320
#define HEIGHT 70

static volatile uint32_t *regs;
static unsigned char *fetched;
static GR_WINDOW_ID window;	/* 0: no window system, and lines on the console */
static const char *doing = "";
static int part;		/* of 256 */

static void
draw(void)
{
	ui_fill(window, 0, 0, WIDTH, HEIGHT, UI_FACE);
	ui_text(window, 14, 14, doing, -1, BLACK, 0);
	ui_bevel(window, 14, 40, WIDTH - 28, 16, 1);
	ui_fill(window, 16, 42, (WIDTH - 32) * part / 256, 12, UI_SELECTED);
	GrFlush();
}

/* What is being done and how far it is: in the window, or a line when it changes. */
static void
show(const char *text, int of_256)
{
	if (!window && text != doing)
		printf("nxpersist: %s\n", text);
	doing = text, part = of_256;
	if (window)
		draw();
}

/* The window, when there is a window system: before the work, so framed first (ui_framed). */
static void
open_window(const char *text)
{
	const GR_SCREEN_INFO *screen;

	if (access("/tmp/.nano-X", F_OK) == 0 && GrOpen() >= 0) {
		ui_init();
		screen = ui_screen();
		window = GrNewWindowEx(UI_FIXED | GR_WM_PROPS_NOAUTOMOVE, "Storage", GR_ROOT_WINDOW_ID, (screen->cols - WIDTH) / 2,
				       (screen->rows - HEIGHT) / 3, WIDTH, HEIGHT, UI_FACE);
		GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE);
		GrMapWindow(window);
		GrRaiseWindow(window);
		ui_framed();
	}
	show(text, 0);
}

/* A last line: left in the window a while, or said in a window of its own to be answered. */
static void
say(const char *text, int answered)
{
	static const char *const ok[] = { "OK" };

	if (!window) {
		printf("nxpersist: %s\n", text);
		return;
	}
	if (answered) {
		GrUnmapWindow(window);
		ui_ask("Storage", text, ok, 1, NULL);
	} else {
		show(text, part);
		sleep(2);
	}
}

/* Asks the host with an address of ours; the answer's status, or 0 for none in time. */
static int
ask(const char *address, uint32_t offset, int seconds)
{
	uint32_t mine;
	int waited;
	size_t n = strlen(address);

	/* one request at a time: wait for another program's to be answered */
	for (waited = 0; regs[REG_ASKED] != regs[REG_REPLY] && waited < 200; waited++)
		usleep(50000);
	memset((void *)&regs[REG_ADDRESS], 0, 256);
	memcpy((void *)&regs[REG_ADDRESS], address, n);
	regs[REG_ASKED + 1] = n;
	regs[REG_ASKED + 2] = KIND_FILE;
	regs[REG_ASKED + 3] = offset;
	mine = regs[REG_ASKED] + 1;
	regs[REG_ASKED] = mine;
	for (waited = 0; regs[REG_REPLY] != mine; waited++) {
		if (waited > seconds * 20)
			return 0;
		usleep(50000);
	}
	return regs[REG_REPLY + 2];
}

/* tar, with the folder's name or the archive's; true if it ended well. */
static int
tar(const char *how)
{
	int status = -1;
	pid_t pid = real_vfork();

	if (pid == 0) {
		execlp("tar", "tar", "-C", "/", how, TAR, how[1] == 'c' ? FOLDER + 1 : (char *)NULL, (char *)NULL);
		_exit(127);
	}
	return pid > 0 && waitpid(pid, &status, 0) == pid && status == 0;
}

static struct {
	char name[3][64];
	long size[3];
} largest;

static int
measure(const char *path, const struct stat *st, int kind, struct FTW *at)
{
	int k, j;

	if (kind != FTW_F)
		return 0;
	for (k = 0; k < 3 && st->st_size <= largest.size[k]; k++)
		continue;
	for (j = 2; j > k; j--) {
		largest.size[j] = largest.size[j - 1];
		memcpy(largest.name[j], largest.name[j - 1], 64);
	}
	if (k < 3) {
		largest.size[k] = st->st_size;
		snprintf(largest.name[k], 64, "%s", path + sizeof FOLDER);
	}
	return 0;
}

/* Deflates `n` bytes as a gzip file into `out`; its length, 0 if it did not fit. */
static long
deflated(const unsigned char *in, long n, unsigned char *out, long room, int level)
{
	z_stream z;
	long at = 0;

	memset(&z, 0, sizeof z);
	if (deflateInit2(&z, level, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) != Z_OK)
		return 0;
	z.next_out = out, z.avail_out = room;
	do {
		long here = n - at < 16384 ? n - at : 16384;

		z.next_in = (unsigned char *)in + at, z.avail_in = here;
		at += here;
		if (deflate(&z, at == n ? Z_FINISH : Z_NO_FLUSH) == Z_STREAM_ERROR || (z.avail_out == 0 && at < n))
			break;
		show(doing, n ? (int)((long long)at * 256 / n) : 256);
	} while (at < n);
	at = z.avail_in == 0 && z.avail_out ? (long)z.total_out : 0;
	deflateEnd(&z);
	return at;
}

/* Packs `n` bytes as an xz file and what they unpack to into `out`; its length, 0 if there was no memory or room.
   Its memory comes in pieces of several megabytes, which a machine that has run for a while may not have. */
static long
lzmaed(const unsigned char *in, long n, unsigned char *out, long room, uint32_t dict)
{
	lzma_stream z = LZMA_STREAM_INIT;
	lzma_options_lzma options;
	lzma_filter filters[2];
	lzma_ret ended = LZMA_OK;
	long at = 0, length = 0;
	uint32_t size = (uint32_t)n;

	if (lzma_lzma_preset(&options, 4))
		return 0;
	options.dict_size = dict;
	filters[0].id = LZMA_FILTER_LZMA2, filters[0].options = &options;
	filters[1].id = LZMA_VLI_UNKNOWN, filters[1].options = NULL;
	if (room < 4 || lzma_stream_encoder(&z, filters, LZMA_CHECK_NONE) != LZMA_OK)
		return 0;
	z.next_out = out, z.avail_out = room - 4;
	while (ended == LZMA_OK) {
		long here = n - at < 16384 ? n - at : 16384;

		z.next_in = in + at, z.avail_in = here;
		at += here;
		do
			ended = lzma_code(&z, at == n ? LZMA_FINISH : LZMA_RUN);
		while (ended == LZMA_OK && z.avail_in && z.avail_out);
		if (ended == LZMA_OK && z.avail_out == 0)
			ended = LZMA_BUF_ERROR;
		show(doing, n ? (int)((long long)at * 256 / n) : 256);
	}
	if (ended == LZMA_STREAM_END) {
		length = (long)z.total_out;
		memcpy(out + length, &size, 4);
		length += 4;
	}
	lzma_end(&z);
	return length;
}

/* Unpacks what lzmaed made; 0 if it could not. */
static int
unlzmaed(const unsigned char *in, long n, unsigned char *out, uint32_t size)
{
	lzma_stream z = LZMA_STREAM_INIT;
	lzma_ret ended = LZMA_OK;
	int done;

	if (lzma_stream_decoder(&z, DICT_MOST * 2, 0) != LZMA_OK)
		return 0;
	z.next_in = in, z.avail_in = 0;
	z.next_out = out, z.avail_out = size;
	while (ended == LZMA_OK) {
		long left = n - (long)z.total_in, here = left < 8192 ? left : 8192;

		z.avail_in = here;
		ended = lzma_code(&z, here ? LZMA_RUN : LZMA_FINISH);
		show(doing, (int)((long long)z.total_in * 256 / n));
	}
	done = ended == LZMA_STREAM_END && z.total_out == size;
	lzma_end(&z);
	return done;
}

/*
 * Version 3: the folder's archive in blocks, each block raw LZMA2 by itself on a worker core, with
 * the bytes before it (PRIME, or all there are) as its preset dictionary: nearly what one stream
 * of the whole would be, and as long as one block takes (docs/system/persist.md, "The bytes").
 * After the header: the archive's size, the block's, the prime's, the count, then a word a block
 * (its packed length; the top bit set when it was packed as text), then the blocks.
 */
#define BLOCK 0x40000
#define PRIME 0x40000
#define WORKERS_MOST 16	/* (an encoder's arena is some MB: memory, not cores, is what limits them) */
#define BLOCKS_MOST 1024
#define TEXT_BIT 0x80000000u

extern const char etext[];
extern void _start(void);

struct block {			/* one core writes it: 16 bytes */
	uint32_t start, length, made, status;	/* status: 0 to do, 1 made (text: 3), 2 failed */
} __attribute__((aligned(16)));

struct arena {			/* a core's memory for its encoder: bumped, put back at every block */
	unsigned char *base;
	uint32_t size, used, pad;
} __attribute__((aligned(16)));

static struct {
	const unsigned char *raw;
	unsigned char *out;	/* block i's packed bytes at out + i * room */
	uint32_t room, prime, blocks_count;
	struct block *blocks;
	struct arena *arenas;	/* one a core: 0 the first */
} pk;

static void *
arena_alloc(void *opaque, size_t count, size_t size)
{
	struct arena *a = opaque;
	size_t n = (count * size + 15) & ~(size_t)15;
	void *p;

	if (n > a->size - a->used)
		return NULL;
	p = a->base + a->used;
	a->used += (uint32_t)n;
	return p;
}

static void
arena_free(void *opaque, void *p)
{
	(void)opaque, (void)p;
}

/* Mostly letters, digits, spaces and line ends: LZMA's literals then go by the byte before (lc 3); a
 * game's binary saves pack better with none (lc 0: OpenTTD's 1.2 MB 137 KB against 140). */
static int
text_like(const unsigned char *p, uint32_t n)
{
	uint32_t i, plain = 0, zeros = 0;

	/* (zeros are not counted: an archive's headers and their padding are mostly zeros, around text) */
	for (i = 0; i < n; i++)
		plain += (p[i] >= 32 && p[i] < 127) || p[i] == '\n' || p[i] == '\t' || p[i] == '\r', zeros += p[i] == 0;
	n -= zeros;
	return n > 0 && plain >= n - n / 10;
}

/* The encoder's (NXPERSIST_MF, _NICE, _DEPTH): hash chains, whose stores are few, as a worker's write cache wants (bt4's
 * tree took a worker three times as long: an Age of Empires save of 1.75 MB 143.5 s to 67,133 bytes, hc4 57.7 s to 69,211) */
static int opt_mf = LZMA_MF_HC4, opt_nice = 64, opt_depth = 24;

/* The options of block `start`, `length` long, with what lies before it: the encoder's and the decoder's. */
static void
block_options(lzma_options_lzma *o, lzma_filter *f, const unsigned char *raw, uint32_t start, uint32_t length, uint32_t prime, int text)
{
	uint32_t before = start < prime ? start : prime, dict = before + length;

	lzma_lzma_preset(o, 6);
	o->dict_size = dict < LZMA_DICT_SIZE_MIN ? LZMA_DICT_SIZE_MIN : dict;
	o->preset_dict = before ? raw + start - before : NULL;
	o->preset_dict_size = before;
	o->nice_len = opt_nice, o->mf = opt_mf, o->depth = opt_depth;
	o->lc = text ? 3 : 0, o->lp = 0, o->pb = 0;
	f[0].id = LZMA_FILTER_LZMA2, f[0].options = o;
	f[1].id = LZMA_VLI_UNKNOWN, f[1].options = NULL;
}

/* Packs block i on whichever core runs it, out of that core's arena (NULL: malloc's, the first core's after a failure). */
static void
pack_block(uint32_t i, struct arena *a)
{
	struct block *b = &pk.blocks[i];
	lzma_allocator mine = { arena_alloc, arena_free, a };
	lzma_stream z = LZMA_STREAM_INIT;
	lzma_options_lzma o;
	lzma_filter f[2];
	lzma_ret r = LZMA_OK;
	int text = text_like(pk.raw + b->start, b->length);

	if (a)
		a->used = 0, z.allocator = &mine;
	block_options(&o, f, pk.raw, b->start, b->length, pk.prime, text);
	if (lzma_raw_encoder(&z, f) != LZMA_OK) {
		b->status = 2;
		return;
	}
	z.next_in = pk.raw + b->start;
	z.next_out = pk.out + (size_t)i * pk.room, z.avail_out = pk.room;
	if (a && mcw_on_worker()) {
		z.avail_in = b->length;
		while (r == LZMA_OK)
			r = lzma_code(&z, LZMA_FINISH);
	} else {
		/* on the first core, in pieces: the window's bar moves */
		uint32_t fed = 0;

		while (r == LZMA_OK) {
			uint32_t here = b->length - fed < 0x4000 ? b->length - fed : 0x4000;

			z.avail_in += here, fed += here;
			do
				r = lzma_code(&z, fed == b->length ? LZMA_FINISH : LZMA_RUN);
			while (r == LZMA_OK && z.avail_in && z.avail_out);
			if (r == LZMA_OK && z.avail_out == 0)
				r = LZMA_BUF_ERROR;
			if (pk.blocks_count == 1)
				show(doing, (int)((uint64_t)fed * 256 / (b->length ? b->length : 1)));
		}
	}
	b->made = (uint32_t)z.total_out;
	lzma_end(&z);
	b->status = r == LZMA_STREAM_END ? 1 + 2 * text : 2;
	if (b->status == 3)
		b->status = 1, b->made |= TEXT_BIT;
}

static void
pack_some(void *ctx, uint32_t from, uint32_t to)
{
	(void)ctx;
	for (; from < to; from++)
		pack_block(from, pk.arenas[mcw_self()].base ? &pk.arenas[mcw_self()] : NULL);
}

/* What the blocks made of `n` bytes: the payload of version 3 in `out`, its length; 0 if there was no memory. */
static long
blocked(const unsigned char *raw, long n, unsigned char **made)
{
	const char *e;
	uint32_t block, count, i, have = 0, done, k;
	size_t need, at;
	lzma_options_lzma o;
	lzma_filter f[2];
	unsigned char *out;
	int most, workers = 0;

	most = (e = getenv("NXPERSIST_WORKERS")) ? atoi(e) : WORKERS_MOST;
	if ((e = getenv("NXPERSIST_MF")))
		opt_mf = !strcmp(e, "hc4") ? LZMA_MF_HC4 : LZMA_MF_BT4;
	if ((e = getenv("NXPERSIST_NICE")))
		opt_nice = atoi(e);
	if ((e = getenv("NXPERSIST_DEPTH")))
		opt_depth = atoi(e);
	block = (e = getenv("NXPERSIST_BLOCK")) ? (uint32_t)atoi(e) : 0;
	pk.prime = (e = getenv("NXPERSIST_PRIME")) ? (uint32_t)atoi(e) : PRIME;
	/* (a folder of one block is packed on this core, which does it three times as fast as a worker) */
	block = block < 4096 ? BLOCK : (block + 15) & ~15u;
	count = n ? (uint32_t)((n + block - 1) / block) : 1;
	if (count > BLOCKS_MOST)
		return 0;
	pk.raw = raw;
	pk.blocks_count = count;
	pk.room = (block + block / 16 + 1024 + 15) & ~15u;
	pk.blocks = aligned_alloc(16, count * sizeof *pk.blocks);
	pk.out = malloc((size_t)count * pk.room);
	if (!pk.blocks || !pk.out)
		return 0;
	for (i = 0; i < count; i++) {
		pk.blocks[i].start = i * block;
		pk.blocks[i].length = (uint32_t)(n - i * block < block ? n - i * block : block);
		pk.blocks[i].made = pk.blocks[i].status = 0;
	}
	/* the workers, each with an arena the size of the largest block's encoder, as many as memory has */
	block_options(&o, f, raw, block, block, pk.prime, 0);
	need = ((size_t)lzma_raw_encoder_memusage(f) + 0x10000 + 15) & ~(size_t)15;
	if (most > 0 && count > 1) {
		const char *text = (const char *)((uintptr_t)_start & ~(uintptr_t)4095);

		/* (a worker stops at every page of code no core has run yet: read it here first) */
		mcw_touch_code(text, (unsigned)(etext - text));
		workers = mcw_open(most < (int)count ? most : (int)count);
	}
	pk.arenas = aligned_alloc(16, (workers + 1) * sizeof *pk.arenas);
	if (!pk.arenas)
		return 0;
	for (k = 0; k <= (uint32_t)workers; k++) {
		pk.arenas[k].base = k ? malloc(need) : NULL;	/* (the first core uses malloc's) */
		pk.arenas[k].size = (uint32_t)need, pk.arenas[k].used = 0;
		if (k && !pk.arenas[k].base)
			break;
		if (k)
			mcw_touch(pk.arenas[k].base, (unsigned)need);
		have = k;
	}
	mcw_touch(pk.out, count * pk.room);
	if (have > 0) {
		mcw_for_opts opts = { 0 };

		opts.grain = 1, opts.workers = (int)have, opts.first_idle = 1;
		mcw_for_begin(count, pack_some, NULL, &opts);
		while (!mcw_for_ready()) {
			for (i = done = 0; i < count; i++)
				done += pk.blocks[i].status != 0;
			show(doing, (int)(done * 256 / count));
			mcw_rest();
			usleep(100000);
		}
		mcw_for_wait();
		mcw_for_end();
	}
	for (i = done = 0; i < count; i++) {
		/* what no worker made, or made in too little memory: here, in malloc's */
		if (pk.blocks[i].status != 1)
			pk.blocks[i].status = 0, pack_block(i, NULL);
		if (pk.blocks[i].status != 1)
			return 0;
		show(doing, (int)(++done * 256 / count));
	}
	for (k = 1; k <= have; k++)
		free(pk.arenas[k].base);
	free(pk.arenas);
	if (workers)
		mcw_close();
	need = 16 + 4 * (size_t)count;
	for (i = 0; i < count; i++)
		need += pk.blocks[i].made & ~TEXT_BIT;
	if ((out = malloc(need)) == NULL)
		return 0;
	memcpy(out, &(uint32_t){ (uint32_t)n }, 4);
	memcpy(out + 4, &block, 4);
	memcpy(out + 8, &pk.prime, 4);
	memcpy(out + 12, &count, 4);
	for (i = 0, at = 16 + 4 * (size_t)count; i < count; i++) {
		uint32_t length = pk.blocks[i].made & ~TEXT_BIT;

		memcpy(out + 16 + 4 * i, &pk.blocks[i].made, 4);
		memcpy(out + at, pk.out + (size_t)i * pk.room, length);
		at += length;
	}
	free(pk.out);
	free(pk.blocks);
	*made = out;
	return (long)need;
}

/* Unpacks what blocked made into `raw`, `size` bytes; 0 if it could not. */
static int
unblocked(const unsigned char *in, long n, unsigned char *raw, uint32_t size)
{
	uint32_t block, prime, count, i, word;
	size_t at;

	if (n < 16)
		return 0;
	memcpy(&block, in + 4, 4);
	memcpy(&prime, in + 8, 4);
	memcpy(&count, in + 12, 4);
	if (block < 16 || count == 0 || count > BLOCKS_MOST || (uint64_t)block * count < size || (uint64_t)block * (count - 1) >= (size ? size : 1) ||
	    16 + 4 * (uint64_t)count > (uint64_t)n)
		return 0;
	for (i = 0, at = 16 + 4 * (size_t)count; i < count; i++) {
		lzma_stream z = LZMA_STREAM_INIT;
		lzma_options_lzma o;
		lzma_filter f[2];
		lzma_ret r = LZMA_OK;
		uint32_t start = i * block, length = size - start < block ? size - start : block, packed;

		memcpy(&word, in + 16 + 4 * i, 4);
		packed = word & ~TEXT_BIT;
		if (packed > (uint64_t)n - at)
			return 0;
		block_options(&o, f, raw, start, length, prime, (word & TEXT_BIT) != 0);
		if (lzma_raw_decoder(&z, f) != LZMA_OK)
			return 0;
		z.next_in = in + at, z.avail_in = packed;
		z.next_out = raw + start, z.avail_out = length;
		while (r == LZMA_OK)
			r = lzma_code(&z, LZMA_FINISH);
		lzma_end(&z);
		if ((r != LZMA_STREAM_END && !(r == LZMA_BUF_ERROR && z.avail_out == 0)) || z.total_out != length)
			return 0;
		at += packed;
		show(doing, (int)((uint64_t)(i + 1) * 256 / count));
	}
	return 1;
}

static int
save(void)
{
	FILE *file;
	unsigned char *raw, *packed;
	long n, room, length;
	uint32_t words[3], dict, version = 1;
	char text[400];
	int status;

	open_window("Compressing data...");
	mkdir(FOLDER, 0777);
	if (!tar("-cf") || (file = fopen(TAR, "rb")) == NULL) {
		unlink(TAR);
		say("The folder could not be read.", 1);
		return 1;
	}
	fseek(file, 0, SEEK_END);
	n = ftell(file);
	if (n > RAW_MOST) {	/* (load would call it damaged, and what the host keeps now would be lost) */
		fclose(file);
		unlink(TAR);
		snprintf(text, sizeof text, "Not saved: the folder holds %ld KB and %d KB can be unpacked.", (n + 999) / 1000, RAW_MOST / 1000);
		say(text, 1);
		return 1;
	}
	rewind(file);
	room = n + 4096;
	raw = malloc(n + 1), packed = malloc(HEADER + room);
	n = raw && packed ? (long)fread(raw, 1, n, file) : -1;
	fclose(file);
	unlink(TAR);
	if (n < 0) {
		say("No memory to pack the folder in.", 1);
		return 1;
	}
	/* LZMA in blocks on the worker cores (version 3); without the memory for it, as before: deflate
	   first, LZMA as one stream only for a folder that does not fit, and not for one twice too large
	   (1.2 MB of OpenTTD's map took 11 minutes at preset 6 on one core, to 146 KB) */
	{
		unsigned char *made = NULL;
		const char *e = getenv("NXPERSIST_FORMAT");

		length = e && atoi(e) < 3 ? 0 : blocked(raw, n, &made);
		if (length > 0 && length <= room) {
			memcpy(packed + HEADER, made, length);
			version = 3;
		} else
			length = 0;
		free(made);
	}
	if (version == 3)
		;
	else if ((length = deflated(raw, n, packed + HEADER, room, 1)), length == 0 ? n < 2 * MOST : HEADER + length > MOST && HEADER + length < 2 * MOST) {
		long smaller = 0;

		show("Compressing data harder...", 0);
		for (dict = n < DICT_MOST ? (n < DICT_LEAST ? DICT_LEAST : (uint32_t)n) : DICT_MOST; !smaller && dict >= DICT_LEAST; dict /= 2)
			smaller = lzmaed(raw, n, packed + HEADER, room, dict);
		if (smaller)
			length = smaller, version = 2;
		else
			length = deflated(raw, n, packed + HEADER, room, 9);
	}
	if (length == 0 || HEADER + length > MOST) {
		nftw(FOLDER, measure, 16, FTW_PHYS);
		snprintf(text, sizeof text,
			 "Not saved: the folder packs to %ld KB and %d KB can be kept.\nThe largest files of %s:\n%s, %ld KB\n%s, %ld KB\n%s, %ld KB",
			 (HEADER + length + 999) / 1000, MOST / 1000, FOLDER, largest.name[0], (largest.size[0] + 999) / 1000, largest.name[1],
			 (largest.size[1] + 999) / 1000, largest.name[2], (largest.size[2] + 999) / 1000);
		say(text, 1);
		return 1;
	}
	memcpy(packed, "SEPD", 4);
	words[0] = version, words[1] = (uint32_t)length, words[2] = crc32(0, packed + HEADER, length);
	memcpy(packed + 4, words, 12);
	show("Handing the data to the host...", 256);
	/* (after another program's request has been answered: its answer lands in the same memory) */
	for (status = 0; regs[REG_ASKED] != regs[REG_REPLY] && status < 200; status++)
		usleep(50000);
	memcpy(fetched, packed, HEADER + length);
	status = ask("persist:save", HEADER + length, 30);
	if (status != 200) {
		snprintf(text, sizeof text, status == 413 ? "Not saved: the host keeps less than %ld KB." :
			 status == 500 ? "Not saved: the host could not keep the data." : "Not saved: this host keeps nothing (%ld KB packed, answer %d).",
			 (HEADER + length + 999) / 1000, status);
		say(text, 1);
		return 1;
	}
	snprintf(text, sizeof text, "Saved: %ld KB of %d.", (HEADER + length + 999) / 1000, MOST / 1000);
	say(text, 0);
	return 0;
}

static int
load(const char *address)
{
	z_stream z;
	FILE *file;
	unsigned char *packed, *raw;
	uint32_t total, words[3], size;
	int status = ask(address, 0, 25), ended = Z_OK;

	if (status != 200) {
		printf("nxpersist: nothing to load (%s)\n", status == 403 ? "loading at start is off" : status == 404 ? "nothing is kept" :
		       "this host keeps nothing");
		return status == 403 || status == 404 ? 0 : 1;
	}
	total = regs[REG_REPLY + 1];
	if (total < HEADER + 18 || total > FETCH_MOST || (packed = malloc(total)) == NULL)
		return 1;
	memcpy(packed, fetched, total);
	memcpy(words, packed + 4, 12);
	memcpy(&size, packed + total - 4, 4);	/* a gzip file, and what lzmaed makes, ends with what it unpacks to */
	if (words[0] == 3)
		memcpy(&size, packed + HEADER, 4);	/* (blocks begin with it) */
	open_window("Decompressing persistence data...");
	if (memcmp(packed, "SEPD", 4) || words[0] < 1 || words[0] > 3 || words[1] != total - HEADER || crc32(0, packed + HEADER, total - HEADER) != words[2] ||
	    size > RAW_MOST || (raw = malloc(size + 1)) == NULL) {
		say("The kept data is damaged: not unpacked.", 0);
		return 1;
	}
	if (words[0] == 3) {
		if (!unblocked(packed + HEADER, total - HEADER, raw, size) || (file = fopen(TAR, "wb")) == NULL) {
			say("The kept data is damaged: not unpacked.", 0);
			return 1;
		}
		goto unpacked;
	}
	if (words[0] == 2) {
		if (!unlzmaed(packed + HEADER, total - HEADER - 4, raw, size) || (file = fopen(TAR, "wb")) == NULL) {
			say("The kept data is damaged: not unpacked.", 0);
			return 1;
		}
		goto unpacked;
	}
	memset(&z, 0, sizeof z);
	inflateInit2(&z, 31);
	z.next_in = packed + HEADER, z.avail_in = total - HEADER;
	z.next_out = raw, z.avail_out = size;
	while (ended == Z_OK && z.avail_in) {
		uint32_t here = z.avail_in < 8192 ? z.avail_in : 8192, left = z.avail_in - here;

		z.avail_in = here;
		ended = inflate(&z, Z_NO_FLUSH);
		z.avail_in += left;
		show(doing, (int)((total - HEADER - z.avail_in) * 256 / (total - HEADER)));
	}
	inflateEnd(&z);
	if (ended != Z_STREAM_END || z.total_out != size || (file = fopen(TAR, "wb")) == NULL) {
		say("The kept data is damaged: not unpacked.", 0);
		return 1;
	}
unpacked:
	fwrite(raw, 1, size, file);
	fclose(file);
	status = tar("-xf");
	unlink(TAR);
	if (!status)
		say("The kept data could not be unpacked.", 0);
	return !status;
}

int
main(int argc, char **argv)
{
	const char *what = argc > 1 ? argv[1] : "";
	char address[32];
	int fd, lock, status = 1;

	signal(SIGCHLD, SIG_DFL);	/* (tar is waited for) */
	fd = open("/dev/gpu", O_RDWR);
	if (fd < 0 || (regs = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01000000)) == MAP_FAILED ||
	    (fetched = mmap(NULL, FETCH_MOST, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x016c0000)) == MAP_FAILED) {
		printf("nxpersist: this machine has no way to ask its host\n");
		return 1;
	}
	/* one at a time: the folder's archive and the host's answer are one each. A second save
	 * (the button pressed twice) goes; anything else waits its turn. */
	lock = open(LOCK, O_CREAT | O_RDWR, 0666);
	if (lock < 0 || flock(lock, LOCK_EX | (strcmp(what, "save") ? 0 : LOCK_NB)) < 0) {
		printf("nxpersist: another is at work\n");
		return 1;
	}
	if (!strcmp(what, "load") || !strcmp(what, "load!")) {
		snprintf(address, sizeof address, "persist:%s", what);
		status = load(address);
	} else if (!strcmp(what, "save"))
		status = save();
	else if (!strcmp(what, "clear"))
		status = ask("persist:clear", 0, 10) != 200;
	else if (!strcmp(what, "auto") && argc > 2) {
		snprintf(address, sizeof address, "persist:auto=%d", atoi(argv[2]) != 0);
		status = ask(address, 0, 10) != 200;
	} else if (!strcmp(what, "state")) {
		status = ask("persist:state", 0, 10) != 200;
		if (!status && regs[REG_REPLY + 1] < 200)
			fwrite(fetched, 1, regs[REG_REPLY + 1], stdout);
	} else if (!strcmp(what, "name")) {
		/* the player's name, as the host gives it: a line (docs/system/persist.md, "The player's name") */
		status = ask("host:name", 0, 2) != 200;
		if (!status && regs[REG_REPLY + 1] > 0 && regs[REG_REPLY + 1] < 200)
			printf("%.*s\n", (int)regs[REG_REPLY + 1], (const char *)fetched);
		else
			status = 1;
	} else if (!strcmp(what, "test") && argc > 2) {
		/* packs a file as a save packs the folder's archive and unpacks it again: the sizes, the
		 * seconds and whether it came back the same (NXPERSIST_FORMAT=1 the way before) */
		FILE *in = fopen(argv[2], "rb");
		unsigned char *raw, *made = NULL, *back;
		long n, length;
		struct timespec t0, t1, t2;

		if (!in)
			return 1;
		fseek(in, 0, SEEK_END), n = ftell(in), rewind(in);
		raw = malloc(n + 1), back = malloc(n + 1);
		if (!raw || !back || fread(raw, 1, n, in) != (size_t)n)
			return 1;
		fclose(in);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		if (getenv("NXPERSIST_FORMAT") && atoi(getenv("NXPERSIST_FORMAT")) == 1) {
			made = malloc(n + 4096);
			length = deflated(raw, n, made, n + 4096, 1);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			t2 = t1;
			status = length == 0;
		} else if (getenv("NXPERSIST_FORMAT") && atoi(getenv("NXPERSIST_FORMAT")) == 2) {
			made = malloc(n + 4096);
			length = lzmaed(raw, n, made, n + 4096, n < DICT_MOST ? (n < DICT_LEAST ? DICT_LEAST : (uint32_t)n) : DICT_MOST);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			t2 = t1;
			status = length == 0;
		} else {
			length = blocked(raw, n, &made);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			status = !length || !unblocked(made, length, back, (uint32_t)n) || memcmp(raw, back, n) != 0;
			clock_gettime(CLOCK_MONOTONIC, &t2);
		}
		printf("nxpersist: test %s: %ld bytes packed to %ld in %.1f s, unpacked in %.1f s: %s\n", argv[2], n, length,
		       (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9, (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec) / 1e9,
		       status ? "WRONG" : "the same");
	} else if (!strcmp(what, "off"))
		status = ask("power:off", 0, 3) != 200;	/* a host that can switches the machine off (nxoff) */
	else
		fprintf(stderr, "usage: nxpersist load | save | clear | auto 0|1 | state | name | off\n");
	if (window)
		GrClose();
	return status;
}
