/*
 * Ogg Vorbis (docs/system/media.md) over stb_vorbis (https://github.com/nothings/stb, public domain or
 * MIT; build.sh fetches it). A packet's samples are of it and of the packet before, so a decoder
 * that seeks to a piece's first sample (the page before it, a packet decoded for its overlap)
 * gives what one that began at the start gives: a piece is any core's.
 */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include <stdlib.h>

/*
 * sin(pi t) and cos(pi t) for t in [0, 1], in floats (a file's twiddle factors and windows:
 * vorbis_tables.py; in soft doubles they were a third of nxplay's start). t is folded to
 * [0, 1/4] exactly (1 - t and 1/2 - t of a float there are exact); the polynomials are musl's
 * __sindf and __cosdf, here in floats.
 */
static float
m_vorbis_sin_kernel(float x)
{
	float z = x * x;

	return x + x * z * (-0x1.555556p-3f + z * (0x1.111108p-7f + z * (-0x1.a00f9ep-13f + z * 0x1.6cd878p-19f)));
}

static float
m_vorbis_cos_kernel(float x)
{
	float z = x * x;

	return 1.0f + z * (-0x1p-1f + z * (0x1.55553ep-5f + z * (-0x1.6c087ep-10f + z * 0x1.99342ep-16f)));
}

static float
m_vorbis_sinpi(float t)
{
	const float pi = 3.14159265358979f;

	if (t > 0.5f)
		t = 1.0f - t;
	return t <= 0.25f ? m_vorbis_sin_kernel(pi * t) : m_vorbis_cos_kernel(pi * (0.5f - t));
}

static float
m_vorbis_cospi(float t)
{
	const float pi = 3.14159265358979f;
	float sign = 1.0f;

	if (t > 0.5f) {
		t = 1.0f - t;
		sign = -1.0f;
	}
	return sign * (t <= 0.25f ? m_vorbis_cos_kernel(pi * t) : m_vorbis_sin_kernel(pi * (0.5f - t)));
}

#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#include "stb_vorbis.c"
#include "m_audio.h"

/*
 * The file's setup (codebooks, floors, windows) is read once, on the first core, and only read
 * after; a decoder is a copy of the stream's state with buffers and temporary memory of its
 * own, so a worker never allocates.
 */
/*
 * A page that says the wrong frame for its end (one of Grieg's, by 412): a seek that starts its
 * count from that page is off by as much for the whole page. Frames from..to are sought from
 * good, the end of the page before it, and decoded up to.
 */
#define VORBIS_LIARS 8

struct vorbis_liar {
	uint32_t from, to, good;
};

struct vorbis {
	stb_vorbis *reader;
	stb_vorbis setup;	/* the reader as it was opened */
	uint32_t bytes, floor_bytes;	/* a decoder's memory, a channel's floor */
	struct vorbis_liar liars[VORBIS_LIARS];
	int liar_count;
};

struct vorbis_dec {
	stb_vorbis v;
	uint32_t next;		/* the frame it would decode next (~0: none) */
};

/*
 * Every block adds a multiple of a quarter of the short block's size, so a position that is not
 * one is wrong; the last page's may be anything, as it cuts the file's end. A wrong position that
 * is such a multiple is not found.
 */
static void
m_vorbis_find_liars(struct media_audio *a, struct vorbis *o, const stb_vorbis *v)
{
	const uint8_t *d = a->data;
	uint32_t unit = (uint32_t)v->blocksize_0 / 4, padding = (uint32_t)(v->blocksize_1 - v->blocksize_0) >> 2;
	uint32_t p = 0, last = 0;
	int open = 0;

	while (p + 27 <= a->size && !memcmp(d + p, "OggS", 4)) {
		uint32_t at = d[p + 6] | d[p + 7] << 8 | d[p + 8] << 16 | (uint32_t)d[p + 9] << 24;
		uint32_t segments = d[p + 26], next = p + 27 + segments, i;

		if (next > a->size)
			break;
		for (i = 0; i < segments; i++)
			next += d[p + 27 + i];
		if (at != ~0u) {
			if (at < last)
				break;		/* (another stream chained on) */
			if (open) {
				o->liars[o->liar_count++].to = at + padding;
				open = 0;
			}
			if (at % unit && !(d[p + 5] & PAGEFLAG_last_page) && o->liar_count < VORBIS_LIARS) {
				o->liars[o->liar_count].from = at;
				o->liars[o->liar_count].good = last;
				open = 1;
			} else
				last = at;
		}
		p = next;
	}
}

static int
m_vorbis_seek_to(const struct vorbis *o, stb_vorbis *v, uint32_t frame)
{
	uint32_t good = frame;
	int i, n;

	for (i = 0; i < o->liar_count; i++)
		if (frame >= o->liars[i].from && frame < o->liars[i].to)
			good = o->liars[i].good;
	if (!stb_vorbis_seek(v, good))
		return 0;
	while (good < frame) {
		n = v->channel_buffer_end - v->channel_buffer_start;
		if (!n) {
			if (!stb_vorbis_get_frame_float(v, NULL, NULL))
				return 0;
			continue;
		}
		if ((uint32_t)n > frame - good)
			n = (int)(frame - good);
		v->channel_buffer_start += n;
		good += (uint32_t)n;
	}
	return 1;
}

const char *
m_vorbis_open(struct media_audio *a)
{
	struct vorbis *o = calloc(1, sizeof *o);
	stb_vorbis *v;
	uint32_t floor_values = 2, i;
	int error;

	if (!o)
		return "not enough memory";
	if (!(v = stb_vorbis_open_memory(a->data, (int)a->size, &error, NULL))) {
		free(o);
		return "an Ogg file, but not Vorbis sound this can read";
	}
	o->reader = v;
	a->own = o;
	a->info.rate = (int)v->sample_rate;
	a->info.channels = v->channels > 2 ? 2 : v->channels;
	a->info.frames = stb_vorbis_stream_length_in_samples(v);
	o->setup = *v;
	m_vorbis_find_liars(a, o, v);
	for (i = 0; i < (uint32_t)v->floor_count; i++)
		if (v->floor_types[i] == 1 && v->floor_config[i].floor1.values > (int)floor_values)
			floor_values = (uint32_t)v->floor_config[i].floor1.values;
	/* the decoder, its channels' buffers, its temporary memory (in 16 bytes) */
	o->bytes = (sizeof(struct vorbis_dec) + 15) & ~15u;
	o->floor_bytes = (floor_values * 2 + 15) & ~15u;
	o->bytes += (uint32_t)v->channels * ((uint32_t)v->blocksize_1 * 6 + o->floor_bytes);
	o->bytes += v->temp_memory_required + (uint32_t)v->blocksize_1 * 2 + 4096;
	if (a->info.frames)
		a->unit = 1;
	return NULL;
}

int
m_vorbis_read(struct media_audio *a, int16_t *to, int frames)
{
	const struct vorbis *o = a->own;
	int ch = a->info.channels;

	if (a->info.frames && a->at >= a->info.frames)
		return 0;
	return stb_vorbis_get_samples_short_interleaved(o->reader, ch, to, frames * ch);
}

uint32_t
m_vorbis_seek(struct media_audio *a, uint32_t frame)
{
	const struct vorbis *o = a->own;

	if (a->info.frames && frame >= a->info.frames)
		return a->info.frames;
	if (m_vorbis_seek_to(o, o->reader, frame))
		return frame;
	stb_vorbis_seek_start(o->reader);
	return 0;
}

int
m_vorbis_decoder(struct media_audio *a, int d)
{
	const struct vorbis *o = a->own;
	struct vorbis_dec *x = m_audio_pages(o->bytes);
	uint8_t *p, *end;
	int i;

	if (!x)
		return 0;
	x->v = o->setup;
	x->next = ~0u;
	p = (uint8_t *)x + ((sizeof *x + 15) & ~15u);
	end = (uint8_t *)x + o->bytes;
	for (i = 0; i < x->v.channels; i++) {
		x->v.channel_buffers[i] = (float *)p;
		p += x->v.blocksize_1 * 4;
		x->v.previous_window[i] = (float *)p;
		p += x->v.blocksize_1 * 2;
		x->v.finalY[i] = (int16 *)p;
		p += o->floor_bytes;
	}
	/* (stb_vorbis takes its temporary memory from the top of this, down) */
	x->v.alloc.alloc_buffer = (char *)p;
	x->v.alloc.alloc_buffer_length_in_bytes = (int)(end - p);
	x->v.setup_offset = 0;
	x->v.temp_offset = (int)(end - p);
	a->dec[d] = x;
	return 1;
}

uint32_t
m_vorbis_step(struct media_audio *a, int d, uint32_t piece, int16_t *to, uint32_t done)
{
	const struct vorbis *o = a->own;
	struct vorbis_dec *x = a->dec[d];
	uint32_t ch = (uint32_t)a->info.channels, want = media_audio_piece_frames(a, piece);
	uint32_t first = media_audio_piece_first(a, piece) + done, n = want - done > 4096 ? 4096 : want - done, got = 0;

	if (x->next == first || m_vorbis_seek_to(o, &x->v, first))
		got = (uint32_t)stb_vorbis_get_samples_short_interleaved(&x->v, (int)ch, to + done * ch, (int)(n * ch));
	if (got < n) {
		/* (the file ends sooner than it said, or is damaged there: silence) */
		memset(to + (done + got) * ch, 0, (want - done - got) * ch * 2);
		x->next = ~0u;
		return want;
	}
	x->next = first + n;
	return done + n;
}

void
m_vorbis_close(struct media_audio *a)
{
	struct vorbis *o = a->own;
	int d;

	for (d = 0; d < a->decoders; d++)
		m_audio_pages_free(a->dec[d]);
	stb_vorbis_close(o->reader);
	free(o);
}
