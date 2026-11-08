/*
 * nxexport: a file or folder taken out of the machine as a photo VRChat's camera takes (a zip a PC opens), or as
 * text a visitor copies, and text pasted in front of the screen brought in again (docs/apps/export.md, docs/system/clipboard.md).
 *
 *   nxexport PATH ...           the window: a photo of a zip, or text (as it is, Zip, tar.gz, tar.xz and a level) in the
 *                               field the host shows in front of the screen
 *   nxexport --list FILE        the same for the paths FILE has, a line each (the file manager's selection)
 *   nxexport --import [FOLDER]  the field in front of the screen asked for text: what is pasted there is a file of
 *                               FOLDER (default /root/Desktop), and an archive is unpacked beside it
 *   nxexport --clipboard        the desktop's clipboard shown in that field
 *   nxexport --paste            the field's text put on the desktop's clipboard (the host's "To the machine")
 *
 * For tests, without a window:
 *   nxexport --pack as|zip|tgz|txz LEVEL OUT PATH ...   packs and prints "nxexport: packed ...: N bytes, sum S"
 *   nxexport --text OUT PATH ...                         the text a visitor copies (a data: address), as the window makes it
 *   nxexport --decode TEXTFILE FOLDER                    imports TEXTFILE's text as a paste would
 *   nxexport --photo PATH ...                            packs a zip and shows it as a photo (photo:show), as the window does
 *   NXEXPORT_PACK=zip|tgz|txz|as NXEXPORT_LEVEL=N NXEXPORT_TO=text|photo NXEXPORT_PRINT=1: the window's choices and what it does
 *
 * A photo (docs/apps/export.md, "A photo"): the world draws a zip over the whole picture of VRChat's camera while the
 * camera is inside the panel before the screen (Photo.shader), its bytes laid out as a 24-bit BMP's pixels are. Saved
 * from Paint as a 24-bit bitmap, the file renamed .zip is the zip: the BMP's 54-byte header, the zip's entries (their
 * offsets counted from the file's start), nothing, and its directory at the file's very end, whose one word that
 * depends on the photo's size (where the directory begins) the shader writes.
 *
 * The text is base64 alone by default, for a computer outside the world: `certutil -decode FILE NAME` or
 * `base64 -d FILE > NAME` makes the file; the field's parts are cut on groups of 4 letters with nothing
 * added, so joined in order they are the text. NXEXPORT_FORM=data makes a data: address instead,
 * "data:TYPE;name=NAME;base64,DATA". The window says, before packing, the most characters it can be, or
 * the camera's size the photo needs (measure, packed_most: bounds that hold whatever the files hold).
 */
#include <ctype.h>
#include <stdarg.h>
#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include "zlib.h"
#include "lzma.h"
#include "ui.h"
#include "ui_ask.h"
#include "host_ask.h"

#define WIDTH 560
#define HEIGHT 520
#define PATHS_MOST 256
#define TEXT_PART (HOST_DATA_MOST - 256)	/* what one showing of the field holds: more is shown in parts */
#define ARCHIVE "/tmp/nxexport.tar"

enum { AS_IS, ZIP, TGZ, TXZ };
static const char *const packing_names[] = { "As it is", "Zip", "tar.gz", "tar.xz" };
static const char *const packing_keys[] = { "as", "zip", "tgz", "txz" };
enum { TO_TEXT, TO_PHOTO };
#define BMP_HEAD 54	/* a 24-bit bitmap's header as Paint saves it: the photo's bytes begin after it */
enum { PLAIN, DATA };	/* the text: base64 alone (any computer decodes it as it is), or a data: address */
static const char *const form_names[] = { "Base64 only", "data: address" };

static char *paths[PATHS_MOST];
static unsigned zip_shift, zip_tail;	/* a photo's zip: offsets from the file's start; the directory and its end */
static unsigned char *photo;	/* what the world draws while it is shown: the entries, then the directory (16-byte aligned) */
static unsigned photo_entries, photo_tail;
static int photo_shown;
static int npaths, packing = ZIP, level = 9, target = TO_PHOTO, form = PLAIN, print, windowed;
static GR_WINDOW_ID window;
static char status[200];

/* what was made: the packed bytes, their name, and the text */
static unsigned char *packed;
static size_t packed_n;
static char packed_name[160];
static char *text;
static size_t text_n;
static int text_part, text_parts;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* What the paths hold, before packing (measure): for the bounds the window shows before Export. */
static struct {
	unsigned long long bytes, tar;	/* the files' bytes; what busybox's tar of them is at most */
	unsigned long long zip_heads;	/* a zip's headers of every entry */
	unsigned files, entries;
	int done;
} have;
static unsigned long long packed_so_far;	/* the files' bytes packed: the bar */
static int stopped;	/* Escape or the window closed while packing: the packing ends */

/* The bar inside the window, where its status line is (bar_x, bar_y, bar_w: draw and the import's window set them): what is
 * being done (bar_doing) and how far, drawn when it moves; the window's events looked at meanwhile (Escape stops, an
 * exposure draws the window again by bar_redraw). */
static int bar_x = 20, bar_y, bar_w = 480;
static char bar_doing[160];
static unsigned bar_drawn = ~0u, bar_at;
static void (*bar_redraw)(void);

static unsigned
ms_now(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned)now.tv_sec * 1000u + (unsigned)(now.tv_nsec / 1000000);
}

/* "1.2 of 12.0 MB" */
static const char *
megabytes(unsigned long long done, unsigned long long total)
{
	static char line[48];

	snprintf(line, sizeof line, "%llu.%llu of %llu.%llu MB", done >> 20, (done & 0xfffff) * 10 >> 20, total >> 20, (total & 0xfffff) * 10 >> 20);
	return line;
}

static void
bar_begin(const char *doing)
{
	snprintf(bar_doing, sizeof bar_doing, "%s", doing);
	bar_drawn = ~0u, bar_at = 0, stopped = 0;
}

/* The bar, in the window's programs only (a test's command line shows none): `done` of `total` (0: no share known), and a line. */
static void
bar(unsigned long long done, unsigned long long total, const char *line)
{
	GR_EVENT event;
	unsigned share = total ? (unsigned)(done * 1000 / total) : 0, now;
	char text[260];

	if (!windowed || !window)
		return;
	for (;;) {
		GrCheckNextEvent(&event);
		if (event.type == GR_EVENT_TYPE_NONE)
			break;
		if (event.type == GR_EVENT_TYPE_EXPOSURE) {
			if (bar_redraw)
				bar_redraw();
			bar_drawn = ~0u;
		}
		if (event.type == GR_EVENT_TYPE_CLOSE_REQ || (event.type == GR_EVENT_TYPE_KEY_DOWN && event.keystroke.ch == MWKEY_ESCAPE))
			stopped = 1;
	}
	now = ms_now();
	if (share == bar_drawn && now - bar_at < 500)
		return;
	if (bar_drawn != ~0u && now - bar_at < 100)
		return;
	snprintf(text, sizeof text, "%s: %s%s", bar_doing, line ? line : megabytes(done, total), stopped ? "" : "  (Esc stops)");
	ui_fill(window, bar_x, bar_y, bar_w, 30, UI_FACE);
	ui_text_fit(window, bar_x, bar_y, bar_w, text, BLACK);
	ui_bevel(window, bar_x, bar_y + 18, bar_w, 12, 1);
	ui_fill(window, bar_x + 1, bar_y + 19, bar_w - 2, 10, WHITE);
	if (share)
		ui_fill(window, bar_x + 1, bar_y + 19, (int)((bar_w - 2) * (unsigned long long)share / 1000), 10, UI_SELECTED);
	GrFlush();
	bar_drawn = share, bar_at = now;
}

static void
say(const char *format, ...)
{
	va_list a;

	va_start(a, format);
	vsnprintf(status, sizeof status, format, a);
	va_end(a);
	if (print || !window)
		printf("nxexport: %s\n", status), fflush(stdout);
}

static uint32_t
fnv(const unsigned char *p, size_t n)
{
	uint32_t h = 2166136261u;

	while (n--)
		h = (h ^ *p++) * 16777619u;
	return h;
}

static const char *
base_name(const char *path)
{
	const char *s = strrchr(path, '/');

	return s && s[1] ? s + 1 : path;
}

static int
is_folder(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* A whole file in memory (free it); NULL if it could not be read. */
static unsigned char *
read_whole(const char *path, size_t *n)
{
	FILE *f = fopen(path, "rb");
	unsigned char *p;
	long size;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);
	p = size >= 0 ? malloc(size + 1) : NULL;
	if (p && fread(p, 1, size, f) != (size_t)size)
		free(p), p = NULL;
	fclose(f);
	*n = p ? (size_t)size : 0;
	return p;
}

/* ---- packing ---- */

struct grow {
	unsigned char *p;
	size_t n, room;
	int failed;
};

static void
put(struct grow *g, const void *data, size_t n)
{
	if (g->failed)
		return;
	if (g->n + n > g->room) {
		size_t room = (g->n + n) * 3 / 2 + 4096;
		unsigned char *p = realloc(g->p, room);

		if (!p) {
			g->failed = 1;
			return;
		}
		g->p = p, g->room = room;
	}
	memcpy(g->p + g->n, data, n);
	g->n += n;
}

static void
put16(struct grow *g, unsigned v)
{
	unsigned char b[2] = { v & 0xff, (v >> 8) & 0xff };

	put(g, b, 2);
}

static void
put32(struct grow *g, uint32_t v)
{
	unsigned char b[4] = { v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24 };

	put(g, b, 4);
}

/* The zip being made: its entries' central records kept apart until the end. */
static struct grow zip_out, zip_central;
static int zip_entries, zip_level;
static const char *zip_base;	/* the part of each path before its name in the archive */

static void
dos_time(time_t t, unsigned *time16, unsigned *date16)
{
	struct tm tm;

	if (t < 315532800)	/* (the ROM keeps no dates: a zip's earliest, 1 January 1980) */
		t = 315532800;
	gmtime_r(&t, &tm);
	*time16 = (unsigned)(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
	*date16 = (unsigned)((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}

/* A raw deflate stream of n bytes at `level`; its length, or 0 if it came out no smaller. */
static size_t
deflate_raw(const unsigned char *in, size_t n, unsigned char *out, size_t room, int lvl)
{
	z_stream z;
	size_t made;

	memset(&z, 0, sizeof z);
	if (deflateInit2(&z, lvl, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY) != Z_OK)
		return 0;
	z.next_out = out, z.avail_out = (uInt)room;
	{	/* (in pieces of 64 KB: the bar moves) */
		size_t fed = 0;
		int r = Z_OK;

		while (r == Z_OK && !stopped) {
			size_t here = n - fed < 0x10000 ? n - fed : 0x10000;

			z.next_in = (unsigned char *)in + fed, z.avail_in = (uInt)here, fed += here;
			r = deflate(&z, fed == n ? Z_FINISH : Z_NO_FLUSH);
			if (r == Z_OK && z.avail_out == 0)
				r = Z_BUF_ERROR;
			bar(packed_so_far + fed, have.bytes, NULL);
		}
		made = r == Z_STREAM_END ? z.total_out : 0;
	}
	deflateEnd(&z);
	return made < n ? made : 0;
}

static void
zip_entry(const char *path, const struct stat *st, int folder)
{
	const char *name = path + strlen(zip_base);
	unsigned char *data = NULL, *small = NULL;
	size_t n = 0, csize = 0, nlen;
	unsigned t16, d16, method = 0;
	uint32_t crc = 0, offset = (uint32_t)zip_out.n + zip_shift;
	char named[512];

	while (*name == '/')
		name++;
	snprintf(named, sizeof named, "%s%s", name, folder ? "/" : "");
	nlen = strlen(named);
	if (!folder) {
		if (!(data = read_whole(path, &n))) {
			zip_out.failed = 1;
			return;
		}
		crc = (uint32_t)crc32(0, data, (uInt)n);
		csize = n;
		if (zip_level > 0 && n > 0 && (small = malloc(n))) {
			size_t made = deflate_raw(data, n, small, n, zip_level);

			if (made)
				method = 8, csize = made;
		}
	}
	dos_time(st->st_mtime, &t16, &d16);
	put32(&zip_out, 0x04034b50), put16(&zip_out, 20), put16(&zip_out, 0x0800), put16(&zip_out, method);
	put16(&zip_out, t16), put16(&zip_out, d16), put32(&zip_out, crc), put32(&zip_out, (uint32_t)csize), put32(&zip_out, (uint32_t)n);
	put16(&zip_out, (unsigned)nlen), put16(&zip_out, 0), put(&zip_out, named, nlen);
	if (!folder)
		put(&zip_out, method ? small : data, csize);
	put32(&zip_central, 0x02014b50), put16(&zip_central, 0x031e), put16(&zip_central, 20), put16(&zip_central, 0x0800);
	put16(&zip_central, method), put16(&zip_central, t16), put16(&zip_central, d16), put32(&zip_central, crc);
	put32(&zip_central, (uint32_t)csize), put32(&zip_central, (uint32_t)n), put16(&zip_central, (unsigned)nlen);
	put16(&zip_central, 0), put16(&zip_central, 0), put16(&zip_central, 0), put16(&zip_central, folder ? 0x10 : 0);
	put32(&zip_central, (uint32_t)(st->st_mode & 0xffff) << 16 | (folder ? 0x10 : 0)), put32(&zip_central, offset);
	put(&zip_central, named, nlen);
	zip_entries++;
	packed_so_far += n;
	bar(packed_so_far, have.bytes, NULL);
	if (stopped)
		zip_out.failed = 1;
	free(data);
	free(small);
}

static int
zip_walked(const char *path, const struct stat *st, int kind, struct FTW *at)
{
	(void)at;
	if (kind == FTW_F || kind == FTW_D)
		zip_entry(path, st, kind == FTW_D);
	return zip_out.failed;
}

/* The folder all the paths are in (the parts of their names left out of the archive). */
static void
common_parent(char *to, size_t room)
{
	int i;
	const char *s;

	snprintf(to, room, "%s", paths[0]);
	s = strrchr(to, '/');
	if (s)
		to[s - to] = 0;
	for (i = 1; i < npaths; i++)
		if (strncmp(paths[i], to, strlen(to)) || paths[i][strlen(to)] != '/') {
			to[0] = 0;	/* (from different folders: whole paths) */
			break;
		}
}

static int
pack_zip(struct grow *out)
{
	char base[512];
	int i;
	struct stat st;

	common_parent(base, sizeof base);
	memset(&zip_out, 0, sizeof zip_out);
	memset(&zip_central, 0, sizeof zip_central);
	zip_entries = 0, zip_level = level, zip_base = base;
	for (i = 0; i < npaths && !zip_out.failed; i++) {
		if (is_folder(paths[i]))
			nftw(paths[i], zip_walked, 16, FTW_PHYS);
		else if (stat(paths[i], &st) == 0)
			zip_entry(paths[i], &st, 0);
		else
			zip_out.failed = 1;
	}
	{
		uint32_t at = (uint32_t)zip_out.n, size = (uint32_t)zip_central.n;

		put(&zip_out, zip_central.p, zip_central.n);
		put32(&zip_out, 0x06054b50), put16(&zip_out, 0), put16(&zip_out, 0), put16(&zip_out, (unsigned)zip_entries);
		/* (a photo's directory begins where the photo's size puts it: the world writes that word) */
		put16(&zip_out, (unsigned)zip_entries), put32(&zip_out, size), put32(&zip_out, zip_shift ? 0 : at), put16(&zip_out, 0);
		zip_tail = size + 22;
	}
	free(zip_central.p);
	*out = zip_out;
	return !out->failed;
}

/* tar of the paths, by busybox's tar into ARCHIVE; true if it ended well. */
static int
tar_paths(void)
{
	char base[512];
	char *args[PATHS_MOST + 8];
	int i, n = 0, status = -1;
	pid_t pid;

	common_parent(base, sizeof base);
	args[n++] = "tar", args[n++] = "-C", args[n++] = base[0] ? base : "/", args[n++] = "-cf", args[n++] = ARCHIVE;
	for (i = 0; i < npaths; i++)
		args[n++] = paths[i] + (base[0] ? strlen(base) + 1 : 1);
	args[n] = NULL;
	if ((pid = real_vfork()) == 0) {
		execvp("tar", args);
		_exit(127);
	}
	if (pid <= 0)
		return 0;
	for (;;) {	/* (the archive's size against what it is at most: the bar) */
		struct stat st;
		pid_t got = waitpid(pid, &status, windowed ? WNOHANG : 0);

		if (got == pid)
			break;
		if (got < 0)
			return 0;
		bar(stat(ARCHIVE, &st) == 0 ? (unsigned long long)st.st_size : 0, have.tar, "Gathering the files");
		if (stopped) {
			kill(pid, SIGTERM);
			waitpid(pid, &status, 0);
			return 0;
		}
		usleep(100000);
	}
	return status == 0;
}

static int
pack_tgz(struct grow *out, const unsigned char *raw, size_t n)
{
	z_stream z;
	int done;

	memset(&z, 0, sizeof z);
	memset(out, 0, sizeof *out);
	out->room = n + n / 8 + 1024, out->p = malloc(out->room);
	if (!out->p || deflateInit2(&z, level < 1 ? 1 : level, Z_DEFLATED, 31, 9, Z_DEFAULT_STRATEGY) != Z_OK)
		return 0;
	z.next_out = out->p, z.avail_out = (uInt)out->room;
	{
		size_t fed = 0;
		int r = Z_OK;

		while (r == Z_OK && !stopped) {
			size_t here = n - fed < 0x10000 ? n - fed : 0x10000;

			z.next_in = (unsigned char *)raw + fed, z.avail_in = (uInt)here, fed += here;
			r = deflate(&z, fed == n ? Z_FINISH : Z_NO_FLUSH);
			bar(fed, n, NULL);
		}
		done = r == Z_STREAM_END;
	}
	out->n = z.total_out;
	deflateEnd(&z);
	return done;
}

/* An .xz file at preset `level`, its dictionary no larger than what it packs (xz's own preset 9 asks 674 MB). */
static int
pack_txz(struct grow *out, const unsigned char *raw, size_t n)
{
	lzma_stream z = LZMA_STREAM_INIT;
	lzma_options_lzma o;
	lzma_filter f[2];
	lzma_ret r = LZMA_OK;
	uint32_t dict;

	memset(out, 0, sizeof *out);
	out->room = n + n / 8 + 4096, out->p = malloc(out->room);
	if (!out->p || lzma_lzma_preset(&o, (uint32_t)(level < 0 ? 0 : level > 9 ? 9 : level)))
		return 0;
	dict = n < 4096 ? 4096 : n > 0x800000 ? 0x800000 : (uint32_t)n;
	if (o.dict_size > dict)
		o.dict_size = dict;
	f[0].id = LZMA_FILTER_LZMA2, f[0].options = &o;
	f[1].id = LZMA_VLI_UNKNOWN, f[1].options = NULL;
	if (lzma_stream_encoder(&z, f, LZMA_CHECK_CRC32) != LZMA_OK)
		return 0;
	z.next_out = out->p, z.avail_out = out->room;
	{
		size_t fed = 0;

		while (r == LZMA_OK && !stopped) {	/* (in pieces of 64 KB: the bar) */
			size_t here = n - fed < 0x10000 ? n - fed : 0x10000;

			z.next_in = raw + fed, z.avail_in = here, fed += here;
			do
				r = lzma_code(&z, fed == n ? LZMA_FINISH : LZMA_RUN);
			while (r == LZMA_OK && z.avail_in);
			bar(fed, n, NULL);
		}
	}
	out->n = z.total_out;
	lzma_end(&z);
	return r == LZMA_STREAM_END;
}

static const char *
type_of(void)
{
	static const char *const types[] = { "application/octet-stream", "application/zip", "application/gzip", "application/x-xz" };

	if (packing == AS_IS) {
		size_t i, plain = 0;

		for (i = 0; i < packed_n; i++)
			plain += packed[i] >= 32 || packed[i] == '\n' || packed[i] == '\r' || packed[i] == '\t';
		return packed_n && plain == packed_n ? "text/plain" : types[0];
	}
	return types[packing];
}

static const char base64_letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* The name as an address may have it: what is not a letter, digit or one of a few signs as %XX. */
static void
escaped(const char *from, char *name, size_t room)
{
	size_t i, h;

	for (i = h = 0; from[i] && h < room - 4; i++) {
		unsigned char c = (unsigned char)from[i];

		if (isalnum(c) || strchr("._-()", c))
			name[h++] = (char)c;
		else
			h += (size_t)snprintf(name + h, room - h, "%%%02X", c);
	}
	name[h] = 0;
}

/* The text: base64 alone, or "data:TYPE;name=NAME;base64," and the bytes. */
static int
make_text(void)
{
	char head[300], name[200];
	size_t i, h, at;
	const unsigned char *p = packed;

	escaped(packed_name, name, sizeof name);
	h = form == DATA ? (size_t)snprintf(head, sizeof head, "data:%s;name=%s;base64,", type_of(), name) : 0;
	free(text);
	text_n = h + (packed_n + 2) / 3 * 4;
	if (!(text = malloc(text_n + 1)))
		return 0;
	memcpy(text, head, h);
	for (i = 0, at = h; i + 2 < packed_n; i += 3, at += 4) {
		uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];

		text[at] = base64_letters[v >> 18], text[at + 1] = base64_letters[(v >> 12) & 63];
		text[at + 2] = base64_letters[(v >> 6) & 63], text[at + 3] = base64_letters[v & 63];
	}
	if (i < packed_n) {
		uint32_t v = (uint32_t)p[i] << 16 | (i + 1 < packed_n ? (uint32_t)p[i + 1] << 8 : 0);

		text[at] = base64_letters[v >> 18], text[at + 1] = base64_letters[(v >> 12) & 63];
		text[at + 2] = i + 1 < packed_n ? base64_letters[(v >> 6) & 63] : '=', text[at + 3] = '=';
	}
	text[text_n] = 0;
	return 1;
}

/* What packing `pk` names its file. */
static void
name_of(int pk, char *to, size_t room)
{
	if (pk == AS_IS)
		snprintf(to, room, "%s", base_name(paths[0]));
	else
		snprintf(to, room, "%s.%s", npaths > 1 ? "exported" : base_name(paths[0]), pk == ZIP ? "zip" : pk == TGZ ? "tar.gz" : "tar.xz");
}

static size_t measure_base;

static unsigned long long
in_512(unsigned long long n)
{
	return (n + 511) & ~511ull;
}

/* An entry's share of the bounds: its name in the archive (a folder's with its "/"). */
static int
measured(const char *path, const struct stat *st, int kind, struct FTW *at)
{
	size_t name = strlen(path) > measure_base ? strlen(path) - measure_base : strlen(path);

	(void)at;
	if (kind == FTW_D)
		name++;
	have.entries++;
	have.tar += 512 + (name > 99 ? 512 + in_512(name + 1) : 0);	/* (a long name: GNU's entry before it) */
	if (kind == FTW_F) {
		have.files++, have.bytes += (unsigned long long)st->st_size;
		have.tar += in_512((unsigned long long)st->st_size);
	} else if (kind != FTW_D)
		have.tar += 512 + 1024;	/* (a link: its target's name) */
	if (kind == FTW_F || kind == FTW_D)
		have.zip_heads += 30 + 46 + 2 * (unsigned long long)name;
	return 0;
}

/* The paths walked once: what they hold, and the parts of the bounds no choice changes. */
static void
measure(void)
{
	char base[512];
	int i;
	struct stat st;

	memset(&have, 0, sizeof have);
	common_parent(base, sizeof base);
	measure_base = base[0] ? strlen(base) + 1 : 1;
	for (i = 0; i < npaths; i++)
		if (is_folder(paths[i]))
			nftw(paths[i], measured, 16, FTW_PHYS);
		else if (lstat(paths[i], &st) == 0)
			measured(paths[i], &st, S_ISREG(st.st_mode) ? FTW_F : FTW_SL, NULL);
	have.tar = (have.tar + 1024 + 10239) / 10240 * 10240;	/* (the end's two blocks, and a whole record) */
	have.done = 1;
}

/* The most bytes packing `pk` can make of the paths, whatever they hold: zlib's and liblzma's own bounds. */
static unsigned long long
packed_most(int pk)
{
	unsigned long long t = have.tar;

	if (pk == AS_IS)
		return have.bytes;
	if (pk == ZIP)	/* (a file deflate makes no smaller is stored) */
		return have.bytes + have.zip_heads + 22;
	if (pk == TGZ)	/* (deflateBound's for any level and memory, and gzip's head and tail) */
		return t + ((t + 7) >> 3) + ((t + 63) >> 6) + 5 + 18;
	return lzma_stream_buffer_bound((size_t)t) + 64;
}

/* The text's characters for `bytes` bytes of packing `pk`, in the chosen form. */
static unsigned long long
text_chars(int pk, unsigned long long bytes)
{
	char name[160], esc[200];

	if (form == PLAIN)
		return (bytes + 2) / 3 * 4;
	name_of(pk, name, sizeof name);
	escaped(name, esc, sizeof esc);
	return strlen("data:application/octet-stream;name=;base64,") + strlen(esc) + (bytes + 2) / 3 * 4;
}

/* Packs the paths as chosen into `packed`; false (and the status says why) if it could not. */
static int
pack(void)
{
	struct grow out = { 0 };
	unsigned char *raw;
	size_t n;
	int ok;

	free(packed), packed = NULL, packed_n = 0;
	if (npaths == 0) {
		say("Nothing to export.");
		return 0;
	}
	if (packing == AS_IS && (npaths > 1 || is_folder(paths[0])))
		packing = ZIP;	/* (a folder or many files cannot go as one file is) */
	if (target == TO_PHOTO)
		packing = ZIP;	/* (a photo is a zip: only a zip's reader takes the bitmap's header before it) */
	zip_shift = target == TO_PHOTO ? BMP_HEAD : 0;
	if (packing == AS_IS) {
		if (!(packed = read_whole(paths[0], &packed_n))) {
			say("%s could not be read.", base_name(paths[0]));
			return 0;
		}
		snprintf(packed_name, sizeof packed_name, "%s", base_name(paths[0]));
		return 1;
	}
	name_of(packing, packed_name, sizeof packed_name);
	packed_so_far = 0, stopped = 0;
	if (windowed) {
		char title[200];

		snprintf(title, sizeof title, "Packing %s", packed_name);
		bar_begin(title);
	}
	if (packing == ZIP)
		ok = pack_zip(&out);
	else {
		ok = tar_paths() && (raw = read_whole(ARCHIVE, &n)) != NULL;
		unlink(ARCHIVE);
		if (ok) {
			ok = packing == TGZ ? pack_tgz(&out, raw, n) : pack_txz(&out, raw, n);
			free(raw);
		}
	}
	if (!ok) {
		free(out.p);
		if (stopped)
			say("Stopped: nothing was exported.");
		else
			say("Could not be packed: %s.", out.failed ? "no memory, or a file could not be read" : "no memory");
		return 0;
	}
	packed = out.p, packed_n = out.n;
	return 1;
}

/* ---- the host's field ---- */

/* The text from `from`, n bytes (at most TEXT_PART), shown in the field in front of the screen: the host's status. */
static int
show_in_field(const char *what, const char *from, size_t n)
{
	if (!host_open() || !host_free())
		return 0;
	memcpy(host_data, what, strlen(what) + 1 < 256 ? strlen(what) + 1 : 256);	/* (a line the field shows above the text) */
	memcpy(host_data + 256, from, n);
	return host_ask("clip:show", (uint32_t)(256 + n), 30);
}

static void
show_text_part(void)
{
	char what[256];
	size_t from = (size_t)text_part * TEXT_PART, n = text_n - from < TEXT_PART ? text_n - from : TEXT_PART;
	int answer;

	if (form == PLAIN)
		snprintf(what, sizeof what, "%s, %zu bytes%s. On a PC: paste it into Notepad, save as data.txt, then in a command prompt: "
			 "certutil -decode data.txt \"%s\"", packed_name, packed_n, text_parts > 1 ? " (every part, in order)" : "", packed_name);
	else
		snprintf(what, sizeof what, "%s, %zu bytes%s: paste it into a browser's address bar to save the file", packed_name, packed_n,
			 text_parts > 1 ? ", parts joined in order" : "");
	if (text_parts > 1) {
		size_t k = strlen(what);

		snprintf(what + k, sizeof what - k, " (part %d of %d)", text_part + 1, text_parts);
	}
	answer = show_in_field(what, text + from, n);
	if (answer == 200)
		say("%s is in the field in front of the screen: %zu characters%s.", packed_name, n,
		    text_parts > 1 ? ", part of it: Next for the rest" : "");
	else
		say(answer == 0 ? "The host did not answer: this machine shows no text field." :
		    "This host shows no text field (answer %d): the text is on the clipboard.", answer);
}

/* ---- a photo ---- */

/* The world asked to draw the packed zip into VRChat's camera's pictures; the host's answer. */
static int
photo_show(void)
{
	char address[256];
	unsigned n = (unsigned)packed_n;

	free(photo), photo = NULL;
	if (n < zip_tail || !host_open() || !(photo = aligned_alloc(16, (n + 15) & ~15u)))
		return 0;
	memcpy(photo, packed, n);
	photo_entries = n - zip_tail, photo_tail = zip_tail;
	/* (the bytes' first texel of RAM, counted from 0x80000000 as the state texture has them; a name for the panel) */
	snprintf(address, sizeof address, "photo:show=%u,%u,%u,%s", (unsigned)(((uintptr_t)photo - 0x80000000u) / 16), photo_entries,
		 photo_tail, packed_name);
	photo_shown = host_ask(address, 0, 20) == 200;
	return photo_shown ? 200 : 0;
}

static void
photo_hide(void)
{
	if (photo_shown)
		host_ask("photo:hide", 0, 10);
	photo_shown = 0;
}

/* The smallest of VRChat's camera's sizes a photo of `bytes` bytes fits (the bitmap's pixels), or 0. */
static const char *
photo_size(unsigned long long bytes)
{
	static const unsigned sizes[][2] = { { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
	static char text[40];
	int i;

	for (i = 0; i < 3; i++)
		if (BMP_HEAD + bytes <= BMP_HEAD + 3ull * sizes[i][0] * sizes[i][1]) {
			snprintf(text, sizeof text, "%u x %u", sizes[i][0], sizes[i][1]);
			return text;
		}
	return NULL;
}

/* ---- the window ---- */

struct key {
	int x, y, w, h, id;
	const char *label;
	int down;
};
static struct key keys[24];
static int nkeys;
enum { K_PACK = 1, K_LEVEL = 10, K_TEXT = 20, K_PHOTO, K_FORM = 25, K_GO = 40, K_CLOSE };

static void
key(int x, int y, int w, int h, int id, const char *label, int down)
{
	if (nkeys < (int)(sizeof keys / sizeof keys[0]))
		keys[nkeys++] = (struct key){ x, y, w, h, id, label, down };
	ui_button(window, x, y, w, h, label, down);
}

/* 1234567 as "1,234,567". */
static const char *
grouped(unsigned long long v, char *to)
{
	char d[32];
	int n = snprintf(d, sizeof d, "%llu", v), i, k = 0;

	for (i = 0; i < n; i++) {
		if (i && (n - i) % 3 == 0)
			to[k++] = ',';
		to[k++] = d[i];
	}
	to[k] = 0;
	return to;
}

static void
draw(void)
{
	char line[200];
	int i, y = 14;

	nkeys = 0;
	ui_fill(window, 0, 0, WIDTH, HEIGHT, UI_FACE);
	if (npaths == 1)
		snprintf(line, sizeof line, "Export %s%s", base_name(paths[0]), is_folder(paths[0]) ? " (a folder)" : "");
	else
		snprintf(line, sizeof line, "Export %d entries of %s", npaths, npaths ? paths[0] : "");
	ui_text(window, 20, y, line, -1, BLACK, 0);
	y += 34;
	ui_text(window, 20, y + 6, "Packed as:", -1, BLACK, 0);
	if (target == TO_PHOTO)	/* (a photo is a zip: only a zip's reader takes the bitmap's header before it) */
		ui_text(window, 120, y + 6, "Zip (a photo is always a zip)", -1, BLACK, 0);
	else
		for (i = 0; i < 4; i++)
			key(120 + i * 104, y, 96, 26, K_PACK + i, packing_names[i], packing == i);
	y += 40;
	snprintf(line, sizeof line, "Level: %d", level);
	ui_text(window, 20, y + 3, packing == AS_IS ? "Level: -" : line, -1, BLACK, 0);
	ui_slider(window, 120, y, 300, level, 9);
	keys[nkeys++] = (struct key){ 120, y, 300, 18, K_LEVEL, NULL, 0 };
	ui_text(window, 430, y + 3, packing == ZIP || packing == TGZ ? "(0 to 9: deflate)" : packing == TXZ ? "(LZMA: smallest)" : "", -1,
		UI_SHADOW, 0);
	y += 34;
	ui_text(window, 20, y + 6, "To:", -1, BLACK, 0);
	key(120, y, 140, 26, K_PHOTO, "Photo (a zip)", target == TO_PHOTO);
	key(268, y, 140, 26, K_TEXT, "Text (base64)", target == TO_TEXT);
	y += 34;
	if (target == TO_TEXT) {
		ui_text(window, 20, y + 6, "Text as:", -1, BLACK, 0);
		for (i = 0; i < 2; i++)
			key(120 + i * 148, y, 140, 26, K_FORM + i, form_names[i], form == i);
	}
	y += 44;
	if (have.done) {	/* (what it will be at most, before anything is packed) */
		unsigned long long most = packed_most(packing), chars = text_chars(packing, most);
		char n1[32], n2[32];

		if (target == TO_PHOTO)
			snprintf(line, sizeof line, "%s bytes in %u file%s.", grouped(have.bytes, n1), have.files, have.files == 1 ? "" : "s");
		else
			snprintf(line, sizeof line, "%s bytes in %u file%s: %s %s characters%s.", grouped(have.bytes, n1), have.files,
				 have.files == 1 ? "" : "s", packing == AS_IS ? "the text is" : "at most", grouped(chars, n2), packing == AS_IS ? "" : " before packing");
		ui_text_fit(window, 20, y, WIDTH - 40, line, BLACK);
		y += 18;
		if (target == TO_PHOTO) {
			const char *size = photo_size(packed_most(ZIP));

			if (size)
				snprintf(line, sizeof line, "A zip of it fits a photo of %s or larger, whatever it packs to.", size);
			else
				snprintf(line, sizeof line, "A zip of it may need more than a 3840 x 2160 photo (%s bytes): it fits if it packs smaller.",
					 grouped(packed_most(ZIP), n1));
		} else {
			unsigned long long k = (chars + TEXT_PART - 1) / TEXT_PART;

			if (k <= 1)
				snprintf(line, sizeof line, "One showing of the field holds it (%s characters), whatever it packs to.", grouped(TEXT_PART, n1));
			else
				snprintf(line, sizeof line, "Up to %s showings of the field (%s characters each)%s.", grouped(k, n1), grouped(TEXT_PART, n2),
					 packing == AS_IS ? "" : ": fewer once packed");
		}
		ui_text_fit(window, 20, y, WIDTH - 40, line, BLACK);
	}
	y += 30;
	key(20, y, 140, 30, K_GO, target == TO_PHOTO ? (photo_shown ? "Stop the photo" : "Show the photo") : text_parts > 1 && text_part + 1 < text_parts ? "Next part" :
	    "Export", 0);
	key(WIDTH - 120, y, 100, 30, K_CLOSE, "Close", 0);
	y += 46;
	bar_x = 20, bar_y = y, bar_w = WIDTH - 40;	/* (a packing's bar is drawn over the status line) */
	ui_text_fit(window, 20, y, WIDTH - 40, status, BLACK);
	y += 30;
	if (target == TO_PHOTO) {
		ui_text(window, 20, y, "Hold VRChat's camera inside the glowing cube before the screen and", -1, UI_SHADOW, 0);
		ui_text(window, 20, y + 18, "take a picture (a PNG in Pictures\\VRChat). On the PC open it in Paint,", -1, UI_SHADOW, 0);
		ui_text(window, 20, y + 36, "Save as, BMP picture (never JPEG); rename the .bmp to .zip and open it.", -1, UI_SHADOW, 0);
		ui_text(window, 20, y + 54, "A link to that PNG opened here gives the zip back too.", -1, UI_SHADOW, 0);
	} else if (form == PLAIN) {
		ui_text(window, 20, y, "On a PC: click the field, Ctrl+A, Ctrl+C; paste into Notepad and save", -1, UI_SHADOW, 0);
		ui_text(window, 20, y + 18, "it as data.txt (every part, in order); then in a command prompt:", -1, UI_SHADOW, 0);
		ui_text(window, 20, y + 36, "certutil -decode data.txt NAME   (Linux, macOS: base64 -d data.txt > NAME)", -1, UI_SHADOW, 0);
	} else {
		ui_text(window, 20, y, "A data: address: pasted into a browser's address bar it saves the", -1, UI_SHADOW, 0);
		ui_text(window, 20, y + 18, "file (the name is kept only by this machine's Import); any base64", -1, UI_SHADOW, 0);
		ui_text(window, 20, y + 36, "decoder takes what follows the comma.", -1, UI_SHADOW, 0);
	}
}

static void
go(void)
{
	if (target == TO_TEXT && text && text_parts > 1 && text_part + 1 < text_parts) {
		text_part++;	/* (the next part of what was made) */
		show_text_part();
		return;
	}
	if (target == TO_PHOTO && photo_shown) {
		photo_hide();
		say("The photo is no longer drawn.");
		return;
	}
	say("Packing...");
	draw();
	GrFlush();
	if (target == TO_PHOTO) {
		int answer;

		if (!pack())
			return;
		answer = photo_show();
		if (answer == 200)
			say("%s (%zu bytes) is drawn into the camera's pictures inside the panel: take one.", packed_name, packed_n);
		else
			say(host_open() ? "This host draws no photo (VRChat's world does): nothing shown." : "This machine has no way to ask its host.");
		return;
	}
	if (!pack() || !make_text()) {
		if (!status[0] || !strcmp(status, "Packing..."))
			say("No memory for the text.");
		return;
	}
	if (packing != AS_IS && have.done && text_n > text_chars(packing, packed_most(packing)))
		say("The bound was wrong: %zu characters made.", text_n);	/* (never: a test reads it) */
	ui_clip_set(text, (int)text_n);	/* (and on the clipboard: another program can paste it) */
	text_parts = (int)((text_n + TEXT_PART - 1) / TEXT_PART), text_part = 0;
	show_text_part();
}

static void
clicked(int px, int py)
{
	int i;

	for (i = 0; i < nkeys; i++) {
		struct key *k = &keys[i];

		if (!ui_inside(px, py, k->x, k->y, k->w, k->h))
			continue;
		if (k->id >= K_PACK && k->id < K_PACK + 4)
			packing = k->id - K_PACK, text_parts = 0;
		else if (k->id == K_LEVEL)
			level = ui_slider_value(px, k->x, k->w, 9), text_parts = 0;
		else if (k->id == K_TEXT || k->id == K_PHOTO) {
			photo_hide();
			target = k->id == K_PHOTO ? TO_PHOTO : TO_TEXT, text_parts = 0;
		}
		else if (k->id == K_FORM || k->id == K_FORM + 1)
			form = k->id - K_FORM, text_parts = 0;
		else if (k->id == K_GO)
			go();
		else if (k->id == K_CLOSE) {
			photo_hide();
			GrClose();
			exit(0);
		}
		draw();
		return;
	}
}

static void
key_down(int ch)
{
	if (ch == MWKEY_ENTER)
		go(), draw();
	else if (ch == MWKEY_ESCAPE) {
		photo_hide();
		GrClose();
		exit(0);
	}
}

static int
export_window(void)
{
	GR_EVENT event;

	if (GrOpen() < 0)
		return 1;
	ui_init();
	windowed = 1;
	bar_redraw = draw;
	measure();
	window = GrNewWindowEx(UI_FIXED, "Export", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(window);
	GrSetFocus(window);
	if (getenv("NXEXPORT_GO")) {	/* (a test's: the choices made, and Export pressed at once) */
		ui_framed();
		go();
	}
	for (;;) {
		ui_next_event(&event);
		if (ui_restyle())
			draw();
		switch (event.type) {
		case GR_EVENT_TYPE_EXPOSURE:
			draw();
			break;
		case GR_EVENT_TYPE_BUTTON_DOWN:
			clicked(event.button.x, event.button.y);
			break;
		case GR_EVENT_TYPE_KEY_DOWN:
			key_down(event.keystroke.ch);
			break;
		case GR_EVENT_TYPE_CLOSE_REQ:
			photo_hide();
			GrClose();
			return 0;
		}
	}
}

/* ---- the way in ---- */

static int
base64_value(int c)
{
	const char *s = c ? strchr(base64_letters, c) : NULL;

	return c == '-' ? 62 : c == '_' ? 63 : s ? (int)(s - base64_letters) : -1;	/* (the address-safe letters too) */
}

/*
 * Pasted text made a file: a data: address (its name kept), the "K/N:" parts of an older export's codes, or base64 alone. What is
 * none of these is text, saved as it is. The bytes at *out, their name at name (empty when it has none).
 */
static size_t
decode_text(const char *in, size_t n, unsigned char **out, char *name, size_t room)
{
	char *joined = malloc(n + 1), *b;
	size_t j = 0, i, made = 0;
	unsigned acc = 0;
	int bits = 0, letters = 0, others = 0;
	const char *comma;

	*out = NULL;
	name[0] = 0;
	if (!joined)
		return 0;
	/* parts of codes, or lines of a paste: each "K/N:" dropped, the rest joined, line ends left out */
	for (i = 0; i < n; i++) {
		if (in[i] == '\r' || in[i] == '\n')
			continue;
		if ((i == 0 || in[i - 1] == '\n' || in[i - 1] == '\r') && isdigit((unsigned char)in[i])) {
			size_t k = i;

			while (k < n && (isdigit((unsigned char)in[k]) || in[k] == '/'))
				k++;
			if (k < n && in[k] == ':' && memchr(in + i, '/', k - i)) {
				i = k;
				continue;
			}
		}
		joined[j++] = in[i];
	}
	joined[j] = 0;
	b = joined;
	if (!strncmp(b, "data:", 5) && (comma = strchr(b, ',')) && strstr(b, ";base64") && strstr(b, ";base64") < comma) {
		const char *nm = strstr(b, ";name=");

		if (nm && nm < comma) {
			size_t k = 0;

			for (nm += 6; *nm && *nm != ';' && *nm != ',' && k + 1 < room; nm++) {
				if (*nm == '%' && isxdigit((unsigned char)nm[1]) && isxdigit((unsigned char)nm[2])) {
					char hex[3] = { nm[1], nm[2], 0 };

					name[k++] = (char)strtol(hex, NULL, 16), nm += 2;
				} else
					name[k++] = *nm;
			}
			name[k] = 0;
			if (strchr(name, '/') || !strcmp(name, ".") || !strcmp(name, ".."))
				name[0] = 0;	/* (a name only, never a path) */
		}
		b = (char *)comma + 1;
	} else {
		/* base64 alone? (or text pasted to be saved as it is) */
		for (i = 0; b[i]; i++)
			if (base64_value((unsigned char)b[i]) >= 0 || b[i] == '=')
				letters++;
			else if (!isspace((unsigned char)b[i]))
				others++;
		if (others || letters < 4) {
			*out = (unsigned char *)strdup(in);
			snprintf(name, room, "pasted.txt");
			free(joined);
			return *out ? n : 0;
		}
	}
	*out = malloc(strlen(b) * 3 / 4 + 4);
	for (i = 0; *out && b[i]; i++) {
		int v = base64_value((unsigned char)b[i]);

		if (v < 0)
			continue;	/* ('=' and spaces) */
		acc = acc << 6 | (unsigned)v, bits += 6;
		if (bits >= 8)
			bits -= 8, (*out)[made++] = (unsigned char)(acc >> bits);
	}
	free(joined);
	return made;
}

/* A name of `folder` not yet taken: NAME, else "NAME (2)" and on, its ending kept. */
static void
free_name(const char *folder, const char *name, char *to, size_t room)
{
	const char *dot = strstr(name, ".tar.") ? strstr(name, ".tar.") : strrchr(name, '.');
	int k;

	if (!dot || dot == name)
		dot = name + strlen(name);
	snprintf(to, room, "%s/%s", folder, name);
	for (k = 2; access(to, F_OK) == 0 && k < 1000; k++)
		snprintf(to, room, "%s/%.*s (%d)%s", folder, (int)(dot - name), name, k, dot);
}

static int
run_and_wait(char *const args[])
{
	int status = -1;
	pid_t pid = real_vfork(), got;

	if (pid == 0) {
		execvp(args[0], args);
		_exit(127);
	}
	if (pid <= 0)
		return -1;
	while ((got = waitpid(pid, &status, windowed ? WNOHANG : 0)) == 0) {	/* (the bar answers its window meanwhile) */
		bar(0, 0, "Unpacking");
		usleep(100000);
	}
	return got == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* An archive unpacked into a folder beside it (its name without the ending): that folder, or "" if it is none. */
static void
unpack_beside(const char *file, char *folder, size_t room)
{
	size_t n;
	unsigned char *head = NULL, *raw = NULL;
	const char *kind = NULL;
	char tar_file[] = "/tmp/nxexport-in.tar";
	char *ending;

	folder[0] = 0;
	{
		FILE *f = fopen(file, "rb");
		unsigned char b[6] = { 0 };

		if (!f)
			return;
		n = fread(b, 1, 6, f);
		fclose(f);
		if (n >= 4 && !memcmp(b, "PK\3\4", 4))
			kind = "zip";
		else if (n >= 2 && b[0] == 0x1f && b[1] == 0x8b)
			kind = strstr(file, ".tar.gz") || strstr(file, ".tgz") ? "tgz" : NULL;
		else if (n >= 6 && !memcmp(b, "\xfd" "7zXZ", 5))
			kind = "txz";
	}
	if (!kind)
		return;
	snprintf(folder, room, "%s", file);
	if ((ending = strstr(folder, ".tar.")) || (ending = strrchr(folder, '.')))
		*ending = 0;
	mkdir(folder, 0777);
	if (!strcmp(kind, "txz")) {
		/* .xz made .tar here (unarc knows no xz), then as a tar */
		lzma_stream z = LZMA_STREAM_INIT;
		size_t room_out, in_n;
		lzma_ret r = LZMA_OK;
		FILE *t;

		head = read_whole(file, &in_n);
		room_out = in_n * 8 + 65536;
		raw = head ? malloc(room_out) : NULL;
		if (!raw || lzma_stream_decoder(&z, UINT64_MAX, 0) != LZMA_OK) {
			free(head), free(raw), folder[0] = 0;
			return;
		}
		z.next_in = head, z.avail_in = in_n;
		for (;;) {
			size_t left = room_out - z.total_out;

			z.next_out = raw + z.total_out, z.avail_out = left < 0x40000 ? left : 0x40000;	/* (in pieces: the bar) */
			r = lzma_code(&z, LZMA_FINISH);
			bar(in_n - z.avail_in, in_n, "Unpacking the .xz");
			if (r != LZMA_OK && r != LZMA_BUF_ERROR)
				break;
			if (z.total_out == room_out) {
				unsigned char *more = realloc(raw, room_out * 2);

				if (!more)
					break;
				raw = more, room_out *= 2;
			} else if (r == LZMA_BUF_ERROR && z.avail_out)
				break;
		}
		if (r == LZMA_STREAM_END && (t = fopen(tar_file, "wb"))) {
			fwrite(raw, 1, z.total_out, t);
			fclose(t);
			kind = "tar", file = tar_file;
		} else
			kind = NULL;
		lzma_end(&z);
		free(head), free(raw);
		if (!kind) {
			folder[0] = 0;
			return;
		}
	}
	{
		char *args[] = { "unarc", (char *)kind, (char *)file, folder, NULL };
		int got = run_and_wait(args);

		if (got == 3) {	/* (unarc's "not mine": busybox's) */
			char *z[] = { "unzip", "-oq", (char *)file, "-d", folder, NULL }, *t[] = { "tar", "-xf", (char *)file, "-C", folder, NULL };

			got = run_and_wait(!strcmp(kind, "zip") ? z : t);
		}
		if (got != 0)
			folder[0] = 0;
	}
	unlink(tar_file);
	if (folder[0]) {
		/* an archive of one folder of the same name (a folder exported): that folder's entries, not a folder in a folder */
		DIR *d = opendir(folder);
		struct dirent *e;
		int entries = 0, same = 0;
		char inner[700], moved[700];

		while (d && (e = readdir(d)))
			if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
				entries++, same |= !strcmp(e->d_name, base_name(folder));
		if (d)
			closedir(d);
		snprintf(inner, sizeof inner, "%s/%s", folder, base_name(folder));
		snprintf(moved, sizeof moved, "%s.unpacked", folder);
		if (entries == 1 && same && is_folder(inner) && rename(inner, moved) == 0 && rmdir(folder) == 0)
			rename(moved, folder);
	}
}

/* Text brought in: a file of `folder`, unpacked when it is an archive. What was made, said. */
static int
take_in(const char *in, size_t n, const char *folder)
{
	unsigned char *bytes;
	char name[160], path[600], unpacked[600];
	size_t made = decode_text(in, n, &bytes, name, sizeof name);
	FILE *f;

	if (!bytes || made == 0) {
		say("The text held nothing to save.");
		free(bytes);
		return 1;
	}
	if (!name[0]) {	/* (base64 alone: named by what it is) */
		snprintf(name, sizeof name, "%s", made >= 4 && !memcmp(bytes, "PK\3\4", 4) ? "imported.zip" :
			 made >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b ? "imported.tar.gz" :
			 made >= 6 && !memcmp(bytes, "\xfd" "7zXZ", 5) ? "imported.tar.xz" : "imported.bin");
	}
	mkdir(folder, 0777);
	free_name(folder, name, path, sizeof path);
	if (!(f = fopen(path, "wb")) || fwrite(bytes, 1, made, f) != made || fclose(f)) {
		say("%s could not be written.", path);
		free(bytes);
		return 1;
	}
	free(bytes);
	if (windowed)
		bar_begin("Unpacking what was pasted");
	unpack_beside(path, unpacked, sizeof unpacked);
	if (unpacked[0])
		say("Imported %s (%zu bytes), unpacked into %s.", path, made, unpacked);
	else
		say("Imported %s (%zu bytes).", path, made);
	if (print || !window) {
		size_t k;
		unsigned char *again = read_whole(path, &k);

		printf("nxexport: imported %s: %zu bytes, sum %08x%s%s\n", path, made, again ? fnv(again, k) : 0, unpacked[0] ? ", unpacked into " : "",
		       unpacked);
		free(again);
	}
	return 0;
}

/* The import's window: the host asked for the field, then asked every second for what was pasted there. */
static int
import_window(const char *folder)
{
	GR_EVENT event;
	int asked, k, waited = 0, done = 0;

	if (!host_open()) {
		printf("nxexport: this machine has no way to ask its host\n");
		return 1;
	}
	asked = host_ask("clip:ask", 0, 20);
	if (GrOpen() >= 0) {
		ui_init();
		windowed = 1;
		bar_x = 20, bar_y = 76, bar_w = 480;	/* (the unpacking's bar, under the status line) */
		window = GrNewWindowEx(UI_FIXED, "Import", GR_ROOT_WINDOW_ID, -1, -1, 520, 150, UI_FACE);
		GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_CLOSE_REQ);
		GrMapWindow(window);
	}
	if (asked != 200)
		say(asked == 0 ? "The host did not answer: this machine shows no text field." : "This host shows no text field (answer %d).", asked);
	else
		say("Paste the text into the field in front of the screen, then press To the machine.");
	for (;;) {
		if (window) {
			GrGetNextEventTimeout(&event, 1000);
			if (event.type == GR_EVENT_TYPE_CLOSE_REQ || (event.type == GR_EVENT_TYPE_KEY_DOWN && event.keystroke.ch == MWKEY_ESCAPE) ||
			    (event.type == GR_EVENT_TYPE_BUTTON_DOWN && done && ui_inside(event.button.x, event.button.y, 400, 110, 100, 26)) ||
			    (event.type == GR_EVENT_TYPE_BUTTON_DOWN && !done && ui_inside(event.button.x, event.button.y, 400, 110, 100, 26)))
				break;
			ui_fill(window, 0, 0, 520, 150, UI_FACE);
			ui_text(window, 20, 16, "Import into", -1, BLACK, 0);
			ui_text_fit(window, 110, 16, 390, folder, BLACK);
			ui_text_fit(window, 20, 50, 480, status, BLACK);
			ui_button(window, 400, 110, 100, 26, done ? "Close" : "Cancel", 0);
		} else
			sleep(1);
		if (done || asked != 200)
			continue;
		k = host_ask("clip:take", 0, 10);
		if (k == 200 && host_answered() > 0) {
			char *got = malloc(host_answered() + 1);

			if (got) {
				memcpy(got, host_data, host_answered());
				got[host_answered()] = 0;
				take_in(got, host_answered(), folder);
				free(got);
			}
			done = 1;
			if (!window)
				break;
		} else if (++waited > 600) {
			say("Nothing was pasted in ten minutes.");
			done = 1;
		}
	}
	host_ask("clip:hide", 0, 5);
	if (window)
		GrClose();
	return 0;
}

/* ---- the clipboard to the host and back (docs/system/clipboard.md) ---- */

static int
clipboard_out(void)
{
	int n = 0, answer;
	char *t = ui_clip_get(&n);

	if (!t)
		t = strdup(""), n = 0;
	if ((size_t)n > TEXT_PART)
		n = TEXT_PART;	/* (more than the field shows at once: its start) */
	answer = show_in_field("The machine's clipboard", t ? t : "", (size_t)n);
	printf("nxexport: the clipboard (%d bytes) to the host: %d\n", n, answer);
	free(t);
	return answer != 200;
}

/* The clipboard handed to the host, which keeps it for its Clipboard key (the bar's, at each change): no field shown. */
static int
clipboard_push(void)
{
	int n = 0, answer;
	char *t = ui_clip_get(&n);

	if (!t || !host_open() || !host_free())
		return free(t), 1;
	if ((size_t)n > HOST_DATA_MOST)
		n = HOST_DATA_MOST;
	memcpy(host_data, t, (size_t)n);
	free(t);
	answer = host_ask("clip:set", (uint32_t)n, 30);
	if (print)
		printf("nxexport: the clipboard (%d bytes) kept by the host: %d\n", n, answer);
	return answer != 200;
}

static int
paste_in(void)
{
	int answer = host_ask("clip:take", 0, 10);

	if (answer != 200) {
		printf("nxexport: the host gave no text (%d)\n", answer);
		return 1;
	}
	ui_clip_set((const char *)host_data, (int)host_answered());
	printf("nxexport: %u bytes on the clipboard\n", host_answered());
	return 0;
}

/* ---- the command line ---- */

static int
choose(const char *name)
{
	int i;

	for (i = 0; i < 4; i++)
		if (!strcmp(name, packing_keys[i]))
			return i;
	return -1;
}

static void
take_paths(char **argv, int argc)
{
	int i;

	for (i = 0; i < argc && npaths < PATHS_MOST; i++) {
		char *p = realpath(argv[i], NULL);

		paths[npaths++] = p ? p : strdup(argv[i]);
	}
}

static void
take_list(const char *file)
{
	FILE *f = fopen(file, "r");
	char line[1024];

	while (f && fgets(line, sizeof line, f) && npaths < PATHS_MOST) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0])
			paths[npaths++] = strdup(line);
	}
	if (f)
		fclose(f);
}

int
main(int argc, char **argv)
{
	const char *e;

	signal(SIGCHLD, SIG_DFL);	/* (tar and unarc are waited for) */
	print = getenv("NXEXPORT_PRINT") != NULL;
	if ((e = getenv("NXEXPORT_PACK")) && choose(e) >= 0)
		packing = choose(e);
	if ((e = getenv("NXEXPORT_LEVEL")))
		level = atoi(e);
	if ((e = getenv("NXEXPORT_TO")))
		target = !strcmp(e, "text") ? TO_TEXT : TO_PHOTO;
	if ((e = getenv("NXEXPORT_FORM")))
		form = !strcmp(e, "data") ? DATA : PLAIN;
	if (argc > 1 && !strcmp(argv[1], "--pack") && argc > 5 && choose(argv[2]) >= 0) {
		FILE *f;

		packing = choose(argv[2]), level = atoi(argv[3]), target = TO_TEXT;
		take_paths(argv + 5, argc - 5);
		if (!pack() || !(f = fopen(argv[4], "wb")))
			return 1;
		fwrite(packed, 1, packed_n, f);
		fclose(f);
		printf("nxexport: packed %s as %s: %zu bytes, sum %08x\n", packed_name, packing_keys[packing], packed_n, fnv(packed, packed_n));
		return 0;
	}
	if (argc > 3 && !strcmp(argv[1], "--text")) {
		FILE *f;

		target = TO_TEXT;
		take_paths(argv + 3, argc - 3);
		if (npaths == 1 && !is_folder(paths[0]) && !getenv("NXEXPORT_PACK"))
			packing = AS_IS;	/* (one file: as it is, unless asked, as the window has it) */
		measure();
		if (!pack() || !make_text() || !(f = fopen(argv[2], "wb")))
			return 1;
		fwrite(text, 1, text_n, f);
		fclose(f);
		printf("nxexport: text of %s: %zu characters, sum %08x\n", packed_name, text_n, fnv((unsigned char *)text, text_n));
		printf("nxexport: before packing at most %llu characters (%llu bytes), %s\n", text_chars(packing, packed_most(packing)),
		       packed_most(packing), text_n <= text_chars(packing, packed_most(packing)) ? "bound held" : "BOUND BROKEN");
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "--photo")) {
		int answer;

		target = TO_PHOTO;
		take_paths(argv + 2, argc - 2);
		measure();
		if (!pack())
			return 1;
		answer = photo_show();
		printf("nxexport: photo of %s: %u bytes of entries and %u of its directory, %s at most, sum %08x: %d\n", packed_name, photo_entries,
		       photo_tail, photo_size(packed_n) ? photo_size(packed_n) : "too large", fnv(packed, packed_n), answer);
		if (getenv("NXEXPORT_PHOTO_HOLD"))
			sleep((unsigned)atoi(getenv("NXEXPORT_PHOTO_HOLD")));
		photo_hide();
		return answer != 200;
	}
	if (argc > 3 && !strcmp(argv[1], "--decode")) {
		size_t n;
		char *t = (char *)read_whole(argv[2], &n);

		if (!t)
			return 1;
		t[n] = 0;
		return take_in(t, n, argv[3]);
	}
	if (argc > 1 && !strcmp(argv[1], "--import"))
		return import_window(argc > 2 ? argv[2] : "/root/Desktop");
	if (argc > 1 && !strncmp(argv[1], "--import=", 9))	/* (one argument, as the file manager starts it) */
		return import_window(argv[1][9] ? argv[1] + 9 : "/root/Desktop");
	if (argc > 1 && !strcmp(argv[1], "--clipboard"))
		return clipboard_out();
	if (argc > 1 && !strcmp(argv[1], "--paste"))
		return paste_in();
	if (argc > 1 && !strcmp(argv[1], "--push"))
		return clipboard_push();
	if (argc > 2 && !strcmp(argv[1], "--list"))
		take_list(argv[2]);
	else if (argc == 2 && argv[1][0] == '@')	/* (the file manager's selection: one argument) */
		take_list(argv[1] + 1);
	else
		take_paths(argv + 1, argc - 1);
	if (npaths == 0) {
		fprintf(stderr, "usage: nxexport PATH ... | --list FILE | --import [FOLDER] | --clipboard | --paste\n");
		return 2;
	}
	if (npaths == 1 && !is_folder(paths[0]) && !getenv("NXEXPORT_PACK"))
		packing = AS_IS;	/* (one file: as it is, unless asked) */
	return export_window();
}
