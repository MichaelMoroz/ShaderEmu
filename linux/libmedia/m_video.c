/* libmedia's films (docs/system/media.md, "Films"): what every kind shares, and a GIF animation as a
 * film. The kinds are m_video_mpeg.c (MPEG-1, a group of pictures a worker) and m_video_vqa.c
 * (Westwood's VQA, a frame's rows over every core), m_avi.c (MJPEG, a picture over every core). */
#include <stdlib.h>
#include <time.h>
#include "m_video.h"

uint32_t
m_now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)now.tv_sec * 1000u + (uint32_t)(now.tv_nsec / 1000000);
}

int
m_film_workers(struct media_video *v, int want)
{
	int have = mcw_count();

	if (want <= 0)
		return 0;
	if (have > 0)
		return have < want ? have : want;
	have = mcw_open(want);
	v->opened = have > 0;
	return have > 0 ? have : 0;
}

void
m_film_workers_end(struct media_video *v)
{
	if (!v->opened)
		return;
	mcw_for_end();
	mcw_close();
	v->opened = 0;
}

/* ---------------------------------------------------------------- a GIF animation */

struct gif_film {
	struct media_anim *anim;
	uint32_t *at;		/* when each picture begins, in milliseconds; at[frames] the end */
};

static const uint8_t *
gif_frame(struct media_video *v, int n, const uint32_t **palette)
{
	struct gif_film *g = v->own;

	*palette = NULL;
	return media_anim_frame(g->anim, n, NULL);
}

static int gif_ready(struct media_video *v, int *in_time) { *in_time = 1; return v->info.frames - 1; }
static int gif_seek(struct media_video *v, int n) { (void)v; return n; }
static uint32_t gif_sound(struct media_video *v, uint32_t f, int16_t *t, uint32_t m) { (void)v, (void)f, (void)t, (void)m; return 0; }
static void gif_pump(struct media_video *v) { (void)v; }
static uint32_t gif_time(const struct media_video *v, int n) { return ((struct gif_film *)v->own)->at[n]; }

static void
gif_close(struct media_video *v)
{
	struct gif_film *g = v->own;

	media_anim_close(g->anim);
	free(g->at);
	free(g);
}

static const struct m_film gif_film = { gif_frame, gif_ready, gif_seek, gif_sound, gif_pump, gif_close, gif_time };

/* Every picture's codes are unpacked on the workers as it opens (media_anim_open); a picture
 * is laid on the canvas by every core when it is asked for. */
static const char *
gif_open(struct media_video *v, const uint8_t *data, uint32_t size, int workers)
{
	struct gif_film *g = calloc(1, sizeof *g);
	const char *why = NULL;
	int n, got = m_film_workers(v, workers);
	uint32_t all = 0;

	if (!g)
		return "not enough memory for the film";
	if (!(g->anim = media_anim_open(data, size, got, &why)) || !(g->at = calloc((size_t)media_anim_frames(g->anim) + 1, 4))) {
		if (g->anim)
			media_anim_close(g->anim);
		free(g);
		return why ? why : "not enough memory for the film";
	}
	v->own = g;
	v->film = &gif_film;
	v->info.kind = MEDIA_GIF;
	v->info.width = media_anim_width(g->anim);
	v->info.height = media_anim_height(g->anim);
	v->info.frames = media_anim_frames(g->anim);
	v->info.format = MEDIA_FRAME_RGBA;
	v->info.frame_bytes = ((uint32_t)v->info.width * (uint32_t)v->info.height * 4 + 15) & ~15u;
	v->info.workers = got;
	for (n = 0; n < v->info.frames; n++) {
		g->at[n] = all;
		all += (uint32_t)m_anim_delay(g->anim, n);
	}
	g->at[n] = all;
	v->info.fps_milli = all ? (int)((uint64_t)v->info.frames * 1000000u / all) : 10000;
	return NULL;
}

/* ---------------------------------------------------------------- the calls */

struct media_video *
media_video_open(const void *data, uint32_t size, int workers, const char **why)
{
	struct media_info info;
	struct media_video *v = calloc(1, sizeof *v);
	const char *wrong;

	if (!v) {
		*why = "not enough memory for the film";
		return NULL;
	}
	if ((wrong = media_probe(data, size, &info)) == NULL) {
		switch (info.kind) {
		case MEDIA_MPEG: wrong = m_mpeg_open(v, data, size, workers); break;
		case MEDIA_VQA: wrong = m_vqa_open(v, data, size, workers); break;
		case MEDIA_AVI: wrong = m_avi_open(v, data, size, workers); break;
		case MEDIA_GIF: wrong = gif_open(v, data, size, workers); break;
		default: wrong = "not a film this library plays (MPEG-1, VQA, MJPEG in AVI or GIF)"; break;
		}
	}
	if (wrong) {
		m_film_workers_end(v);
		free(v);
		*why = wrong;
		return NULL;
	}
	return v;
}

void media_video_get_info(const struct media_video *v, struct media_video_info *info) { *info = v->info; }

const uint8_t *
media_video_frame(struct media_video *v, int n, const uint32_t **palette)
{
	const uint32_t *none;

	if (!palette)
		palette = &none;
	*palette = NULL;
	if (n < 0 || n >= v->info.frames)
		return NULL;
	return v->film->frame(v, n, palette);
}

uint32_t
media_video_sound(struct media_video *v, uint32_t first, int16_t *to, uint32_t most)
{
	return v->info.rate ? v->film->sound(v, first, to, most) : 0;
}

void media_video_pump(struct media_video *v) { v->film->pump(v); }

int
media_video_ready(struct media_video *v, int *in_time)
{
	int ignored;

	return v->film->ready(v, in_time ? in_time : &ignored);
}

int
media_video_seek(struct media_video *v, int n)
{
	n = n < 0 ? 0 : n >= v->info.frames ? v->info.frames - 1 : n;
	return v->film->seek(v, n);
}

/* n * 1000000 / f and ms * f / 1000000 in 32-bit divides where they fit: a 64-bit divide is a call of about
 * 400 instructions, and a film's player asks for times at every poll (nxvideo's mpeg_ready: 34% of core 0) */
static uint32_t
frames_ms(uint32_t n, uint32_t f)
{
	uint32_t a;

	if (n <= 4294u)
		return n * 1000000u / f;
	if (n >= 4294967u || f >= 4294967u)
		return (uint32_t)((uint64_t)n * 1000000u / f);
	a = n * 1000u;	/* a * 1000 / f = 1000 (a / f) + 1000 (a % f) / f */
	return a / f * 1000u + a % f * 1000u / f;
}

static uint32_t
ms_frames(uint32_t ms, uint32_t f)
{
	uint32_t s = ms / 1000u, u;

	if (f >= 4294967u || (s && s > (0xffffffffu - 4294967u) / f))
		return (uint32_t)((uint64_t)ms * f / 1000000u);
	/* ms = 1000 s + t: (1000 s f + t f) / 1000000 = (s f + t f / 1000) / 1000, every floor taken */
	u = ms % 1000u * f / 1000u;
	return (s * f + u) / 1000u;
}

uint32_t
media_video_time(const struct media_video *v, int n)
{
	n = n < 0 ? 0 : n > v->info.frames ? v->info.frames : n;
	if (v->film->time)
		return v->film->time(v, n);
	return frames_ms((uint32_t)n, (uint32_t)v->info.fps_milli);
}

int
media_video_at(const struct media_video *v, uint32_t ms)
{
	int low = 0, high = v->info.frames - 1, middle;

	if (!v->film->time) {
		uint32_t n = ms_frames(ms, (uint32_t)v->info.fps_milli);

		return n >= (uint32_t)v->info.frames ? v->info.frames - 1 : (int)n;
	}
	while (low < high) {
		middle = (low + high + 1) / 2;
		if (v->film->time(v, middle) <= ms)
			low = middle;
		else
			high = middle - 1;
	}
	return low;
}

const char *
media_video_places(struct media_video *v, uint8_t *const *places, int slots)
{
	int k;

	if (v->info.format == MEDIA_FRAME_RGBA)
		return "an animation's pictures are its canvas: it has no places";
	if (slots < 2 || slots < v->info.least_places || !(v->places = malloc((size_t)slots * sizeof *v->places)))
		return "too few places for this film's pictures";
	for (k = 0; k < slots; k++) {
		v->places[k] = places[k];
		mcw_touch(places[k], v->info.frame_bytes);
	}
	v->slots = slots;
	return NULL;
}

void
media_video_close(struct media_video *v)
{
	if (!v)
		return;
	v->film->close(v);
	m_film_workers_end(v);
	free(v->places);
	free(v);
}
