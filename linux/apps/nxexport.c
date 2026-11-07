/*
 * nxexport: a file or folder taken out of the machine as text a visitor can copy, or as QR codes a phone can
 * scan, and text pasted in front of the screen brought in again (docs/apps/export.md, docs/system/clipboard.md).
 *
 *   nxexport PATH ...           the window: how it is packed (as it is, Zip, tar.gz, tar.xz and a level), and
 *                               to QR codes or to the text field the host shows in front of the screen
 *   nxexport --list FILE        the same for the paths FILE has, a line each (the file manager's selection)
 *   nxexport --import [FOLDER]  the field in front of the screen asked for text: what is pasted there is a file of
 *                               FOLDER (default /root/Desktop), and an archive is unpacked beside it
 *   nxexport --clipboard        the desktop's clipboard shown in that field
 *   nxexport --paste            the field's text put on the desktop's clipboard (the host's "To the machine")
 *
 * For tests, without a window:
 *   nxexport --pack as|zip|tgz|txz LEVEL OUT PATH ...   packs and prints "nxexport: packed ...: N bytes, sum S"
 *   nxexport --text OUT PATH ...                         the text a visitor copies (a data: address), as the window makes it
 *   nxexport --qr BIG|MEDIUM|DENSE TEXTFILE              the codes of that text: their count and a sum of every module
 *   nxexport --decode TEXTFILE FOLDER                    imports TEXTFILE's text as a paste would
 *   NXEXPORT_PACK=zip|tgz|txz|as NXEXPORT_LEVEL=N NXEXPORT_TO=qr|text NXEXPORT_PRINT=1: the window's choices and what it does
 *
 * The text is a data: address, "data:TYPE;name=NAME;base64,DATA": a browser that is given it saves the file,
 * and every base64 decoder takes what follows the comma. Many QR codes are the same text in parts, each code
 * "K/N:" and its part: joined in order without those, they are the text again.
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
#include "qrcodegen.h"
#include "ui.h"
#include "ui_ask.h"
#include "host_ask.h"

#define WIDTH 560
#define HEIGHT 520
#define PATHS_MOST 256
#define TEXT_PART (HOST_DATA_MOST - 256)	/* what one showing of the field holds: more is shown in parts */
#define CODES_MOST 400
#define ARCHIVE "/tmp/nxexport.tar"

enum { AS_IS, ZIP, TGZ, TXZ };
static const char *const packing_names[] = { "As it is", "Zip", "tar.gz", "tar.xz" };
static const char *const packing_keys[] = { "as", "zip", "tgz", "txz" };
enum { TO_QR, TO_TEXT };
enum { BIG, MEDIUM, DENSE };
static const char *const density_names[] = { "Big codes", "Medium", "Dense" };
static const int density_versions[] = { 10, 20, 30 };	/* the largest QR version used: 57, 97 and 137 modules */

static char *paths[PATHS_MOST];
static int npaths, packing = ZIP, level = 9, target = TO_TEXT, density = MEDIUM, print;
static GR_WINDOW_ID window;
static char status[200];

/* what was made: the packed bytes, their name, and the text */
static unsigned char *packed;
static size_t packed_n;
static char packed_name[160];
static char *text;
static size_t text_n;
/* the codes: each part's text starts at code_at[k], code_len[k] long; the code shown */
static size_t code_at[CODES_MOST], code_len[CODES_MOST];
static int codes, code_shown, showing_codes, text_part, text_parts;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));

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
	z.next_in = (unsigned char *)in, z.avail_in = (uInt)n;
	z.next_out = out, z.avail_out = (uInt)room;
	made = deflate(&z, Z_FINISH) == Z_STREAM_END ? z.total_out : 0;
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
	uint32_t crc = 0, offset = (uint32_t)zip_out.n;
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
		put16(&zip_out, (unsigned)zip_entries), put32(&zip_out, size), put32(&zip_out, at), put16(&zip_out, 0);
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
	return pid > 0 && waitpid(pid, &status, 0) == pid && status == 0;
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
	z.next_in = (unsigned char *)raw, z.avail_in = (uInt)n;
	z.next_out = out->p, z.avail_out = (uInt)out->room;
	done = deflate(&z, Z_FINISH) == Z_STREAM_END;
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
	z.next_in = raw, z.avail_in = n;
	z.next_out = out->p, z.avail_out = out->room;
	while (r == LZMA_OK)
		r = lzma_code(&z, LZMA_FINISH);
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

/* The text: "data:TYPE;name=NAME;base64," and the bytes. */
static int
make_text(void)
{
	char head[300], name[200];
	size_t i, h, at;
	const unsigned char *p = packed;

	/* (the name as an address may have it: what is not a letter, digit or one of a few signs as %XX) */
	for (i = h = 0; packed_name[i] && h < sizeof name - 4; i++) {
		unsigned char c = (unsigned char)packed_name[i];

		if (isalnum(c) || strchr("._-()", c))
			name[h++] = (char)c;
		else
			h += (size_t)snprintf(name + h, sizeof name - h, "%%%02X", c);
	}
	name[h] = 0;
	h = (size_t)snprintf(head, sizeof head, "data:%s;name=%s;base64,", type_of(), name);
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
	if (packing == AS_IS) {
		if (!(packed = read_whole(paths[0], &packed_n))) {
			say("%s could not be read.", base_name(paths[0]));
			return 0;
		}
		snprintf(packed_name, sizeof packed_name, "%s", base_name(paths[0]));
		return 1;
	}
	snprintf(packed_name, sizeof packed_name, "%s%s.%s", npaths > 1 ? "exported" : base_name(paths[0]), npaths > 1 ? "" : "",
		 packing == ZIP ? "zip" : packing == TGZ ? "tar.gz" : "tar.xz");
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
		say("Could not be packed: %s.", out.failed ? "no memory, or a file could not be read" : "no memory");
		return 0;
	}
	packed = out.p, packed_n = out.n;
	return 1;
}

/* ---- QR codes ---- */

/* The most bytes of text one code of the chosen size holds (byte mode, ECC M), less its "K/N:". */
static size_t
code_room(void)
{
	static size_t room[3];
	static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], temp[qrcodegen_BUFFER_LEN_MAX];
	char *probe;
	size_t lo = 1, hi = 3000;

	if (room[density])
		return room[density];
	if (!(probe = malloc(hi + 1)))
		return 100;
	memset(probe, 'a', hi);
	while (lo < hi) {	/* (the largest that fits) */
		size_t mid = (lo + hi + 1) / 2;

		probe[mid] = 0;
		if (qrcodegen_encodeText(probe, temp, qr, qrcodegen_Ecc_MEDIUM, 1, density_versions[density], qrcodegen_Mask_AUTO, false))
			lo = mid;
		else
			hi = mid - 1;
		probe[mid] = 'a';
	}
	free(probe);
	return room[density] = lo > 12 ? lo - 12 : lo;	/* ("K/N:" is 9 at most: 400/400:) */
}

/* The text cut into codes: false if it would be more than CODES_MOST. */
static int
make_codes(void)
{
	size_t room = code_room(), at = 0;
	int n;

	n = (int)((text_n + room - 1) / room);
	if (text_n <= room + 12)
		n = 1;	/* (one code: the text alone, no "1/1:") */
	if (n > CODES_MOST)
		return 0;
	for (codes = 0; codes < n; codes++) {
		code_at[codes] = at;
		code_len[codes] = n == 1 ? text_n : (text_n - at < room ? text_n - at : room);
		at += code_len[codes];
	}
	code_shown = 0;
	return 1;
}

/* Code k made; its size in modules, 0 if it did not fit. */
static int
make_code(int k, uint8_t qr[qrcodegen_BUFFER_LEN_MAX])
{
	static uint8_t temp[qrcodegen_BUFFER_LEN_MAX];
	char *part = malloc(code_len[k] + 16);
	int head = codes > 1 ? snprintf(part, 16, "%d/%d:", k + 1, codes) : 0, ok;

	if (!part)
		return 0;
	memcpy(part + head, text + code_at[k], code_len[k]);
	part[head + code_len[k]] = 0;
	ok = qrcodegen_encodeText(part, temp, qr, qrcodegen_Ecc_MEDIUM, 1, density_versions[density], qrcodegen_Mask_AUTO, false);
	free(part);
	return ok ? qrcodegen_getSize(qr) : 0;
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
	char what[200];
	size_t from = (size_t)text_part * TEXT_PART, n = text_n - from < TEXT_PART ? text_n - from : TEXT_PART;
	int answer;

	if (text_parts > 1)
		snprintf(what, sizeof what, "%s: part %d of %d (join the parts in order)", packed_name, text_part + 1, text_parts);
	else
		snprintf(what, sizeof what, "%s (%zu bytes)", packed_name, packed_n);
	answer = show_in_field(what, text + from, n);
	if (answer == 200)
		say("%s is in the field in front of the screen: %zu characters%s.", packed_name, n,
		    text_parts > 1 ? ", part of it: Next for the rest" : "");
	else
		say(answer == 0 ? "The host did not answer: this machine shows no text field." :
		    "This host shows no text field (answer %d): the text is on the clipboard.", answer);
}

/* ---- the window ---- */

struct key {
	int x, y, w, h, id;
	const char *label;
	int down;
};
static struct key keys[24];
static int nkeys;
enum { K_PACK = 1, K_LEVEL = 10, K_QR = 20, K_TEXT, K_DENSITY = 30, K_GO = 40, K_PREV, K_NEXT, K_BACK, K_CLOSE };

static void
key(int x, int y, int w, int h, int id, const char *label, int down)
{
	if (nkeys < (int)(sizeof keys / sizeof keys[0]))
		keys[nkeys++] = (struct key){ x, y, w, h, id, label, down };
	ui_button(window, x, y, w, h, label, down);
}

static void
draw_code(void)
{
	static uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
	int size = make_code(code_shown, qr), area = HEIGHT - 60, scale, x0, y0, x, y;
	char line[80];

	ui_fill(window, 0, 0, WIDTH, HEIGHT, UI_FACE);
	if (!size) {
		ui_text(window, 20, 20, "The code could not be made.", -1, BLACK, 0);
		return;
	}
	scale = area / (size + 8);
	x0 = (WIDTH - (size + 8) * scale) / 2, y0 = 10;
	ui_fill(window, x0, y0, (size + 8) * scale, (size + 8) * scale, WHITE);
	GrSetGCForeground(ui_gc, BLACK);
	for (y = 0; y < size; y++)
		for (x = 0; x < size; x++) {
			int run = 0;

			while (x + run < size && qrcodegen_getModule(qr, x + run, y))
				run++;
			if (run)
				GrFillRect(window, ui_gc, x0 + (x + 4) * scale, y0 + (y + 4) * scale, run * scale, scale);
			x += run;
		}
	nkeys = 0;
	snprintf(line, sizeof line, "Code %d of %d", code_shown + 1, codes);
	ui_text(window, 20, HEIGHT - 38, line, -1, BLACK, 0);
	key(200, HEIGHT - 42, 100, 26, K_PREV, "< Previous", 0);
	key(310, HEIGHT - 42, 100, 26, K_NEXT, "Next >", 0);
	key(440, HEIGHT - 42, 100, 26, K_BACK, "Back", 0);
}

static void
draw(void)
{
	char line[200];
	int i, y = 14;

	if (showing_codes) {
		draw_code();
		return;
	}
	nkeys = 0;
	ui_fill(window, 0, 0, WIDTH, HEIGHT, UI_FACE);
	if (npaths == 1)
		snprintf(line, sizeof line, "Export %s%s", base_name(paths[0]), is_folder(paths[0]) ? " (a folder)" : "");
	else
		snprintf(line, sizeof line, "Export %d entries of %s", npaths, npaths ? paths[0] : "");
	ui_text(window, 20, y, line, -1, BLACK, 0);
	y += 34;
	ui_text(window, 20, y + 6, "Packed as:", -1, BLACK, 0);
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
	key(120, y, 140, 26, K_TEXT, "Text (base64)", target == TO_TEXT);
	key(268, y, 140, 26, K_QR, "QR codes", target == TO_QR);
	y += 34;
	if (target == TO_QR) {
		ui_text(window, 20, y + 6, "Codes:", -1, BLACK, 0);
		for (i = 0; i < 3; i++)
			key(120 + i * 104, y, 96, 26, K_DENSITY + i, density_names[i], density == i);
	}
	y += 44;
	key(20, y, 140, 30, K_GO, target == TO_QR ? "Make the codes" : text_parts > 1 && text_part + 1 < text_parts ? "Next part" :
	    "Export", 0);
	key(WIDTH - 120, y, 100, 30, K_CLOSE, "Close", 0);
	y += 46;
	ui_text_fit(window, 20, y, WIDTH - 40, status, BLACK);
	y += 30;
	ui_text(window, 20, y, "The text is a data: address. A browser given it saves the file; any", -1, UI_SHADOW, 0);
	ui_text(window, 20, y + 18, "base64 decoder takes what follows the comma. Codes of many parts:", -1, UI_SHADOW, 0);
	ui_text(window, 20, y + 36, "each begins K/N: - join their texts in order without it.", -1, UI_SHADOW, 0);
}

static void
go(void)
{
	if (target == TO_TEXT && text && text_parts > 1 && text_part + 1 < text_parts) {
		text_part++;	/* (the next part of what was made) */
		show_text_part();
		return;
	}
	say("Packing...");
	draw();
	GrFlush();
	if (!pack() || !make_text()) {
		if (!status[0] || !strcmp(status, "Packing..."))
			say("No memory for the text.");
		return;
	}
	if (target == TO_QR) {
		if (!make_codes()) {
			say("%zu bytes are %zu characters: more than %d codes. Pack it smaller or send it as text.", packed_n, text_n, CODES_MOST);
			return;
		}
		say("%s: %zu bytes, %zu characters, %d code%s.", packed_name, packed_n, text_n, codes, codes == 1 ? "" : "s");
		showing_codes = 1;
		return;
	}
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
		else if (k->id == K_QR || k->id == K_TEXT)
			target = k->id == K_QR ? TO_QR : TO_TEXT, text_parts = 0;
		else if (k->id >= K_DENSITY && k->id < K_DENSITY + 3)
			density = k->id - K_DENSITY;
		else if (k->id == K_GO)
			go();
		else if (k->id == K_PREV && code_shown > 0)
			code_shown--;
		else if (k->id == K_NEXT && code_shown + 1 < codes)
			code_shown++;
		else if (k->id == K_BACK)
			showing_codes = 0;
		else if (k->id == K_CLOSE) {
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
	if (showing_codes) {
		if ((ch == MWKEY_RIGHT || ch == MWKEY_PAGEDOWN || ch == ' ') && code_shown + 1 < codes)
			code_shown++;
		else if ((ch == MWKEY_LEFT || ch == MWKEY_PAGEUP) && code_shown > 0)
			code_shown--;
		else if (ch == MWKEY_ESCAPE)
			showing_codes = 0;
		draw();
	} else if (ch == MWKEY_ENTER)
		go(), draw();
	else if (ch == MWKEY_ESCAPE) {
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
	window = GrNewWindowEx(UI_FIXED, "Export", GR_ROOT_WINDOW_ID, -1, -1, WIDTH, HEIGHT, UI_FACE);
	GrSelectEvents(window, GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_KEY_DOWN | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_CLOSE_REQ);
	GrMapWindow(window);
	GrSetFocus(window);
	if (getenv("NXEXPORT_GO")) {	/* (a test's: the choices made, and Export pressed at once) */
		ui_framed();
		go();
		if (print && target == TO_QR && codes)
			printf("nxexport: showing code 1 of %d\n", codes), fflush(stdout);
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
 * Pasted text made a file: a data: address (its name kept), parts of QR codes ("K/N:"), or base64 alone. What is
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
	pid_t pid = real_vfork();

	if (pid == 0) {
		execvp(args[0], args);
		_exit(127);
	}
	return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
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
			z.next_out = raw + z.total_out, z.avail_out = room_out - z.total_out;
			r = lzma_code(&z, LZMA_FINISH);
			if (r != LZMA_OK && r != LZMA_BUF_ERROR)
				break;
			if (z.avail_out == 0) {
				unsigned char *more = realloc(raw, room_out * 2);

				if (!more)
					break;
				raw = more, room_out *= 2;
			} else if (r == LZMA_BUF_ERROR)
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
		target = !strcmp(e, "qr") ? TO_QR : TO_TEXT;
	if ((e = getenv("NXEXPORT_CODES")))
		density = !strcmp(e, "big") ? BIG : !strcmp(e, "dense") ? DENSE : MEDIUM;
	if (argc > 1 && !strcmp(argv[1], "--pack") && argc > 5 && choose(argv[2]) >= 0) {
		FILE *f;

		packing = choose(argv[2]), level = atoi(argv[3]);
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

		take_paths(argv + 3, argc - 3);
		if (npaths == 1 && !is_folder(paths[0]) && !getenv("NXEXPORT_PACK"))
			packing = AS_IS;	/* (one file: as it is, unless asked, as the window has it) */
		if (!pack() || !make_text() || !(f = fopen(argv[2], "wb")))
			return 1;
		fwrite(text, 1, text_n, f);
		fclose(f);
		printf("nxexport: text of %s: %zu characters, sum %08x\n", packed_name, text_n, fnv((unsigned char *)text, text_n));
		return 0;
	}
	if (argc > 3 && !strcmp(argv[1], "--qr")) {
		static uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
		uint32_t h = 2166136261u;
		int k, x, y, size;

		density = !strcmp(argv[2], "big") || !strcmp(argv[2], "BIG") ? BIG : !strcmp(argv[2], "dense") || !strcmp(argv[2], "DENSE") ? DENSE : MEDIUM;
		if (!(text = (char *)read_whole(argv[3], &text_n)) || !make_codes())
			return 1;
		for (k = 0; k < codes; k++) {
			if (!(size = make_code(k, qr)))
				return 1;
			for (y = 0; y < size; y++)
				for (x = 0; x < size; x++)
					h = (h ^ (uint32_t)qrcodegen_getModule(qr, x, y)) * 16777619u;
			if (k == 0)
				printf("nxexport: code 1 is %d modules across\n", size);
		}
		printf("nxexport: %zu characters in %d codes of at most %zu, sum %08x\n", text_n, codes, code_room(), h);
		return 0;
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
