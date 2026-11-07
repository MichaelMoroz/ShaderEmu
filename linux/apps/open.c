/*
 * open [--dry] ADDRESS-OR-FILE: opens a file, or what an address holds, with the program
 * registered for its kind (docs/system/open.md). --dry prints the kind and the command only.
 * open --unpack zip|tar|tgz|gz FILE TO: an archive unpacked, with its progress (nxopen-archive).
 */
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ftw.h>
#include <poll.h>
#include <unistd.h>
#include "nano-X.h"
#include "ui.h"
#include "ui_progress.h"
#include "ui_ask.h"

#define FETCH_MOST 0x40000	/* bytes the host hands over at a time (docs/system/fetch.md) */
#define REG_ASKED 64		/* requests made, the address's length, the kind, the offset */
#define REG_REPLY 68		/* requests answered, bytes in all, status, a picture's size */
#define FETCH_RGBA 0x8000	/* in a picture's size: its pixels are red, green, blue and alpha bytes */
#define REG_ADDRESS 72
#define KIND_FILE 3
#define HEADER 256		/* of a file a picture carried: mark, length, CRC-32, name */
#define FOLDER "/tmp/open"
#define MAX_KINDS 32
#define STOPPED (-1)		/* ask's answer when the progress window was closed */

static struct kind {
	char name[16], endings[96], magic[128], command[96];
} kinds[MAX_KINDS];
static int kind_count;
static volatile uint32_t *regs;
static const unsigned char *fetched;
static unsigned long long fetch_total;	/* the answer's size, once the host has said it */
static char why[200];			/* why a file did not come, for the window that says so */

static uint32_t
crc32(const unsigned char *p, size_t n, uint32_t crc)
{
	int k;

	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (k = 0; k < 8; k++)
			crc = crc >> 1 ^ (0xedb88320u & -(crc & 1));
	}
	return ~crc;
}

static int
registry_entry(const struct dirent *entry)
{
	return !strncmp(entry->d_name, "nxopen.", 7);
}

/* A word of a line into `to` (of `room` bytes), and the place after it and its spaces; NULL
 * when the line has no word there, or one too long for a word of the registry. */
static const char *
word_of(const char *at, char *to, int room)
{
	int n = 0;

	while (*at == ' ' || *at == '\t')
		at++;
	for (; (unsigned char)*at > ' '; at++) {
		if (n == room - 1)
			return NULL;
		to[n++] = *at;
	}
	to[n] = 0;
	while (*at == ' ' || *at == '\t' || *at == '\r' || *at == '\n')
		at++;
	return n ? at : NULL;
}

/* The registry: every line of every /usr/share/nxopen.* file is "kind endings magic command".
 * (Only those files' names are kept and put in order: the folder has a hundred others.) */
static void
read_kinds(void)
{
	struct dirent **names;
	int n = scandir("/usr/share", &names, registry_entry, alphasort), i;

	for (i = 0; i < n; i++) {
		char path[300], line[400];
		FILE *file;

		memcpy(path, "/usr/share/", 11);
		strcpy(path + 11, names[i]->d_name);
		if ((file = fopen(path, "r")) == NULL)
			continue;
		while (fgets(line, sizeof line, file) && kind_count < MAX_KINDS) {
			struct kind *k = &kinds[kind_count];
			const char *at = line;
			size_t length;

			if (line[0] == '#' || !(at = word_of(at, k->name, sizeof k->name)) || !(at = word_of(at, k->endings, sizeof k->endings)) ||
			    !(at = word_of(at, k->magic, sizeof k->magic)))
				continue;
			length = strcspn(at, "\n");
			length = length < sizeof k->command - 1 ? length : sizeof k->command - 1;
			memcpy(k->command, at, length);
			k->command[length] = 0;
			if (k->command[0])
				kind_count++;
		}
		fclose(file);
	}
}

/* True if the bytes begin with one of a kind's marks: a list with commas, \xNN for a byte, \x.. any.
 * A mark that begins with '<' is a page's tag, compared in either case; any other is bytes. */
static int
has_magic(const struct kind *k, const unsigned char *head, int n)
{
	const char *m = k->magic;

	if (!strcmp(m, "-"))
		return 0;
	while (*m) {
		int at = 0, same = 1, tag = *m == '<';

		while (*m && *m != ',') {
			int c = (unsigned char)*m++;

			if (c == '\\' && *m == 'x' && isxdigit((unsigned char)m[1]) && isxdigit((unsigned char)m[2])) {
				char hex[3] = { m[1], m[2], 0 };

				c = (int)strtol(hex, NULL, 16);
				m += 3;
				if (at >= n || head[at] != c)
					same = 0;
			} else if (c == '\\' && !strncmp(m, "x..", 3)) {
				m += 3;
				if (at >= n)
					same = 0;
			} else if (at >= n || (tag ? tolower(head[at]) != tolower(c) : head[at] != c))
				same = 0;
			at++;
		}
		if (same && at > 0)
			return 1;
		if (*m == ',')
			m++;
	}
	return 0;
}

static int
has_ending(const struct kind *k, const char *name)
{
	const char *dot = strrchr(name, '.'), *e = k->endings;
	size_t n;

	if (!dot || !strcmp(e, "-"))
		return 0;
	n = strlen(++dot);
	while (*e) {
		size_t len = strcspn(e, ",");

		if (len == n && !strncasecmp(e, dot, n))
			return 1;
		e += len + (e[len] == ',');
	}
	return 0;
}

/* A file's kind: by its first bytes where a kind's marks say, else by its name's ending. */
static const struct kind *
kind_of(const char *path, const unsigned char *head, int n)
{
	int i, skip = 0;

	for (i = 0; i < kind_count; i++)
		if (has_magic(&kinds[i], head, n))
			return &kinds[i];
	/* (a page may begin with a byte order mark and blank lines) */
	if (n >= 3 && !memcmp(head, "\xef\xbb\xbf", 3))
		skip = 3;
	while (skip < n && isspace(head[skip]))
		skip++;
	for (i = 0; skip > 0 && i < kind_count; i++)
		if (has_magic(&kinds[i], head + skip, n - skip))
			return &kinds[i];
	for (i = 0; i < kind_count; i++)
		if (has_ending(&kinds[i], path))
			return &kinds[i];
	return NULL;
}

/* Asks the host for a part of what an address holds; the answer's status, or 0 for none. */
static int
ask(const char *address, uint32_t offset, int seconds)
{
	uint32_t mine;
	unsigned began = progress_ms();
	size_t n = strlen(address);

	/* one request at a time: wait for another program's to be answered, 10 s at most */
	while (regs[REG_ASKED] != regs[REG_REPLY] && progress_ms() - began < 10000) {
		if (fetch_total)
			progress_poll();
		else
			progress_show(0, 0, "Waiting for another program's request to the host");
		if (progress_closed)
			return STOPPED;
		usleep(50000);
	}
	if (n > 255)
		n = 255;
	memset((void *)&regs[REG_ADDRESS], 0, 256);
	memcpy((void *)&regs[REG_ADDRESS], address, n);
	regs[REG_ASKED + 1] = n;
	regs[REG_ASKED + 2] = KIND_FILE;
	regs[REG_ASKED + 3] = offset;
	mine = regs[REG_ASKED] + 1;
	regs[REG_ASKED] = mine;
	for (began = progress_ms(); regs[REG_REPLY] != mine;) {
		if (progress_ms() - began > seconds * 1000u)
			return 0;
		if (!fetch_total) {
			/* (the host has nothing to say until it has all of it: VRChat tells a world no more) */
			char line[64];

			snprintf(line, sizeof line, "The host is downloading it: %u s", (progress_ms() - progress_began) / 1000);
			progress_show(0, 0, line);
		} else
			progress_poll();
		if (progress_closed)
			return STOPPED;
		usleep(50000);
	}
	return regs[REG_REPLY + 2];
}

/* What a status of the host's says (docs/system/fetch.md), into `why`. */
static void
why_of(int status)
{
	if (status == 0)
		snprintf(why, sizeof why, "the host did not answer in 40 s");
	else if (status == 1)
		snprintf(why, sizeof why, "the world was not handed this address (the links panel by the display)");
	else if (status == 2)
		snprintf(why, sizeof why, "the host's download gave no answer (VRChat gives up after 20 s)");
	else if (status == 3)
		snprintf(why, sizeof why, "an address the host may not open (VRChat opens only https)");
	else if (status == 404 || status == 410)
		snprintf(why, sizeof why, "not found (%d)", status);
	else if (status == 401 || status == 403)
		snprintf(why, sizeof why, "refused (%d)", status);
	else if (status == 415)
		snprintf(why, sizeof why, "a picture the host could not decode (415)");
	else if (status == 413)
		snprintf(why, sizeof why, "a picture over the 2048 pixels a side the host's loader takes (413)");
	else if (status >= 400 && status < 500)
		snprintf(why, sizeof why, "the server would not give it (%d)", status);
	else if (status >= 500)
		snprintf(why, sizeof why, "the server's error (%d)", status);
	else
		snprintf(why, sizeof why, "the host said %d", status);
}

/* The last piece of an address's path, without what follows a question mark, as a file's name. */
static void
name_from(const char *address, char *name, size_t room)
{
	const char *end = address + strcspn(address, "?#"), *from = end;
	size_t i, n;

	while (from > address && from[-1] != '/')
		from--;
	n = (size_t)(end - from) < room - 1 ? (size_t)(end - from) : room - 1;
	for (i = 0; i < n; i++)
		name[i] = isalnum((unsigned char)from[i]) || strchr("._-", from[i]) ? from[i] : '_';
	name[n] = 0;
	if (!name[0] || !strcmp(name, ".") || !strcmp(name, ".."))
		snprintf(name, room, "download");
}

/*
 * A photo of a file (nxexport's, docs/apps/export.md, "A photo"): a picture whose pixels are the bytes of a 24-bit
 * bitmap (54 bytes of header, then rows from the bottom, blue, green, red) that is a zip: its entries from byte 54,
 * its directory at the file's end. The bitmap's byte at `f` of a picture w pixels wide, from its pixels as the host
 * gives them (a word each, rows from the top, red in the low byte).
 */
static uint8_t
photo_byte(const uint32_t *pixels, uint32_t w, uint32_t h, uint64_t f)
{
	uint64_t i = f - 54, p = i / 3;
	uint32_t row = (uint32_t)(p / w), x = (uint32_t)(p % w), c = pixels[(uint64_t)(h - 1 - row) * w + x];

	return (uint8_t)(i % 3 == 0 ? c >> 16 : i % 3 == 1 ? c >> 8 : c);
}

static uint32_t
le32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The picture's top row (the file's last bytes) ends with a zip's end whose directory lies in a file of this size. */
static int
photo_like(const uint32_t *top, uint32_t w, uint32_t h)
{
	uint64_t size = 54 + 3ull * w * h, i;
	uint8_t end[22];
	uint32_t cd_size, cd_at;

	if (w < 8 || (w * 3) % 4)
		return 0;
	for (i = 0; i < 22; i++)	/* (the file's last w * 3 bytes are the top row: counted from the bottom, row h - 1) */
		end[i] = photo_byte(top, w, h, size - 22 + i);	/* (only row h - 1 is read: the top row, index x) */
	cd_size = le32(end + 12), cd_at = le32(end + 16);
	return le32(end) == 0x06054b50 && end[20] == 0 && end[21] == 0 && cd_at >= 54 && (uint64_t)cd_at + cd_size + 22 == size;
}

/* The zip a photo's pixels hold, made whole again without the bitmap's header and the gap before its directory:
 * at `zip`, its length; 0 if it is none. */
static uint32_t
photo_zip(const uint32_t *pixels, uint32_t w, uint32_t h, uint8_t **zip)
{
	uint64_t size = 54 + 3ull * w * h, f;
	uint8_t *file = malloc((size_t)size), *out, *e;
	uint32_t cd_size, cd_at, entries_end = 54, n, k, at;

	*zip = NULL;
	if (!file)
		return 0;
	for (f = 54; f < size; f++)
		file[f] = photo_byte(pixels, w, h, f);
	e = file + size - 22;
	cd_size = le32(e + 12), cd_at = le32(e + 16), n = e[10] | e[11] << 8;
	/* where the last entry ends: each local header's own name and extra lengths, and its packed size */
	for (k = 0, at = cd_at; k < n; k++) {
		uint32_t local, end;

		if (at + 46 > cd_at + cd_size || le32(file + at) != 0x02014b50)
			return free(file), 0;
		local = le32(file + at + 42);
		if (local < 54 || local + 30 > cd_at || le32(file + local) != 0x04034b50)
			return free(file), 0;
		end = local + 30 + (file[local + 26] | file[local + 27] << 8) + (file[local + 28] | file[local + 29] << 8) + le32(file + at + 20);
		if (end > cd_at)
			return free(file), 0;
		if (end > entries_end)
			entries_end = end;
		at += 46 + (file[at + 28] | file[at + 29] << 8) + (file[at + 30] | file[at + 31] << 8) + (file[at + 32] | file[at + 33] << 8);
	}
	if (!(out = malloc(entries_end - 54 + cd_size + 22)))
		return free(file), 0;
	memcpy(out, file + 54, entries_end - 54);
	memcpy(out + entries_end - 54, file + cd_at, cd_size + 22);
	/* the offsets from the zip's own start: every entry's in the directory, and the directory's in its end */
	for (k = 0, at = entries_end - 54; k < n; k++) {
		uint32_t v = le32(out + at + 42) - 54;

		out[at + 42] = (uint8_t)v, out[at + 43] = (uint8_t)(v >> 8), out[at + 44] = (uint8_t)(v >> 16), out[at + 45] = (uint8_t)(v >> 24);
		at += 46 + (out[at + 28] | out[at + 29] << 8) + (out[at + 30] | out[at + 31] << 8) + (out[at + 32] | out[at + 33] << 8);
	}
	e = out + entries_end - 54 + cd_size;
	e[16] = (uint8_t)(entries_end - 54), e[17] = (uint8_t)((entries_end - 54) >> 8), e[18] = (uint8_t)((entries_end - 54) >> 16);
	e[19] = (uint8_t)((entries_end - 54) >> 24);
	free(file);
	*zip = out;
	return entries_end - 54 + cd_size + 22;
}

/* What an address holds, into a file under FOLDER whose path is returned in `path`; 0, with a
 * line said, if it did not come. */
static int
fetch(const char *address, char *path, size_t room)
{
	char name[200];
	uint32_t total, got = 0, skip = 0, crc = 0, want_crc = 0;
	int status, tries, fd, packed, by_hand = 0;
	FILE *file;

	fd = open("/dev/gpu", O_RDWR);
	if (fd < 0 || (regs = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x01000000)) == MAP_FAILED ||
	    (fetched = mmap(NULL, FETCH_MOST, PROT_READ, MAP_SHARED, fd, 0x016c0000)) == MAP_FAILED) {
		snprintf(why, sizeof why, "this machine has no way to ask its host for a file");
		printf("open: %s\n", why);
		return 0;
	}
	name_from(address, name, sizeof name - 4);
	snprintf(path, room, "Loading %s", name);
	progress_begin(path);
	/* (a world may not have been handed the address yet: asked again for a minute) */
	for (tries = 0; (status = ask(address, 0, 40)) == 1 && tries < 20; tries++) {
		int wait;

		if (!by_hand++)
			printf("open: the host has to be handed this address first\n");
		for (wait = 0; wait < 60 && !progress_closed; wait++) {
			progress_show(0, 0, "Hand the world this address: the links panel by the display");
			usleep(50000);
		}
	}
	if (status != 200 && status != 201) {
		why_of(status);
		if (status == STOPPED)
			printf("open: stopped: the window was closed\n");
		else
			printf("open: the host could not get %s (%d): %s\n", address, status, why);
		return 0;
	}
	total = regs[REG_REPLY + 1];
	fetch_total = total;
	packed = status == 201;
	mkdir(FOLDER, 0777);
	if (!packed && regs[REG_REPLY + 3] != 0) {
		/* a picture the host could only give as pixels: a word each, in parts of 65,536. With
		 * FETCH_RGBA its four bytes are red, green, blue and alpha (VRChat's picture loader, the
		 * harness: docs/system/fetch.md), kept as a TGA of 32 bits with its alpha; else 0x00RRGGBB
		 * (a host from before), kept as a PPM file. A part's bytes are made first and written at
		 * once: a byte a call was 76 instructions a pixel, this is 12. */
		static uint8_t rgb[FETCH_MOST];
		uint32_t size = regs[REG_REPLY + 3], w = size & 0x7fff, h = size >> 16, i, done = 0;
		int rgba = (size & FETCH_RGBA) != 0;
		uint32_t *photo = NULL;	/* a photo of a file: every pixel kept, the zip made of them at the end */

		name_from(address, name, sizeof name - 4);
		if (rgba && w && h && photo_like((const uint32_t *)fetched, w, h) && (photo = malloc((size_t)w * h * 4)) != NULL) {
			/* (its name without the picture's ending: a zip) */
			char *dot = strrchr(name, '.');

			if (dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".bmp")))
				*dot = 0;
			for (;;) {
				uint32_t here = w * h - done < FETCH_MOST / 4 ? w * h - done : FETCH_MOST / 4;

				memcpy(photo + done, fetched, here * 4);
				done += here;
				progress_show(done, (unsigned long long)w * h, progress_megabytes(done * 3ull, w * h * 3ull));
				if (done >= w * h)
					break;
				status = ask(address, done * 4, 40);
				if (status != 200 || regs[REG_REPLY + 3] != size) {
					why_of(status);
					printf("open: the host stopped at %u of %u pixels (%d)\n", (unsigned)done, (unsigned)(w * h), status);
					free(photo);
					return 0;
				}
			}
			{
				uint8_t *zip;
				uint32_t n = photo_zip(photo, w, h, &zip);

				free(photo);
				snprintf(path, room, FOLDER "/%s.zip", name);
				if (!n || (file = fopen(path, "wb")) == NULL || fwrite(zip, 1, n, file) != n) {
					if (file)
						fclose(file);
					free(zip);
					snprintf(why, sizeof why, "the picture looked like a photo of a zip, but its directory is damaged");
					printf("open: %s\n", why);
					return 0;
				}
				fclose(file);
				printf("open: %s, %u bytes from a photo of %u x %u, sum %08x\n", path, (unsigned)n, (unsigned)w, (unsigned)h,
				       (unsigned)crc32(zip, n, 0));
				free(zip);
				return 1;
			}
		}
		snprintf(path, room, FOLDER "/%s.%s", name, rgba ? "tga" : "ppm");
		if (w == 0 || h == 0 || (file = fopen(path, "wb")) == NULL) {
			snprintf(why, sizeof why, "the picture cannot be kept in " FOLDER);
			printf("open: the picture of %s cannot be kept\n", address);
			return 0;
		}
		if (rgba) {
			/* TGA: uncompressed true colour, 32 bits a pixel with 8 of alpha, rows from the top */
			uint8_t head[18] = { 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8), 32, 0x28 };

			fwrite(head, 1, sizeof head, file);
		} else
			fprintf(file, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
		for (;;) {
			uint32_t here = w * h - done < FETCH_MOST / 4 ? w * h - done : FETCH_MOST / 4;

			uint8_t *to = rgb;

			if (rgba)
				for (i = 0; i < here; i++, to += 4) {
					uint32_t c = ((const uint32_t *)fetched)[i];

					to[0] = c >> 16, to[1] = c >> 8, to[2] = c, to[3] = c >> 24;	/* (TGA's order: blue first) */
				}
			else
				for (i = 0; i < here; i++, to += 3) {
					uint32_t c = ((const uint32_t *)fetched)[i];

					to[0] = c >> 16, to[1] = c >> 8, to[2] = c;
				}
			fwrite(rgb, rgba ? 4 : 3, here, file);
			done += here;
			progress_show(done, (unsigned long long)w * h, progress_megabytes(done * 3ull, w * h * 3ull));
			if (done >= w * h)
				break;
			status = ask(address, done * 4, 40);
			if (status != 200 || regs[REG_REPLY + 3] != size) {
				why_of(status);
				snprintf(why + strlen(why), sizeof why - strlen(why), ", after %u of %u pixels", (unsigned)done, (unsigned)(w * h));
				printf("open: the host stopped at %u of %u pixels (%d)\n", (unsigned)done, (unsigned)(w * h), status);
				fclose(file);
				return 0;
			}
		}
		fclose(file);
		printf("open: %s, %u x %u\n", path, (unsigned)w, (unsigned)h);
		return 1;
	}
	if (packed) {
		/* a file a picture carried: its own name, and a sum to check it by */
		char own[237];

		if (total < HEADER) {
			snprintf(why, sizeof why, "the file in the picture is cut short");
			return 0;
		}
		memcpy(&want_crc, fetched + 16, 4);
		memcpy(own, fetched + 20, 236);
		own[236] = 0;
		name_from(own, name, sizeof name);
		skip = HEADER;
	} else
		name_from(address, name, sizeof name);
	snprintf(path, room, FOLDER "/%s", name);
	if ((file = fopen(path, "wb")) == NULL) {
		snprintf(why, sizeof why, "%.150s cannot be written", path);
		printf("open: %s\n", why);
		return 0;
	}
	for (;;) {
		uint32_t here = total - got < FETCH_MOST ? total - got : FETCH_MOST;

		if (here > skip) {
			fwrite(fetched + skip, 1, here - skip, file);
			crc = crc32(fetched + skip, here - skip, crc);
		}
		skip = 0;
		got += here;
		progress_show(got, total, progress_megabytes(got, total));
		if (got >= total)
			break;
		status = ask(address, got, 40);
		if ((status != 200 && status != 201) || regs[REG_REPLY + 1] != total) {
			why_of(status);
			snprintf(why + strlen(why), sizeof why - strlen(why), ", after %s", progress_megabytes(got, total));
			printf("open: the host stopped at %u of %u bytes (%d)\n", (unsigned)got, (unsigned)total, status);
			fclose(file);
			return 0;
		}
	}
	{
		int failed = ferror(file);

		if (fclose(file) != 0 || failed) {
			printf("open: %s could not be written whole: %s\n", name, strerror(errno));
			snprintf(why, sizeof why, "it could not be written whole (%s)", strerror(errno));
			return 0;
		}
	}
	if (packed && crc != want_crc) {
		printf("open: %s arrived damaged (sum %08x, should be %08x): was the picture changed on its way?\n", name,
		       (unsigned)crc, (unsigned)want_crc);
		snprintf(why, sizeof why, "it arrived damaged (its sum is wrong): was the picture changed on its way?");
		return 0;
	}
	printf("open: %s, %u bytes%s, sum %08x\n", path, (unsigned)(total - (packed ? HEADER : 0)), packed ? " from a picture" : "",
	       (unsigned)crc);
	return 1;
}

/* A program run with its output (and its errors) to be read. */
static FILE *
reading(char *const argv[], pid_t *pid)
{
	int out[2];

	if (pipe(out) != 0 || (*pid = fork()) < 0)
		return NULL;
	if (*pid == 0) {
		dup2(out[1], 1);
		dup2(out[1], 2);
		close(out[0]);
		close(out[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(out[1]);
	return fdopen(out[0], "r");
}

static unsigned long long unpacked;	/* the bytes of the files in the folder an archive goes to */

static int
count_bytes(const char *name, const struct stat *info, int kind, struct FTW *where)
{
	(void)name, (void)where;
	if (kind == FTW_F)
		unpacked += (unsigned long long)info->st_size;
	return 0;
}

/* An archive unpacked into `to` (a .gz of one file: into the file `to`), with a window that
 * says how far: a zip by the bytes in the folder against its listing's, anything else by the bytes
 * it has been fed. */
static int
unpack(const char *kind, const char *path, const char *to)
{
	static char block[65536];
	char line[300], title[300];
	const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
	int feed[2], status = 1;
	struct stat info;
	pid_t pid;

	snprintf(title, sizeof title, "Unpacking %s", name);
	progress_begin(title);
	{
		/* unarc first, on the worker cores; busybox's unzip or tar for what it says it cannot (3) */
		char *const unarc_args[] = { "unarc", (char *)kind, (char *)path, (char *)to, NULL };
		FILE *said = reading(unarc_args, &pid);
		unsigned long long now = 0, all = 0;
		size_t have = 0;
		ssize_t n;

		if (said) {
			/* (its lines as they come, the window looked at every 200 ms: one stream is one long wait) */
			for (;;) {
				struct pollfd wait = { fileno(said), POLLIN, 0 };
				char *end;

				if (poll(&wait, 1, 200) > 0) {
					if ((n = read(fileno(said), line + have, sizeof line - 1 - have)) <= 0)
						break;
					have += (size_t)n;
					line[have] = 0;
					while ((end = strchr(line, '\n')) != NULL) {
						sscanf(line, "progress %llu %llu", &now, &all);
						have -= (size_t)(end + 1 - line);
						memmove(line, end + 1, have + 1);
					}
					if (have == sizeof line - 1)
						have = 0;
				}
				progress_show(now, all, progress_megabytes(now, all));
				if (progress_closed) {
					kill(pid, SIGTERM);
					break;
				}
			}
			fclose(said);
			waitpid(pid, &status, 0);
			if (progress_closed || (WIFEXITED(status) && WEXITSTATUS(status) != 3 && WEXITSTATUS(status) != 127)) {
				progress_end();
				return !progress_closed && WIFEXITED(status) && WEXITSTATUS(status) == 0;
			}
		}
	}
	if (!strcmp(kind, "zip")) {
		unsigned long long total = 0;
		char *const list_args[] = { "unzip", "-l", (char *)path, NULL };
		char *const unzip_args[] = { "unzip", "-o", (char *)path, "-d", (char *)to, NULL };
		FILE *list;

		/* (the listing's last line has the bytes of every file: "  12345   3 files") */
		if ((list = reading(list_args, &pid)) != NULL) {
			while (fgets(line, sizeof line, list))
				if (strstr(line, " file"))
					total = strtoull(line, NULL, 10);
			fclose(list);
			waitpid(pid, &status, 0);
		}
		if ((list = reading(unzip_args, &pid)) == NULL)
			return 0;
		/* its lines are read as they come, and the folder's bytes counted twice a second, whether
		 * a line came or not: one large file is one line, and a long wait */
		for (;;) {
			struct pollfd wait = { fileno(list), POLLIN, 0 };
			int ready = poll(&wait, 1, 500);

			if (ready > 0 && read(fileno(list), line, sizeof line) <= 0)
				break;
			unpacked = 0;
			nftw(to, count_bytes, 16, FTW_PHYS);
			progress_show(unpacked, total, progress_megabytes(unpacked, total));
			if (progress_closed) {
				kill(pid, SIGTERM);
				break;
			}
		}
		fclose(list);
		waitpid(pid, &status, 0);
		progress_end();
		return WIFEXITED(status) && WEXITSTATUS(status) == 0;
	}
	if (stat(path, &info) != 0 || pipe(feed) != 0)
		return 0;
	if ((pid = fork()) == 0) {
		dup2(feed[0], 0);
		close(feed[0]);
		close(feed[1]);
		if (!strcmp(kind, "gz")) {
			int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);

			if (out < 0)
				_exit(1);
			dup2(out, 1);
			execlp("gunzip", "gunzip", "-c", (char *)NULL);
		} else
			execlp("tar", "tar", !strcmp(kind, "tgz") ? "-xzf" : "-xf", "-", "-C", to, (char *)NULL);
		_exit(127);
	}
	close(feed[0]);
	if (pid > 0) {
		int from = open(path, O_RDONLY);
		unsigned long long fed = 0;
		ssize_t n;

		/* (fed as the pipe has room, the window looked at meanwhile: a slow unpacker held a
		   blocking write, and a close of the window waited for it) */
		fcntl(feed[1], F_SETFL, O_NONBLOCK);
		while (from >= 0 && !progress_closed && (n = read(from, block, sizeof block)) > 0) {
			ssize_t put = 0, w;

			while (put < n && !progress_closed) {
				struct pollfd room = { feed[1], POLLOUT, 0 };

				if (poll(&room, 1, 200) > 0) {
					if ((w = write(feed[1], block + put, (size_t)(n - put))) < 0 && errno != EAGAIN)
						break;
					put += w > 0 ? w : 0;
				}
				progress_show(fed + put, (unsigned long long)info.st_size,
					      progress_megabytes(fed + put, (unsigned long long)info.st_size));
			}
			if (put < n)
				break;
			fed += (unsigned long long)n;
		}
		if (progress_closed)
			kill(pid, SIGTERM);
		if (from >= 0)
			close(from);
		close(feed[1]);
		waitpid(pid, &status, 0);
	}
	progress_end();
	return pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* A copy of a file in /root, under its own name. */
static int
save(const char *path, const char *name)
{
	char to[300], block[4096];
	FILE *in = fopen(path, "rb"), *out;
	size_t n;
	int fine = 1;

	snprintf(to, sizeof to, "/root/%.280s", name);
	{
		/* it lies there already: opening it to write would empty what is being copied */
		struct stat a, b;

		if (stat(path, &a) == 0 && stat(to, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
			if (in)
				fclose(in);
			return 1;
		}
	}
	if (!in || (out = fopen(to, "wb")) == NULL) {
		if (in)
			fclose(in);
		return 0;
	}
	while ((n = fread(block, 1, sizeof block, in)) > 0)
		fine = fine && fwrite(block, 1, n, out) == n;
	fclose(in);
	return fclose(out) == 0 && fine;
}

/* Text between single quotes for the shell, whatever is in it. */
static int
quoted(char *to, int room, const char *text)
{
	int o = 0;

	if (room < 3)
		return 0;
	to[o++] = '\'';
	for (; *text && o + 6 < room; text++) {
		if (*text == '\'')
			o += snprintf(to + o, room - o, "'\\''");
		else
			to[o++] = *text;
	}
	to[o++] = '\'';
	to[o] = 0;
	return o;
}

static unsigned told_at;

static int
told_long(void)
{
	return progress_ms() - told_at > 30000;
}

/* Why `name` could not be opened, in a small window that goes at OK, Enter, Escape, its close
 * button or by itself after 30 s; nothing where no window system answers. */
static void
tell(const char *name, const char *reason)
{
	static const char *const ok[] = { "OK" };
	char text[400];

	if (GrOpen() < 0)
		return;
	ui_init();
	snprintf(text, sizeof text, "%.100s could not be opened:\n%.200s", name, reason);
	ui_ask_icon = "error";
	ui_ask_over = told_long;
	told_at = progress_ms();
	ui_ask("Open", text, ok, 1, NULL);
	GrClose();
}

/* A file nobody is registered for: what it is, and two things to do with it. */
static void
unknown(const char *path, long size)
{
	GR_WINDOW_ID window;
	GR_EVENT event;
	char line[300];
	const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;

	if (GrOpen() < 0) {
		printf("open: no program is registered for %s\n", path);
		return;
	}
	ui_init();
	window = GrNewWindowEx(UI_FIXED, "Open", GR_ROOT_WINDOW_ID, 60, 60, 420, 110, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(window);
	for (;;) {
		GrGetNextEvent(&event);
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ)
			break;
		if (event.type == GR_EVENT_TYPE_EXPOSURE) {
			snprintf(line, sizeof line, "%.200s: %ld bytes, and no program is registered for it.", name, size);
			ui_text_fit(window, 12, 14, 396, line, GR_RGB(0, 0, 0));
			ui_button(window, 12, 60, 150, 28, "Open as text", 0);
			ui_button(window, 174, 60, 150, 28, "Save to /root", 0);
		}
		if (event.type != GR_EVENT_TYPE_BUTTON_DOWN)
			continue;
		if (ui_inside(event.button.x, event.button.y, 12, 60, 150, 28)) {
			GrClose();
			/* (as a program of its own: started in this one's place, which had a window, the
			   editor's window got no frame) */
			if (fork() == 0) {
				execlp("nxedit", "nxedit", path, (char *)NULL);
				_exit(1);
			}
			return;
		}
		if (ui_inside(event.button.x, event.button.y, 174, 60, 150, 28)) {
			if (!save(path, name))
				printf("open: %s was not saved\n", path);
			break;
		}
	}
	GrClose();
}

int
main(int argc, char **argv)
{
	char path[300], command[1500], address[256] = "";
	unsigned char head[64];
	const struct kind *k;
	const char *what, *use;
	struct stat info;
	int dry = argc > 2 && !strcmp(argv[1], "--dry"), n, fd, i, o = 0;

	/* What this program's starter had open is not this program's. Its window system's socket, kept, held the window of a
	 * game that had gone until this ended (a program refused the GPU, and its Notice; a link: docs/system/fuzz.md). */
	for (i = 3; i < 64; i++)
		close(i);
	signal(SIGCHLD, SIG_DFL);	/* (the bar that starts this ignores it: the opened program must not) */

	if (argc == 5 && !strcmp(argv[1], "--unpack")) {
		n = unpack(argv[2], argv[3], argv[4]);
		if (progress_connected)
			GrClose();
		return n ? 0 : 1;
	}
	if (argc < 2 + dry) {
		fprintf(stderr, "usage: open [--dry] ADDRESS-OR-FILE, open --unpack zip|tar|tgz|gz FILE TO\n");
		return 2;
	}
	what = argv[1 + dry];
	/* the host's button for the kept folder comes as an address (docs/system/persist.md) */
	if (!strcmp(what, "persist:save") && !dry) {
		execlp("nxpersist", "nxpersist", "save", (char *)NULL);
		return 1;
	}
	read_kinds();
	if (strstr(what, "://")) {
		snprintf(address, sizeof address, "%s", what);
		n = fetch(address, path, sizeof path);
		progress_end();
		if (!n && !progress_closed) {
			name_from(address, path, 100);
			tell(path, why);
			return 1;
		}
		if (progress_connected)
			GrClose();	/* (not left open in the program run next) */
		if (!n)
			return 1;
	} else
		snprintf(path, sizeof path, "%s", what);
	if (stat(path, &info) != 0) {
		printf("open: there is no %s\n", path);
		if (!dry)
			tell(path, "there is no such file");
		return 1;
	}
	if (S_ISDIR(info.st_mode)) {
		k = NULL;
		o = snprintf(command, sizeof command, "nxfiles ");
		quoted(command + o, (int)sizeof command - o, path);
		goto run;
	}
	fd = open(path, O_RDONLY);
	n = fd < 0 ? 0 : (int)read(fd, head, sizeof head);
	if (fd >= 0)
		close(fd);
	if (n < 0)
		n = 0;
	k = kind_of(path, head, n);
	/* a program of another machine's is not run, and a script only if it says what runs it */
	if (n >= 20 && !memcmp(head, "\177ELF", 4) && !(head[4] == 1 && head[18] == 243 && head[19] == 0)) {
		printf("open: %s is a program, but not one for this machine (RISC-V, 32 bits)\n", path);
		if (!dry)
			tell(path, "it is a program, but not one for this machine (RISC-V, 32 bits)");
		return 1;
	}
	if (!k) {
		printf("open: %s: unknown kind\n", path);
		if (!dry)
			unknown(path, (long)info.st_size);
		return dry ? 0 : 1;
	}
	/* %s is the file; a page that came from an address is opened as that address, so that
	 * its links and pictures are found */
	use = address[0] && !strncmp(address, "http", 4) && !strcmp(k->name, "page") ? address : path;
	for (i = 0; k->command[i] && o < (int)sizeof command - 1; i++) {
		if (k->command[i] == '%' && k->command[i + 1] == 's') {
			o += quoted(command + o, (int)sizeof command - o, use);
			i++;
		} else
			command[o++] = k->command[i];
	}
	command[o < (int)sizeof command ? o : (int)sizeof command - 1] = 0;
run:
	printf("open: %s is %s: %s\n", path, k ? k->name : "a folder", command);
	fflush(stdout);
	if (dry)
		return 0;
	/* a line of plain words and the file is run as it is: a shell between is a program started
	 * for nothing. Anything a shell would read otherwise is the shell's still. */
	if (!k) {
		execlp("nxfiles", "nxfiles", path, (char *)NULL);
	} else if (!strpbrk(k->command, "'\"\\$|&;<>()*?[]~`#!{}=")) {
		static char line[sizeof k->command];
		char *words[12];

		strcpy(line, k->command);
		n = 0;
		for (words[n] = strtok(line, " \t"); words[n] && n < 10; words[n] = strtok(NULL, " \t")) {
			if (!strcmp(words[n], "%s"))
				words[n] = (char *)use;
			else if (strchr(words[n], '%'))
				break;
			n++;
		}
		if (!words[n] && n) {
			execvp(words[0], words);
			return 127;
		}
	}
	execl("/bin/sh", "sh", "-c", command, (char *)NULL);
	return 127;
}
