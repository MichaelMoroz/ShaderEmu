/* cab.h: a Microsoft cabinet read where it lies (alone, or behind the program of a self-extracting installer),
 * and its MSZip folders unpacked on the worker cores (libmedia's media_mszip) straight into the memory of the
 * files they make (docs/system/install.md). Nothing here draws: cabinst.c is the program, and
 * linux/libmedia/test/cab_test.c the same unpacking on the build computer. */
#ifndef SHADEREMU_APPS_CAB_H
#define SHADEREMU_APPS_CAB_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "media.h"
#include "mcwx.h"

#define CAB_WINDOW 32768u
#include <time.h>
static double cab_times[3];	/* seconds in the blocks' sums, the copies into the files, inflating (cabinst prints them) */
static double cab_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

struct cab_file {
	char name[256];		/* as the cabinet has it, '\\' between folders */
	uint32_t size, offset, folder;
	uint8_t *to;		/* where its bytes go, its size of them (NULL: not wanted, or `fd`) */
	int fd;			/* or written there by this core, when no `to` (-1: neither) */
	uint32_t crc, made;	/* its CRC-32 so far (sums), and how many of its bytes are there */
};
struct cab_folder { uint32_t first, blocks, kind; };	/* its first block's place in the file */

/* A piece of the output copied into a file by a core: whole texels of the file but at its ends (48 bytes: three texels of
 * its own, as a core writes its crc) */
struct cab_piece { uint32_t from, length, file, crc; uint8_t *to; uint32_t pad[3]; };

struct cab {
	const uint8_t *f;
	uint32_t size, at, length;	/* the file, and the cabinet's place and length in it */
	uint32_t nfolders, nfiles, data_extra;
	struct cab_folder *folders;
	struct cab_file *files;
	/* how to unpack */
	int workers;			/* the most to use (0: this core alone) */
	uint32_t batch;			/* blocks a round */
	int sums;			/* a CRC-32 of each file wanted */
	void (*progress)(void *ctx, uint64_t done, uint64_t total);	/* after each round, on this core */
	void *progress_ctx;
	/* what it did */
	uint64_t made;			/* bytes of the folders inflated */
	uint32_t rounds, bad_sums, sums_seen;
	/* the folder being unpacked */
	const uint8_t **in;
	uint32_t *in_size, *plain, *sum, nblocks;
	uint64_t *start;		/* each block's first byte in the folder */
	uint8_t *staging, *room;
	uint32_t staging_bytes, room_bytes;
	struct cab_piece *pieces;
	uint32_t npieces;
	uint32_t *sum_bad;		/* a texel a block of the round: its sum was not as the cabinet says */
};

static uint32_t cab_le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t cab_le16(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8; }

/* The cabinet's place: a cabinet's own file, or the first good header after a program's sections. */
static const char *
cab_find_header(const uint8_t *f, uint32_t size, uint32_t *at)
{
	uint32_t i;

	/* (behind a program: in its resources or after its sections, some tens of kilobytes in) */
	for (i = 0; i + 36 <= size; i++) {
		const uint8_t *m = memchr(f + i, 'M', size - 36 - i);

		if (!m)
			break;
		i = (uint32_t)(m - f);
		if (memcmp(m, "MSCF\0\0\0\0", 8))
			continue;
		if (cab_le32(m + 8) <= size - i && cab_le32(m + 16) < cab_le32(m + 8) && m[24] == 3 && m[25] == 1 &&
		    cab_le16(m + 26) >= 1 && cab_le16(m + 28) >= 1) {
			*at = i;
			return NULL;
		}
	}
	return "no cabinet in it";
}

/* The cabinet's folders and files (all held here: a ROM file is not written). */
static const char *
cab_open(struct cab *c, const uint8_t *f, uint32_t size)
{
	const uint8_t *h;
	uint32_t p, i, flags, files_at, header_extra = 0, folder_extra = 0;
	const char *why;

	memset(c, 0, sizeof *c);
	c->f = f, c->size = size, c->batch = 256, c->workers = 255;
	if ((why = cab_find_header(f, size, &c->at)))
		return why;
	h = f + c->at;
	c->length = cab_le32(h + 8);
	files_at = cab_le32(h + 16);
	c->nfolders = cab_le16(h + 26), c->nfiles = cab_le16(h + 28), flags = cab_le16(h + 30);
	p = 36;
	if (flags & 4) {
		header_extra = cab_le16(h + 36), folder_extra = h[38], c->data_extra = h[39];
		p += 4 + header_extra;
	}
	if (flags & 3)
		return "a cabinet of a set of several";
	if (!(c->folders = calloc(c->nfolders, sizeof *c->folders)) || !(c->files = calloc(c->nfiles, sizeof *c->files)))
		return "not enough memory";
	for (i = 0; i < c->nfolders; i++, p += 8 + folder_extra) {
		if (p + 8 > c->length)
			return "a cabinet cut short";
		c->folders[i].first = c->at + cab_le32(h + p), c->folders[i].blocks = cab_le16(h + p + 4), c->folders[i].kind = cab_le16(h + p + 6);
	}
	for (p = files_at, i = 0; i < c->nfiles; i++) {
		struct cab_file *e = &c->files[i];
		uint32_t n;

		if (p + 16 > c->length)
			return "a cabinet cut short";
		e->size = cab_le32(h + p), e->offset = cab_le32(h + p + 4), e->folder = cab_le16(h + p + 8), e->fd = -1;
		for (n = 0; p + 16 + n < c->length && h[p + 16 + n] && n < sizeof e->name - 1; n++)
			e->name[n] = (char)h[p + 16 + n];
		if (h[p + 16 + n])
			return "a file's name too long";
		p += 16 + n + 1;
		if (e->folder >= c->nfolders)
			return "a file in a folder there is not";
	}
	return NULL;
}

/* A file's number by its name ('/' or '\\' between folders, any case, the leading '\\' or not); -1 if none. */
static int
cab_find(const struct cab *c, const char *name)
{
	uint32_t i;

	while (*name == '/' || *name == '\\')
		name++;
	for (i = 0; i < c->nfiles; i++) {
		const char *a = c->files[i].name, *b = name;

		while (*a == '\\')
			a++;
		for (; *a && *b; a++, b++) {
			char x = *a == '\\' ? '/' : *a, y = *b == '\\' ? '/' : *b;

			if (x != y && (x | 32) != (y | 32))
				break;
			if (x != y && !((x | 32) >= 'a' && (x | 32) <= 'z'))
				break;
		}
		if (!*a && !*b)
			return (int)i;
	}
	return -1;
}

/* A block's sum as the cabinet's (cabextract's cabd_checksum): words in the little end's order, xored, the last bytes
 * a word of their own most significant first. */
static uint32_t
cab_checksum(const uint8_t *d, uint32_t bytes, uint32_t sum)
{
	uint32_t n, last = 0;

	for (n = bytes >> 2; n--; d += 4)
		sum ^= cab_le32(d);
	switch (bytes & 3) {
	case 3: last |= (uint32_t)*d++ << 16; /* fall through */
	case 2: last |= (uint32_t)*d++ << 8; /* fall through */
	case 1: last |= *d;
	}
	return sum ^ last;
}

static void
cab_sums_job(void *ctx, uint32_t from, uint32_t to)
{
	struct cab *c = ctx;
	uint32_t k;

	for (k = from; k < to; k++) {
		/* (the block's index in the folder: the round's first + k, kept in sum_bad[0]'s spare word) */
		uint32_t b = c->sum_bad[1] + k;
		const uint8_t *hd = c->in[b] - 2 - c->data_extra - 8;
		uint32_t want = cab_le32(hd), got;

		if (!want)
			continue;
		got = cab_checksum(c->in[b] - 2, c->in_size[b] + 2, 0);
		got = cab_checksum(hd + 4, 4, got);
		c->sum_bad[4 + k * 4] = got != want;
	}
}

static void
cab_pieces_job(void *ctx, uint32_t from, uint32_t to)
{
	struct cab *c = ctx;
	uint32_t k;

	for (k = from; k < to; k++) {
		struct cab_piece *p = &c->pieces[k];
		const uint8_t *src = c->staging + CAB_WINDOW + p->from;

		if (p->to)
			memcpy(p->to, src, p->length);
		if (c->sums)
			p->crc = media_crc32(0, src, p->length);
	}
}

#define CAB_PIECE 16384u	/* of a file: a piece begins on whole texels of it (16384-aligned), so no two share one */

/* The pieces of the round's output (folder bytes [lo, hi)) that go into wanted files. */
static uint32_t
cab_cut(struct cab *c, uint32_t folder, uint64_t lo, uint64_t hi)
{
	uint32_t i, n = 0;

	for (i = 0; i < c->nfiles; i++) {
		struct cab_file *e = &c->files[i];
		uint64_t a = e->offset > lo ? e->offset : lo, b = (uint64_t)e->offset + e->size < hi ? (uint64_t)e->offset + e->size : hi;

		if (e->folder != folder || (!e->to && e->fd < 0) || a >= b)
			continue;
		while (a < b) {
			uint64_t at = a - e->offset, end = (at / CAB_PIECE + 1) * CAB_PIECE;
			uint32_t len = (uint32_t)((uint64_t)e->offset + end < b ? e->offset + end - a : b - a);
			struct cab_piece *p = &c->pieces[n++];

			memset(p, 0, sizeof *p);
			p->from = (uint32_t)(a - lo), p->length = len, p->file = i;
			p->to = e->to ? e->to + at : NULL;
			a += len;
		}
	}
	return n;
}

/* A folder's wanted files, from its first block to the last byte any of them wants. */
static const char *
cab_unpack_folder(struct cab *c, uint32_t folder)
{
	struct cab_folder *fo = &c->folders[folder];
	uint64_t total = 0, last = 0, done;
	uint32_t k, b, i, p, most_pieces;
	const char *why = NULL;
	mcw_for_opts o;
	double t;

	if ((fo->kind & 15) != 1)
		return (fo->kind & 15) == 0 ? "stored, not MSZip (not done)" : "not MSZip (Quantum or LZX: not done)";
	for (i = 0; i < c->nfiles; i++)
		if (c->files[i].folder == folder && (c->files[i].to || c->files[i].fd >= 0) &&
		    (uint64_t)c->files[i].offset + c->files[i].size > last)
			last = (uint64_t)c->files[i].offset + c->files[i].size;
	if (!last)
		return NULL;
	c->nblocks = fo->blocks;
	c->in = calloc(fo->blocks, sizeof *c->in), c->in_size = calloc(fo->blocks, 4), c->plain = calloc(fo->blocks, 4);
	c->start = calloc(fo->blocks, 8);
	if (!c->in || !c->in_size || !c->plain || !c->start)
		return "not enough memory";
	/* the blocks: where each lies, how long, what it makes */
	for (p = fo->first, k = 0; k < fo->blocks; k++) {
		uint32_t packed, plain;

		if (p + 8 + c->data_extra > c->at + c->length)
			return "the cabinet ends before its last block";
		packed = cab_le16(c->f + p + 4), plain = cab_le16(c->f + p + 6);
		if (!plain)
			plain = 0x10000;	/* (never in MSZip) */
		p += 8 + c->data_extra;
		if (p + packed > c->at + c->length || packed < 2 || c->f[p] != 'C' || c->f[p + 1] != 'K')
			return "a block that is not MSZip";
		c->in[k] = c->f + p + 2, c->in_size[k] = packed - 2, c->plain[k] = plain, c->start[k] = total;
		total += plain;
		p += packed;
	}
	if (last > total)
		return "a file past the folder's end";
	/* a round: its blocks' output after the window of the block before */
	if (c->batch < 1)
		c->batch = 1;
	c->staging_bytes = CAB_WINDOW * (c->batch + 1);
	c->room_bytes = media_mszip_room(c->batch);
	most_pieces = c->batch * (CAB_WINDOW / CAB_PIECE) + 2 * c->nfiles + 8;
	if (posix_memalign((void **)&c->staging, 16, c->staging_bytes) || posix_memalign((void **)&c->room, 16, c->room_bytes) ||
	    posix_memalign((void **)&c->pieces, 16, most_pieces * sizeof *c->pieces) ||
	    posix_memalign((void **)&c->sum_bad, 16, (c->batch + 1) * 16))
		return "not enough memory";
	memset(&o, 0, sizeof o);
	o.workers = c->workers;
	o.grain = 1;
	for (b = 0; b < fo->blocks && c->start[b] < last && !why; b += k) {
		uint32_t count = fo->blocks - b < c->batch ? fo->blocks - b : c->batch;
		uint64_t lo = c->start[b], hi;

		/* (no further than the last byte wanted) */
		for (k = 0; k < count && c->start[b + k] < last; k++)
			continue;
		count = k;
		hi = c->start[b + count - 1] + c->plain[b + count - 1];
		/* the cabinet's sums of the round's blocks */
		memset(c->sum_bad, 0, (c->batch + 1) * 16);
		c->sum_bad[1] = b;
		t = cab_now();
		mcw_for_opt(count, cab_sums_job, c, &o);
		cab_times[0] += cab_now() - t;
		for (k = 0; k < count; k++)
			if (cab_le32(c->in[b + k] - 2 - c->data_extra - 8)) {
				c->sums_seen++;
				if (c->sum_bad[4 + k * 4])
					c->bad_sums++;
			}
		if (c->bad_sums) {
			why = "a block's sum is not as the cabinet says";
			break;
		}
		t = cab_now();
		why = media_mszip(c->in + b, c->in_size + b, c->plain + b, count, c->staging + CAB_WINDOW, b ? CAB_WINDOW : 0,
				       c->room, c->workers);
		cab_times[2] += cab_now() - t;
		if (why)
			break;
		c->made += hi - lo;
		c->rounds++;
		/* into the files */
		c->npieces = cab_cut(c, folder, lo, hi);
		if (c->npieces)
		{
			t = cab_now();
			mcw_for_opt(c->npieces, cab_pieces_job, c, &o);
			cab_times[1] += cab_now() - t;
		}
		for (i = 0; i < c->npieces; i++) {
			struct cab_piece *q = &c->pieces[i];
			struct cab_file *e = &c->files[q->file];

			if (!q->to && e->fd >= 0) {
				uint32_t w = 0;

				while (w < q->length) {
					ssize_t n = pwrite(e->fd, c->staging + CAB_WINDOW + q->from + w, q->length - w,
							   (off_t)(lo + q->from + w - e->offset));

					if (n <= 0) {
						why = "a file could not be written";
						break;
					}
					w += (uint32_t)n;
				}
			}
			if (c->sums)
				e->crc = e->made ? media_crc32_join(e->crc, q->crc, q->length) : q->crc;
			e->made += q->length;
		}
		/* the window of the next round: this one's last block */
		if (b + count < fo->blocks)
			memcpy(c->staging, c->staging + CAB_WINDOW + (size_t)(count - 1) * CAB_WINDOW, CAB_WINDOW);
		done = hi;
		if (c->progress)
			c->progress(c->progress_ctx, done, last);
	}
	free(c->in), free(c->in_size), free(c->plain), free(c->start);
	free(c->staging), free(c->room), free(c->pieces), free(c->sum_bad);
	c->in = NULL, c->in_size = c->plain = NULL, c->start = NULL, c->staging = c->room = NULL, c->pieces = NULL, c->sum_bad = NULL;
	for (i = 0; i < c->nfiles && !why; i++)
		if (c->files[i].folder == folder && (c->files[i].to || c->files[i].fd >= 0) && c->files[i].made != c->files[i].size)
			why = "a file did not come out whole";
	return why;
}

/* Every folder's wanted files (a file's `to` or `fd` set). */
static const char *
cab_unpack(struct cab *c)
{
	uint32_t i;
	const char *why = NULL;

	for (i = 0; i < c->nfolders && !why; i++)
		why = cab_unpack_folder(c, i);
	return why;
}

#endif
