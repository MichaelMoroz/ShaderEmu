/* MPEG-1 films (docs/system/media.md "Films", docs/apps/video.md): pl_mpeg (MIT; fetched and patched by
 * build.sh) arranged for the workers. A closed group of pictures is one worker's job, copied
 * into its places a picture a job; the sound is chunks of frames, a job each. One MPEG-1 film
 * is open at a time: the jobs find it in M. */
#include <stdlib.h>
#include "m_video.h"

#define VIDEOS 48		/* decoders at most, each on a worker of its own */
#define AUDIOS 16
#define GOP_MOST 16		/* pictures in a group, at most */
#define CHUNK 14		/* sound frames in a chunk: half a second at 32 kHz */
#define AGAIN 0x80000000u	/* in a group's number: go on with it where the last job left it */

struct mpeg;
static struct mpeg *M;

/* ---------------------------------------------------------------- memory */

/* Memory a worker may write: there now, every page of it, and the film's (unmapped at its close). */
static void *space(unsigned bytes);

/* A decoder's own three pictures are never used (a picture is decoded where its group keeps
 * it): every decoder has the same ones, brought in once. */
static unsigned own_bytes;
static void *own_frames;

static void *
large(unsigned bytes)
{
	if (!own_bytes || bytes != own_bytes)
		return space(bytes);
	if (!own_frames)
		own_frames = space(bytes);
	return own_frames;
}

#define PLM_NO_STDIO
#define PLM_MALLOC(sz) ((sz) > 4096 ? large((unsigned)(sz)) : space((unsigned)(sz)))
#define PLM_FREE(p) ((void)(p))
#define PLM_REALLOC(p, sz) (abort(), (void *)(p))	/* (only a buffer that grows asks, and none here does) */
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

struct gop { unsigned at, bytes; int first, frames; };
struct video {
	plm_video_t *decoder, *made;	/* made: as it was before its first picture */
	plm_buffer_t buffer;		/* over the group being decoded */
	uint8_t *frames;		/* the group's pictures, one after another */
	uint8_t *last[3];		/* where the last pictures were decoded, by their number's remainder */
	int worker, done;
};
struct audio { plm_audio_t *decoder, *made; plm_buffer_t buffer; };

enum { IDLE, DECODING, DECODED, COPYING, SOUND };
struct unit {			/* a worker and the decoder it has */
	int video, audio;	/* one of them is -1 */
	int state, gop, frames;
	uint32_t began;		/* when its group was begun, in milliseconds */
	int copy_next, copied, own;	/* COPYING: pictures given out, pictures there, and whether one is this worker's now */
	int helping;		/* the unit whose picture this worker is copying (it had no job of its own) */
	int busy;
	uint32_t result;
	mcw_handle call;	/* lent workers: its job on whichever was free (0: made here) */
};

struct mpeg {
	volatile int stop;		/* the film is closing: jobs end where they are */
	volatile int drop;		/* a seek: the groups being decoded are not wanted any more */
	int width, height, fps_milli, frames, gops, gop_most, rate, chunks, pcm_frames;
	unsigned frame_bytes;
	uint8_t *video_es, *audio_es;
	unsigned video_bytes, audio_bytes;
	struct gop *gop;
	unsigned *sound_at;		/* where each sound frame begins, and one more: the end */
	int sound_frames;
	int16_t *pcm;
	struct video *videos[VIDEOS];
	struct audio *audios[AUDIOS];
	int video_count, audio_count;

	struct unit units[MC_MAX_CORES + 1];
	int unit_count, sounds, workers, lent;
	unsigned char *gop_state;	/* 0 to do, 1 being decoded, 2 its pictures are in their places */
	unsigned char *chunk_done;
	int first_gop, next_gop, next_chunk, chunks_done;
	int ready_gop, ready_last;	/* the first group not yet in its places, and the picture before it */
	int copies_wanted;		/* pictures of groups being copied that no worker has yet */
	int asked;			/* the last picture asked for: its place is still in use */
	int seeking;			/* the picture to decode from once the groups being decoded are gone */
	uint8_t **places;
	int slots, own_places;

	uint8_t *chunk;			/* small pieces of memory come from here */
	uint32_t chunk_left;
	struct { void *at; uint32_t bytes; } *held;
	int nheld, room;
};

static void *
keep(void *memory, uint32_t bytes)
{
	if (!memory)
		abort();
	if (M->nheld == M->room && !(M->held = realloc(M->held, (size_t)(M->room += 64) * sizeof *M->held)))
		abort();
	M->held[M->nheld].at = memory;
	M->held[M->nheld++].bytes = bytes;
	return memory;
}

static void *
space(unsigned bytes)
{
	void *m;

	bytes = (bytes + 15) & ~15u;
	if (bytes > 16384)
		return keep(m_map(bytes), bytes);
	if (bytes > M->chunk_left) {
		M->chunk = keep(m_map(65536), 65536);
		M->chunk_left = 65536;
	}
	m = M->chunk;
	M->chunk += bytes;
	M->chunk_left -= bytes;
	return m;
}

/* ---------------------------------------------------------------- the file taken apart */

/* A buffer over bytes that are there already, without asking pl_mpeg for memory. */
static void
over(plm_buffer_t *buffer, uint8_t *bytes, unsigned length)
{
	memset(buffer, 0, sizeof *buffer);
	buffer->bytes = bytes;
	buffer->capacity = buffer->length = buffer->total_size = length;
	buffer->mode = PLM_BUFFER_MODE_FIXED_MEM;
}

/* The next start code (00 00 01, then which) from `at` on: where it is, or `bytes` when there is none. */
static unsigned
next_code(const uint8_t *es, unsigned bytes, unsigned at, int *code)
{
	/* `at` is where the code's 1 would be: a byte over 1 there rules out the two places after it */
	for (at += 2; at + 1 < bytes;) {
		unsigned b = es[at];

		if (b > 1)
			at += 3;
		else if (b == 0)
			at++;
		else if (es[at - 1] == 0 && es[at - 2] == 0) {
			*code = es[at + 1];
			return at - 2;
		} else
			at += 3;
	}
	return bytes;
}

/* The length of the layer II sound frame that begins at p, or 0 when none does. */
static unsigned
sound_frame(const uint8_t *p)
{
	static const int kbps[16] = { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 };
	static const int hz[4] = { 44100, 48000, 32000, 0 };
	int bits = kbps[p[2] >> 4], rate = hz[(p[2] >> 2) & 3];

	if (p[0] != 0xff || (p[1] & 0xfe) != 0xfc || !bits || !rate)
		return 0;
	return (unsigned)(144000 * bits / rate) + ((p[2] >> 1) & 1);
}

static const char *
take_apart(struct mpeg *m, const uint8_t *file, unsigned bytes)
{
	plm_buffer_t whole, probe;
	plm_demux_t *demux;
	plm_packet_t *packet;
	plm_video_t *video;
	unsigned at, n, headers = 0, groups = 0;
	int code, begins, g;

	m->video_es = space(bytes);
	m->audio_es = space(bytes);
	over(&whole, (uint8_t *)file, bytes);
	demux = plm_demux_create(&whole, 0);
	while ((packet = plm_demux_decode(demux)) != NULL) {
		if (packet->type == PLM_DEMUX_PACKET_VIDEO_1 && m->video_bytes + packet->length <= bytes) {
			memcpy(m->video_es + m->video_bytes, packet->data, packet->length);
			m->video_bytes += (unsigned)packet->length;
		} else if (packet->type == PLM_DEMUX_PACKET_AUDIO_1 && m->audio_bytes + packet->length <= bytes) {
			memcpy(m->audio_es + m->audio_bytes, packet->data, packet->length);
			m->audio_bytes += (unsigned)packet->length;
		}
	}
	if (!m->video_bytes)
		return "not an MPEG-1 program stream with video in it";
	over(&probe, m->video_es, m->video_bytes);
	video = plm_video_create_with_buffer(&probe, 0);
	if (!plm_video_has_header(video))
		return "its video has no sequence header";
	m->width = plm_video_get_width(video);
	m->height = plm_video_get_height(video);
	m->fps_milli = (int)(plm_video_get_framerate(video) * 1000.0 + 0.5);
	if ((m->width | m->height) & 15 || m->width > 1024 || m->height > 768 || m->fps_milli < 1000)
		return "its pictures' width and height must be multiples of 16, 1024 x 768 at most";
	m->frame_bytes = (unsigned)(m->width * m->height * 3 / 2);

	/* Its groups of pictures: each begins at a sequence header when every group has one, else
	 * at its own header. One pass over the start codes notes the headers and the pictures. */
	{
		struct { unsigned at; int code; } *notes = NULL;
		unsigned noted = 0, held = 0, k;

		for (at = next_code(m->video_es, m->video_bytes, 0, &code); at < m->video_bytes;
		     at = next_code(m->video_es, m->video_bytes, at + 4, &code)) {
			if (code != 0xb3 && code != 0xb8 && code != 0x00)
				continue;
			if (noted == held && (notes = realloc(notes, (held += 1024) * sizeof *notes)) == NULL)
				abort();
			notes[noted].at = at;
			notes[noted++].code = code;
			headers += code == 0xb3;
			groups += code == 0xb8;
		}
		begins = headers > 1 ? 0xb3 : 0xb8;
		m->gop = space(((headers > 1 ? headers : groups) + 1) * sizeof *m->gop);
		g = -1;
		for (k = 0; k < noted; k++) {
			at = notes[k].at;
			if (notes[k].code == begins) {
				if (g >= 0)
					m->gop[g].bytes = at - m->gop[g].at;
				g++;
				m->gop[g].at = g ? at : 0;
				m->gop[g].first = m->frames;
			} else if (notes[k].code == 0x00 && g >= 0) {
				m->gop[g].frames++;	/* a picture */
				m->frames++;
			}
		}
		free(notes);
	}
	if (g < 0)
		return "its video has no groups of pictures";
	m->gop[g].bytes = m->video_bytes - m->gop[g].at;
	m->gops = g + 1;
	for (g = 0; g < m->gops; g++) {
		if (m->gop[g].frames > GOP_MOST)
			return "a group of its pictures is longer than 16 (encode it with -g 6, closed)";
		if (m->gop[g].frames > m->gop_most)
			m->gop_most = m->gop[g].frames;
	}

	/* its sound's frames (layer II, 1152 samples each) */
	for (at = 0; at + 4 <= m->audio_bytes && (n = sound_frame(m->audio_es + at)) != 0 && at + n <= m->audio_bytes; at += n)
		m->sound_frames++;
	if (m->sound_frames) {
		static const int hz[4] = { 44100, 48000, 32000, 0 };

		m->sound_at = space((unsigned)(m->sound_frames + 1) * sizeof *m->sound_at);
		for (g = 0, at = 0; g < m->sound_frames; at += sound_frame(m->audio_es + at), g++)
			m->sound_at[g] = at;
		m->sound_at[g] = at;
		m->rate = hz[(m->audio_es[2] >> 2) & 3];
		m->chunks = (m->sound_frames + CHUNK - 1) / CHUNK;
		m->pcm_frames = m->sound_frames * PLM_AUDIO_SAMPLES_PER_FRAME;
		m->pcm = space((unsigned)m->pcm_frames * 4);
	}
	return NULL;
}

static int
gop_of(const struct mpeg *m, int frame)
{
	int low = 0, high = m->gops - 1, middle;

	while (low < high) {
		middle = (low + high + 1) / 2;
		if (m->gop[middle].first <= frame)
			low = middle;
		else
			high = middle - 1;
	}
	return low;
}

/* A decoder with room for one group's pictures, for worker `worker`. */
static int
video_new(struct mpeg *m, int worker)
{
	struct video *x;
	plm_buffer_t probe;

	if (m->video_count == VIDEOS)
		return -1;
	x = space(sizeof *x);
	over(&probe, m->video_es, m->video_bytes);
	own_bytes = 3 * m->frame_bytes;
	x->decoder = plm_video_create_with_buffer(&probe, 0);
	own_bytes = 0;
	plm_video_set_no_delay(x->decoder, 1);	/* no B pictures: each is out as soon as it is decoded */
	x->made = space(sizeof *x->made);
	*x->made = *x->decoder;
	x->frames = space((unsigned)m->gop_most * m->frame_bytes);
	x->worker = worker;
	m->videos[m->video_count] = x;
	return m->video_count++;
}

static int
audio_new(struct mpeg *m)
{
	struct audio *x;
	plm_buffer_t probe;

	if (m->audio_count == AUDIOS || !m->sound_frames)
		return -1;
	x = space(sizeof *x);
	over(&probe, m->audio_es, 0);	/* (nothing to read: it finds its first header in a job) */
	x->decoder = plm_audio_create_with_buffer(&probe, 0);
	x->made = space(sizeof *x->made);
	*x->made = *x->decoder;
	m->audios[m->audio_count] = x;
	return m->audio_count++;
}

/* ---------------------------------------------------------------- the jobs */

/* The next picture is decoded where it is kept, and the picture before it is read there. Its
 * planes lie in pl_mpeg's order, red chroma before blue (a block at a plane's edge is
 * predicted from bytes past it, as it was). */
static void
decode_into(plm_video_t *decoder, uint8_t *to, uint8_t *before)
{
	unsigned luma = (unsigned)(M->width * M->height);

	decoder->stale = before ? before - to : 0;	/* (what a block that is not predicted keeps: plm_fast.py) */
	decoder->frame_current.y.data = to;
	decoder->frame_current.cr.data = to + luma;
	decoder->frame_current.cb.data = to + luma + luma / 4;
}

/* A macroblock of a kept picture copied from another's. */
static void
block_from(uint8_t *to, const uint8_t *from, int column, int row)
{
	int w = M->width, y;
	unsigned luma = (unsigned)(w * M->height), at = (unsigned)(row * 16 * w + column * 16);
	unsigned chroma = luma + (unsigned)(row * 8 * (w / 2) + column * 8);

	for (y = 0; y < 16; y++)
		memcpy(to + at + (unsigned)(y * w), from + at + (unsigned)(y * w), 16);
	for (y = 0; y < 8; y++) {
		memcpy(to + chroma + (unsigned)(y * (w / 2)), from + chroma + (unsigned)(y * (w / 2)), 8);
		memcpy(to + chroma + luma / 4 + (unsigned)(y * (w / 2)), from + chroma + luma / 4 + (unsigned)(y * (w / 2)), 8);
	}
}

static uint32_t
decode_gop(uint32_t video, uint32_t gop)
{
	struct video *x = M->videos[video];
	plm_frame_t *frame;
	uint8_t *to, *before;
	int g = (int)(gop & ~AGAIN), mb;

	if (!(gop & AGAIN)) {
		*x->decoder = *x->made;
		over(&x->buffer, M->video_es + M->gop[g].at, M->gop[g].bytes);
		x->decoder->buffer = &x->buffer;
		x->decoder->start_code = -1;
		x->done = 0;
	}
	while (x->done < M->gop[g].frames) {
		/* pl_mpeg went round three pictures of its own: a block it does not predict kept the
		 * picture three before, or the one of the group before that lay there */
		to = x->frames + (unsigned)x->done * M->frame_bytes;
		before = x->last[x->done % 3];
		decode_into(x->decoder, to, before);
		x->last[x->done % 3] = to;
		if ((frame = plm_video_decode(x->decoder)) == NULL)
			break;
		/* and so did the macroblocks after the last one a picture's slices came to */
		if (before && before != to)
			for (mb = x->decoder->macroblock_address + 1; mb < x->decoder->mb_size; mb++)
				block_from(to, before, mb % x->decoder->mb_width, mb / x->decoder->mb_width);
		x->done++;
		if (M->stop || M->drop)
			return (uint32_t)M->gop[g].frames;
		if (mcw_recalled(mcw_self()))	/* the kernel wants this core: the rest in another job */
			break;
	}
	return (uint32_t)x->done;
}

static uint8_t *place(const struct mpeg *m, int frame) { return m->places[frame % m->slots]; }

/* Picture (which & 255) of decoder (which >> 8)'s group, as picture `frame` in its place: luma, blue, red. */
static uint32_t
copy_one(uint32_t which, uint32_t frame)
{
	const uint8_t *kept = M->videos[which >> 8]->frames + (which & 255) * M->frame_bytes;
	uint8_t *o = place(M, (int)frame);
	unsigned luma = (unsigned)(M->width * M->height);

	memcpy(o, kept, luma);
	memcpy(o + luma, kept + luma + luma / 4, luma / 4);
	memcpy(o + luma + luma / 4, kept + luma, luma / 4);
	return 1;
}

static uint32_t
decode_chunk(uint32_t audio, uint32_t chunk)
{
	struct audio *x = M->audios[audio];
	int first = (int)chunk * CHUNK, end = first + CHUNK < M->sound_frames ? first + CHUNK : M->sound_frames;
	int from = first ? first - 1 : 0, f, i, v;	/* a frame early: its samples fill the filter's past */
	plm_samples_t *samples;
	int16_t *to = M->pcm + (unsigned)first * PLM_AUDIO_SAMPLES_PER_FRAME * 2;

	*x->decoder = *x->made;
	over(&x->buffer, M->audio_es + M->sound_at[from], M->sound_at[end] - M->sound_at[from]);
	x->decoder->buffer = &x->buffer;
	x->decoder->next_frame_data_size = 0;
	/* where the filter's ring would be had every frame before been decoded (36 steps of 64 a
	 * frame): the same sums in the same order, and so the same samples to the last bit */
	x->decoder->v_pos = (int)(0u - 2304u * (unsigned)from) & 1023;
	for (f = from; f < end && !M->stop && (samples = plm_audio_decode(x->decoder)) != NULL; f++) {
		if (f < first)
			continue;
		for (i = 0; i < PLM_AUDIO_SAMPLES_PER_FRAME * 2; i++) {
			v = (int)(samples->interleaved[i] * 32767.0f);
			*to++ = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
	}
	return (uint32_t)(f - first);
}

/* ---------------------------------------------------------------- the first core's side */

/* A job for unit k: on its worker, or here and now when there are none. A program's lent
 * workers may be busy with its own calls: there the job goes to one that is free, or is made here. */
static void
post(struct mpeg *m, int k, mcw_fn fn, uint32_t a0, uint32_t a1)
{
	struct unit *u = &m->units[k];

	if (m->lent) {
		if (!(u->call = mcw_try_call(fn, a0, a1)))
			u->result = fn(a0, a1);
	} else if (m->workers)
		mcw_post(k, fn, a0, a1);
	else
		u->result = fn(a0, a1);
	u->busy = 1;
}

static int
finished(struct mpeg *m, int k)
{
	struct unit *u = &m->units[k];

	if (!u->busy)
		return 0;
	if (m->lent) {
		if (u->call && !mcw_ready(u->call))
			return 0;
		if (u->call)
			u->result = mcw_join(u->call), u->call = 0;
	} else if (m->workers) {
		if (!mcw_done(k))
			return 0;
		u->result = mcw_result(k);
	}
	u->busy = 0;
	return 1;
}

/* Gives every unit that has nothing to do its next job, and takes what the others finished. */
static void
dispatch(struct mpeg *m)
{
	int k, i;

	for (k = 1; k <= m->unit_count; k++) {
		struct unit *u = &m->units[k];

		if (u->helping) {
			if (!finished(m, k))
				continue;
			m->units[u->helping].copied++;
			u->helping = 0;
		}
		if (u->state == SOUND && finished(m, k)) {
			m->chunk_done[u->gop] = 1;
			u->state = IDLE;
		} else if (u->state == DECODING && finished(m, k)) {
			if (m->seeking >= 0)
				u->state = IDLE;	/* (it ended early: what it has is of no use) */
			else if ((int)u->result < u->frames)	/* its core was wanted: the rest of the group */
				post(m, k, decode_gop, (uint32_t)u->video, (uint32_t)u->gop | AGAIN);
			else
				u->state = DECODED;
		} else if (u->state == COPYING && u->own && finished(m, k)) {
			u->own = 0;
			u->copied++;
		}
		if (u->state == DECODED && m->seeking >= 0)
			u->state = IDLE;
		/* A group goes to its places once the pictures there have been let go: a picture a job,
		 * this worker's and every worker's that has none (a worker keeps few kilobytes of stores
		 * a pass, and the group must be there in the time its places are free). */
		if (u->state == DECODED && m->gop[u->gop].first + u->frames - 1 <= m->asked + m->slots - 1) {
			u->state = COPYING;
			u->copy_next = u->copied = u->own = 0;
			m->copies_wanted += u->frames;
		}
		if (u->state == COPYING && u->copied == u->frames) {
			m->gop_state[u->gop] = 2;
			u->state = IDLE;
		}
		if (u->state == COPYING && !u->own && u->copy_next < u->frames) {
			m->copies_wanted--;
			u->own = 1;
			post(m, k, copy_one, (uint32_t)(u->video << 8 | u->copy_next),
			     (uint32_t)(m->gop[u->gop].first + u->copy_next));
			u->copy_next++;
		}
		/* (a worker whose group waits for its places has no job either) */
		if (u->state == DECODED && m->copies_wanted > 0) {
			for (i = 1; i <= m->unit_count; i++)
				if (m->units[i].state == COPYING && m->units[i].copy_next < m->units[i].frames) {
					m->copies_wanted--;
					u->helping = i;
					post(m, k, copy_one, (uint32_t)(m->units[i].video << 8 | m->units[i].copy_next),
					     (uint32_t)(m->gop[m->units[i].gop].first + m->units[i].copy_next));
					m->units[i].copy_next++;
					break;
				}
		}
		if (u->state != IDLE)
			continue;
		if (u->audio >= 0 && m->next_chunk < m->chunks) {
			u->gop = m->next_chunk++;
			u->state = SOUND;
			post(m, k, decode_chunk, (uint32_t)u->audio, (uint32_t)u->gop);
		} else if (u->video >= 0 && m->next_gop < m->gops && m->seeking < 0) {
			u->gop = m->next_gop++;
			u->frames = m->gop[u->gop].frames;
			u->began = m_now_ms();
			m->gop_state[u->gop] = 1;
			u->state = DECODING;
			post(m, k, decode_gop, (uint32_t)u->video, (uint32_t)u->gop);
		}
	}
	while (m->chunks_done < m->chunks && m->chunk_done[m->chunks_done])
		m->chunks_done++;
}

static int
pictures_idle(const struct mpeg *m)
{
	int k;

	for (k = 1; k <= m->unit_count; k++)
		if (m->units[k].helping || (m->units[k].state != IDLE && m->units[k].state != SOUND))
			return 0;
	return 1;
}

/* Places of the library's own when the program gave none: two groups' and one more. */
static int
own_places(struct media_video *v, struct mpeg *m)
{
	int k;

	m->slots = 2 * m->gop_most + 1;
	if (!(m->places = calloc((size_t)m->slots, sizeof *m->places)))
		return 0;
	for (k = 0; k < m->slots; k++)
		m->places[k] = space(m->frame_bytes);
	m->own_places = 1;
	(void)v;
	return 1;
}

static void
mpeg_pump(struct media_video *v)
{
	struct mpeg *m = v->own;

	if (!m->places) {
		if (v->places)
			m->places = v->places, m->slots = v->slots;
		else if (!own_places(v, m))
			return;
	}
	dispatch(m);
	if (m->seeking >= 0 && pictures_idle(m)) {
		/* the groups that were being decoded are gone: on from the group asked for */
		m->drop = 0;
		m->first_gop = m->next_gop = m->ready_gop = gop_of(m, m->seeking);
		memset(m->gop_state, 0, (size_t)m->gops);
		m->asked = m->gop[m->first_gop].first - 1;
		m->seeking = -1;
		dispatch(m);
	}
}

static int
mpeg_ready(struct media_video *v, int *in_time)
{
	struct mpeg *m = v->own;
	uint32_t now = m_now_ms(), asked;
	int k;

	*in_time = 0;
	if (m->seeking >= 0)
		return m->gop[gop_of(m, m->seeking)].first - 1;
	/* (groups only become ready, so the walk goes on from where it stopped) */
	for (; m->ready_gop < m->gops && m->gop_state[m->ready_gop] == 2; m->ready_gop++)
		m->ready_last = m->gop[m->ready_gop].first + m->gop[m->ready_gop].frames - 1;
	/* whether the groups being decoded will be there in time, each going on as fast as it has got
	 * to where it is (the first groups are all begun at once and end seconds apart) */
	*in_time = 1;
	asked = media_video_time(v, m->asked + 1);
	for (k = 1; k <= m->unit_count; k++) {
		struct unit *u = &m->units[k];
		int done, until;

		if (u->state != DECODING)
			continue;
		done = m->videos[u->video]->done;
		until = (int)(media_video_time(v, m->gop[u->gop].first) - asked);
		if (until < 0 || (now - u->began) * (unsigned)(u->frames - done) > (unsigned)until * (unsigned)(done ? done : 1))
			*in_time = 0;
	}
	return m->ready_gop > m->first_gop ? m->ready_last : m->asked;
}

static int
mpeg_seek(struct media_video *v, int n)
{
	struct mpeg *m = v->own;

	m->seeking = n;
	m->drop = 1;
	mpeg_pump(v);
	return m->gop[gop_of(m, n)].first;
}

static const uint8_t *
mpeg_frame(struct media_video *v, int n, const uint32_t **palette)
{
	struct mpeg *m = v->own;
	int in_time;

	(void)palette;
	if (m->seeking < 0 && (n < m->gop[m->first_gop].first || n < m->asked))
		mpeg_seek(v, n);	/* (before what is kept: from its group again) */
	if (m->seeking < 0 && n - 1 > m->asked)
		m->asked = n - 1;
	if (mpeg_ready(v, &in_time) < n) {
		mpeg_pump(v);
		if (mpeg_ready(v, &in_time) < n)
			return NULL;
	}
	m->asked = n;
	return place(m, n);
}

static uint32_t
mpeg_sound(struct media_video *v, uint32_t first, int16_t *to, uint32_t most)
{
	struct mpeg *m = v->own;
	uint32_t per = CHUNK * PLM_AUDIO_SAMPLES_PER_FRAME, c = first / per, end;

	while ((int)c < m->chunks && m->chunk_done[c])
		c++;
	end = c * per < (uint32_t)m->pcm_frames ? c * per : (uint32_t)m->pcm_frames;
	if (first >= end)
		return 0;
	if (most > end - first)
		most = end - first;
	memcpy(to, m->pcm + first * 2, (size_t)most * 4);
	return most;
}

static void
mpeg_close(struct media_video *v)
{
	struct mpeg *m = v->own;
	int k;

	m->stop = 1;	/* the groups being decoded end at their next picture */
	for (k = 1; k <= m->unit_count; k++)
		if (m->units[k].busy && m->lent && m->units[k].call)
			mcw_join(m->units[k].call);
		else if (m->units[k].busy && m->workers && !m->lent)
			mcw_wait(k);
	if (m->own_places)
		free(m->places);
	free(m->gop_state);
	free(m->chunk_done);
	for (k = m->nheld - 1; k >= 0; k--)
		m_unmap(m->held[k].at, m->held[k].bytes);
	free(m->held);
	m_unmap(m, sizeof *m);
	M = NULL;
	own_frames = NULL;
}

static const struct m_film mpeg_film = { mpeg_frame, mpeg_ready, mpeg_seek, mpeg_sound, mpeg_pump, mpeg_close, NULL };

const char *
m_mpeg_open(struct media_video *v, const uint8_t *data, uint32_t size, int workers)
{
	struct mpeg *m;
	const char *why;
	int most, decoders, wanted, k;

	if (M)
		return "an MPEG-1 film is open already (one at a time)";
	if (!(m = m_map(sizeof *m)))
		return "not enough memory for the film";
	M = m;
	m->seeking = -1;
	m->asked = -1;
	if ((why = take_apart(m, data, size)) != NULL) {
		mpeg_close(&(struct media_video){ .own = m });
		return why;
	}
	/* a decoder each for 16 workers at pictures of 320 x 176 (24 a second take some 10), more
	 * for larger ones as far as 32 MB of their groups go, and 8 for the sound */
	most = (int)((32u << 20) / ((unsigned)m->gop_most * m->frame_bytes));
	decoders = (16 * m->width * m->height + 320 * 176 - 1) / (320 * 176);
	decoders = decoders > VIDEOS ? VIDEOS : decoders > most ? most : decoders;
	wanted = workers < decoders + 8 ? workers : decoders + 8;
	m->workers = m_film_workers(v, wanted);
	m->lent = m->workers && !v->opened;
	/* (one worker: the pictures have it, and the film is silent) */
	m->sounds = !m->rate || m->workers == 1 ? 0 : m->workers >= decoders + 8 ? 8 : m->workers >= 6 ? m->workers / 4 : 1;
	m->unit_count = m->workers ? m->workers : 1 + m->sounds;
	if (m->unit_count > m->sounds + decoders)
		m->unit_count = m->sounds + decoders;
	/* the decoders, all before a worker has a job: memory brought in on this core while workers
	 * run holds them up */
	for (k = 1; k <= m->unit_count; k++) {
		m->units[k].video = m->units[k].audio = -1;
		if (k <= m->sounds)
			m->units[k].audio = audio_new(m);
		else
			m->units[k].video = video_new(m, k);
	}
	m->gop_state = calloc((size_t)m->gops + 1, 1);
	m->chunk_done = calloc((size_t)m->chunks + 1, 1);
	if (!m->gop_state || !m->chunk_done) {
		mpeg_close(&(struct media_video){ .own = m });
		return "not enough memory for the film";
	}
	mcw_touch_code((const void *)decode_gop, 4096);
	mcw_touch_code((const void *)decode_chunk, 4096);
	v->own = m;
	v->film = &mpeg_film;
	v->info.kind = MEDIA_MPEG;
	v->info.width = m->width;
	v->info.height = m->height;
	v->info.fps_milli = m->fps_milli;
	v->info.frames = m->frames;
	v->info.format = MEDIA_FRAME_YUV420;
	v->info.frame_bytes = m->frame_bytes;
	v->info.rate = m->sounds ? m->rate : 0;
	v->info.channels = m->sounds ? 2 : 0;
	v->info.sound_frames = m->sounds ? (uint32_t)m->pcm_frames : 0;
	v->info.workers = m->workers;
	v->info.least_places = 2 * m->gop_most;
	return NULL;
}
