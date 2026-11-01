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
#include "zlib.h"
#include "lzma.h"
#include "ui.h"
#include "ui_ask.h"

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
#define HEADER 16		/* "SEPD", a version (1 gzip, 2 xz), the packed bytes' count, their CRC-32 */
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
	/* fast first; LZMA only for a folder that does not fit, with less memory if it cannot have it,
	   and not for one twice too large: 1.2 MB of OpenTTD's map took 11 minutes at preset 6, to 146 KB */
	length = deflated(raw, n, packed + HEADER, room, 1);
	if (length == 0 ? n < 2 * MOST : HEADER + length > MOST && HEADER + length < 2 * MOST) {
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
	open_window("Decompressing persistence data...");
	if (memcmp(packed, "SEPD", 4) || (words[0] != 1 && words[0] != 2) || words[1] != total - HEADER || crc32(0, packed + HEADER, total - HEADER) != words[2] ||
	    size > RAW_MOST || (raw = malloc(size + 1)) == NULL) {
		say("The kept data is damaged: not unpacked.", 0);
		return 1;
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
	} else if (!strcmp(what, "off"))
		status = ask("power:off", 0, 3) != 200;	/* a host that can switches the machine off (nxoff) */
	else
		fprintf(stderr, "usage: nxpersist load | save | clear | auto 0|1 | state | name | off\n");
	if (window)
		GrClose();
	return status;
}
