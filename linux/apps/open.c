/*
 * open [--dry] ADDRESS-OR-FILE: opens a file, or what an address holds, with the program
 * registered for its kind (docs/open.md). --dry prints the kind and the command only.
 */
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nano-X.h"
#include "ui.h"

#define FETCH_MOST 0x40000	/* bytes the host hands over at a time (docs/fetch.md) */
#define REG_ASKED 64		/* requests made, the address's length, the kind, the offset */
#define REG_REPLY 68		/* requests answered, bytes in all, status, a picture's size */
#define REG_ADDRESS 72
#define KIND_FILE 3
#define HEADER 256		/* of a file a picture carried: mark, length, CRC-32, name */
#define FOLDER "/tmp/open"
#define MAX_KINDS 32

static struct kind {
	char name[16], endings[96], magic[128], command[96];
} kinds[MAX_KINDS];
static int kind_count;
static volatile uint32_t *regs;
static const unsigned char *fetched;

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

/* The registry: every line of every /usr/share/nxopen.* file is "kind endings magic command". */
static void
read_kinds(void)
{
	struct dirent **names;
	int n = scandir("/usr/share", &names, NULL, alphasort), i;

	for (i = 0; i < n; i++) {
		char path[300], line[400];
		FILE *file;

		if (strncmp(names[i]->d_name, "nxopen.", 7) != 0)
			continue;
		snprintf(path, sizeof path, "/usr/share/%s", names[i]->d_name);
		if ((file = fopen(path, "r")) == NULL)
			continue;
		while (fgets(line, sizeof line, file) && kind_count < MAX_KINDS) {
			struct kind *k = &kinds[kind_count];
			int at = 0;

			if (line[0] == '#' || sscanf(line, "%15s %95s %127s %n", k->name, k->endings, k->magic, &at) < 3 || !at)
				continue;
			snprintf(k->command, sizeof k->command, "%s", line + at);
			k->command[strcspn(k->command, "\n")] = 0;
			if (k->command[0])
				kind_count++;
		}
		fclose(file);
	}
}

/* True if the bytes begin with one of a kind's marks: a list with commas, \xNN for a byte.
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
	int waited;
	size_t n = strlen(address);

	/* one request at a time: wait for another program's to be answered */
	for (waited = 0; regs[REG_ASKED] != regs[REG_REPLY] && waited < 200; waited++)
		usleep(50000);
	if (n > 255)
		n = 255;
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
		printf("open: this machine has no way to ask its host for a file\n");
		return 0;
	}
	/* (a world may not have been handed the address yet: asked again for a minute) */
	for (tries = 0; (status = ask(address, 0, 40)) == 1 && tries < 20; tries++) {
		if (!by_hand++)
			printf("open: the host has to be handed this address first\n");
		sleep(3);
	}
	if (status != 200 && status != 201) {
		printf("open: the host could not get %s (%d)\n", address, status);
		return 0;
	}
	total = regs[REG_REPLY + 1];
	packed = status == 201;
	mkdir(FOLDER, 0777);
	if (!packed && regs[REG_REPLY + 3] != 0) {
		/* a picture the host could only give as pixels: a word each, kept as a PPM file */
		uint32_t w = regs[REG_REPLY + 3] & 0xffff, h = regs[REG_REPLY + 3] >> 16, i;

		name_from(address, name, sizeof name - 4);
		snprintf(path, room, FOLDER "/%s.ppm", name);
		if ((uint64_t)w * h * 4 > FETCH_MOST || (file = fopen(path, "wb")) == NULL) {
			printf("open: the picture of %s cannot be kept\n", address);
			return 0;
		}
		fprintf(file, "P6\n%u %u\n255\n", (unsigned)w, (unsigned)h);
		for (i = 0; i < w * h; i++) {
			uint32_t c = ((const uint32_t *)fetched)[i];

			fputc(c >> 16 & 255, file);
			fputc(c >> 8 & 255, file);
			fputc(c & 255, file);
		}
		fclose(file);
		return 1;
	}
	if (packed) {
		/* a file a picture carried: its own name, and a sum to check it by */
		char own[237];

		if (total < HEADER)
			return 0;
		memcpy(&want_crc, fetched + 16, 4);
		memcpy(own, fetched + 20, 236);
		own[236] = 0;
		name_from(own, name, sizeof name);
		skip = HEADER;
	} else
		name_from(address, name, sizeof name);
	snprintf(path, room, FOLDER "/%s", name);
	if ((file = fopen(path, "wb")) == NULL) {
		printf("open: %s cannot be written\n", path);
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
		if (got >= total)
			break;
		status = ask(address, got, 40);
		if ((status != 200 && status != 201) || regs[REG_REPLY + 1] != total) {
			printf("open: the host stopped at %u of %u bytes (%d)\n", (unsigned)got, (unsigned)total, status);
			fclose(file);
			return 0;
		}
	}
	fclose(file);
	if (packed && crc != want_crc) {
		printf("open: %s arrived damaged (sum %08x, should be %08x): was the picture changed on its way?\n", name,
		       (unsigned)crc, (unsigned)want_crc);
		return 0;
	}
	printf("open: %s, %u bytes%s, sum %08x\n", path, (unsigned)(total - (packed ? HEADER : 0)), packed ? " from a picture" : "",
	       (unsigned)crc);
	return 1;
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
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Open", GR_ROOT_WINDOW_ID, 60, 60, 420, 110, UI_FACE);
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
			execlp("nxedit", "nxedit", path, (char *)NULL);
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

	if (argc < 2 + dry) {
		fprintf(stderr, "usage: open [--dry] ADDRESS-OR-FILE\n");
		return 2;
	}
	what = argv[1 + dry];
	read_kinds();
	if (strstr(what, "://")) {
		snprintf(address, sizeof address, "%s", what);
		if (!fetch(address, path, sizeof path))
			return 1;
	} else
		snprintf(path, sizeof path, "%s", what);
	if (stat(path, &info) != 0) {
		printf("open: there is no %s\n", path);
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
	execl("/bin/sh", "sh", "-c", command, (char *)NULL);
	return 127;
}
