/*
 * unarc: archives unpacked with the worker cores (docs/system/open.md, "Archives").
 *   unarc zip|tar|tgz|gz FILE TO      a zip, tar or .tar.gz into the folder TO; a .gz into the file TO
 * A zip's entries are inflated a core each and a large one, or a gzip stream, on every core
 * (libmedia's media_inflate_stream); then this core writes the files. "progress DONE TOTAL"
 * lines on the output say how far. Exit 3: a kind of entry it does not unpack, or no memory in
 * one piece for it all (open.c then runs busybox's unzip or tar), 1 anything else wrong.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "media.h"
#include "mcw.h"
#include "mcwx.h"

#define CANNOT 3
static int workers, no_room;

/* (a whole archive or its output in one block: a machine without an MMU may have no such block) */
static void *
room_for(size_t bytes)
{
	/* (more than the machine has is a size read from a damaged file) */
	void *m = bytes > 0x0f000000u ? NULL : aligned_alloc(16, (bytes + 15) & ~(size_t)15);

	no_room |= !m;
	return m;
}
static unsigned long long done_bytes, total_bytes;

static void
progress(void)
{
	printf("progress %llu %llu\n", done_bytes, total_bytes);
	fflush(stdout);
}

static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static uint8_t *
read_all(const char *path, uint32_t *size)
{
	struct stat st;
	uint8_t *f;
	uint32_t got = 0;
	int fd = open(path, O_RDONLY);

	if (fd < 0 || fstat(fd, &st) || !(f = room_for((size_t)st.st_size + 16)))
		return NULL;
	while (got < (uint32_t)st.st_size) {
		ssize_t n = read(fd, f + got, (size_t)st.st_size - got);

		if (n <= 0)
			break;
		got += (uint32_t)n;
	}
	close(fd);
	*size = got;
	return f;
}

/* ---------------------------------------------------------------- files */

/* A name inside TO, or NULL for one that would leave it ("..", or from the root). */
static const char *
inside(const char *to, const char *name, char *path, size_t room)
{
	const char *p;

	while (*name == '/')
		name++;
	for (p = name; *p; p++)
		if (p[0] == '.' && p[1] == '.' && (p == name || p[-1] == '/') && (p[2] == '/' || !p[2]))
			return NULL;
	if (!*name || snprintf(path, room, "%s/%s", to, name) >= (int)room)
		return NULL;
	return path;
}

static void
make_parents(char *path)
{
	char *p;

	for (p = path + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(path, 0755);
			*p = '/';
		}
}

static int
write_file(const char *path, const uint8_t *data, uint32_t size, int mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode & 07777 : 0644);
	uint32_t at = 0;

	if (fd < 0 && errno == ENOENT) {
		make_parents((char *)path);
		fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode & 07777 : 0644);
	}
	if (fd < 0)
		return -1;
	while (at < size) {
		ssize_t n = write(fd, data + at, size - at);

		if (n <= 0)
			break;
		at += (uint32_t)n;
	}
	close(fd);
	done_bytes += size;
	return at == size ? 0 : -1;
}

static void
make_dir(char *path)
{
	size_t n = strlen(path);

	if (n && path[n - 1] == '/')
		path[n - 1] = 0;
	make_parents(path);
	mkdir(path, 0755);
}

/* ---------------------------------------------------------------- tar */

static unsigned long long
octal(const uint8_t *p, int n)
{
	unsigned long long v = 0;

	if (p[0] & 0x80) {	/* base 256 */
		int i;

		for (i = 1; i < n; i++)
			v = v << 8 | p[i];
		return v;
	}
	for (; n && (*p == ' ' || *p == 0); p++, n--)
		;
	for (; n && *p >= '0' && *p <= '7'; p++, n--)
		v = v * 8 + (unsigned)(*p - '0');
	return v;
}

/* A pax header's "path" or "linkpath" record into out. */
static void
pax_value(const uint8_t *p, uint32_t size, const char *key, char *out, size_t room)
{
	uint32_t at = 0;
	size_t k = strlen(key);

	while (at < size) {
		uint32_t len = 0, s = at;

		while (s < size && p[s] >= '0' && p[s] <= '9')
			len = len * 10 + (uint32_t)(p[s++] - '0');
		if (!len || len > size - at)
			return;
		if (s + 1 + k + 1 < at + len && !memcmp(p + s + 1, key, k) && p[s + 1 + k] == '=') {
			uint32_t from = s + 2 + (uint32_t)k, n = at + len - 1 - from;

			if (n >= room)
				n = (uint32_t)room - 1;
			memcpy(out, p + from, n);
			out[n] = 0;
		}
		at += len;
	}
}

static int
untar(const uint8_t *t, uint32_t size, const char *to)
{
	char name[1024], link_name[1024], path[1200], target[1200];
	uint32_t at = 0;
	int bad = 0;

	name[0] = link_name[0] = 0;
	total_bytes = size, done_bytes = 0;
	while (at + 512 <= size) {
		const uint8_t *h = t + at;
		unsigned long long n = octal(h + 124, 12);
		int type = h[156], mode = (int)octal(h + 100, 8);
		uint32_t data = at + 512;

		if (!h[0]) {
			at += 512;
			continue;	/* (the end's zero blocks) */
		}
		if (n > size - data)
			return 1;
		at = data + (uint32_t)((n + 511) & ~511ull);
		if (type == 'L' || type == 'K') {	/* GNU's long names */
			char *o = type == 'L' ? name : link_name;
			uint32_t k = n < sizeof name ? (uint32_t)n : sizeof name - 1;

			memcpy(o, t + data, k);
			o[k] = 0;
			continue;
		}
		if (type == 'x') {
			pax_value(t + data, (uint32_t)n, "path", name, sizeof name);
			pax_value(t + data, (uint32_t)n, "linkpath", link_name, sizeof link_name);
			continue;
		}
		if (type == 'g')
			continue;
		if (!name[0]) {
			if (!memcmp(h + 257, "ustar", 5) && h[345])
				snprintf(name, sizeof name, "%.155s/%.100s", (const char *)h + 345, (const char *)h);
			else
				snprintf(name, sizeof name, "%.100s", (const char *)h);
		}
		if (!link_name[0])
			snprintf(link_name, sizeof link_name, "%.100s", (const char *)h + 157);
		if (inside(to, name, path, sizeof path)) {
			if (type == '5') {
				make_dir(path);
			} else if (type == '2') {
				make_parents(path);
				unlink(path);
				if (symlink(link_name, path))
					bad = 1;
			} else if (type == '1') {
				make_parents(path);
				unlink(path);
				if (!inside(to, link_name, target, sizeof target) || link(target, path))
					bad = 1;
			} else if (type == '0' || type == 0 || type == '7') {
				if (write_file(path, t + data, (uint32_t)n, mode))
					bad = 1;
			}
		}
		name[0] = link_name[0] = 0;
		if (at - done_bytes >= (1u << 20)) {
			done_bytes = at;
			progress();
		}
	}
	done_bytes = size;
	progress();
	return bad;
}

/* ---------------------------------------------------------------- gzip */

/* The deflate stream of a gzip member at f; 0 if f is not one. */
static uint32_t
gzip_body(const uint8_t *f, uint32_t size)
{
	uint32_t at = 10;

	if (size < 18 || f[0] != 0x1f || f[1] != 0x8b || f[2] != 8)
		return 0;
	if (f[3] & 4)
		at += 2 + le16(f + 10);
	if (f[3] & 8)
		while (at < size && f[at++]);
	if (f[3] & 16)
		while (at < size && f[at++]);
	if (f[3] & 2)
		at += 2;
	return at + 8 <= size ? at : 0;
}

/* Every member of a gzip file inflated, one after another into one buffer. */
static uint8_t *
gunzip(const uint8_t *f, uint32_t size, uint32_t *made, const char **why)
{
	uint32_t room = le32(f + size - 4), have = 0, at = 0;
	uint8_t *out;

	/* (the last member's size: the whole when there is one, which there nearly always is) */
	if (room < size)
		room = size * 4;
	if (!(out = room_for(room))) {
		*why = "not enough memory";
		return NULL;
	}
	mcw_touch(out, room);
	total_bytes = room;
	while (at < size) {
		uint32_t body = gzip_body(f + at, size - at), n = 0, used = 0, crc = 0;

		if (!body) {
			if (at)
				break;	/* (what follows the last member) */
			*why = "not gzip";
			free(out);
			return NULL;
		}
		*why = media_inflate_stream(f + at + body, size - at - body - 8, out + have, room - have, &n, &used, &crc, workers);
		if (*why && !strcmp(*why, "larger than the room for it")) {
			uint8_t *more = room_for((size_t)room * 2 + 16);

			if (!more) {
				*why = "not enough memory";
				free(out);
				return NULL;
			}
			mcw_touch(more, room * 2);
			memcpy(more, out, have);
			free(out);
			out = more, room *= 2, total_bytes = room;
			continue;
		}
		if (*why) {
			free(out);
			return NULL;
		}
		if (at + body + used + 8 > size || crc != le32(f + at + body + used) || n != le32(f + at + body + used + 4)) {
			*why = "its CRC-32 or length is wrong";
			free(out);
			return NULL;
		}
		have += n;
		at += body + used + 8;
		done_bytes = have;
		progress();
	}
	*made = have;
	*why = NULL;
	return out;
}

/* ---------------------------------------------------------------- zip */

/* (48 bytes: a worker writes `failed` of its entries, and an entry is three texels of its own) */
struct entry { const uint8_t *data; uint32_t packed, size, crc, out, method, mode, failed; char *name; uint32_t pad[3]; };
struct zip { struct entry *e; uint32_t n; uint8_t *out; uint8_t *rooms; uint32_t room; };

#define ALONE (256u << 10)	/* entries packed smaller than this: a core each; larger: every core */

static void
zip_job(void *ctx, uint32_t from, uint32_t to)
{
	struct zip *z = ctx;
	void *room = z->rooms + (size_t)mcw_self() * z->room;

	for (; from < to; from++) {
		struct entry *e = &z->e[from];
		uint32_t made = 0, crc = 0;

		if (e->packed >= ALONE && e->method == 8)
			continue;
		if (e->method == 0) {
			if (e->packed != e->size) {
				e->failed = 1;
				continue;
			}
			memcpy(z->out + e->out, e->data, e->size);
			crc = media_crc32(0, z->out + e->out, e->size), made = e->size;
		} else if (media_inflate_one(e->data, e->packed, z->out + e->out, e->size, &made, NULL, &crc, room)) {
			e->failed = 1;
			continue;
		}
		e->failed = made != e->size || crc != e->crc;
	}
}

static int
unzip(const uint8_t *f, uint32_t size, const char *to)
{
	uint32_t end, at, k, n, total = 0;
	struct zip z;
	char path[1200];
	int bad = 0;
	mcw_for_opts o;

	/* the end record, then the central folder */
	for (end = size >= 22 ? size - 22 : 0; end > 0 && le32(f + end) != 0x06054b50; end--)
		if (size - end > 65557)
			return 1;
	if (le32(f + end) != 0x06054b50)
		return 1;
	n = le16(f + end + 10), at = le32(f + end + 16);
	if (n == 0xffff || at == 0xffffffff)
		return CANNOT;	/* (zip64) */
	memset(&z, 0, sizeof z);
	if (!(z.e = room_for((n + 1) * sizeof *z.e)))
		return 1;
	memset(z.e, 0, (n + 1) * sizeof *z.e);
	for (k = 0; k < n; k++) {
		struct entry *e = &z.e[k];
		uint32_t flags, name_len, local;

		if (at + 46 > size || le32(f + at) != 0x02014b50)
			return 1;
		flags = le16(f + at + 8), e->method = le16(f + at + 10), e->crc = le32(f + at + 16);
		e->packed = le32(f + at + 20), e->size = le32(f + at + 24), name_len = le16(f + at + 28);
		local = le32(f + at + 42);
		if ((flags & 1) || (e->method != 0 && e->method != 8) || e->size == 0xffffffff)
			return CANNOT;	/* encrypted, another method, zip64 */
		e->mode = (f[at + 5] == 3) ? le32(f + at + 38) >> 16 : 0;	/* (made on Unix: its mode) */
		if (!(e->name = room_for(name_len + 1)))
			return 1;
		memcpy(e->name, f + at + 46, name_len);
		e->name[name_len] = 0;
		at += 46 + name_len + le16(f + at + 30) + le16(f + at + 32);
		if (local + 30 > size || le32(f + local) != 0x04034b50)
			return 1;
		e->data = f + local + 30 + le16(f + local + 26) + le16(f + local + 28);
		if (e->data + e->packed > f + size)
			return 1;
		if (e->size > 0x40000000u || total + e->size > 0x40000000u)
			return 1;	/* (the sizes are the file's word: their sum must not wrap, or an entry has room it has not) */
		e->out = total;
		total += (e->size + 15) & ~15u;	/* (each its own texels: a core writes it) */
	}
	z.n = n;
	total_bytes = total;
	if (!(z.out = room_for((size_t)total + 16)))
		return 1;
	mcw_touch(z.out, total + 16);
	progress();
	/* the small entries a core each, then every large one on every core */
	z.room = (media_inflate_room() + 15) & ~15u;
	if (!(z.rooms = room_for((size_t)z.room * (unsigned)(workers + 1))))
		return 1;
	memset(&o, 0, sizeof o);
	o.workers = workers;
	o.grain = 1;
	mcw_for_opt(n, zip_job, &z, &o);
	for (k = 0; k < n; k++) {
		struct entry *e = &z.e[k];
		uint32_t made = 0, crc = 0;

		if (e->packed < ALONE || e->method != 8)
			continue;
		if (media_inflate_stream(e->data, e->packed, z.out + e->out, e->size, &made, NULL, &crc, workers) || made != e->size ||
		    crc != e->crc)
			e->failed = 1;
	}
	/* the files */
	for (k = 0; k < n; k++) {
		struct entry *e = &z.e[k];
		size_t len = strlen(e->name);

		if (e->failed) {
			fprintf(stderr, "unarc: %s: damaged\n", e->name);
			bad = 1;
			continue;
		}
		if (!inside(to, e->name, path, sizeof path))
			continue;
		if (len && e->name[len - 1] == '/') {
			make_dir(path);
		} else if ((e->mode & 0170000) == 0120000) {
			char target[1024];
			uint32_t t = e->size < sizeof target - 1 ? e->size : sizeof target - 1;

			memcpy(target, z.out + e->out, t);
			target[t] = 0;
			make_parents(path);
			unlink(path);
			if (symlink(target, path))
				bad = 1;
		} else if (write_file(path, z.out + e->out, e->size, e->mode & 0777)) {
			bad = 1;
		}
		if ((k & 63) == 63)
			progress();
	}
	done_bytes = total;
	progress();
	return bad;
}

int
main(int argc, char **argv)
{
	uint8_t *f, *out;
	uint32_t size, made;
	const char *why = NULL;
	int r;

	if (argc != 4) {
		fprintf(stderr, "usage: unarc zip|tar|tgz|gz FILE TO\n");
		return 2;
	}
	if (!(f = read_all(argv[2], &size))) {
		fprintf(stderr, "unarc: %s: %s\n", argv[2], no_room ? "not enough memory in one piece" : "cannot read it");
		return no_room ? CANNOT : 1;
	}
	media_crc32(0, NULL, 0);	/* (its tables made here, before any worker uses them) */
	workers = mcw_open(255);
	if (!strcmp(argv[1], "zip")) {
		r = unzip(f, size, argv[3]);
	} else if (!strcmp(argv[1], "tar")) {
		mkdir(argv[3], 0755);
		r = untar(f, size, argv[3]);
	} else if (!strcmp(argv[1], "tgz") || !strcmp(argv[1], "gz")) {
		if (!(out = gunzip(f, size, &made, &why))) {
			fprintf(stderr, "unarc: %s: %s\n", argv[2], why);
			mcw_close();
			return no_room || !strcmp(why, "not enough memory") ? CANNOT : 1;
		}
		free(f);
		if (!strcmp(argv[1], "gz")) {
			total_bytes = made, done_bytes = 0;
			r = write_file(argv[3], out, made, 0644) ? 1 : 0;
			progress();
		} else {
			mkdir(argv[3], 0755);
			r = untar(out, made, argv[3]);
		}
	} else {
		fprintf(stderr, "unarc: %s: not a kind it knows\n", argv[1]);
		r = 2;
	}
	mcw_close();
	if (r && no_room) {
		fprintf(stderr, "unarc: %s: not enough memory in one piece\n", argv[2]);
		r = CANNOT;
	} else if (r == CANNOT)
		fprintf(stderr, "unarc: %s: entries it does not unpack (encrypted, another method, zip64)\n", argv[2]);
	else if (r)
		fprintf(stderr, "unarc: %s: not all of it was unpacked\n", argv[2]);
	return r;
}
