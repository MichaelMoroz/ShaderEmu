/*
 * Motion JPEG in AVI written (docs/system/media.md, "Films written"): each picture a JPEG of this library's
 * encoder (restart intervals, encoded over every core: m_jpeg_enc.c), so that m_avi.c and any player decode
 * it, and PCM sound (16 bits) in chunks between the pictures, in the order they were given. The chunks are
 * kept in memory and the file is made whole at the end: its headers, LIST movi, and the index idx1.
 */
#include <stdlib.h>
#include "m_int.h"

struct avi_entry { char id[4]; uint32_t at, bytes; };	/* at: from the movi list's "movi" */

struct media_avi_out {
	int w, h, fps_milli, rate, channels;
	uint8_t *movi;			/* the chunks so far, from "movi" on */
	uint32_t movi_bytes, movi_room;
	struct avi_entry *index;
	uint32_t entries, entries_room;
	uint32_t pictures, sound_frames, most_chunk;
};

static int
grow(struct media_avi_out *a, uint32_t more)
{
	if (a->movi_bytes + more > a->movi_room) {
		uint32_t room = a->movi_room * 2 > a->movi_bytes + more ? a->movi_room * 2 : (a->movi_bytes + more) * 2;
		uint8_t *m = m_map(room);

		if (!m)
			return 0;
		memcpy(m, a->movi, a->movi_bytes);
		m_unmap(a->movi, a->movi_room);
		a->movi = m, a->movi_room = room;
	}
	if (a->entries == a->entries_room) {
		uint32_t n = a->entries_room ? a->entries_room * 2 : 1024;
		struct avi_entry *e = realloc(a->index, n * sizeof *e);

		if (!e)
			return 0;
		a->index = e, a->entries_room = n;
	}
	return 1;
}

static inline void
le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24);
}

/* A chunk of the movi list: its id, its bytes (padded to even). */
static int
chunk(struct media_avi_out *a, const char *id, const void *bytes, uint32_t n)
{
	uint8_t *p;

	if (!grow(a, 8 + n + 1))
		return 0;
	p = a->movi + a->movi_bytes;
	memcpy(p, id, 4);
	le32(p + 4, n);
	memcpy(p + 8, bytes, n);
	if (n & 1)
		p[8 + n] = 0;
	memcpy(a->index[a->entries].id, id, 4);
	a->index[a->entries].at = a->movi_bytes;
	a->index[a->entries].bytes = n;
	a->entries++;
	a->movi_bytes += 8 + ((n + 1) & ~1u);
	if (n > a->most_chunk)
		a->most_chunk = n;
	return 1;
}

struct media_avi_out *
media_avi_begin(int width, int height, int fps_milli, int rate, int channels)
{
	struct media_avi_out *a;

	if (width < 1 || height < 1 || width > 65535 || height > 65535 || fps_milli < 1 || (rate && (channels < 1 || channels > 2)))
		return NULL;
	if (!(a = calloc(1, sizeof *a)))
		return NULL;
	a->w = width, a->h = height, a->fps_milli = fps_milli, a->rate = rate, a->channels = rate ? channels : 0;
	a->movi_room = 1u << 20;
	if (!(a->movi = m_map(a->movi_room))) {
		free(a);
		return NULL;
	}
	memcpy(a->movi, "movi", 4);
	a->movi_bytes = 4;
	return a;
}

const char *
media_avi_picture(struct media_avi_out *a, const struct media_image *picture, int quality, int workers)
{
	uint32_t bytes;
	uint8_t *jpeg;
	int ok;

	if (picture->width != a->w || picture->height != a->h)
		return "a picture of another size than the film's";
	if (!(jpeg = media_jpeg_encode(picture, quality, &bytes, workers)))
		return "not enough memory to encode a picture";
	ok = chunk(a, "00dc", jpeg, bytes);
	media_free(jpeg, bytes);
	if (!ok)
		return "not enough memory for the film";
	a->pictures++;
	return NULL;
}

const char *
media_avi_sound(struct media_avi_out *a, const int16_t *frames, uint32_t count)
{
	if (!a->rate)
		return "the film was begun without sound";
	if (count && !chunk(a, "01wb", frames, count * (uint32_t)a->channels * 2))
		return "not enough memory for the film";
	a->sound_frames += count;
	return NULL;
}

uint8_t *
media_avi_end(struct media_avi_out *a, uint32_t *size)
{
	uint32_t streams = a->rate ? 2 : 1, strl_v = 4 + 8 + 56 + 8 + 40, strl_a = 4 + 8 + 56 + 8 + 16;
	uint32_t hdrl = 4 + 8 + 56 + 8 + strl_v + (a->rate ? 8 + strl_a : 0), idx = a->entries * 16, total, i, align = (uint32_t)a->channels * 2;
	uint8_t *f = NULL, *p;

	total = 12 + 8 + hdrl + 8 + a->movi_bytes + 8 + idx;
	if (!a->pictures || !(f = m_map(total)))
		goto done;
	memset(f, 0, total);
	p = f;
#define FOUR(s) (memcpy(p, s, 4), p += 4)
#define W32(v) (le32(p, (uint32_t)(v)), p += 4)
#define W16(v) (p[0] = (uint8_t)(v), p[1] = (uint8_t)((uint32_t)(v) >> 8), p += 2)
	FOUR("RIFF"), W32(total - 8), FOUR("AVI ");
	FOUR("LIST"), W32(hdrl), FOUR("hdrl");
	FOUR("avih"), W32(56);
	W32((uint32_t)(1000000000ull / (uint32_t)a->fps_milli));	/* microseconds a picture */
	W32(0), W32(0), W32(0x10 | (a->rate ? 0x100 : 0));		/* has an index, interleaved */
	W32(a->pictures), W32(0), W32(streams), W32(a->most_chunk + 8), W32(a->w), W32(a->h), W32(0), W32(0), W32(0), W32(0);
	FOUR("LIST"), W32(strl_v), FOUR("strl");
	FOUR("strh"), W32(56), FOUR("vids"), FOUR("MJPG"), W32(0), W16(0), W16(0), W32(0);
	W32(1000), W32(a->fps_milli), W32(0), W32(a->pictures), W32(a->most_chunk), W32(~0u), W32(0);
	W16(0), W16(0), W16(a->w), W16(a->h);
	FOUR("strf"), W32(40), W32(40), W32(a->w), W32(a->h), W16(1), W16(24), FOUR("MJPG"), W32((uint32_t)a->w * (uint32_t)a->h * 3);
	W32(0), W32(0), W32(0), W32(0);
	if (a->rate) {
		FOUR("LIST"), W32(strl_a), FOUR("strl");
		FOUR("strh"), W32(56), FOUR("auds"), W32(0), W32(0), W16(0), W16(0), W32(0);
		W32(align), W32((uint32_t)a->rate * align), W32(0), W32(a->sound_frames), W32(a->rate * (int)align), W32(~0u), W32(align);
		W16(0), W16(0), W16(0), W16(0);
		FOUR("strf"), W32(16), W16(1), W16(a->channels), W32(a->rate), W32((uint32_t)a->rate * align), W16(align), W16(16);
	}
	FOUR("LIST"), W32(a->movi_bytes);
	memcpy(p, a->movi, a->movi_bytes), p += a->movi_bytes;
	FOUR("idx1"), W32(idx);
	for (i = 0; i < a->entries; i++) {
		/* (a picture is a key frame; offsets from the "movi" word, as most writers have them) */
		FOUR(a->index[i].id), W32(a->index[i].id[2] == 'd' ? 0x10 : 0), W32(a->index[i].at), W32(a->index[i].bytes);
	}
#undef FOUR
#undef W32
#undef W16
	*size = (uint32_t)(p - f);
done:
	m_unmap(a->movi, a->movi_room);
	free(a->index);
	free(a);
	return f;
}

/* ---------------------------------------------------------------- a film's picture as pixels */

struct picture_job {
	const struct media_video_info *info;
	const uint8_t *frame;
	const uint32_t *palette;
	const struct media_image *out;
};

static void
picture_rows(void *ctx, uint32_t from, uint32_t to)
{
	const struct picture_job *j = ctx;
	const struct media_video_info *in = j->info;
	const struct media_image *o = j->out;
	int w = in->width, h = in->height, x;
	uint32_t y;

	for (y = from; y < to; y++) {
		uint8_t *row = o->pixels + y * (uint32_t)o->stride;

		for (x = 0; x < w; x++) {
			int r, g, b, a = 255;

			if (in->format == MEDIA_FRAME_YUV420) {
				const uint8_t *Y = j->frame, *U = Y + w * h, *V = U + (w / 2) * (h / 2);
				int cx = x / 2 < w / 2 ? x / 2 : w / 2 - 1, cy = (int)y / 2 < h / 2 ? (int)y / 2 : h / 2 - 1;
				int l = (Y[(int)y * w + x] - 16) * 76309, u = U[cy * (w / 2) + cx] - 128, v = V[cy * (w / 2) + cx] - 128;

				/* BT.601, the studio range MPEG-1 has */
				r = (l + 104597 * v + 32768) >> 16;
				g = (l - 25675 * u - 53279 * v + 32768) >> 16;
				b = (l + 132201 * u + 32768) >> 16;
				r = m_clip(r), g = m_clip(g), b = m_clip(b);
			} else if (in->format == MEDIA_FRAME_PAL8) {
				uint32_t c = j->palette ? j->palette[j->frame[(int)y * w + x]] : 0;

				r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
			} else if (in->format == MEDIA_FRAME_WORDS) {
				const uint8_t *p = j->frame + ((uint32_t)y * (uint32_t)w + (uint32_t)x) * 4;

				r = p[2], g = p[1], b = p[0];
			} else {
				const uint8_t *p = j->frame + ((uint32_t)y * (uint32_t)w + (uint32_t)x) * 4;

				r = p[0], g = p[1], b = p[2], a = p[3];
			}
			m_put(o, row + x * o->channels, r, g, b, a);
		}
	}
}

const char *
media_video_picture(const struct media_video_info *info, const uint8_t *frame, const uint32_t *palette, struct media_image *out,
		    int workers)
{
	struct picture_job j = { info, frame, palette, out };
	int got;

	if (!out->pixels || out->width != info->width || out->height != info->height || (out->channels != 3 && out->channels != 4))
		return "pixels of another size than the film's";
	if (info->format == MEDIA_FRAME_YUV420 && ((info->width | info->height) & 1))
		return "a YUV picture of an odd size";
	got = m_workers_begin(workers);
	m_for((uint32_t)info->height, picture_rows, &j, (uint32_t)out->stride, got);
	m_workers_end();
	return NULL;
}

/* ---------------------------------------------------------------- a film's pictures side by side */

#define WORK_MOST (32u << 20)	/* the pieces' pixels at most */

struct film_job {
	const struct media_video_info *info;
	const uint8_t *frames;
	uint32_t stride;
	const uint32_t *palettes;
	int quality, w, h;
	uint8_t *work, *rooms;
	uint32_t work_bytes, full_bytes, out_bytes, room, grain;
	uint32_t *bytes;		/* each picture's JPEG, 16 bytes apart (0: it did not fit its room) */
};

static void
pixels_of(struct media_image *im, int w, int h, uint8_t *pixels)
{
	memset(im, 0, sizeof *im);
	im->width = w, im->height = h, im->channels = 3, im->stride = (w * 3 + 15) & ~15;
	im->background = 0xffffff;
	im->pixels = pixels;
}

/* A piece's pictures, one after another on its core: made pixels, fitted to the film's size, encoded into their rooms. */
static void
film_pictures(void *ctx, uint32_t from, uint32_t to)
{
	const struct film_job *f = ctx;
	uint8_t *work = f->work + (from / f->grain) * f->work_bytes;
	struct media_image full, out;
	int scaled = f->w != f->info->width || f->h != f->info->height;

	pixels_of(&full, f->info->width, f->info->height, work);
	pixels_of(&out, f->w, f->h, scaled ? work + f->full_bytes : work);
	for (; from < to; from++) {
		struct picture_job j = { f->info, f->frames + from * f->stride, f->palettes ? f->palettes + from * 256 : NULL, &full };

		picture_rows(&j, 0, (uint32_t)full.height);
		if (scaled)
			m_fit_alone(&full, &out, 0);
		f->bytes[from * 4] = m_jpeg_encode_alone(&out, f->quality, work + f->full_bytes + f->out_bytes, f->rooms + from * f->room, f->room);
	}
}

const char *
media_avi_film(struct media_avi_out *a, const struct media_video_info *info, const uint8_t *frames, uint32_t stride,
	       const uint32_t *palettes, int count, int quality, const int16_t *sound, const uint32_t *sound_counts, int workers)
{
	struct film_job f;
	const char *why = NULL;
	uint32_t pieces, rooms_bytes, bytes_bytes = (uint32_t)count * 16, k;
	int got;

	if (count < 1)
		return NULL;
	if (info->format == MEDIA_FRAME_YUV420 && ((info->width | info->height) & 1))
		return "a YUV picture of an odd size";
	memset(&f, 0, sizeof f);
	f.info = info, f.frames = frames, f.stride = stride, f.palettes = palettes, f.quality = quality, f.w = a->w, f.h = a->h;
	f.full_bytes = (((uint32_t)info->width * 3 + 15) & ~15u) * (uint32_t)info->height;
	f.out_bytes = f.w == info->width && f.h == info->height ? 0 : (((uint32_t)f.w * 3 + 15) & ~15u) * (uint32_t)f.h;
	f.work_bytes = (f.full_bytes + f.out_bytes + m_jpeg_encode_room() + 4095) & ~4095u;
	/* a JPEG's room: a byte a pixel, more than any picture but noise needs (one that does not fit is encoded again here) */
	f.room = ((uint32_t)f.w * (uint32_t)f.h + 4096u + 15u) & ~15u;
	got = m_workers_begin(workers);
	pieces = (uint32_t)got + 1;
	while (pieces > 1 && pieces * f.work_bytes > WORK_MOST)
		pieces--;
	f.grain = ((uint32_t)count + pieces - 1) / pieces;
	pieces = ((uint32_t)count + f.grain - 1) / f.grain;
	rooms_bytes = f.room * (uint32_t)count;
	if (!(f.work = m_map(pieces * f.work_bytes)) || !(f.rooms = m_map(rooms_bytes)) || !(f.bytes = m_map(bytes_bytes))) {
		why = "not enough memory to encode the pictures";
		goto done;
	}
	m_for_grain((uint32_t)count, film_pictures, &f, f.grain, got);
	for (k = 0; k < (uint32_t)count && !why; k++) {
		uint32_t n = f.bytes[k * 4];

		if (n) {
			if (!chunk(a, "00dc", f.rooms + k * f.room, n))
				why = "not enough memory for the film";
			else
				a->pictures++;
		} else {
			/* (larger than its room: made again here, over every core) */
			struct media_image full, out;
			uint8_t *pixels = m_map(f.full_bytes + f.out_bytes);

			if (!pixels) {
				why = "not enough memory to encode a picture";
				break;
			}
			pixels_of(&full, info->width, info->height, pixels);
			pixels_of(&out, f.w, f.h, f.out_bytes ? pixels + f.full_bytes : pixels);
			media_video_picture(info, frames + k * stride, palettes ? palettes + k * 256 : NULL, &full, got);
			if (f.out_bytes)
				media_image_fit(&full, &out, 0, got);
			why = media_avi_picture(a, &out, quality, got);
			m_unmap(pixels, f.full_bytes + f.out_bytes);
		}
		if (!why && sound_counts && sound_counts[k]) {
			why = media_avi_sound(a, sound, sound_counts[k]);
			sound += sound_counts[k] * (uint32_t)a->channels;
		}
	}
done:
	m_workers_end();
	m_unmap(f.work, pieces * f.work_bytes);
	m_unmap(f.rooms, rooms_bytes);
	m_unmap(f.bytes, bytes_bytes);
	return why;
}
