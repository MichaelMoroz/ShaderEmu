/*
 * MP3 (docs/system/media.md): a table of the file's frames, made when it is opened, so that any core
 * decodes any piece. A piece's decoder starts 12 frames early: the first eleven only fill the
 * bit reservoir (MP3D_SKIP), the last leaves what the next granule needs (MP3D_WARM), and its
 * samples are then those of a decoder that began at the file's start (docs/apps/play.md).
 */
#include <stdlib.h>
#include "m_audio.h"
#include "minimp3.h"

int m_mp3full_frame(mp3dec_t *dec, const uint8_t *mp3, int bytes, mp3d_sample_t *pcm, mp3dec_frame_info_t *info);
int m_mp3half_frame(mp3dec_t *dec, const uint8_t *mp3, int bytes, mp3d_sample_t *pcm, mp3dec_frame_info_t *info);

#define BACK 12		/* frames before a piece that fill its bit reservoir */

struct mp3 {
	uint32_t *at;		/* where each frame begins in the file */
	int count, samples, layer, warm;	/* frames, samples a channel in each, how many decoded early */
	int rate;		/* the file's */
	mp3d_sample_t *warm_as;
	struct mp3_dec *reader;
};

struct mp3_dec {
	mp3dec_t dec;
	int next, half;		/* the frame it would decode next (-1: none), and at what rate */
	int left, used;		/* the reader's samples a channel still in frame[] */
	int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
};

const char *
m_mp3_open(struct media_audio *a)
{
	const uint8_t *f = a->data;
	mp3dec_frame_info_t info;
	mp3dec_t dec;
	uint32_t pos = 0, room = 0, *more;
	int n, layer = 0;
	struct mp3 *m = calloc(1, sizeof *m);

	if (!m)
		return "not enough memory";
	if (a->size > 10 && !memcmp(f, "ID3", 3))
		pos = 10 + ((f[6] & 127u) << 21 | (f[7] & 127u) << 14 | (f[8] & 127u) << 7 | (f[9] & 127u));
	memset(&dec, 0, sizeof dec);
	while (pos < a->size) {
		info.frame_bytes = 0;
		n = m_mp3full_frame(&dec, f + pos, (int)(a->size - pos), NULL, &info);
		if (info.frame_bytes <= 0)
			break;
		if (n > 0 && !m->count) {
			a->info.rate = info.hz;
			a->info.channels = info.channels;
			layer = info.layer;
			m->samples = n;
		}
		/* a frame of another kind than the first is left out: every frame is as long */
		if (n > 0 && info.hz == a->info.rate && info.channels == a->info.channels && info.layer == layer && n == m->samples) {
			if ((uint32_t)m->count == room) {
				room = room ? room * 2 : 4096;
				if (!(more = realloc(m->at, room * sizeof m->at[0])))
					break;
				m->at = more;
			}
			m->at[m->count++] = pos + (uint32_t)info.frame_offset;
		}
		pos += (uint32_t)info.frame_bytes;
	}
	if (m->count < 4 || !(m->reader = m_audio_pages(sizeof *m->reader))) {
		free(m->at);
		free(m);
		return "not an MP3 file";
	}
	/* a layer III granule's overlap comes of its own coefficients and the filter's past of the
	 * granule before: two granules decoded without their samples are all a piece needs */
	m->layer = layer;
	m->rate = a->info.rate;
	m->warm = layer == 3 && m->samples == 1152 ? 1 : 2;
	m->warm_as = layer == 3 ? MP3D_WARM : NULL;
	m->reader->next = -1;
	a->own = m;
	a->unit = (uint32_t)m->samples;
	a->info.frames = (uint32_t)m->count * a->unit;
	return NULL;
}

/* A frame into `pcm` (MP3D_SKIP: only its bits kept); its samples a channel, 0 for none. */
static int
one(const struct media_audio *a, struct mp3_dec *x, int frame, int16_t *pcm)
{
	const struct mp3 *m = a->own;
	mp3dec_frame_info_t info;
	uint32_t at = m->at[frame];
	int n = (x->half ? m_mp3half_frame : m_mp3full_frame)(&x->dec, a->data + at, (int)(a->size - at), pcm, &info);

	return n > 0 && info.channels == a->info.channels && info.hz == m->rate ? n : 0;
}

/* Makes a decoder ready for `frame` as if it had decoded every frame before it. */
static void
warm(const struct media_audio *a, struct mp3_dec *x, int frame)
{
	const struct mp3 *m = a->own;
	int f = frame > BACK ? frame - BACK : 0;

	memset(&x->dec, 0, sizeof x->dec);
	x->half = a->half;
	for (; f < frame; f++)
		one(a, x, f, f < frame - m->warm ? MP3D_SKIP : m->warm_as ? m->warm_as : x->frame);
	x->next = frame;
}

/* Frame `frame` (silence when it cannot be decoded); the decoder goes on from there. */
static void
decode(const struct media_audio *a, struct mp3_dec *x, int frame, int16_t *to)
{
	if (x->next != frame || x->half != a->half)
		warm(a, x, frame);
	if (one(a, x, frame, to) != (int)a->unit)
		memset(to, 0, a->unit * (uint32_t)a->info.channels * 2);
	x->next = frame + 1;
}

int
m_mp3_read(struct media_audio *a, int16_t *to, int frames)
{
	const struct mp3 *m = a->own;
	struct mp3_dec *x = m->reader;
	int got = 0, n, ch = a->info.channels;

	while (got < frames) {
		if (!x->left) {
			if (x->next < 0)
				x->next = 0;
			if (x->next >= m->count)
				break;
			decode(a, x, x->next, x->frame);
			x->left = (int)a->unit, x->used = 0;
		}
		n = frames - got < x->left ? frames - got : x->left;
		memcpy(to + got * ch, x->frame + x->used * ch, (size_t)n * ch * 2);
		x->used += n, x->left -= n, got += n;
	}
	return got;
}

uint32_t
m_mp3_seek(struct media_audio *a, uint32_t frame)
{
	const struct mp3 *m = a->own;
	struct mp3_dec *x = m->reader;
	int f = (int)(frame / a->unit);

	x->left = 0;
	if (frame >= a->info.frames) {
		x->next = m->count;
		return a->info.frames;
	}
	decode(a, x, f, x->frame);
	x->used = (int)(frame - (uint32_t)f * a->unit);
	x->left = (int)a->unit - x->used;
	return frame;
}

int
m_mp3_decoder(struct media_audio *a, int d)
{
	struct mp3_dec *x = m_audio_pages(sizeof *x);

	if (!x)
		return 0;
	x->next = -1;
	a->dec[d] = x;
	return 1;
}

uint32_t
m_mp3_step(struct media_audio *a, int d, uint32_t piece, int16_t *to, uint32_t done)
{
	int frame = (int)(media_audio_piece_first(a, piece) / a->unit + done / a->unit);

	decode(a, a->dec[d], frame, to + done * (uint32_t)a->info.channels);
	return done + a->unit;
}

int
m_mp3_half(struct media_audio *a, int on)
{
	const struct mp3 *m = a->own;
	uint32_t per_piece = a->piece / a->unit, at = a->at;

	on = !!on;
	if (on == a->half)
		return 1;
	a->half = on;
	a->info.half = on;
	a->unit = (uint32_t)m->samples >> on;
	a->info.rate = m->rate >> on;
	a->info.frames = (uint32_t)m->count * a->unit;
	media_audio_set_piece(a, per_piece * a->unit);
	/* (the reader goes on where it was, in the new rate's frames) */
	media_audio_seek(a, on ? at / 2 : at * 2);
	return 1;
}

void
m_mp3_close(struct media_audio *a)
{
	struct mp3 *m = a->own;
	int d;

	for (d = 0; d < a->decoders; d++)
		m_audio_pages_free(a->dec[d]);
	m_audio_pages_free(m->reader);
	free(m->at);
	free(m);
}

/* An MP3 decoded whole on the core that calls, from its first frame, with no memory but `room` (media_mp3_room() bytes,
 * 16-aligned): for files decoded side by side, a core each. Its frames as m_mp3_open keeps them (those of the first one's
 * kind), at most `frames` a channel's samples into to[]: how many. */
uint32_t
media_mp3_room(void)
{
	return (uint32_t)((sizeof(mp3dec_t) + 15) & ~(size_t)15);
}

uint32_t
media_mp3_decode_alone(const void *data, uint32_t size, int16_t *to, uint32_t frames, int channels, void *room)
{
	const uint8_t *f = data;
	mp3dec_t *dec = room;
	mp3dec_frame_info_t info;
	mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	uint32_t pos = 0, made = 0, k;
	int n, hz = 0, ch = 0, layer = 0, samples = 0;

	if (size > 10 && !memcmp(f, "ID3", 3))
		pos = 10 + ((f[6] & 127u) << 21 | (f[7] & 127u) << 14 | (f[8] & 127u) << 7 | (f[9] & 127u));
	memset(dec, 0, sizeof *dec);
	while (pos < size && made < frames) {
		info.frame_bytes = 0;
		n = m_mp3full_frame(dec, f + pos, (int)(size - pos), pcm, &info);
		if (info.frame_bytes <= 0)
			break;
		if (n > 0 && !samples)
			hz = info.hz, ch = info.channels, layer = info.layer, samples = n;
		if (n > 0 && info.hz == hz && info.channels == ch && info.layer == layer && n == samples && ch == channels) {
			k = (uint32_t)n < frames - made ? (uint32_t)n : frames - made;
			memcpy(to + made * (uint32_t)channels, pcm, (size_t)k * (uint32_t)channels * 2);
			made += k;
		}
		pos += (uint32_t)info.frame_bytes;
	}
	return made;
}
