/*
 * MIDI on the sound card's FM voices (docs/machine/sound.md, kind 5): a General MIDI bank for OPL in its
 * GENMIDI form (DMXOPL's genmidi.op2) made into the card's instruments, and a song's messages, as they come,
 * made into notes given to the voices. No file reader and no memory of its own: OpenTTD gives it what its
 * MidiFile read (linux/openttd/shaderemu_m.cpp), Age of Empires what its own reader did (linux/aoe/port/aoe_music.cpp).
 * Include linux/userland/shaderemu_sound.h first. It may run on a worker: it stores only to what it is given.
 */
#pragma once

#include <math.h>

#define FM_TONES 175                   /* 128 instruments and 47 drums, each with up to two voices */
#define FM_BANK_BYTES (8 + FM_TONES * 36)   /* the name, then the instruments; the instruments' names after them are not read */
#define FM_MAX_VOICES SND_MAX_VOICES
#define FM_NOTE_OPEN 0xffffffu         /* held until the song says otherwise */

/** Levels fallen per sample (16.16) at an OPL decay or release rate: 512 levels take 39.28 s at rate 1. */
static inline uint32_t fm_fall_rate(int rate, uint32_t out)
{
	uint32_t ms = rate ? 39281 >> (rate - 1) : 0;
	return rate ? (uint32_t)(512ull * 65536 * 1000 / ((uint64_t)(ms ? ms : 1) * out)) : 0;
}

static inline void fm_operator(snd_operator *op, const uint8_t *r, uint32_t out)
{
	static const uint8_t twice[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };
	int attack = r[1] >> 4, decay = r[1] & 15, sustain = r[2] >> 4, release = r[2] & 15;

	memset(op, 0, sizeof(*op));
	op->flags = twice[r[0] & 15] | SND_OP_WAVE(r[3] & 3) | (r[0] & 0x20 ? SND_OP_HOLDS : 0);
	op->level = (r[5] & 63) * 4;
	op->attack = attack == 0 ? FM_NOTE_OPEN : attack == 15 ? 0 : (out * 2826 / 1000) >> (attack - 1);
	op->sustain = sustain == 15 ? 496 : sustain * 16;
	op->decay = fm_fall_rate(decay, out);
	op->decay_samples = op->decay ? (op->sustain << 16) / op->decay : FM_NOTE_OPEN;
	/* a note that would never die away is given two seconds to */
	op->release = release ? fm_fall_rate(release, out) : 512 * 65536 / (2 * out);
}

/** The bank's instruments, 2 * FM_TONES of them: tone t is an instrument's first voice, FM_TONES + t its second. */
static inline void fm_make_tones(snd_tone *tone, const uint8_t *bank, uint32_t out)
{
	int t;

	for (t = 0; t < 2 * FM_TONES; t++) {
		const uint8_t *r = bank + 8 + 36 * (t % FM_TONES) + 4 + 16 * (t / FM_TONES);
		fm_operator(&tone[t].shaper, r, out);
		fm_operator(&tone[t].sounder, r + 7, out);
		tone[t].shaper.flags |= SND_OP_FEEDBACK((r[6] >> 1) & 7) | (r[6] & 1 ? SND_OP_BOTH : 0);
	}
}

/** Each MIDI note's turns per output sample, times 2^32. */
static inline void fm_make_pitches(uint32_t *pitch, uint32_t out)
{
	static const uint32_t lowest[12] = { 535809, 567670, 601425, 637188, 675077, 715219, 757748, 802806, 850544, 901120,
			954703, 1011473 }; /* Hz times 65536 of MIDI notes 0 to 11 */
	int n;

	for (n = 0; n < 128; n++)
		pitch[n] = (uint32_t)(((uint64_t)(lowest[n % 12] << (n / 12)) << 16) / out);
}

typedef struct {
	snd_note note;
	int voice;
} fm_note;

/** The second voices' tuning factors, kept from song to song (a pow is 7,000 instructions here). */
typedef struct {
	double factor[FM_TONES];
	uint8_t made[FM_TONES];
} fm_tunes;

/** A song being made: the caller fills what is above the line and sets now before each message. */
typedef struct fm_song {
	const uint8_t *bank;
	const uint32_t *pitches;
	fm_tunes *tunes;
	int voices;                 /* at most FM_MAX_VOICES */
	uint32_t tail;              /* output samples a voice let go keeps sounding */
	uint32_t max_notes;
	fm_note *notes;             /* room for cap notes, in the order they start */
	uint32_t cap;
	void (*grow)(struct fm_song *); /* or 0: at cap, no more notes */
	void *owner;
	uint32_t now;               /* output samples */
	/* ---- */
	uint32_t count;
	uint32_t stolen;            /* notes cut because every voice was sounding */
	uint32_t free_at[FM_MAX_VOICES];
	int sounding[FM_MAX_VOICES], key_of[FM_MAX_VOICES];
	uint8_t patch[16], level[16], expression[16], pan[16];
} fm_song;

static inline void fm_song_begin(fm_song *s)
{
	int i;

	s->count = s->stolen = 0;
	for (i = 0; i < FM_MAX_VOICES; i++) {
		s->free_at[i] = 0;
		s->sounding[i] = -1;
		s->key_of[i] = 0;
	}
	for (i = 0; i < 16; i++) {
		s->patch[i] = 0;
		s->level[i] = 100;
		s->expression[i] = 127;
		s->pan[i] = 64;
	}
}

static inline void fm_release(fm_song *s, int v)
{
	s->notes[s->sounding[v]].note.held = s->now - s->notes[s->sounding[v]].note.start;
	s->sounding[v] = -1;
	s->free_at[v] = s->now + s->tail;
}

static inline void fm_note_on(fm_song *s, int channel, int key, int velocity)
{
	int tone = channel == 9 ? 128 + key - 35 : s->patch[channel], k;
	const uint8_t *r;
	int flags, loud;

	if (tone < 0 || tone >= FM_TONES)
		return;
	r = s->bank + 8 + 36 * tone;
	flags = r[0] | r[1] << 8;
	loud = velocity * s->level[channel] * s->expression[channel] * 2 / (127 * 127);
	for (k = 0; k < ((flags & 4) ? 2 : 1) && s->count < s->max_notes; k++) {
		int offset = (int16_t)(r[18 + 16 * k] | r[19 + 16 * k] << 8);
		int pitch = (channel == 9 || (flags & 1)) ? r[3] : key + offset;
		uint32_t turns;
		int best = 0, v;
		fm_note *n;

		pitch = pitch < 0 ? 0 : pitch > 127 ? 127 : pitch;
		turns = s->pitches[pitch];
		/* the second voice is tuned apart by the bank's fine tune, in 32nds of a semitone */
		if (k == 1) {
			if (!s->tunes->made[tone]) {
				s->tunes->factor[tone] = pow(2.0, ((r[2] / 2) - 64) / (32.0 * 12.0));
				s->tunes->made[tone] = 1;
			}
			turns = (uint32_t)(turns * s->tunes->factor[tone]);
		}
		/* a voice that is silent, else one let go longest ago, else the oldest note */
		for (v = 1; v < s->voices; v++) {
			int idle = s->sounding[v] < 0, best_idle = s->sounding[best] < 0;
			if (idle != best_idle ? idle : idle ? s->free_at[v] < s->free_at[best] : s->notes[s->sounding[v]].note.start < s->notes[s->sounding[best]].note.start)
				best = v;
		}
		if (s->sounding[best] >= 0) {
			fm_release(s, best);
			s->stolen++;
		}
		if (s->count == s->cap) {
			if (!s->grow)
				return;
			s->grow(s);
		}
		n = &s->notes[s->count];
		memset(n, 0, sizeof(*n));
		n->note.start = s->now;
		n->note.held = FM_NOTE_OPEN;
		n->note.pitch = turns;
		n->note.tone = tone + FM_TONES * k;
		n->note.left = loud * (s->pan[channel] > 64 ? 127 - s->pan[channel] : 64) / 64;
		n->note.right = loud * (s->pan[channel] < 64 ? s->pan[channel] : 64) / 64;
		n->voice = best;
		s->sounding[best] = (int)s->count;
		s->key_of[best] = channel << 8 | key;
		s->count++;
	}
}

/** One channel message; heard: 0 for what comes before the place a song is played from (it only sets the channels). */
static inline void fm_message(fm_song *s, int status, int a, int b, int heard)
{
	int channel = status & 15, v;

	switch (status & 0xF0) {
		case 0xC0: /* a program */
			s->patch[channel] = a & 127;
			break;
		case 0x90: /* a note on */
			if (b > 0) {
				if (heard)
					fm_note_on(s, channel, a & 127, b);
				break;
			}
			/* fall through */
		case 0x80: /* a note off */
			for (v = 0; v < s->voices; v++) {
				if (s->sounding[v] >= 0 && s->key_of[v] == (channel << 8 | a))
					fm_release(s, v);
			}
			break;
		case 0xB0: /* a controller */
			if (a == 7)
				s->level[channel] = b;
			if (a == 11)
				s->expression[channel] = b;
			if (a == 10)
				s->pan[channel] = b;
			if (a == 121)
				s->expression[channel] = 127;
			if (a == 120 || a == 123) {
				for (v = 0; v < s->voices; v++) {
					if (s->sounding[v] >= 0 && (s->key_of[v] >> 8) == channel)
						fm_release(s, v);
				}
			}
			break;
	}
}

/** The song's end: what is still held is let go at s->now. */
static inline void fm_song_end(fm_song *s)
{
	int v;

	for (v = 0; v < s->voices; v++) {
		if (s->sounding[v] >= 0)
			fm_release(s, v);
	}
}

/**
 * The notes written as the voices' tracks at to: each voice's header and notes one after the other. to's physical address
 * is at; track_of says where each voice's lies. Bytes written: the sum of (16 + 32 notes) over the voices.
 */
static inline void fm_write_tracks(const fm_song *s, void *to, uint32_t at, uint32_t *track_of)
{
	uint32_t count[FM_MAX_VOICES], placed[FM_MAX_VOICES], i;
	snd_note *first[FM_MAX_VOICES];
	char *p = (char *)to;
	int v;

	/* (counted first, then each placed: a pass over the notes for each voice was a third of a song's start) */
	for (v = 0; v < s->voices; v++)
		count[v] = placed[v] = 0;
	for (i = 0; i < s->count; i++)
		count[s->notes[i].voice]++;
	for (v = 0; v < s->voices; v++) {
		snd_track *head = (snd_track *)p;
		first[v] = (snd_note *)(p + sizeof(*head));
		track_of[v] = at + (uint32_t)(p - (char *)to);
		memset(head, 0, sizeof(*head));
		head->count = count[v];
		p = (char *)(first[v] + count[v]);
	}
	for (i = 0; i < s->count; i++) {
		int w = s->notes[i].voice;
		first[w][placed[w]++] = s->notes[i].note;
	}
}
