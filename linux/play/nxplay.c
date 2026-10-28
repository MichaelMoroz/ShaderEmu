/*
 * nxplay: an audio player (docs/apps/play.md). nxplay FILE plays a WAV, MOD, FLAC, MP3 or Ogg Vorbis
 * file through the sound card's stream voice, decoded by libmedia (docs/system/media.md). A tune is
 * decoded in pieces, ahead of what is heard; an MP3, Vorbis or FLAC file's pieces go to the
 * worker cores. The NXPLAY_* variables are in docs/apps/play.md.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include "ui.h"
#include "ui_files.h"
#include "ui_glyph.h"
#include "ui_ask.h"
#include "ui_menu.h"
#include <dirent.h>
#include "shaderemu_sound.h"
#include "mcw.h"
#include "media.h"

#define RING ring_frames	/* the stream's ring, in frames: a power of two */
#define RING_LONG 262144u	/* six seconds of 44.1 kHz: room for every piece that is being decoded */
#define RING_SHORT 65536u	/* when sound memory has no megabyte left */
#define SLICE 2048	/* frames this core decodes between two looks at everything else */
#define OWN_UNDER 8	/* with fewer workers than this the first core takes pieces too */
#define FLAC_WORKERS 24	/* a FLAC file's: a block is a tenth of a second, a worker's for half a second */
#define SLOTS 2048
#define KINDS ".wav .mp3 .flac .mod .ogg .oga"
#define TUNES 512

enum { K_NONE, K_WAV, K_MOD, K_FLAC, K_MP3, K_OGG };
enum { STOPPED, BUFFERING, PLAYING, ENDED };
static int buffered;	/* how much of what it waits for before it plays is decoded, in hundredths */
enum { SLOT_FREE, SLOT_MINE, SLOT_WORKER, SLOT_READY, SLOT_DROPPED };

/* Frames are the library's: at half rate a frame is two of the file's. */
static struct {
	char path[512];
	const char *name;
	uint8_t *file;
	uint32_t size;
	struct media_audio *audio;
	int kind, rate, channels, half;
	int pieced;		/* the library's pieces, on the workers (else read in order on this core) */
	uint32_t total;		/* ~0u while not known */
	uint32_t piece;		/* frames a piece (the library's first, when pieced) */
	uint32_t origin;	/* the frame piece 0 begins at, in order */
} tune;

/* A piece's memory: 16 bytes its worker writes (frames done), 16 this core writes (give up),
 * then the samples. */
static struct slot {
	uint32_t *head;
	int16_t *pcm, *own;	/* where its samples go: its own memory, or its place in the ring */
	uint32_t piece, frames, done;
	int state, direct;
} slots[SLOTS];
static int slot_count, slot_most;
static uint32_t slot_bytes;
static struct slot *mine, *job[MC_MAX_CORES];
static int workers, workers_wanted = MC_MAX_CORES - 1;

static struct {
	int on, voice, volume, running;
	int16_t *ring;
	uint32_t ring_at;	/* the ring's physical address */
	uint32_t written;
	uint32_t base;		/* the tune's frame at the ring's first */
	uint32_t first;		/* and the piece that begins there */
} out;
static uint32_t ring_frames = RING_SHORT;

static int state, paused, quit;
static uint32_t play_piece, play_at, next_piece;
static uint32_t consumed;	/* without a card: frames taken */
static uint32_t decoded, work_ms, last_ms;	/* since the last seek, for the speed */
static int was_working, decided, half_wanted = -1, underruns;
static uint64_t own_cycles, worker_cycles, all_decoded;
static uint32_t began_ms, began_cycles, limit_seconds, memory_mb;
static int want_sum, exit_at_end, mod_rate = 22050, piece_frames_asked;
static int own_under = OWN_UNDER;
static uint32_t sum = 2166136261u, sum_count;
static char note[160];

static GR_WINDOW_ID window;
static int window_on, win_w = 480, win_h = 420;
static int drag, drag_value, drag_moved, play_on_release, shown_second = -1, shown_state = -1;
/* The list: any tunes, in the order they play; `ms` is a tune's length, -1 while not
 * measured, -2 when it cannot be played, -3 when its file does not say. */
struct entry { char *path; int ms; };
static struct entry *tunes[TUNES], *order[TUNES], *playing, *resume_entry;
static struct ui_list list = { 8, 150, 464, 200, 0, 0, -1 };
static int repeat, shuffle, muted, remaining;	/* repeat: 0 off, 1 the list, 2 the tune */
static int order_dirty = 1, list_changed, resume_ms;
static uint32_t list_changed_ms, saved_ms;
static int want_next;		/* the tune ended: the next of the list is to be played */
enum { D_MENUS = 1, D_PANEL = 2, D_SEEK = 4, D_KEYS = 8, D_LIST = 16, D_STATUS = 32, D_ALL = 63 };
static int dirty;		/* the parts of the window to draw again */

static uint32_t
cycles(void)
{
	uint32_t n;

	__asm__ volatile("rdcycle %0" : "=r"(n));
	return n;
}

static uint32_t
now_ms(void)
{
	struct timeval t;

	gettimeofday(&t, NULL);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_usec / 1000);
}

static void
say(const char *text)
{
	snprintf(note, sizeof note, "%s", text);
	printf("nxplay: %s\n", text);
	fflush(stdout);
	dirty |= D_STATUS;
}

static uint32_t
pieces(void)
{
	if (tune.pieced)
		return media_audio_piece_of(tune.audio, tune.total - 1) + 1;
	return tune.total == ~0u ? ~0u : (tune.total - tune.origin + tune.piece - 1) / tune.piece;
}

/* The frame a piece begins at, and its frames as decoded (`whole`) or as played (up to the tune's end). */
static uint32_t
piece_start(uint32_t piece)
{
	return tune.pieced ? media_audio_piece_first(tune.audio, piece) : tune.origin + piece * tune.piece;
}

static uint32_t
piece_size(uint32_t piece, int whole)
{
	uint32_t from = piece_start(piece), n = tune.pieced ? media_audio_piece_frames(tune.audio, piece) : tune.piece;

	return whole || tune.total - from >= n ? n : tune.total - from;
}

static uint32_t
piece_frames(uint32_t piece)
{
	return piece_size(piece, 0);
}

/* ---- a piece on a worker ---- */

/* A worker's job: piece `piece` into the slot at `at` (or the place its head names) with its
 * own decoder, a step at a time: it says how far it is, and stops when told to. */
static uint32_t
piece_job(uint32_t piece, uint32_t at)
{
	volatile uint32_t *head = (volatile uint32_t *)at;
	int16_t *to = head[5] ? (int16_t *)head[5] : (int16_t *)(at + 32);
	uint32_t before = cycles(), want = media_audio_piece_frames(tune.audio, piece), done = 0;
	int d = (int)head[7];

	while (done < want && !head[4]) {
		done = media_audio_piece_step(tune.audio, d, piece, to, done);
		head[0] = done;
	}
	return cycles() - before;
}

/* ---- the card ---- */

/* The frame of the ring the card is at. */
static uint32_t
card_at(void)
{
	return SND_STATE[out.voice].seen != SND_VOICE[out.voice].key ? 0 : SND_STATE[out.voice].position;
}

/* A step of 0 holds the voice where it is. */
static void
voice_run(int on)
{
	out.running = on;
	if (out.on)
		SND_VOICE[out.voice].step = on ? snd_step((uint32_t)tune.rate) : 0;
}

/* The tune's frame that is heard now. */
static uint32_t
position(void)
{
	uint32_t at = out.base + (out.on ? card_at() : consumed);

	return state == ENDED || at > tune.total ? tune.total : at;
}

/* ---- pieces ---- */

/* A slot for one more piece while fewer than `most` are in use; `look`: only whether there is one. */
static struct slot *
slot_get(int most, int look)
{
	struct slot *s = NULL;
	uint8_t *memory;
	int i, used = 0;

	for (i = 0; i < slot_count; i++) {
		if (slots[i].state != SLOT_FREE)
			used++;
		else if (!s)
			s = &slots[i];
	}
	if (used >= most || s || slot_count >= slot_most)
		return used >= most ? NULL : s;
	if (look)
		return &slots[slot_count];
	memory = mmap(NULL, slot_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED) {
		slot_most = slot_count;
		return NULL;
	}
	/* its pages there before a worker stores to them */
	mcw_touch(memory, slot_bytes);
	s = &slots[slot_count++];
	s->head = (uint32_t *)memory;
	s->pcm = s->own = (int16_t *)(memory + 32);
	return s;
}

static struct slot *
slot_of(uint32_t piece)
{
	int i;

	for (i = 0; i < slot_count; i++)
		if (slots[i].state == SLOT_READY && slots[i].piece == piece)
			return &slots[i];
	return NULL;
}

/*
 * A piece's place in the ring, when it has one now: it is decoded there, and copied by nobody.
 * The stream's frames are the pieces' in order, so the place is known before the piece is
 * made; it must be clear of what is still to be heard, and not go round the ring's end.
 * `far`: it has none because the ring is full that far ahead.
 */
static int16_t *
piece_place(uint32_t piece, int *far)
{
	uint32_t first = piece_start(piece) - out.base, n = piece_size(piece, 1);
	uint32_t at = first & (RING - 1);

	*far = out.on && first + n > card_at() + RING;
	if (!out.on || *far || at + n > RING)
		return NULL;
	return out.ring + at * (uint32_t)tune.channels;
}

/* Whether a worker still writes for a piece that was dropped: its place may be another's by now. */
static int
dropping(void)
{
	int i;

	for (i = 0; i < slot_count; i++)
		if (slots[i].state == SLOT_DROPPED)
			return 1;
	return 0;
}

/* Forgets every piece decoded or begun; a worker's is given back when it has heard. */
static void
drop_all(void)
{
	int i;

	for (i = 0; i < slot_count; i++) {
		if (slots[i].state == SLOT_WORKER) {
			slots[i].head[4] = 1;
			slots[i].state = SLOT_DROPPED;
		} else if (slots[i].state != SLOT_DROPPED) {
			slots[i].state = SLOT_FREE;
		}
	}
	mine = NULL;
}

/* Frames decoded since the last seek, the workers' unfinished pieces included. */
static uint32_t
progress(void)
{
	uint32_t frames = decoded;
	int k;

	for (k = 1; k <= workers; k++)
		if (job[k] && job[k]->state == SLOT_WORKER)
			frames += job[k]->head[0];
	return frames;
}

/* Sound decoded for a second of decoding, in thousandths; 0 while too little is known. */
static uint32_t
speed(void)
{
	if (work_ms < 300)
		return 0;
	return (uint32_t)((uint64_t)progress() * 1000000u / ((uint64_t)work_ms * (uint32_t)tune.rate)) + 1;
}

static void
seek_to(uint32_t frame)
{
	drop_all();
	if (tune.total != ~0u && frame >= tune.total)
		frame = tune.total ? tune.total - 1 : 0;
	if (tune.pieced) {
		play_piece = next_piece = media_audio_piece_of(tune.audio, frame);
		out.base = piece_start(play_piece);
	} else {
		tune.origin = out.base = media_audio_seek(tune.audio, frame);
		play_piece = next_piece = 0;
	}
	out.first = play_piece;
	play_at = consumed = 0;
	decoded = work_ms = 0;
	was_working = 0;
	out.written = 0;
	if (out.on)
		snd_play(out.voice, SND_STREAM | SND_SMOOTH | (tune.channels == 2 ? SND_STEREO : 0), out.ring_at, RING, 0, 0,
			 SND_VOLUME(out.volume, out.volume));
	out.running = 0;
	state = BUFFERING, buffered = 0;
}

/* One step of this core's own piece: a step of the library's piece (an MP3 frame, a FLAC
 * block...), or a slice of a tune read in order. */
static void
own_step(void)
{
	struct slot *s = mine;
	uint32_t want = piece_frames(s->piece), before = cycles(), was = s->done;

	if (tune.pieced) {
		s->done = media_audio_piece_step(tune.audio, 0, s->piece, s->pcm, s->done);
	} else {
		int ask = want - s->done > SLICE ? SLICE : (int)(want - s->done);
		int got = ask > 0 ? media_audio_read(tune.audio, s->pcm + s->done * (uint32_t)tune.channels, ask) : 0;

		s->done += (uint32_t)got;
		if (got < ask) {
			/* the tune's end, sooner than it said or where it did not say */
			tune.total = tune.origin + s->piece * tune.piece + s->done;
			want = s->done;
		}
	}
	decoded += s->done - was;
	all_decoded += s->done - was;
	own_cycles += cycles() - before;
	if (s->done >= want) {
		s->frames = want;
		s->state = SLOT_READY;
		mine = NULL;
	}
}

/* Finished pieces into the ring, in order, as far as it has room: the few that were not
 * decoded in it are copied. */
static void
feed(void)
{
	struct slot *s;

	while ((s = slot_of(play_piece)) != NULL) {
		const int16_t *from = s->pcm + play_at * (uint32_t)tune.channels;
		uint32_t n = s->frames - play_at, i;

		if (!n) {
			s->state = SLOT_FREE;
			play_piece++;
			play_at = 0;
			continue;
		}
		if (s->direct) {
			/* (it was decoded where it lies: the card is told, and that is all) */
			out.written += n;
			SND_VOICE[out.voice].loop = out.written;
		} else if (out.on) {
			uint32_t room = RING - (out.written - card_at()), at = out.written & (RING - 1);

			n = n > room ? room : n;
			n = n > RING - at ? RING - at : n;
			if (!n)
				return;
			memcpy(out.ring + at * (uint32_t)tune.channels, from, n * (uint32_t)tune.channels * 2);
			out.written += n;
			SND_VOICE[out.voice].loop = out.written;
		} else {
			consumed += n;
		}
		if (want_sum) {
			for (i = 0; i < n * (uint32_t)tune.channels; i++)
				sum = (sum ^ (uint16_t)from[i]) * 16777619u;
			sum_count += n * (uint32_t)tune.channels;
		}
		play_at += n;
	}
}

static void
stats(void)
{
	uint32_t ms = now_ms() - began_ms, all = cycles() - began_cycles;
	uint64_t per = all_decoded ? (own_cycles + worker_cycles) * (uint32_t)tune.rate / all_decoded : 0;
	static const char *kinds[] = { "", "wav", "mod", "flac", "mp3", "ogg" };

	if (!all_decoded)
		return;
	printf("playstat: %s %d Hz %d ch%s, %u ms of sound decoded in %u ms: %u thousand instructions a second of sound "
	       "(this core %u, %d workers %u), this core ran %u thousand in all, %d underruns\n",
	       kinds[tune.kind], tune.rate, tune.channels, tune.half ? " at half rate" : "",
	       (uint32_t)(all_decoded * 1000 / (uint32_t)tune.rate), ms, (uint32_t)(per / 1000),
	       (uint32_t)(own_cycles * (uint32_t)tune.rate / all_decoded / 1000), workers,
	       (uint32_t)(worker_cycles * (uint32_t)tune.rate / all_decoded / 1000), all / 1000, underruns);
	if (want_sum)
		printf("nxplay: sum %08x of %u samples\n", sum, sum_count);
	fflush(stdout);
	all_decoded = own_cycles = worker_cycles = 0;
}

static void
finish(void)
{
	voice_run(0);
	state = ENDED;
	stats();
	if (exit_at_end || !window_on)
		quit = 1;
	else
		want_next = 1;
}

/* What the library says of the tune now (its rate and frames are halved at half rate). */
static void
tune_info(void)
{
	struct media_audio_info info;

	media_audio_get_info(tune.audio, &info);
	tune.rate = info.rate;
	tune.channels = info.channels;
	tune.half = info.half;
	tune.total = info.frames ? info.frames : ~0u;
	if (limit_seconds && tune.total > limit_seconds * (uint32_t)tune.rate)
		tune.total = limit_seconds * (uint32_t)tune.rate;
	if (tune.pieced)
		tune.piece = media_audio_piece_frames(tune.audio, 0);
}

/* Drops every piece and waits for the workers to leave theirs (at their next step). */
static void
settle(void)
{
	int k;

	drop_all();
	for (k = 1; k <= workers; k++)
		if (job[k]) {
			mcw_wait(k);
			worker_cycles += mcw_result(k);
			job[k]->state = SLOT_FREE;
			job[k] = NULL;
		}
}

/* An MP3 file from where it is on at half the rate. */
static void
go_half(const char *why)
{
	uint32_t at = position();

	settle();
	media_audio_half(tune.audio, 1);
	tune_info();
	say(why);
	seek_to(at / 2);
}

/* Whether to play, wait for more, or change how it is decoded. */
static void
decide(int stuck)
{
	uint32_t count = pieces(), ahead, left, need, rate, s, p;
	struct slot *ready;

	if (state != BUFFERING && state != PLAYING)
		return;
	if (!out.on) {
		if (play_piece >= count)
			finish();
		return;
	}
	ahead = out.written - card_at();
	if (state == PLAYING) {
		if (ahead || paused)
			return;
		if (play_piece >= count) {
			finish();
		} else {
			voice_run(0);
			state = BUFFERING, buffered = 0;
			underruns++;
			/* With workers this is the one sign that they do not keep up: the second time, an
			 * MP3 file goes on at half the rate. (Their first two seconds say nothing: they are
			 * being started, and it was taken for slowness: half the rate and 13 s of waiting
			 * for a tune that was decoded 1.8 times as fast as it plays.) */
			if (workers && tune.kind == K_MP3 && half_wanted < 0 && !tune.half && underruns >= 2) {
				go_half("it ran dry twice: decoding it at half the rate");
			} else {
				say("it ran dry: waiting for more to be decoded");
			}
		}
		return;
	}
	for (p = play_piece; (ready = slot_of(p)) != NULL; p++)
		ahead += ready->frames - (p == play_piece ? play_at : 0);
	s = speed();
	if (!decided && !workers && work_ms >= 2000 && p < count) {
		decided = 1;
		/* too slow: an MP3 file at half the rate first, and if that is too slow too, or for
		 * the other kinds, as much decoded before it plays as it takes to play without a gap */
		if (s < 1050 && tune.kind == K_MP3 && half_wanted < 0 && !tune.half) {
			decided = 0;
			go_half("cannot decode this as fast as it plays: decoding it at half the rate");
			return;
		}
		if (s < 1050)
			say("cannot decode this as fast as it plays: decoding ahead before it plays");
	}
	rate = (uint32_t)tune.rate;
	left = tune.total == ~0u ? 0 : tune.total - position();
	/* at that speed the rest is decoded while this much plays */
	need = s >= 1100 ? rate / 2 : left - (uint32_t)((uint64_t)left * s / 1100) + rate;
	if (workers)
		need = rate * 3 / 2;	/* (a second and a half in hand, whatever their start looked like) */
	if (need && (int)((uint64_t)ahead * 100 / need) != buffered) {
		buffered = (int)((uint64_t)ahead * 100 / need);
		dirty |= D_STATUS;
	}
	if (p >= count || stuck || (s && ahead >= need)) {
		state = PLAYING;
		voice_run(!paused);
		if (paused)
			return;
		printf("nxplay: plays from %u ms of the tune, %u ms after it was opened, with %u ms decoded ahead\n",
		       (uint32_t)((uint64_t)position() * 1000 / (uint32_t)tune.rate), now_ms() - began_ms,
		       (uint32_t)((uint64_t)ahead * 1000 / rate));
		fflush(stdout);
	}
}

/* Everything that goes on by itself; true while this core has decoding to do. */
static int
pump(void)
{
	uint32_t count = pieces(), now = now_ms(), s;
	struct slot *slot;
	int16_t *place;
	int k, most, far, held, working = 0, active = state == BUFFERING || state == PLAYING;

	if (was_working)
		work_ms += now - last_ms;
	last_ms = now;
	for (k = 1; k <= workers; k++) {
		if (!(slot = job[k]) || !mcw_done(k))
			continue;
		worker_cycles += mcw_result(k);
		job[k] = NULL;
		if (slot->state == SLOT_DROPPED) {
			slot->state = SLOT_FREE;
			continue;
		}
		slot->frames = piece_frames(slot->piece);
		slot->state = SLOT_READY;
		decoded += piece_frames(slot->piece);
		all_decoded += piece_frames(slot->piece);
	}
	/* as far ahead as memory lets it when decoding is slow, a few pieces when it is not */
	s = speed();
	most = s && s < 1100 && !workers ? slot_most : workers + 4;
	held = active && dropping();
	for (k = 1; k <= workers && active; k++) {
		if (job[k]) {
			working = 1;
		} else if (!held && next_piece < count && ((place = piece_place(next_piece, &far)) != NULL || !far) &&
			   (slot = slot_get(most, 0)) != NULL) {
			/* (with workers a piece the ring has no room for yet waits: there are seconds in hand) */
			slot->piece = next_piece++;
			slot->state = SLOT_WORKER;
			slot->head[0] = slot->head[4] = 0;
			slot->head[5] = (uint32_t)place;
			slot->head[7] = (uint32_t)k;
			slot->direct = place != NULL;
			slot->pcm = place ? place : slot->own;
			job[k] = slot;
			mcw_post(k, piece_job, slot->piece, (uint32_t)slot->head);
			working = 1;
		}
	}
	/* with workers enough the first core decodes nothing: it gives pieces out sooner for it */
	if (!mine && !held && active && next_piece < count && (workers < own_under || !workers) &&
	    ((place = piece_place(next_piece, &far)) != NULL || !far || !workers) &&
	    (mine = slot_get(most, 0)) != NULL) {
		mine->piece = next_piece++;
		mine->state = SLOT_MINE;
		mine->done = 0;
		mine->direct = place != NULL;
		mine->pcm = place ? place : mine->own;
	}
	if (mine)
		own_step();
	feed();
	decide(active && !working && !mine && !held && next_piece < count && !slot_get(most, 1));
	was_working = working || mine != NULL;
	return mine != NULL;
}

/* ---- a file ---- */

static void
unload(void)
{
	int i;

	settle();
	if (out.on && tune.kind)
		snd_stop(out.voice);
	stats();
	for (i = 0; i < slot_count; i++)
		munmap(slots[i].head, slot_bytes);
	slot_count = 0;
	if (tune.pieced && tune.kind == K_FLAC) {	/* (a FLAC file's workers are fewer than another kind's) */
		mcw_close();
		workers = 0;
	}
	if (tune.audio)
		media_audio_close(tune.audio);
	free(tune.file);
	memset(&tune, 0, sizeof tune);
	state = STOPPED;
}

/* Up to `most` workers, or those it has when they are no more than that. */
static void
workers_open(int most)
{
	extern char __DATA_BEGIN__[], _end[];

	if (workers > most) {
		mcw_close();
		workers = 0;
	}
	if (workers || workers_wanted <= 0)
		return;
	workers = mcw_open(workers_wanted < most ? workers_wanted : most);
	/* what starts as zeros is not there for a worker until this core has touched it */
	if (workers)
		mcw_touch(__DATA_BEGIN__, (unsigned)(_end - __DATA_BEGIN__));
	if (workers && out.on)
		mcw_touch(out.ring, RING * 4);
}

/* Why not, or NULL with the tune ready to play from its start. */
static const char *
load(const char *path)
{
	FILE *file = fopen(path, "rb");
	struct sysinfo memory;
	struct media_audio_info info;
	const char *why;
	uint64_t room;
	long size;

	if (!file)
		return "cannot open the file";
	unload();
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	rewind(file);
	if (size < 64 || !(tune.file = malloc((size_t)size)) || fread(tune.file, 1, (size_t)size, file) != (size_t)size) {
		fclose(file);
		free(tune.file);
		tune.file = NULL;
		return "cannot read the file";
	}
	fclose(file);
	tune.size = (uint32_t)size;
	snprintf(tune.path, sizeof tune.path, "%s", path);
	tune.name = strrchr(tune.path, '/') ? strrchr(tune.path, '/') + 1 : tune.path;
	if (!(tune.audio = media_audio_open(tune.file, tune.size, &why))) {
		free(tune.file);
		tune.file = NULL;
		return why;
	}
	media_audio_get_info(tune.audio, &info);
	tune.kind = info.kind == MEDIA_WAV ? K_WAV : info.kind == MEDIA_MOD ? K_MOD : info.kind == MEDIA_FLAC ? K_FLAC :
		    info.kind == MEDIA_MP3 ? K_MP3 : K_OGG;
	if (tune.kind == K_MOD)
		media_audio_mix_rate(tune.audio, mod_rate);
	if (tune.kind == K_MP3 && half_wanted > 0)
		media_audio_half(tune.audio, 1);
	tune_info();
	/* An MP3, Vorbis or FLAC file's pieces go to the workers (a FLAC file's to fewer: a block is
	 * a tenth of a second); WAV and MOD are one core's work, and so is any kind without workers. */
	if (info.pieces && workers_wanted > 0 && (tune.kind == K_MP3 || tune.kind == K_OGG || tune.kind == K_FLAC))
		workers_open(tune.kind == K_FLAC ? FLAC_WORKERS : MC_MAX_CORES);
	tune.pieced = info.pieces && workers > 0 && tune.kind != K_WAV && tune.kind != K_MOD &&
		      media_audio_decoders(tune.audio, workers + 1) == workers + 1;
	if (tune.pieced) {
		uint32_t unit = (media_audio_set_piece(tune.audio, 1), media_audio_piece_frames(tune.audio, 0));

		/* MP3: half a second; with many workers (63 make 8 s of sound in 2.5 s) four frames, heard
		 * a piece sooner. FLAC: blocks of a sixteenth of a second (half a second with few
		 * workers). Vorbis: a second, half with many (a piece begins with a seek). */
		if (tune.kind == K_MP3)
			media_audio_set_piece(tune.audio, piece_frames_asked > 0 ? (uint32_t)piece_frames_asked * unit :
					      workers >= 24 ? 4 * unit : (uint32_t)tune.rate / 2);
		else if (tune.kind == K_FLAC)
			media_audio_set_piece(tune.audio, (uint32_t)tune.rate / (workers >= 12 ? 16 : 2));
		else
			media_audio_set_piece(tune.audio, (uint32_t)tune.rate / (workers >= 24 ? 2 : 1));
		tune_info();
	} else {
		/* the workers are someone else's meanwhile */
		mcw_close();
		workers = 0;
		tune.piece = (uint32_t)tune.rate / 2 > 4096 ? (uint32_t)tune.rate / 2 : 4096;
	}
	/* a frame more than a piece: a frame that is not what the table says is caught after it is written */
	slot_bytes = (32 + (tune.piece + 1152) * (uint32_t)tune.channels * 2 + 4095) & ~4095u;
	sysinfo(&memory);
	room = memory_mb ? (uint64_t)memory_mb << 20 : (uint64_t)memory.freeram * memory.mem_unit / 2;
	slot_most = room / slot_bytes > SLOTS ? SLOTS : (int)(room / slot_bytes);
	if (slot_most < workers + 4)
		slot_most = workers + 4;
	decided = underruns = 0;
	sum = 2166136261u;
	sum_count = 0;
	began_ms = now_ms();
	began_cycles = cycles();
	note[0] = 0;
	paused = 0;
	seek_to(0);
	return NULL;
}

/* ---- the list ---- */

#define LIST_KEPT 12	/* the registry's keys for the list (nxplay.list.N), CHUNK bytes each at most */
#define CHUNK 250

static const char *
title_of(const char *path, char *to, int room)
{
	const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path, *dot = strrchr(base, '.');

	snprintf(to, (size_t)room, "%.*s", dot && dot > base ? (int)(dot - base) : (int)strlen(base), base);
	return to;
}

/* "m:ss", or "h:mm:ss" for an hour or more. */
static char *
time_text(char *to, int room, int ms)
{
	int s = ms / 1000;

	if (s >= 3600)
		snprintf(to, (size_t)room, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(to, (size_t)room, "%d:%02d", s / 60, s % 60);
	return to;
}

static int
index_of(const struct entry *e)
{
	int i;

	for (i = 0; e && i < list.count; i++)
		if (tunes[i] == e)
			return i;
	return -1;
}

static struct entry *
find_tune(const char *path)
{
	int i;

	for (i = 0; i < list.count; i++)
		if (!strcmp(tunes[i]->path, path))
			return tunes[i];
	return NULL;
}

/* The list changed: kept in the registry once it has been still a while. */
static void
listed(void)
{
	order_dirty = list_changed = 1;
	list_changed_ms = now_ms();
	dirty |= D_LIST | D_STATUS | D_KEYS;
}

/* A tune into the list at row `at` (-1: at its end); NULL when the list is full. */
static struct entry *
add_tune(const char *path, int at)
{
	struct entry *e = malloc(sizeof *e);

	if (!e || list.count >= TUNES || !(e->path = strdup(path))) {
		free(e);
		return NULL;
	}
	e->ms = -1;
	at = at < 0 || at > list.count ? list.count : at;
	memmove(&tunes[at + 1], &tunes[at], (size_t)(list.count - at) * sizeof tunes[0]);
	tunes[at] = e;
	list.count++;
	if (list.selected >= at)
		list.selected++;
	listed();
	return e;
}

static int
by_name(const void *a, const void *b)
{
	return strcasecmp(*(char *const *)a, *(char *const *)b);
}

/* Every tune of a folder not in the list yet, by name; how many. */
static int
add_folder(const char *folder)
{
	static char *names[TUNES];
	char path[800];
	struct dirent *item;
	DIR *d = opendir(folder);
	int n = 0, i, added = 0;

	while (d && n < TUNES && (item = readdir(d)) != NULL)
		if (item->d_name[0] != '.' && ui_files_wanted(item->d_name, KINDS) && (names[n] = strdup(item->d_name)) != NULL)
			n++;
	if (d)
		closedir(d);
	qsort(names, (size_t)n, sizeof names[0], by_name);
	for (i = 0; i < n; i++) {
		snprintf(path, sizeof path, "%s%s%s", folder, folder[0] && folder[1] ? "/" : "", names[i]);
		if (!find_tune(path) && add_tune(path, -1))
			added++;
		free(names[i]);
	}
	return added;
}

static void
folder_of(const char *path, char *to, int room)
{
	const char *slash = strrchr(path, '/');

	snprintf(to, (size_t)room, "%.*s", slash > path ? (int)(slash - path) : 1, slash ? path : "/");
}

/* Takes row `row` out of the list; the tune goes on playing if it is the one played. */
static void
remove_row(int row)
{
	struct entry *e = tunes[row];
	int most;

	memmove(&tunes[row], &tunes[row + 1], (size_t)(list.count - row - 1) * sizeof tunes[0]);
	list.count--;
	if (e == playing)
		playing = NULL;
	if (e == resume_entry)
		resume_entry = NULL;
	free(e->path);
	free(e);
	if (list.selected >= list.count)
		list.selected = list.count - 1;
	most = list.count - ui_list_rows(&list);
	if (list.top > (most > 0 ? most : 0))
		list.top = most > 0 ? most : 0;
	listed();
}

static void
move_row(int from, int to)
{
	struct entry *e;

	if (from < 0 || to < 0 || from >= list.count || to >= list.count || from == to)
		return;
	e = tunes[from];
	if (from < to)
		memmove(&tunes[from], &tunes[from + 1], (size_t)(to - from) * sizeof tunes[0]);
	else
		memmove(&tunes[to + 1], &tunes[to], (size_t)(from - to) * sizeof tunes[0]);
	tunes[to] = e;
	list.selected = to;
	ui_list_show(&list);
	listed();
}

static int
by_title(const void *a, const void *b)
{
	const struct entry *x = *(struct entry *const *)a, *y = *(struct entry *const *)b;
	char p[160], q[160];

	return strcasecmp(title_of(x->path, p, sizeof p), title_of(y->path, q, sizeof q));
}

static int
by_length(const void *a, const void *b)
{
	const struct entry *x = *(struct entry *const *)a, *y = *(struct entry *const *)b;

	return x->ms - y->ms;
}

/* 0 by title, 1 by length, 2 any order. */
static void
sort_list(int how)
{
	struct entry *picked = list.selected >= 0 ? tunes[list.selected] : NULL, *e;
	int i, j;

	if (how == 2) {
		for (i = list.count - 1; i > 0; i--)
			j = rand() % (i + 1), e = tunes[i], tunes[i] = tunes[j], tunes[j] = e;
	} else {
		qsort(tunes, (size_t)list.count, sizeof tunes[0], how ? by_length : by_title);
	}
	list.selected = index_of(picked);
	ui_list_show(&list);
	listed();
}

/* Shuffle's order: every tune once, the one playing first. */
static void
make_order(void)
{
	struct entry *e;
	int i, j;

	memcpy(order, tunes, (size_t)list.count * sizeof tunes[0]);
	for (i = list.count - 1; i > 0; i--)
		j = rand() % (i + 1), e = order[i], order[i] = order[j], order[j] = e;
	for (i = 1; i < list.count; i++)
		if (order[i] == playing)
			order[i] = order[0], order[0] = playing;
	order_dirty = 0;
}

/* The tune `by` on from the one played; one that ended by itself (`ended`) is followed after
 * the list's last only when repeating. NULL: none. */
static struct entry *
neighbour(int by, int ended)
{
	int n = list.count, i;

	if (!n)
		return NULL;
	if (ended && repeat == 2 && playing)
		return playing;
	if (shuffle && n > 1) {
		if (order_dirty)
			make_order();
		for (i = 0; i < n && order[i] != playing; i++)
			;
		i = (i == n ? (by > 0 ? -1 : 0) : i) + by;
		if (i >= n) {
			if (ended && !repeat)
				return NULL;
			make_order();	/* (a new round: the one just played is its first, and is not played again) */
			return order[order[0] == playing ? 1 : 0];
		}
		return order[i < 0 ? n - 1 : i];
	}
	i = index_of(playing);
	i = (i < 0 ? (by > 0 ? -1 : n) : i) + by;
	if (i >= n) {
		if (ended && !repeat)
			return NULL;
		i = 0;
	}
	return tunes[i < 0 ? n - 1 : i];
}

/* ---- the registry: the list, the tune and the place in it, the settings (nxplay.*) ---- */

static void
keep(const char *key, const char *value)
{
	const char *was = reg_get(key, NULL);

	if (value ? !was || strcmp(was, value) : was != NULL)
		reg_set(key, value);
}

static void
keep_int(const char *key, int value)
{
	char text[16];

	snprintf(text, sizeof text, "%d", value);
	keep(key, text);
}

/* The list's paths, a tab after each, in keys of CHUNK bytes: under the first tune's folder by
 * their names alone. */
static void
save_list(void)
{
	char text[CHUNK + 1], key[32], folder[512] = "";
	const char *name;
	int i, k = 0, n = 0, before, size;
	size_t f;

	reg_changed();
	before = reg_int("nxplay.lists", 0);
	if (list.count)
		folder_of(tunes[0]->path, folder, sizeof folder);
	f = strlen(folder);
	for (i = 0; i < list.count && k < LIST_KEPT; i++) {
		name = tunes[i]->path;
		if (f > 1 && !strncmp(name, folder, f) && name[f] == '/')
			name += f + 1;
		size = (int)strlen(name);
		if (size + 1 > CHUNK)
			continue;
		if (n + size + 1 > CHUNK) {
			text[n] = 0;
			snprintf(key, sizeof key, "nxplay.list.%d", k++);
			keep(key, text);
			n = 0;
			if (k == LIST_KEPT)
				break;
		}
		memcpy(text + n, name, (size_t)size);
		n += size;
		text[n++] = '\t';
	}
	if (n && k < LIST_KEPT) {
		text[n] = 0;
		snprintf(key, sizeof key, "nxplay.list.%d", k++);
		keep(key, text);
	}
	for (i = k; i < before; i++) {
		snprintf(key, sizeof key, "nxplay.list.%d", i);
		keep(key, NULL);
	}
	keep_int("nxplay.lists", k);
	keep("nxplay.folder", folder);
	list_changed = 0;
}

static void
save_place(void)
{
	int at = resume_entry && !tune.kind ? resume_ms : 0;

	if (tune.kind && playing && state != ENDED)
		at = (int)((uint64_t)position() * 1000 / (uint32_t)tune.rate);
	reg_changed();
	keep("nxplay.tune", playing ? playing->path : resume_entry ? resume_entry->path : NULL);
	keep_int("nxplay.at", at);
	keep_int("nxplay.volume", out.volume);
	keep_int("nxplay.muted", muted);
	keep_int("nxplay.repeat", repeat);
	keep_int("nxplay.shuffle", shuffle);
	keep_int("nxplay.remaining", remaining);
	saved_ms = now_ms();
}

/* The list and the settings as the last session left them; tunes whose files have gone are left out. */
static void
restore(void)
{
	char chunk[CHUNK + 1], folder[512], key[32], path[800], *name, *tab;
	const char *was;
	int k, lists = reg_int("nxplay.lists", 0);

	snprintf(folder, sizeof folder, "%s", reg_get("nxplay.folder", ""));
	for (k = 0; k < lists && k < LIST_KEPT; k++) {
		snprintf(key, sizeof key, "nxplay.list.%d", k);
		snprintf(chunk, sizeof chunk, "%s", reg_get(key, ""));
		for (name = chunk; (tab = strchr(name, '\t')) != NULL; name = tab + 1) {
			*tab = 0;
			if (name[0] == '/')
				snprintf(path, sizeof path, "%s", name);
			else
				snprintf(path, sizeof path, "%s/%s", folder, name);
			if (access(path, R_OK) == 0 && !find_tune(path))
				add_tune(path, -1);
		}
	}
	repeat = reg_int("nxplay.repeat", 0) % 3;
	shuffle = reg_int("nxplay.shuffle", 0) != 0;
	muted = reg_int("nxplay.muted", 0) != 0;
	remaining = reg_int("nxplay.remaining", 0) != 0;
	if (!getenv("NXPLAY_VOLUME"))
		out.volume = reg_int("nxplay.volume", out.volume);
	out.volume = out.volume < 0 ? 0 : out.volume > 256 ? 256 : out.volume;
	if ((was = reg_get("nxplay.tune", NULL)) != NULL && (resume_entry = find_tune(was)) != NULL) {
		resume_ms = reg_int("nxplay.at", 0);
		list.selected = index_of(resume_entry);
		ui_list_show(&list);
	}
	list_changed = 0;
}

/* One tune's length, measured while nothing else is to be done; false when every one is known. */
static int
measure_next(void)
{
	struct media_audio_info info;
	struct media_audio *a;
	struct entry *e = NULL;
	struct stat st;
	const char *why;
	uint8_t *data;
	int i, fd, mapped = 1;

	for (i = 0; i < list.count && !e; i++)
		if (tunes[i]->ms == -1)
			e = tunes[i];
	if (!e)
		return 0;
	e->ms = -2;
	dirty |= D_LIST | D_STATUS;
	if ((fd = open(e->path, O_RDONLY | O_CLOEXEC)) < 0)
		return 1;
	if (fstat(fd, &st) || st.st_size < 64) {
		close(fd);
		return 1;
	}
	data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if (data == MAP_FAILED) {
		/* (no mapping of that file: read it) */
		mapped = 0;
		if ((data = malloc((size_t)st.st_size)) == NULL || read(fd, data, (size_t)st.st_size) != st.st_size) {
			free(data);
			close(fd);
			return 1;
		}
	}
	close(fd);
	if ((a = media_audio_open(data, (uint32_t)st.st_size, &why)) != NULL) {
		media_audio_get_info(a, &info);
		e->ms = info.frames && info.rate ? (int)((uint64_t)info.frames * 1000 / (uint32_t)info.rate) : -3;
		media_audio_close(a);
	}
	if (mapped)
		munmap(data, (size_t)st.st_size);
	else
		free(data);
	return 1;
}

/* ---- the window ---- */

#define MENU_H 20
#define PANEL_Y 26
#define PANEL_H 46
#define SEEK_Y 78
#define SEEK_X 52
#define KEYS_Y 102
#define KEY_H 24
#define HEAD_H 18
#define LIST_Y (KEYS_Y + KEY_H + 8 + HEAD_H)
#define STATUS_H 20
#define LENGTH_W 58
#define METER_W 100
#define VOLUME_W 74
#define MIN_W 470
#define MIN_H 260
#define EVENTS (GR_EVENT_MASK_EXPOSURE | GR_EVENT_MASK_BUTTON_DOWN | GR_EVENT_MASK_BUTTON_UP | GR_EVENT_MASK_KEY_DOWN | \
	GR_EVENT_MASK_UPDATE | GR_EVENT_MASK_CLOSE_REQ)
#define MENUS 4
#define HANDED "/tmp/nxplay.fifo"

enum { B_PREV, B_PLAY, B_STOP, B_NEXT, B_SHUFFLE, B_REPEAT, B_MUTE, BUTTONS };
enum { C_OPEN = 1, C_ADD, C_FOLDER, C_FILES, C_REMOVE, C_CLEAR, C_EXIT, C_PLAY, C_PLAY_ROW, C_STOP, C_PREV, C_NEXT, C_BACK, C_ON,
       C_SHUFFLE, C_REPEAT, C_REPEAT0, C_REPEAT1, C_REPEAT2, C_MUTE, C_LOUDER, C_QUIETER, C_UP, C_DOWN, C_SORT_TITLE,
       C_SORT_LENGTH, C_MIX, C_KEYS, C_ABOUT };

static const char *const menu_names[MENUS] = { "File", "Play", "List", "Help" };
static int menu_x[MENUS + 1], menu_at = -1;
static struct ui_menu_item rows[16];
static int row_count;
static int key_x[BUTTONS], key_w[BUTTONS], volume_x, print_places;
static int meter[2], meter_shown[2] = { -1, -1 };
static uint32_t meter_ms, looked_ms;
static float meter_steps[20];
static pid_t chooser;
static int chooser_fd = -1, chooser_how, chooser_length, handed_fd = -1;
static int others, chooser_reaped;	/* others: a program was started (Files), and is waited for when it ends */
static char chooser_text[1100];

static void command(int id);

static int
seek_w(void)
{
	return win_w - 2 * SEEK_X > 20 ? win_w - 2 * SEEK_X : 20;	/* (seek_value divides by it less 10) */
}

static void
layout(void)
{
	static const int widths[BUTTONS] = { 30, 30, 30, 30, 62, 84, 24 };
	int x = 8, i, w = win_w < MIN_W ? MIN_W : win_w, h = win_h < MIN_H ? MIN_H : win_h;

	for (i = 0; i < B_MUTE; i++) {
		key_x[i] = x, key_w[i] = widths[i];
		x += widths[i] + (i == B_NEXT ? 12 : 4);
	}
	volume_x = w - 8 - 36 - VOLUME_W;
	key_x[B_MUTE] = volume_x - 28, key_w[B_MUTE] = 24;
	list.x = 8, list.y = LIST_Y, list.width = w - 16;
	list.height = h - STATUS_H - 6 - LIST_Y;
	ui_list_show(&list);
	if (print_places && menu_x[1]) {
		/* (for a test that clicks: where things are in the window) */
		printf("nxplay: places: menus %d,%d,%d,%d y 10; keys y %d:", menu_x[0], menu_x[1], menu_x[2], menu_x[3], KEYS_Y + KEY_H / 2);
		for (i = 0; i < BUTTONS; i++)
			printf(" %d", key_x[i] + key_w[i] / 2);
		printf("; volume %d to %d; seek %d to %d y %d; rows from y %d\n", volume_x, volume_x + VOLUME_W, SEEK_X, SEEK_X + seek_w(),
		       SEEK_Y + 10, LIST_Y + 2);
		fflush(stdout);
	}
}

static void
draw_menus(void)
{
	int i, x = 2, w;

	ui_fill(window, 0, 0, win_w, MENU_H, UI_FACE);
	for (i = 0; i < MENUS; i++) {
		w = ui_width(&ui_var_widths, ui_var, menu_names[i], -1) + 16;
		menu_x[i] = x;
		if (i == menu_at)
			ui_fill(window, x, 1, w, MENU_H - 3, UI_SELECTED);
		ui_text(window, x + 8, 3, menu_names[i], -1, i == menu_at ? WHITE : BLACK, 0);
		x += w;
	}
	menu_x[MENUS] = x;
	ui_fill(window, 0, MENU_H - 1, win_w, 1, UI_SHADOW);
}

/* The level meter's segments from `from` to `to` of channel k, lit or not. */
static void
draw_segments(int k, int from, int to)
{
	int x = win_w - 16 - METER_W, y = PANEL_Y + 11 + k * 16, i;

	for (i = from; i < to; i++)
		ui_fill(window, x + i * 5, y, 4, 8, i >= meter[k] ? MWRGB(222, 222, 222) : i < 14 ? MWRGB(40, 170, 40) :
			i < 18 ? MWRGB(225, 190, 0) : MWRGB(215, 40, 30));
}

static void
draw_meter(int all)
{
	int k, a, b;

	for (k = 0; k < 2; k++) {
		if (all || meter_shown[k] < 0) {
			draw_segments(k, 0, 20);
		} else if (meter[k] != meter_shown[k]) {
			a = meter[k] < meter_shown[k] ? meter[k] : meter_shown[k];
			b = meter[k] < meter_shown[k] ? meter_shown[k] : meter[k];
			draw_segments(k, a, b);
		}
		meter_shown[k] = meter[k];
	}
}

/* The peaks of the last 512 frames the card has played, 2 dB a segment; a level falls a segment a look. */
static void
meter_take(void)
{
	uint32_t at, i, c = (uint32_t)tune.channels;
	int peak[2] = { 0, 0 }, k, v, n;
	const int16_t *s;

	if (out.on && tune.kind && state == PLAYING && !paused && out.running && !muted) {
		at = card_at();
		for (i = 0; i < 512; i++) {
			s = out.ring + ((at + i) & (RING - 1)) * c;
			v = s[0] < 0 ? -s[0] : s[0];
			peak[0] = v > peak[0] ? v : peak[0];
			v = s[c - 1] < 0 ? -s[c - 1] : s[c - 1];
			peak[1] = v > peak[1] ? v : peak[1];
		}
	}
	for (k = 0; k < 2; k++) {
		v = peak[k] * out.volume / 256;
		for (n = 0; n < 20 && v >= meter_steps[n]; n++)
			;
		meter[k] = n > meter[k] ? n : meter[k] > 0 ? meter[k] - 1 : 0;
	}
}

static void
draw_panel(void)
{
	static const char *kinds[] = { "", "WAV", "MOD", "FLAC", "MP3", "Ogg Vorbis" };
	struct entry *e = tune.kind ? playing : resume_entry;
	char name[160], line[200], more[40] = "", at[24];
	int mx = win_w - 16 - METER_W;

	ui_fill(window, 8, PANEL_Y, win_w - 16, PANEL_H, WHITE);
	ui_bevel(window, 8, PANEL_Y, win_w - 16, PANEL_H, 1);
	if (tune.kind)
		title_of(tune.path, name, sizeof name);
	else if (e)
		title_of(e->path, name, sizeof name);
	else
		snprintf(name, sizeof name, "Nothing is playing");
	ui_text_fit(window, 16, PANEL_Y + 7, mx - 36, name, BLACK);
	ui_text_fit(window, 17, PANEL_Y + 7, mx - 36, name, BLACK);	/* twice: bold */
	if (tune.kind) {
		char buffering[40];

		snprintf(buffering, sizeof buffering, " (decoding ahead: %d%%)", buffered < 0 ? 0 : buffered > 99 ? 99 : buffered);
		if (workers)
			snprintf(more, sizeof more, ", %d workers", workers);
		snprintf(line, sizeof line, "%s, %d Hz %s%s%s%s", kinds[tune.kind], tune.rate, tune.channels == 2 ? "stereo" : "mono",
			 tune.half ? ", half rate" : "", more, paused ? (position() == 0 ? " (stopped)" : " (paused)") :
			 state == BUFFERING ? buffering : state == ENDED ? " (ended)" : "");
	} else if (e && resume_ms > 0) {
		snprintf(line, sizeof line, "Play goes on from %s", time_text(at, sizeof at, resume_ms));
	} else {
		snprintf(line, sizeof line, list.count ? "Choose a tune in the list and press Play" : "File > Add tunes... puts tunes in the list");
	}
	ui_text_fit(window, 16, PANEL_Y + 26, mx - 36, line, MWRGB(90, 90, 90));
	ui_text(window, mx - 12, PANEL_Y + 8, "L", 1, MWRGB(90, 90, 90), 0);
	ui_text(window, mx - 12, PANEL_Y + 24, "R", 1, MWRGB(90, 90, 90), 0);
	draw_meter(1);
}

/* The place in the tune: the time, a bar to drag, and the length (or what is left: a click on it). */
static void
draw_seek(void)
{
	uint32_t rate = tune.kind ? (uint32_t)tune.rate : 1, total = tune.kind && tune.total != ~0u ? tune.total : 0;
	uint32_t at = drag == 1 && total ? (uint32_t)((uint64_t)total * (uint32_t)drag_value / 1000) : tune.kind ? position() : 0;
	int x = SEEK_X, w = seek_w(), ms = tune.kind ? (int)((uint64_t)at * 1000 / rate) : resume_entry ? resume_ms : 0;
	int all = tune.kind ? (total ? (int)((uint64_t)total * 1000 / rate) : -1) : resume_entry ? resume_entry->ms : -1;
	int value = all > 0 ? (int)((int64_t)ms * 1000 / all) : 0, knob, tw;
	char t[32], left[24];

	value = value > 1000 ? 1000 : value < 0 ? 0 : value;
	knob = x + (w - 10) * value / 1000;
	ui_fill(window, 0, SEEK_Y, win_w, 20, UI_FACE);
	time_text(t, sizeof t, ms);
	tw = ui_width(&ui_var_widths, ui_var, t, -1);
	ui_text(window, x - 6 - tw, SEEK_Y + 3, t, -1, BLACK, 0);
	if (all < 0)
		snprintf(t, sizeof t, "-:--");
	else if (remaining)
		snprintf(t, sizeof t, "-%s", time_text(left, sizeof left, all > ms ? all - ms : 0));
	else
		time_text(t, sizeof t, all);
	ui_text(window, x + w + 6, SEEK_Y + 3, t, -1, BLACK, 0);
	ui_fill(window, x, SEEK_Y + 7, w, 6, ui_seven ? MWRGB(231, 234, 234) : WHITE);
	if (knob > x + 1)
		ui_fill(window, x + 1, SEEK_Y + 8, knob - x, 4, UI_SELECTED);
	ui_bevel(window, x, SEEK_Y + 7, w, 6, 1);
	if (all > 0 && ui_seven) {
		ui_button_seven(window, knob, SEEK_Y + 1, 10, 18, drag == 1);
	} else if (all > 0) {
		ui_fill(window, knob, SEEK_Y + 1, 10, 18, UI_FACE);
		ui_bevel(window, knob, SEEK_Y + 1, 10, 18, 0);
	}
	shown_second = ms / 1000;
}

static int
seek_value(int px)
{
	int v = (px - SEEK_X - 5) * 1000 / (seek_w() - 10);

	return v < 0 ? 0 : v > 1000 ? 1000 : v;
}

static void
draw_volume(void)
{
	char pct[16];

	ui_fill(window, key_x[B_MUTE], KEYS_Y, win_w - key_x[B_MUTE], KEY_H, UI_FACE);
	ui_tool(window, key_x[B_MUTE], KEYS_Y, key_w[B_MUTE], KEY_H, muted ? UI_G_MUTE : UI_G_SOUND, muted);
	ui_slider(window, volume_x, KEYS_Y + 3, VOLUME_W, out.volume, 256);
	snprintf(pct, sizeof pct, "%d%%", muted ? 0 : (out.volume * 100 + 128) / 256);
	ui_text(window, volume_x + VOLUME_W + 4, KEYS_Y + 6, pct, -1, BLACK, 0);
}

static void
draw_keys(void)
{
	static const int signs[] = { UI_G_PREV, UI_G_PLAY, UI_G_STOP, UI_G_NEXT };
	int going = tune.kind && !paused && state != ENDED && state != STOPPED, i;

	ui_fill(window, 0, KEYS_Y - 2, win_w, KEY_H + 4, UI_FACE);
	for (i = B_PREV; i <= B_NEXT; i++)
		ui_tool(window, key_x[i], KEYS_Y, key_w[i], KEY_H, i == B_PLAY && going ? UI_G_PAUSE : signs[i],
			(i == B_PREV || i == B_NEXT) && !list.count ? 2 : i == B_STOP && !tune.kind ? 2 : 0);
	ui_button(window, key_x[B_SHUFFLE], KEYS_Y, key_w[B_SHUFFLE], KEY_H, "Shuffle", shuffle);
	ui_button(window, key_x[B_REPEAT], KEYS_Y, key_w[B_REPEAT], KEY_H, repeat == 2 ? "Repeat one" : "Repeat", repeat != 0);
	draw_volume();
}

static int
list_bar(void)
{
	return list.count > ui_list_rows(&list) ? UI_SCROLL_W : 0;
}

static void
draw_row(int i)
{
	struct entry *e = tunes[list.top + i];
	int y = list.y + 2 + i * UI_ROW, w = list.width - 4 - list_bar(), picked = list.top + i == list.selected;
	int now = e == playing && tune.kind, tw;
	GR_COLOR ink = picked ? WHITE : e->ms == -2 ? UI_SHADOW : BLACK;
	char name[160], length[24];

	ui_fill(window, list.x + 2, y, w, UI_ROW, picked ? UI_SELECTED : WHITE);
	if (now)
		ui_glyph(window, list.x + 11, y + UI_ROW / 2, paused ? UI_G_PAUSE : UI_G_PLAY, picked ? WHITE : UI_SELECTED);
	title_of(e->path, name, sizeof name);
	ui_text_fit(window, list.x + 22, y + 3, w - 22 - LENGTH_W, name, ink);
	if (now)
		ui_text_fit(window, list.x + 23, y + 3, w - 22 - LENGTH_W, name, ink);
	if (e->ms >= 0)
		time_text(length, sizeof length, e->ms);
	else
		snprintf(length, sizeof length, "%s", e->ms == -2 ? "cannot play" : e->ms == -3 ? "" : "...");
	tw = ui_width(&ui_var_widths, ui_var, length, -1);
	ui_text(window, list.x + 2 + w - 8 - tw, y + 3, length, -1, ink, 0);
}

static void
draw_list(void)
{
	int rows = ui_list_rows(&list), bar = list_bar(), i, hy = list.y - HEAD_H, lx = list.x + list.width - 2 - bar - LENGTH_W;

	ui_fill(window, list.x, hy, list.width, HEAD_H, UI_FACE);
	ui_bevel(window, list.x, hy, lx - list.x, HEAD_H, 0);
	ui_bevel(window, lx, hy, list.x + list.width - lx, HEAD_H, 0);
	ui_text(window, list.x + 22, hy + 3, "Title", -1, BLACK, 0);
	ui_text(window, lx + 6, hy + 3, "Length", -1, BLACK, 0);
	ui_fill(window, list.x, list.y, list.width, list.height, WHITE);
	for (i = 0; i < rows && list.top + i < list.count; i++)
		draw_row(i);
	if (!list.count)
		ui_text(window, list.x + 10, list.y + 8, "No tunes: File > Add tunes... or Add a folder...", -1, UI_SHADOW, 0);
	if (bar)
		ui_scrollbar(window, list.x + list.width - 2 - UI_SCROLL_W, list.y + 2, list.height - 4, list.count, rows, list.top);
	ui_bevel(window, list.x, list.y, list.width, list.height, 1);
}

static void
draw_status(void)
{
	char sum[64], t[24];
	int y = win_h - STATUS_H, i, ms = 0, unknown = 0, tw;

	for (i = 0; i < list.count; i++)
		if (tunes[i]->ms >= 0)
			ms += tunes[i]->ms;
		else
			unknown += tunes[i]->ms == -1;
	snprintf(sum, sizeof sum, "%d tune%s, %s%s", list.count, list.count == 1 ? "" : "s", time_text(t, sizeof t, ms), unknown ? "+" : "");
	tw = ui_width(&ui_var_widths, ui_var, sum, -1);
	ui_fill(window, 0, y, win_w, STATUS_H, UI_FACE);
	ui_fill(window, 0, y, win_w, 1, UI_SHADOW);
	ui_text_fit(window, 8, y + 4, win_w - 24 - tw, note, BLACK);
	ui_text(window, win_w - 8 - tw, y + 4, sum, -1, BLACK, 0);
}

static void
draw(void)
{
	ui_fill(window, 0, 0, win_w, win_h, UI_FACE);
	dirty = D_ALL;
}

/* What `dirty` says, and the state as drawn. */
static void
redraw(void)
{
	int d = dirty;

	dirty = 0;
	if (d & D_MENUS)
		draw_menus();
	if (d & D_PANEL)
		draw_panel();
	if (d & D_SEEK)
		draw_seek();
	if (d & D_KEYS)
		draw_keys();
	if (d & D_LIST)
		draw_list();
	if (d & D_STATUS)
		draw_status();
	shown_state = state * 2 + paused;
}

/* How loud: 0 to 256. Muted, the card plays nothing and the slider keeps its place. */
static void
set_volume(int volume)
{
	out.volume = volume < 0 ? 0 : volume > 256 ? 256 : volume;
	if (out.on)
		SND_VOICE[out.voice].volume = muted ? 0 : SND_VOLUME(out.volume, out.volume);
	dirty |= D_KEYS;
}

static void
other_windows(GR_EVENT *event)
{
	if (event->type == GR_EVENT_TYPE_EXPOSURE && event->general.wid == window)
		draw(), redraw();
	if (event->type == GR_EVENT_TYPE_CLOSE_REQ && event->general.wid == window)
		quit = 1;
}

static void
ask_error(const char *path, const char *why)
{
	static const char *const ok[] = { "OK" };
	char text[400], name[160];

	title_of(path, name, sizeof name);
	why = why && why[0] ? why : "it cannot be read";
	snprintf(text, sizeof text, "\"%.60s\" cannot be played:\n%c%.200s.", name, toupper((unsigned char)why[0]), why + 1);
	ui_ask_icon = "error";
	ui_ask("Music", text, ok, 1, other_windows);
	ui_ask_icon = NULL;
	GrSetFocus(window);
}

/* Plays a tune of the list from `from_ms`; false when it cannot (and says why, in a window
 * when `asked`). The tune before is let go either way. */
static int
play_entry(struct entry *e, int from_ms, int asked)
{
	char title[160];
	const char *why;

	if (!e)
		return 0;
	if ((why = load(e->path)) != NULL) {
		unload();
		e->ms = -2;
		snprintf(title, sizeof title, "%s: %s", strrchr(e->path, '/') ? strrchr(e->path, '/') + 1 : e->path, why);
		say(title);
		dirty |= D_ALL;
		if (asked && window_on)
			ask_error(e->path, why);
		return 0;
	}
	playing = e;
	if (tune.total != ~0u)
		e->ms = (int)((uint64_t)tune.total * 1000 / (uint32_t)tune.rate);
	set_volume(out.volume);		/* (the voice is a new one's) */
	if (from_ms > 0 && tune.total != ~0u)
		seek_to((uint32_t)((uint64_t)from_ms * (uint32_t)tune.rate / 1000));
	resume_entry = NULL;
	resume_ms = 0;
	list.selected = index_of(e);
	ui_list_show(&list);
	if (window_on) {
		snprintf(title, sizeof title, "%.120s - Music", tune.name);
		GrSetWindowTitle(window, title);
		save_place();
	}
	dirty |= D_ALL & ~D_MENUS;
	return 1;
}

/* The tune `by` on (Previous within three seconds of a tune's start; else it starts again);
 * a tune that cannot be played is passed over. */
static void
advance(int by, int ended)
{
	struct entry *e;
	int tries;

	if (!ended && by < 0 && tune.kind && state != ENDED && position() > 3u * (uint32_t)tune.rate) {
		paused = 0;
		seek_to(0);
		return;
	}
	for (tries = 0; tries < list.count && (e = neighbour(by, ended)) != NULL; tries++) {
		if (e == playing && tune.kind) {
			paused = 0;
			seek_to(0);
			return;
		}
		if (play_entry(e, 0, 0))
			return;
		playing = e;	/* (the next one after it, then) */
		if (ended && repeat == 2)
			break;
	}
	if (ended)
		say("the end of the list");
	dirty |= D_ALL & ~D_MENUS;
}

static void
toggle(void)
{
	struct entry *e;

	if (!tune.kind) {
		/* nothing loaded: the selected tune (where the last session left it), or a file to choose */
		e = list.selected >= 0 ? tunes[list.selected] : list.count ? tunes[0] : NULL;
		if (e)
			play_entry(e, e == resume_entry ? resume_ms : 0, 1);
		else
			command(C_OPEN);
		return;
	}
	if (state == ENDED || state == STOPPED) {
		paused = 0;
		seek_to(0);
	} else {
		paused = !paused;
		if (state == PLAYING)
			voice_run(!paused);
		if (paused && window_on)
			save_place();
	}
}

/* ---- the file window, in a program of its own: the tune plays on meanwhile ---- */

/* nxplay --choose HOW FOLDER: the file window; prints each file chosen ('a' asks again until Cancel). */
static int
choose_main(int how, const char *start)
{
	char path[600], folder[512];

	if (GrOpen() < 0)
		return 1;
	ui_init();
	snprintf(folder, sizeof folder, "%s", start);
	while (ui_choose_file(how == 'o' ? "Open a tune" : how == 'a' ? "Add tunes to the list (Cancel when done)" :
			      "Add a folder: open any tune in it", folder, KINDS, path, sizeof path, NULL)) {
		printf("%s\n", path);
		fflush(stdout);
		if (how != 'a')
			break;
		folder_of(path, folder, sizeof folder);
	}
	GrClose();
	return 0;
}

static void
choose(int how)
{
	char folder[512] = "/usr/share", mode[2] = { (char)how, 0 };
	struct entry *e = playing ? playing : list.selected >= 0 ? tunes[list.selected] : NULL;
	int p[2];
	pid_t child;

	if (chooser > 0) {
		say("the file window is open already");
		return;
	}
	if (access("/root/Music", R_OK) == 0)	/* the image's own tunes (tools/make_media.py) */
		strcpy(folder, "/root/Music");
	if (e)
		folder_of(e->path, folder, sizeof folder);
	if (pipe(p))
		return;
	fcntl(p[0], F_SETFD, FD_CLOEXEC);
	if ((child = real_vfork()) == 0) {
		dup2(p[1], 1);
		execlp("nxplay", "nxplay", "--choose", mode, folder, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	if (child < 0) {
		close(p[0]);
		return;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	chooser = child, chooser_fd = p[0], chooser_how = how, chooser_length = 0;
}

static void
chosen(const char *path)
{
	struct entry *e = find_tune(path);
	char folder[512], text[160];
	int n;

	if (chooser_how == 'f') {
		folder_of(path, folder, sizeof folder);
		n = add_folder(folder);
		snprintf(text, sizeof text, "%d tune%s added", n, n == 1 ? "" : "s");
		say(text);
		return;
	}
	if (!e && !(e = add_tune(path, -1))) {
		say("the list is full");
		return;
	}
	list.selected = index_of(e);
	ui_list_show(&list);
	if (chooser_how == 'o')
		play_entry(e, 0, 1);
}

/* What the file window has printed, a line a file; it is gone at the pipe's end. */
static void
chooser_take(void)
{
	ssize_t n = -1;
	char *end;
	int status;

	if (chooser_fd < 0)
		return;
	while (chooser_length < (int)sizeof chooser_text - 1 &&
	       (n = read(chooser_fd, chooser_text + chooser_length, sizeof chooser_text - 1 - (size_t)chooser_length)) > 0)
		chooser_length += (int)n;
	chooser_text[chooser_length] = 0;
	while ((end = strchr(chooser_text, '\n')) != NULL) {
		*end = 0;
		chosen(chooser_text);
		chooser_length -= (int)(end + 1 - chooser_text);
		memmove(chooser_text, end + 1, (size_t)chooser_length + 1);
	}
	if (chooser_length >= (int)sizeof chooser_text - 1)
		chooser_length = 0;
	if (n == 0 || (n < 0 && errno != EAGAIN)) {
		close(chooser_fd);
		chooser_fd = -1;
		if (!chooser_reaped)
			waitpid(chooser, &status, 0);
		chooser = chooser_reaped = 0;
		GrSetFocus(window);
	}
}

/* A second start (`open` for a sound file) gives this player its files through a pipe with a
 * name: "!PATH" plays, "+PATH" adds, an empty line brings the window to the front. */
static int
hand_off(int argc, char **argv)
{
	char whole[PATH_MAX], line[PATH_MAX + 4];
	int fd = open(HANDED, O_WRONLY | O_NONBLOCK | O_CLOEXEC), i, n, ok = 1;

	if (fd < 0)
		return 0;	/* (none, or nobody reading it) */
	if (argc < 2)
		ok = write(fd, "\n", 1) == 1;
	for (i = 1; i < argc && ok; i++) {
		if (!realpath(argv[i], whole))
			snprintf(whole, sizeof whole, "%s", argv[i]);
		n = snprintf(line, sizeof line, "%c%s\n", i == 1 ? '!' : '+', whole);
		ok = write(fd, line, (size_t)n) == n;
	}
	close(fd);
	if (ok)
		printf("nxplay: given to the player that is open\n");
	return ok;
}

static void
take_handed(void)
{
	static char text[PATH_MAX + 8];
	static int kept;	/* a line's start that came without its end */
	char *line, *end;
	struct entry *e;
	ssize_t n;

	if (handed_fd < 0 || (n = read(handed_fd, text + kept, sizeof text - 1 - (size_t)kept)) <= 0)
		return;
	n += kept;
	text[n] = 0;
	for (line = text; (end = strchr(line, '\n')) != NULL; line = end + 1) {
		*end = 0;
		if ((line[0] == '!' || line[0] == '+') && line[1]) {
			if (access(line + 1, R_OK) != 0) {
				ask_error(line + 1, "cannot open the file");
				continue;
			}
			e = find_tune(line + 1);
			if (!e)
				e = add_tune(line + 1, -1);
			if (e && line[0] == '!')
				play_entry(e, 0, 1);
		}
	}
	kept = (int)(text + n - line);
	kept = kept < (int)sizeof text - 1 ? kept : 0;
	memmove(text, line, (size_t)kept);
	GrRaiseWindow(window);
	GrSetFocus(window);
}

/* ---- menus, keys, the pointer ---- */

static void
row(const char *label, int id, int flags)
{
	struct ui_menu_item item = { label, -1, id, NULL, 0, flags, NULL };

	if (row_count < (int)(sizeof rows / sizeof rows[0]))
		rows[row_count++] = item;
}

/* The rows of menu `which` (4: the list's, by the right button). */
static void
menu_make(int which)
{
	int none = list.selected < 0 ? UI_MENU_GREY : 0, going = tune.kind && !paused && state != ENDED;

	row_count = 0;
	switch (which) {
	case 0:
		row("Open a tune...  (O)", C_OPEN, 0);
		row("Add tunes...  (A)", C_ADD, 0);
		row("Add a folder...", C_FOLDER, 0);
		row("Show in Files", C_FILES, UI_MENU_LINE | (none && !playing ? UI_MENU_GREY : 0));
		row("Remove from the list  (Delete)", C_REMOVE, UI_MENU_LINE | none);
		row("Clear the list", C_CLEAR, list.count ? 0 : UI_MENU_GREY);
		row("Exit  (Q)", C_EXIT, UI_MENU_LINE);
		break;
	case 1:
		row(going ? "Pause  (Space)" : "Play  (Space)", C_PLAY, 0);
		row("Stop  (X)", C_STOP, tune.kind ? 0 : UI_MENU_GREY);
		row("Previous  (P)", C_PREV, list.count ? 0 : UI_MENU_GREY);
		row("Next  (N)", C_NEXT, list.count ? 0 : UI_MENU_GREY);
		row("Back 10 seconds  (Left)", C_BACK, UI_MENU_LINE | (tune.kind ? 0 : UI_MENU_GREY));
		row("On 10 seconds  (Right)", C_ON, tune.kind ? 0 : UI_MENU_GREY);
		row("Shuffle  (S)", C_SHUFFLE, UI_MENU_LINE | (shuffle ? UI_MENU_CHECK : 0));
		row("Repeat off", C_REPEAT0, repeat == 0 ? UI_MENU_CHECK : 0);
		row("Repeat the list  (R)", C_REPEAT1, repeat == 1 ? UI_MENU_CHECK : 0);
		row("Repeat the tune", C_REPEAT2, repeat == 2 ? UI_MENU_CHECK : 0);
		row("Mute  (M)", C_MUTE, UI_MENU_LINE | (muted ? UI_MENU_CHECK : 0));
		row("Louder  (+)", C_LOUDER, 0);
		row("Quieter  (-)", C_QUIETER, 0);
		break;
	case 2:
		row("Play the selected  (Enter)", C_PLAY_ROW, none);
		row("Move up  (Alt+Up)", C_UP, list.selected > 0 ? 0 : UI_MENU_GREY);
		row("Move down  (Alt+Down)", C_DOWN, list.selected >= 0 && list.selected < list.count - 1 ? 0 : UI_MENU_GREY);
		row("Sort by title", C_SORT_TITLE, UI_MENU_LINE | (list.count > 1 ? 0 : UI_MENU_GREY));
		row("Sort by length", C_SORT_LENGTH, list.count > 1 ? 0 : UI_MENU_GREY);
		row("Mix the order", C_MIX, list.count > 1 ? 0 : UI_MENU_GREY);
		break;
	case 3:
		row("Keys  (F1)", C_KEYS, 0);
		row("About", C_ABOUT, 0);
		break;
	default:
		row("Play", C_PLAY_ROW, none);
		row("Remove from the list", C_REMOVE, none);
		row("Move up", C_UP, list.selected > 0 ? 0 : UI_MENU_GREY);
		row("Move down", C_DOWN, list.selected >= 0 && list.selected < list.count - 1 ? 0 : UI_MENU_GREY);
		row("Add tunes...", C_ADD, UI_MENU_LINE);
		row("Show in Files", C_FILES, none);
	}
}

static void
menu_open(int which, int keyboard)
{
	int x, y;

	ui_window_origin(window, &x, &y);
	menu_make(which);
	menu_at = which;
	dirty |= D_MENUS;
	redraw();
	ui_menu_open(rows, row_count, x + menu_x[which], y + MENU_H, 0, keyboard);
}

static void
show_text(const char *title, const char *text)
{
	static const char *const ok[] = { "OK" };

	ui_ask_icon = "information";
	ui_ask(title, text, ok, 1, other_windows);
	ui_ask_icon = NULL;
	GrSetFocus(window);
}

static void
command(int id)
{
	static const char *const clear[] = { "Clear", "Cancel" };
	struct entry *e = list.selected >= 0 ? tunes[list.selected] : NULL;
	char folder[512];

	switch (id) {
	case C_OPEN: choose('o'); break;
	case C_ADD: choose('a'); break;
	case C_FOLDER: choose('f'); break;
	case C_FILES:
		if ((e = e ? e : playing) != NULL) {
			folder_of(e->path, folder, sizeof folder);
			others |= ui_run("nxfiles", folder) > 0;
		}
		break;
	case C_REMOVE:
		if (e)
			remove_row(list.selected);
		break;
	case C_CLEAR:
		if (list.count && ui_ask("Music", "Take every tune off the list?", clear, 2, other_windows) == 0) {
			while (list.count)
				remove_row(list.count - 1);
			list.top = 0;
		}
		GrSetFocus(window);
		break;
	case C_EXIT: quit = 1; break;
	case C_PLAY: toggle(); break;
	case C_PLAY_ROW:
		if (e)
			play_entry(e, e == resume_entry ? resume_ms : 0, 1);
		break;
	case C_STOP:
		if (tune.kind) {
			paused = 1;
			seek_to(0);
			save_place();
		}
		break;
	case C_PREV: advance(-1, 0); break;
	case C_NEXT: advance(1, 0); break;
	case C_BACK:
		if (tune.kind)
			seek_to(position() > 10u * (uint32_t)tune.rate ? position() - 10u * (uint32_t)tune.rate : 0);
		break;
	case C_ON:
		if (tune.kind)
			seek_to(position() + 10u * (uint32_t)tune.rate);
		break;
	case C_SHUFFLE:
		shuffle = !shuffle, order_dirty = 1;
		say(shuffle ? "shuffle: every tune of the list once, in any order" : "shuffle is off");
		break;
	case C_REPEAT: case C_REPEAT0: case C_REPEAT1: case C_REPEAT2:
		repeat = id == C_REPEAT ? (repeat + 1) % 3 : id - C_REPEAT0;
		say(repeat == 1 ? "repeat: the list starts again after its last" : repeat == 2 ? "repeat one: the tune plays again" :
		    "repeat is off");
		break;
	case C_MUTE:
		muted = !muted;
		set_volume(out.volume);
		say(muted ? "muted" : "the sound is on again");
		break;
	case C_LOUDER: case C_QUIETER:
		set_volume(out.volume + (id == C_LOUDER ? 16 : -16));
		printf("nxplay: volume %d of 256\n", out.volume);
		fflush(stdout);
		break;
	case C_UP: move_row(list.selected, list.selected - 1); break;
	case C_DOWN: move_row(list.selected, list.selected + 1); break;
	case C_SORT_TITLE: sort_list(0); break;
	case C_SORT_LENGTH: sort_list(1); break;
	case C_MIX: sort_list(2); break;
	case C_KEYS:
		show_text("Keys", "Space plays or pauses, X stops; N and P: the next tune and the one before.\n"
			  "Left and Right: ten seconds back and on; 0 to 9: a tenth of the tune each.\n"
			  "Up, Down, Enter: the list; Delete takes a tune off it, Alt+Up and Alt+Down move it.\n"
			  "+ and -: louder and quieter, M mutes; S shuffles, R repeats (the list, the tune, off).\n"
			  "O opens a tune, A adds tunes; F10 opens the menus, Q leaves.");
		break;
	case C_ABOUT:
		show_text("About", "Music: plays WAV, FLAC, MP3, Ogg Vorbis and MOD files,\n"
			  "decoded by libmedia on the worker cores (docs/apps/play.md).\n"
			  "The list and the place in it are kept in the registry (nxplay.*).");
		break;
	}
	dirty |= D_ALL & ~D_MENUS;
}

static void
key_down(const GR_EVENT *event)
{
	int ch = event->keystroke.ch, mods = event->keystroke.modifiers;

	if ((mods & MWKMOD_ALT) && (ch == MWKEY_UP || ch == MWKEY_DOWN)) {
		command(ch == MWKEY_UP ? C_UP : C_DOWN);
		return;
	}
	if ((mods & MWKMOD_CTRL) && (ch == 'o' || ch == 'O' || ch == 15)) {
		command(C_OPEN);
		return;
	}
	if (ui_list_key(&list, ch)) {
		dirty |= D_LIST;
		return;
	}
	if (ch >= '0' && ch <= '9' && tune.kind && tune.total != ~0u) {
		seek_to((uint32_t)((uint64_t)tune.total * (uint32_t)(ch - '0') / 10));
		dirty |= D_SEEK | D_PANEL;
		return;
	}
	switch (ch < 128 ? tolower(ch) : ch) {
	case ' ': command(C_PLAY); break;
	case MWKEY_ENTER: command(C_PLAY_ROW); break;
	case 'o': command(C_OPEN); break;
	case 'a': command(C_ADD); break;
	case 'x': command(C_STOP); break;
	case 'q': case MWKEY_ESCAPE: quit = 1; break;
	case '+': case '=': command(C_LOUDER); break;
	case '-': command(C_QUIETER); break;
	case 'm': command(C_MUTE); break;
	case 'n': command(C_NEXT); break;
	case 'p': command(C_PREV); break;
	case 'r': command(C_REPEAT); break;
	case 's': command(C_SHUFFLE); break;
	case MWKEY_LEFT: command(C_BACK); break;
	case MWKEY_RIGHT: command(C_ON); break;
	case MWKEY_DELETE: command(C_REMOVE); break;
	case MWKEY_F1: command(C_KEYS); break;
	case MWKEY_F10: menu_open(0, 1); break;
	}
}

/* The row of the list under the pointer at y, kept within the list. */
static int
row_at(int y)
{
	int r = y < list.y + 2 ? list.top - 1 : list.top + (y - list.y - 2) / UI_ROW;

	return r < 0 ? 0 : r >= list.count ? list.count - 1 : r;
}

static void
button_down(const GR_EVENT *event)
{
	int x = event->button.x, y = event->button.y, wheel = ui_wheel(event), i, how, hy = list.y - HEAD_H;
	static const int keys[BUTTONS] = { C_PREV, C_PLAY, C_STOP, C_NEXT, C_SHUFFLE, C_REPEAT, C_MUTE };

	if (wheel) {
		/* over the list the wheel moves it; elsewhere it is the volume */
		if (ui_inside(x, y, list.x, list.y, list.width, list.height)) {
			if (ui_list_event(&list, event))
				dirty |= D_LIST;
		} else {
			set_volume(out.volume - 16 * wheel);
			printf("nxplay: volume %d of 256\n", out.volume);
			fflush(stdout);
		}
		return;
	}
	if (y < MENU_H) {
		for (i = 0; i < MENUS; i++)
			if (x >= menu_x[i] && x < menu_x[i + 1])
				menu_open(i, 0);
		return;
	}
	if (event->button.changebuttons & GR_BUTTON_R) {
		if (ui_inside(x, y, list.x, list.y, list.width - list_bar(), list.height) && list.count) {
			list.selected = row_at(y);
			dirty |= D_LIST;
			redraw();
			menu_make(4);
			menu_at = -1;
			ui_menu_open(rows, row_count, event->button.rootx, event->button.rooty, 0, 0);
		}
		return;
	}
	if (!(event->button.changebuttons & GR_BUTTON_L))
		return;
	if (ui_inside(x, y, SEEK_X - 4, SEEK_Y, seek_w() + 8, 20)) {
		if (tune.kind && tune.total != ~0u) {
			drag = 1;
			drag_value = seek_value(x);
			dirty |= D_SEEK;
		}
	} else if (ui_inside(x, y, SEEK_X + seek_w() + 4, SEEK_Y, 50, 20)) {
		remaining = !remaining;
		dirty |= D_SEEK;
	} else if (ui_inside(x, y, volume_x, KEYS_Y, VOLUME_W, KEY_H)) {
		drag = 2;
		set_volume(ui_slider_value(x, volume_x, VOLUME_W, 256));
		printf("nxplay: volume %d of 256\n", out.volume);
		fflush(stdout);
	} else if (ui_inside(x, y, list.x, hy, list.width, HEAD_H)) {
		command(x >= list.x + list.width - 2 - list_bar() - LENGTH_W ? C_SORT_LENGTH : C_SORT_TITLE);
	} else if (ui_inside(x, y, list.x, list.y, list.width, list.height)) {
		how = ui_list_event(&list, event);
		if (list_bar() && x >= list.x + list.width - 2 - UI_SCROLL_W) {
			drag = 4;	/* (the scroll bar) */
		} else if (how) {
			drag = 3, drag_moved = 0, play_on_release = how == 2;
		}
		dirty |= D_LIST;
	} else {
		for (i = 0; i < BUTTONS; i++)
			if (ui_inside(x, y, key_x[i], KEYS_Y, key_w[i], KEY_H))
				command(keys[i]);
	}
	if (drag)	/* (the moves only for a drag: a click asks for none) */
		ui_moves(window, EVENTS, 1);
}

static void
pointer_moved(const GR_EVENT *event)
{
	int r;

	if (!(event->mouse.buttons & GR_BUTTON_L))
		return;
	switch (drag) {
	case 1:
		drag_value = seek_value(event->mouse.x);
		dirty |= D_SEEK;
		break;
	case 2:
		set_volume(ui_slider_value(event->mouse.x, volume_x, VOLUME_W, 256));
		break;
	case 3:
		/* a row dragged up or down the list */
		if ((r = row_at(event->mouse.y)) != list.selected && list.selected >= 0) {
			move_row(list.selected, r);
			drag_moved = 1;
		}
		break;
	case 4:
		if (ui_list_event(&list, event))
			dirty |= D_LIST;
		break;
	}
}

static void
button_up(void)
{
	ui_moves(window, EVENTS, 0);
	if (drag == 1 && tune.kind && tune.total != ~0u)
		seek_to((uint32_t)((uint64_t)tune.total * (uint32_t)drag_value / 1000));
	if (drag == 3 && play_on_release && !drag_moved)
		command(C_PLAY_ROW);
	drag = 0;
	dirty |= D_SEEK | D_PANEL;
}

static void
handle(GR_EVENT *event)
{
	int id = 0, menu;

	ui_origin_note(event);
	if (menu_at >= 0 && ui_menu_depth == 1 && event->type == GR_EVENT_TYPE_KEY_DOWN &&
	    (event->keystroke.ch == MWKEY_LEFT || (event->keystroke.ch == MWKEY_RIGHT &&
	     !(ui_menu_level[0].hover >= 0 && ui_menu_level[0].items[ui_menu_level[0].hover].sub)))) {
		/* Left and Right in a menu of the bar: the menu beside it */
		menu_open((menu_at + (event->keystroke.ch == MWKEY_LEFT ? MENUS - 1 : 1)) % MENUS, 1);
		return;
	}
	menu = ui_menu_event(event, &id);
	if (menu == UI_MENU_CHOSEN || menu == UI_MENU_CLOSED) {
		if (menu_at >= 0)
			dirty |= D_MENUS;
		menu_at = -1;
		GrSetFocus(window);
		if (menu == UI_MENU_CHOSEN)
			command(id);
		return;
	}
	if (menu)
		return;
	switch (event->type) {
	case GR_EVENT_TYPE_EXPOSURE:
		if (event->exposure.wid == window)
			draw();
		break;
	case GR_EVENT_TYPE_UPDATE:
		if (event->update.utype == GR_UPDATE_SIZE && event->update.wid == window) {
			win_w = event->update.width;
			win_h = event->update.height;
			layout();
			draw();
		}
		break;
	case GR_EVENT_TYPE_CLOSE_REQ:
		quit = 1;
		break;
	case GR_EVENT_TYPE_KEY_DOWN:
		key_down(event);
		break;
	case GR_EVENT_TYPE_BUTTON_DOWN:
		button_down(event);
		break;
	case GR_EVENT_TYPE_MOUSE_MOTION:
		pointer_moved(event);
		break;
	case GR_EVENT_TYPE_BUTTON_UP:
		if (drag)
			button_up();
		break;
	}
}

static void
events(int busy)
{
	GR_EVENT event;
	uint32_t t;

	/* paused, stopped or ended with the meter down: the fifo and the file window looked at only */
	if (busy)
		GrCheckNextEvent(&event);
	else if ((state == PLAYING && !paused) || state == BUFFERING || meter[0] || meter[1])
		GrGetNextEventTimeout(&event, 30);
	else
		GrGetNextEventTimeout(&event, chooser > 0 ? 200 : 1000);
	while (!quit && event.type != GR_EVENT_TYPE_NONE && event.type != GR_EVENT_TYPE_TIMEOUT) {
		handle(&event);
		GrCheckNextEvent(&event);
	}
	if (quit)
		return;
	t = now_ms();
	if (t - looked_ms >= 1000) {
		looked_ms = t;
		if (ui_restyle()) {	/* (Settings changed the look) */
			ui_menu_restyle();
			draw();
		}
	}
	if (shown_state != state * 2 + paused)
		dirty |= D_PANEL | D_SEEK | D_KEYS | D_LIST;
	else if (tune.kind && (int)((uint64_t)position() * 1000 / (uint32_t)tune.rate / 1000) != shown_second && !drag)
		dirty |= D_SEEK;
	if (dirty)
		redraw();
	if (out.on && t - meter_ms >= 70) {
		meter_ms = t;
		meter_take();
		draw_meter(0);
	}
}

static void
leave(int sig)
{
	quit = 1;
}

static int
number(const char *name, int otherwise)
{
	const char *value = getenv(name);

	return value && *value ? atoi(value) : otherwise;
}

/* The window, the list as it was left, and the files asked for: the first plays. */
static void
window_start(int argc, char **argv)
{
	char whole[PATH_MAX], folder[PATH_MAX];
	struct entry *e, *first = NULL;
	int i;

	ui_init();
	print_places = number("NXPLAY_PRINT", 0);
	for (i = 0; i < 20; i++)
		meter_steps[i] = 32767.0f * powf(10.0f, -(float)(19 - i) * 0.1f);
	restore();
	layout();
	window = GrNewWindowEx(GR_WM_PROPS_APPWINDOW, "Music", GR_ROOT_WINDOW_ID, -1, -1, win_w, win_h, UI_FACE);
	GrSelectEvents(window, EVENTS);
	GrMapWindow(window);
	GrSetFocus(window);
	/* the window first: opening a file starts the workers, which is seconds, and a desktop with
	 * nothing new on it looks stuck */
	ui_framed();
	if (argc > 1)
		snprintf(note, sizeof note, "opening it...");
	draw();
	redraw();
	layout();
	GrFlush();
	for (i = 1; i < argc; i++) {
		if (!realpath(argv[i], whole))
			snprintf(whole, sizeof whole, "%s", argv[i]);
		if (i == 1 && !list.count) {
			/* nothing in the list: the tunes of the file's folder, so that Next has somewhere to go */
			folder_of(whole, folder, sizeof folder);
			add_folder(folder);
		}
		if (access(whole, R_OK) != 0) {
			ask_error(whole, "cannot open the file");
			continue;
		}
		if ((e = find_tune(whole)) == NULL)
			e = add_tune(whole, -1);
		if (i == 1)
			first = e;
	}
	if (!list.count)	/* nothing asked for and nothing kept: the image's own tunes */
		add_folder(access("/root/Music", R_OK) == 0 ? "/root/Music" : "/usr/share");
	note[0] = 0;
	if (first)
		play_entry(first, 0, 1);
	mkfifo(HANDED, 0600);
	handed_fd = open(HANDED, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	dirty = D_ALL;
}

int
main(int argc, char **argv)
{
	const char *why = NULL;
	int busy, status;
	pid_t child;

	if (argc > 3 && !strcmp(argv[1], "--choose"))
		return choose_main(argv[2][0], argv[3]);
	exit_at_end = number("NXPLAY_EXIT", 0);
	limit_seconds = (uint32_t)number("NXPLAY_SECONDS", 0);
	/* 48: a tune is some ten cores' work, and every worker is time to start (with all 255 it
	 * played 5.4 s after it was opened, with 48 after 2.9 s) */
	workers_wanted = number("NXPLAY_WORKERS", 48);
	want_sum = number("NXPLAY_SUM", 0);
	half_wanted = number("NXPLAY_HALF", -1);
	memory_mb = (uint32_t)number("NXPLAY_MEMORY", 0);
	mod_rate = number("NXPLAY_MODRATE", mod_rate);
	piece_frames_asked = number("NXPLAY_PIECE", 0);
	if (number("NXPLAY_OWN", 0))		/* the first core takes pieces whatever the workers */
		own_under = MC_MAX_CORES;
	out.volume = number("NXPLAY_VOLUME", 200);
	window_on = !number("NXPLAY_NOWINDOW", 0) && access("/tmp/.nano-X", F_OK) == 0;
	/* (a test's player is its own) */
	if (window_on && !exit_at_end && !limit_seconds && !want_sum && hand_off(argc, argv))
		return 0;
	signal(SIGINT, leave);
	signal(SIGTERM, leave);
	signal(SIGPIPE, SIG_IGN);
	atexit(mcw_close);
	window_on = window_on && GrOpen() >= 0;
	out.on = snd_open() == 0 && (out.voice = snd_claim()) >= 0;
	if (out.on && (out.ring = snd_memory(RING_LONG * 4, &out.ring_at)) != NULL)
		ring_frames = RING_LONG;
	else
		out.on = out.on && (out.ring = snd_memory(RING_SHORT * 4, &out.ring_at)) != NULL;
	if (!out.on)
		say("no sound card here: decoding only");
	if (window_on) {
		window_start(argc, argv);
	} else if (argc < 2) {
		fprintf(stderr, "nxplay FILE: plays a WAV, MOD, FLAC, MP3 or Ogg Vorbis file (docs/apps/play.md)\n");
		return 1;
	} else if ((why = load(argv[1])) != NULL) {
		say(why);
		return 1;
	} else {
		set_volume(out.volume);
	}
	while (!quit) {
		if (want_next) {
			want_next = 0;
			advance(1, 1);
		}
		busy = pump();
		if (window_on) {
			if (!busy && !drag && state != BUFFERING)	/* (an MP3 file's table is seconds: not before it plays) */
				busy = measure_next();
			while (others && (child = waitpid(-1, &status, WNOHANG)) > 0)
				chooser_reaped |= child == chooser;
			chooser_take();
			take_handed();
			if (list_changed && now_ms() - list_changed_ms > 2000)
				save_list();
			if (state == PLAYING && !paused && now_ms() - saved_ms > 30000)
				save_place();
			events(busy);
		} else if (!busy && !quit && out.on) {
			usleep(5000);
		} else if (!busy && !quit) {
			mcw_rest();
		}
	}
	if (window_on) {
		if (list_changed)
			save_list();
		save_place();
		if (chooser > 0)
			kill(chooser, SIGTERM);
		if (handed_fd >= 0)
			unlink(HANDED);
	}
	if (out.on && tune.kind)
		snd_stop(out.voice);
	stats();
	if (window_on)
		GrClose();
	return 0;
}
