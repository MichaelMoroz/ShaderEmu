/* libmedia's core: workers, memory, what a file is, pictures to a size, the simple encoders (docs/system/media.md). */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "m_int.h"

/* ---------------------------------------------------------------- workers, memory */

static int opened_here, depth;
extern const char etext[];
extern void _start(void);

int
m_workers_begin(int want)
{
	static int text_read;
	int have = mcw_count();

	depth++;
	if (want <= 0)
		return 0;
	/* a worker stops at every page of code no core has run: the program's text read here once */
	if (!text_read) {
		text_read = 1;
		/* (from _start's page: a flat program's __executable_start is 0, a GOT word the loader
		 * leaves alone, and was every page from address 0 up) */
		const char *text = (const char *)((uintptr_t)_start & ~(uintptr_t)4095);

		mcw_touch_code(text, (unsigned)(etext - text));
	}
	if (have > 0)
		return have < want ? have : want;
	have = mcw_open(want);
	opened_here = have > 0;
	return have;
}

void
m_workers_end(void)
{
	/* (calls inside calls: the outermost closes what it opened) */
	if (--depth > 0)
		return;
	depth = 0;
	/* (the team leaves: a program with workers of its own posts to them next) */
	mcw_for_end();
	if (!opened_here)
		return;
	mcw_close();
	opened_here = 0;
}

void (*media_progress)(uint32_t done, uint32_t total);

void
m_progress(uint32_t done, uint32_t total)
{
	if (media_progress && !mcw_on_worker())
		media_progress(done, total);
}

void
m_look(void)
{
	int k, n = mcw_count();

	if (mcw_on_worker())
		return;
	m_progress(0, 0);
	for (k = 1; k <= n; k++)
		mcw_done(k);
}

int
m_wait(volatile uint32_t *count, uint32_t least, volatile uint32_t *failed)
{
	while (*count < least) {
		if (failed && *failed)
			return 0;
		if (!mcw_on_worker()) {
			m_look();
			mcw_rest();
		}
	}
	return 1;
}

void *
m_map(uint32_t bytes)
{
	void *m;

	if (!bytes)
		bytes = 16;
	m = mmap(NULL, (bytes + 4095) & ~4095u, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (m == MAP_FAILED)
		return NULL;
	mcw_touch(m, bytes);
	return m;
}

/* Working memory kept between calls, a slot each (a call at a time), not cleared; over 1 MB it is
 * mapped and let go each time. m_scratch_done() with what m_scratch() gave. */
#define KEEP_MOST (1u << 20)
static struct { void *at; uint32_t bytes; } kept[M_SLOTS];

void *
m_scratch(int slot, uint32_t bytes)
{
	if (bytes > KEEP_MOST)
		return m_map(bytes);
	if (kept[slot].bytes < bytes) {
		m_unmap(kept[slot].at, kept[slot].bytes);
		kept[slot].bytes = (bytes + 4095) & ~4095u;
		if (!(kept[slot].at = m_map(kept[slot].bytes)))
			kept[slot].bytes = 0;
	}
	return kept[slot].at;
}

void
m_scratch_done(int slot, void *memory, uint32_t bytes)
{
	if (memory && memory != kept[slot].at)
		m_unmap(memory, bytes);
}

void
m_unmap(void *memory, uint32_t bytes)
{
	if (memory)
		munmap(memory, (bytes ? bytes + 4095 : 4096) & ~4095u);
}

void
media_free(void *memory, uint32_t size)
{
	m_unmap(memory, size);
}

uint32_t
media_sum(const void *data, uint32_t bytes, uint32_t sum)
{
	const uint8_t *p = data;

	while (bytes--)
		sum = (sum ^ *p++) * 16777619u;
	return sum;
}

/* ---------------------------------------------------------------- what a file is */

const char *
media_kind_name(enum media_kind kind)
{
	static const char *const names[] = { "unknown", "PNG", "JPEG", "GIF", "BMP", "PNM", "TGA", "WAV", "FLAC", "MP3",
		"Ogg Vorbis", "MOD", "MPEG-1", "VQA", "AVI" };

	return (unsigned)kind < sizeof names / sizeof names[0] ? names[kind] : "unknown";
}

enum media_kind
media_kind_of_name(const char *path)
{
	static const struct { const char *end; enum media_kind kind; } ends[] = {
		{ ".png", MEDIA_PNG }, { ".jpg", MEDIA_JPEG }, { ".jpeg", MEDIA_JPEG }, { ".gif", MEDIA_GIF },
		{ ".bmp", MEDIA_BMP }, { ".ppm", MEDIA_PNM }, { ".pgm", MEDIA_PNM }, { ".pbm", MEDIA_PNM }, { ".pnm", MEDIA_PNM },
		{ ".tga", MEDIA_TGA }, { ".wav", MEDIA_WAV }, { ".flac", MEDIA_FLAC }, { ".mp3", MEDIA_MP3 }, { ".ogg", MEDIA_OGG },
		{ ".mod", MEDIA_MOD }, { ".mpg", MEDIA_MPEG }, { ".mpeg", MEDIA_MPEG }, { ".vqa", MEDIA_VQA }, { ".avi", MEDIA_AVI },
	};
	const char *dot = strrchr(path, '.');
	unsigned i;

	if (!dot)
		return MEDIA_UNKNOWN;
	for (i = 0; i < sizeof ends / sizeof ends[0]; i++)
		if (!strcasecmp(dot, ends[i].end))
			return ends[i].kind;
	return MEDIA_UNKNOWN;
}

const char *
media_probe(const void *data, uint32_t size, struct media_info *info)
{
	const uint8_t *f = data;

	memset(info, 0, sizeof *info);
	info->frames = 1;
	if (size < 4)
		return "too short to be anything";
	if (size >= 8 && !memcmp(f, "\x89PNG\r\n\x1a\n", 8))
		return m_png_probe(f, size, info);
	if (f[0] == 0xff && f[1] == 0xd8)
		return m_jpeg_probe(f, size, info);
	if (size >= 6 && (!memcmp(f, "GIF87a", 6) || !memcmp(f, "GIF89a", 6)))
		return m_gif_probe(f, size, info);
	if (size >= 12 && !memcmp(f, "RIFF", 4) && !memcmp(f + 8, "WAVE", 4))
		return info->kind = MEDIA_WAV, NULL;
	if (size >= 12 && !memcmp(f, "RIFF", 4) && !memcmp(f + 8, "AVI ", 4))
		return info->kind = MEDIA_AVI, NULL;
	if (!memcmp(f, "fLaC", 4))
		return info->kind = MEDIA_FLAC, NULL;
	if (!memcmp(f, "OggS", 4))
		return info->kind = MEDIA_OGG, NULL;
	if (!memcmp(f, "ID3", 3) || (f[0] == 0xff && (f[1] & 0xe6) == 0xe2))
		return info->kind = MEDIA_MP3, NULL;
	if (f[0] == 0 && f[1] == 0 && f[2] == 1 && (f[3] == 0xba || f[3] == 0xb3))
		return info->kind = MEDIA_MPEG, NULL;
	if (size >= 12 && !memcmp(f, "FORM", 4) && !memcmp(f + 8, "WVQA", 4))
		return info->kind = MEDIA_VQA, NULL;
	if (size >= 1084 && (!memcmp(f + 1080, "M.K.", 4) || !memcmp(f + 1080, "M!K!", 4) || !memcmp(f + 1081, "CHN", 3) ||
			     !memcmp(f + 1082, "CH", 2) || !memcmp(f + 1080, "FLT4", 4)))
		return info->kind = MEDIA_MOD, NULL;
	return m_simple_probe(f, size, info);
}

/* ---------------------------------------------------------------- pictures */

static const char *
check_out(struct media_image *out, int w, int h)
{
	if (out->channels != 3 && out->channels != 4)
		out->channels = 3;
	if (!out->pixels) {
		out->width = w, out->height = h;
		out->stride = (w * out->channels + 15) & ~15;
		if (!(out->pixels = m_map((uint32_t)out->stride * (uint32_t)h)))
			return "not enough memory for the picture";
		out->owned = 1;
	} else if (out->width != w || out->height != h || out->stride < w * out->channels) {
		return "the picture is not the size its pixels were given for";
	}
	return NULL;
}

const char *
media_image_decode(const void *data, uint32_t size, struct media_image *out, int workers)
{
	struct media_info info;
	const char *why = media_probe(data, size, &info);
	int given = out->pixels != NULL;

	if (why)
		return why;
	if (!info.width || !info.height)
		return "not a picture";
	if (info.width > M_MOST_SIDE || info.height > M_MOST_SIDE)
		return "a picture too large for this machine";
	if (!out->background && !out->bgr)
		out->background = 0xffffff;
	if ((why = check_out(out, info.width, info.height)) != NULL)
		return why;
	switch (info.kind) {
	case MEDIA_PNG: why = m_png_decode(data, size, out, workers); break;
	case MEDIA_JPEG: why = m_jpeg_decode(data, size, out, workers); break;
	case MEDIA_GIF: why = m_gif_decode(data, size, out, workers); break;
	default: why = m_simple_decode(data, size, out, workers); break;
	}
	if (why && !given)
		media_image_free(out);
	return why;
}

void
media_image_free(struct media_image *image)
{
	if (image->owned && image->pixels)
		m_unmap(image->pixels, (uint32_t)image->stride * (uint32_t)image->height);
	image->pixels = NULL;
	image->owned = 0;
}

/* A file mapped whole; its pages read here first, so a worker never stops at one. */
static const uint8_t *
map_file(const char *path, uint32_t *size)
{
	struct stat st;
	const uint8_t *f;
	volatile uint32_t sum = 0;
	uint32_t i;
	int fd = open(path, O_RDONLY);

	if (fd < 0 || fstat(fd, &st) != 0 || st.st_size < 4 || st.st_size > 0x7fffffff) {
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	*size = (uint32_t)st.st_size;
	f = mmap(NULL, *size, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
	close(fd);
	if (f == MAP_FAILED)
		return NULL;
	for (i = 0; i < *size; i += 4096)
		sum += f[i];
	(void)sum;
	return f;
}

const char *
media_image_load(const char *path, struct media_image *out, int channels, int workers)
{
	uint32_t size;
	const uint8_t *f = map_file(path, &size);
	const char *why;

	memset(out, 0, sizeof *out);
	out->channels = channels;
	if (!f)
		return "the file cannot be read";
	why = media_image_decode(f, size, out, workers);
	munmap((void *)f, size);
	return why;
}

const char *
media_image_to_ppm(const char *path, const char *ppm_path, int *width, int *height, int workers)
{
	struct media_info info;
	struct media_image out;
	uint32_t size, mapped, pixels;
	const uint8_t *f = map_file(path, &size);
	const char *why;
	char header[64];
	int head, fd = -1, in_memory = 0;
	uint8_t *map = MAP_FAILED;

	if (!f)
		return "the file cannot be read";
	if ((why = media_probe(f, size, &info)) != NULL || (!info.width && (why = "not a picture")))
		goto done;
	if (info.width > M_MOST_SIDE || info.height > M_MOST_SIDE) {
		why = "a picture too large for this machine";
		goto done;
	}
	/* its header a multiple of 16 bytes long, so that the pixels begin on one */
	head = snprintf(header, sizeof header, "P6\n%d %d\n", info.width, info.height);
	while ((head + 4) & 15)
		header[head++] = ' ';
	memcpy(header + head, "255\n", 4);
	head += 4;
	pixels = (uint32_t)info.width * (uint32_t)info.height * 3;
	mapped = (uint32_t)head + pixels;
	fd = open(ppm_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0 && ftruncate(fd, mapped) == 0 &&
	    (map = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0)) == MAP_FAILED)
		/* no MMU: a file is not mapped to be written; the picture is made in memory and written after */
		in_memory = (map = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED;
	if (map == MAP_FAILED) {
		why = "the picture cannot be written";
		goto done;
	}
	memcpy(map, header, head);
	mcw_touch(map, mapped);
	memset(&out, 0, sizeof out);
	out.width = info.width, out.height = info.height, out.channels = 3, out.stride = info.width * 3;
	out.pixels = map + head;
	out.background = 0xffffff;
	why = media_image_decode(f, size, &out, workers);
	if (!why && in_memory && pwrite(fd, map, mapped, 0) != (ssize_t)mapped)
		why = "the picture cannot be written";
	*width = info.width;
	*height = info.height;
done:
	if (map != MAP_FAILED)
		munmap(map, mapped);
	if (fd >= 0)
		close(fd);
	if (why && fd >= 0)
		unlink(ppm_path);
	munmap((void *)f, size);
	return why;
}

/* ---------------------------------------------------------------- a picture to a size */

struct fit {
	const struct media_image *in;
	struct media_image *out;
	int sx0, sy0, sw, sh;		/* the part of `in` shown */
	int dx0, dy0, dw, dh;		/* where it lies in `out` */
};

static inline void
fit_pixel(const struct fit *f, const uint8_t *p, int *r, int *g, int *b, int *a)
{
	if (f->in->channels == 4)
		*r = p[0], *g = p[1], *b = p[2], *a = p[3];
	else
		*r = p[0], *g = p[1], *b = p[2], *a = 255;
}

/* Output rows from..to-1: each pixel the mean of the input's under it (smaller), or the four nearest weighed (larger). */
static void
fit_rows(void *ctx, uint32_t from, uint32_t to)
{
	const struct fit *f = ctx;
	const struct media_image *in = f->in;
	struct media_image *out = f->out;
	int ch = in->channels, y, x;
	/* the input's x under a pixel's middle, (2 rx + 1) sw 2^15 / dw, goes up by sw 2^16 / dw a pixel: kept as a
	 * quotient and a remainder in 32 bits, with one 64-bit division a row where it begins (one a pixel was 10%
	 * of nxview's first core) */
	const uint32_t dw = (uint32_t)f->dw, step_q = dw ? (uint32_t)(((uint64_t)(uint32_t)f->sw << 16) / dw) : 0,
		step_r = dw ? (uint32_t)(((uint64_t)(uint32_t)f->sw << 16) % dw) : 0;

	for (y = (int)from; y < (int)to; y++) {
		uint8_t *o = out->pixels + (uint32_t)y * (uint32_t)out->stride;
		int ry = y - f->dy0, uy = 0, mean = f->sw >= 2 * f->dw && f->sh >= 2 * f->dh, begun = 0;
		uint32_t qx = 0, rx_rem = 0;

		if (ry < 0 || ry >= f->dh) {
			for (x = 0; x < out->width; x++)
				m_put(out, o + x * out->channels, out->background >> 16 & 255, out->background >> 8 & 255,
				      out->background & 255, 255);
			continue;
		}
		for (x = 0; x < out->width; x++) {
			int rx = x - f->dx0, r, g, b, a;

			if (rx < 0 || rx >= f->dw) {
				m_put(out, o + x * out->channels, out->background >> 16 & 255, out->background >> 8 & 255,
				      out->background & 255, 255);
				continue;
			}
			/* (the mean under each where it is two pixels or more; below that the four nearest to its middle) */
			if (mean) {
				int x0 = f->sx0 + rx * f->sw / f->dw, x1 = f->sx0 + (rx + 1) * f->sw / f->dw;
				int y0 = f->sy0 + ry * f->sh / f->dh, y1 = f->sy0 + (ry + 1) * f->sh / f->dh, i, j, n, sr = 0, sg = 0, sb = 0, sa = 0;

				if (x1 <= x0) x1 = x0 + 1;
				if (y1 <= y0) y1 = y0 + 1;
				n = (x1 - x0) * (y1 - y0);
				for (j = y0; j < y1; j++) {
					const uint8_t *p = in->pixels + (uint32_t)j * (uint32_t)in->stride + x0 * ch;

					for (i = x0; i < x1; i++, p += ch) {
						fit_pixel(f, p, &r, &g, &b, &a);
						sr += r, sg += g, sb += b, sa += a;
					}
				}
				r = sr / n, g = sg / n, b = sb / n, a = sa / n;
			} else {
				/* 16.16: the input's point under this pixel's middle */
				int ux, ix, iy, fx, fy, c[4][4], k;
				const uint8_t *p;

				if (!begun) {	/* (the row's first pixel of the picture: the pixels after it follow on) */
					uint64_t n = (uint64_t)(2 * rx + 1) * (uint32_t)f->sw << 15;

					qx = (uint32_t)(n / dw), rx_rem = (uint32_t)(n % dw);
					uy = (int)(((int64_t)(2 * ry + 1) * f->sh << 15) / f->dh) - 32768 + (f->sy0 << 16);
					begun = 1;
				} else {
					qx += step_q, rx_rem += step_r;
					if (rx_rem >= dw)
						qx++, rx_rem -= dw;
				}
				ux = (int)qx - 32768 + (f->sx0 << 16);

				if (ux < f->sx0 << 16) ux = f->sx0 << 16;
				if (uy < f->sy0 << 16) uy = f->sy0 << 16;
				ix = ux >> 16, iy = uy >> 16, fx = ux >> 8 & 255, fy = uy >> 8 & 255;
				for (k = 0; k < 4; k++) {
					int px = ix + (k & 1), py = iy + (k >> 1);

					if (px >= f->sx0 + f->sw) px = f->sx0 + f->sw - 1;
					if (py >= f->sy0 + f->sh) py = f->sy0 + f->sh - 1;
					p = in->pixels + (uint32_t)py * (uint32_t)in->stride + px * ch;
					fit_pixel(f, p, &c[k][0], &c[k][1], &c[k][2], &c[k][3]);
				}
#define FIT_MIX(i) (((c[0][i] * (256 - fx) + c[1][i] * fx) * (256 - fy) + (c[2][i] * (256 - fx) + c[3][i] * fx) * fy + 32768) >> 16)
				r = FIT_MIX(0), g = FIT_MIX(1), b = FIT_MIX(2), a = FIT_MIX(3);
#undef FIT_MIX
			}
			m_put(out, o + x * out->channels, r, g, b, a);
		}
	}
}

static void
stage_items(void *ctx, uint32_t from, uint32_t to)
{
	mcw_fn stage = (mcw_fn)ctx;

	for (; from < to; from++)
		stage(from, 0);
}

void
m_stages(uint32_t n, mcw_fn stage, int workers)
{
	mcw_for_opts o;

	if (workers <= 0) {
		stage_items((void *)stage, 0, n);
		return;
	}
	memset(&o, 0, sizeof o);
	o.workers = workers;
	o.grain = 1;
	mcw_for_opt(n, stage_items, (void *)stage, &o);
}

/* m_for's loop, its pieces on the first core told (media_progress): set by the first core
 * before the round and only read by the workers. */
static struct { mcw_body fn; uint32_t n; int on; } told_for;

static void
told_piece(void *ctx, uint32_t from, uint32_t to)
{
	told_for.fn(ctx, from, to);
	m_progress(to, told_for.n);
}

void
m_for(uint32_t n, mcw_body fn, void *ctx, uint32_t out_bytes, int workers)
{
	mcw_for_opts o;
	int tell = media_progress && !told_for.on && !mcw_on_worker();

	if (workers <= 0 || n < 2) {
		if (n)
			fn(ctx, 0, n);
		return;
	}
	memset(&o, 0, sizeof o);
	o.workers = workers;
	o.out_bytes = out_bytes;
	if (tell)
		told_for.fn = fn, told_for.n = n, told_for.on = 1;
	mcw_for_opt(n, tell ? told_piece : fn, ctx, &o);
	if (tell)
		told_for.on = 0;
}

void
media_image_fit(const struct media_image *in, struct media_image *out, int cover, int workers)
{
	struct fit f;
	int w = in->width, h = in->height, got;

	memset(&f, 0, sizeof f);
	f.in = in, f.out = out;
	f.sw = w, f.sh = h, f.dw = out->width, f.dh = out->height;
	/* covered when the shapes are near (the window cuts it), else fitted inside */
	if (cover && (int64_t)w * out->height * 10 <= (int64_t)h * out->width * 13 && (int64_t)h * out->width * 10 <= (int64_t)w * out->height * 13) {
		if ((int64_t)w * out->height > (int64_t)h * out->width)
			f.sw = (int)((int64_t)h * out->width / out->height);
		else
			f.sh = (int)((int64_t)w * out->height / out->width);
		f.sx0 = (w - f.sw) / 2, f.sy0 = (h - f.sh) / 2;
	} else {
		if ((int64_t)w * out->height > (int64_t)h * out->width)
			f.dh = (int)((int64_t)h * out->width / w);
		else
			f.dw = (int)((int64_t)w * out->height / h);
		if (f.dw < 1) f.dw = 1;
		if (f.dh < 1) f.dh = 1;
		f.dx0 = (out->width - f.dw) / 2, f.dy0 = (out->height - f.dh) / 2;
	}
	got = m_workers_begin(workers);
	m_for((uint32_t)out->height, fit_rows, &f, (uint32_t)out->stride, got);
	m_workers_end();
}

/* How much smaller a picture of w x h may be decoded and still be at least the size it is shown at. */
static int
fit_shrink(int w, int h, const struct media_image *out, int cover)
{
	int s;

	/* (as media_image_fit: covered only when the shapes are near) */
	cover = cover && (int64_t)w * out->height * 10 <= (int64_t)h * out->width * 13 &&
		(int64_t)h * out->width * 10 <= (int64_t)w * out->height * 13;
	for (s = 8; s > 1; s /= 2)
		if (cover ? s * out->width <= w && s * out->height <= h : s * out->width <= w || s * out->height <= h)
			return s;
	return 1;
}

const char *
media_image_decode_fit(const void *data, uint32_t size, struct media_image *out, int cover, int workers)
{
	struct media_image in;
	const char *why;
	int got;

	memset(&in, 0, sizeof in);
	in.channels = out->channels;
	in.background = out->background;
	if (!out->pixels || out->width < 1 || out->height < 1)
		return "no pixels to decode into";
	/* a picture of the size already: straight into the pixels, nothing in between */
	{
		struct media_info info;

		if (!media_probe(data, size, &info) && info.width == out->width && info.height == out->height)
			return media_image_decode(data, size, out, workers);
	}
	/* (the workers kept open for both steps) */
	got = m_workers_begin(workers);
	{
		struct media_info info;
		int shrink = 1;

		/* a JPEG shown at half its size or less is decoded smaller (its transform cut down) */
		if (!media_probe(data, size, &info) && info.kind == MEDIA_JPEG)
			shrink = fit_shrink(info.width, info.height, out, cover);
		if (shrink > 1) {
			in.width = (info.width + shrink - 1) / shrink, in.height = (info.height + shrink - 1) / shrink;
			in.stride = (in.width * in.channels + 15) & ~15;
			if (!(in.pixels = m_map((uint32_t)in.stride * (uint32_t)in.height)))
				why = "not enough memory for the picture";
			else
				in.owned = 1, why = m_jpeg_decode_shrunk(data, size, &in, got, shrink);
		} else {
			why = media_image_decode(data, size, &in, got);
		}
		/* (a smaller decoding's last column and row are partly past the picture: not shown) */
		if (!why && shrink > 1) {
			struct media_image whole = in;

			whole.width = info.width / shrink ? info.width / shrink : 1;
			whole.height = info.height / shrink ? info.height / shrink : 1;
			media_image_fit(&whole, out, cover, got);
			goto fitted;
		}
	}
	if (!why)
		media_image_fit(&in, out, cover, got);
fitted:
	media_image_free(&in);
	m_workers_end();
	return why;
}

/* ---------------------------------------------------------------- the simple encoders */

uint8_t *
media_ppm_encode(const struct media_image *image, uint32_t *size)
{
	char header[48];
	int head = snprintf(header, sizeof header, "P6\n%d %d\n255\n", image->width, image->height), y, x;
	uint8_t *f, *o;

	*size = (uint32_t)head + (uint32_t)image->width * (uint32_t)image->height * 3;
	if (!(f = m_map(*size)))
		return NULL;
	memcpy(f, header, head);
	o = f + head;
	for (y = 0; y < image->height; y++) {
		const uint8_t *p = image->pixels + (uint32_t)y * (uint32_t)image->stride;

		if (image->channels == 3) {
			memcpy(o, p, (size_t)image->width * 3);
			o += image->width * 3;
			continue;
		}
		for (x = 0; x < image->width; x++, p += 4, o += 3)
			if (image->bgr)
				o[0] = p[2], o[1] = p[1], o[2] = p[0];
			else
				o[0] = p[0], o[1] = p[1], o[2] = p[2];
	}
	return f;
}

uint8_t *
media_bmp_encode(const struct media_image *image, uint32_t *size)
{
	uint32_t row = ((uint32_t)image->width * 3 + 3) & ~3u, bytes = row * (uint32_t)image->height;
	uint8_t *f, *o;
	int y, x;

	*size = 54 + bytes;
	if (!(f = m_map(*size)))
		return NULL;
	memset(f, 0, 54);
	f[0] = 'B', f[1] = 'M';
#define PUT32(at, v) (f[at] = (uint8_t)(v), f[(at) + 1] = (uint8_t)((v) >> 8), f[(at) + 2] = (uint8_t)((v) >> 16), f[(at) + 3] = (uint8_t)((v) >> 24))
	PUT32(2, *size);
	PUT32(10, 54);
	PUT32(14, 40);
	PUT32(18, (uint32_t)image->width);
	PUT32(22, (uint32_t)image->height);
	f[26] = 1, f[28] = 24;
	PUT32(34, bytes);
	PUT32(38, 2835);
	PUT32(42, 2835);
#undef PUT32
	for (y = 0; y < image->height; y++) {
		const uint8_t *p = image->pixels + (uint32_t)(image->height - 1 - y) * (uint32_t)image->stride;

		o = f + 54 + (uint32_t)y * row;
		for (x = 0; x < image->width; x++, p += image->channels, o += 3)
			if (image->bgr)
				o[0] = p[0], o[1] = p[1], o[2] = p[2];
			else
				o[0] = p[2], o[1] = p[1], o[2] = p[0];
	}
	return f;
}

const char *
media_image_save(const char *path, const struct media_image *image, int quality, int workers)
{
	uint32_t size = 0, done = 0;
	uint8_t *f;
	int fd;

	switch (media_kind_of_name(path)) {
	case MEDIA_PNG: f = media_png_encode(image, quality > 0 && quality <= 9 ? quality : 1, &size, workers); break;
	case MEDIA_JPEG: f = media_jpeg_encode(image, quality > 0 ? quality : 90, &size, workers); break;
	case MEDIA_BMP: f = media_bmp_encode(image, &size); break;
	case MEDIA_GIF: f = media_gif_encode(image, &size, workers); break;
	default: f = media_ppm_encode(image, &size); break;
	}
	if (!f)
		return "not enough memory to encode the picture";
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		while (done < size) {
			ssize_t n = write(fd, f + done, size - done);

			if (n <= 0)
				break;
			done += (uint32_t)n;
		}
		close(fd);
	}
	media_free(f, size);
	return fd >= 0 && done == size ? NULL : "the picture cannot be written";
}
