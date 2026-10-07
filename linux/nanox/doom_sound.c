/*
 * Doom's sound on the machine's sound card (docs/sound.md), in place of the port's i_sound.c.
 * An effect is a voice pointed at its lump where the WAD lies in the ROM: nothing is loaded
 * or mixed here. A start costs a few stores, and the game's own channel logic stays as it is.
 */
#include <stdio.h>
#include <stdlib.h>

#include "doomdef.h"
#include "doomstat.h"
#include "i_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

#include "shaderemu_sound.h"

#define VOICES 8

extern int snd_SfxVolume, snd_MusicVolume;

static int ready;                 /* the card is there and the WAD is in the ROM */
static uint32_t wad_at;           /* the WAD's first byte, as the device addresses it */
static int voice[VOICES];         /* the card's voices we hold */
static int playing[VOICES];       /* the handle each was last started with, 0 for none */
static int effect[VOICES];        /* and the effect, to keep some of them to one at a time */
static int started[VOICES];       /* the tic it started in */
static uint32_t pitch_step[256];  /* 16.16: 2 to the power (pitch - 128) / 64 */
static struct {
	uint32_t address, length, rate;   /* 0 length: not looked at yet */
} lump_sound[NUMSFX];

/* A lump's samples: DMX's header is a format, a rate and a count, with 16 bytes of lead-in and
 * lead-out among the samples that are not played. */
static int sound_of(int id)
{
	lumpinfo_t *lump = &lumpinfo[S_sfx[id].lumpnum];
	unsigned char head[8];
	uint32_t count;

	if (lump_sound[id].length)
		return lump_sound[id].rate != 0;
	lump_sound[id].length = 1;
	if (lump->size < 8 + 33 || pread(fileno(lump->handle), head, 8, lump->position) != 8 || head[0] != 3)
		return 0;
	count = head[4] | head[5] << 8 | head[6] << 16 | (uint32_t)head[7] << 24;
	if (count < 33 || count > (uint32_t)lump->size - 8)
		return 0;
	lump_sound[id].address = wad_at + lump->position + 8 + 16;
	lump_sound[id].length = count - 32;
	lump_sound[id].rate = head[2] | head[3] << 8;
	return 1;
}

/* Doom's volume (0..15) and separation (0..255) as the card's left and right, as the port mixed them. */
static uint32_t volume_of(int vol, int sep)
{
	int left, right;

	vol *= 8;
	sep += 1;
	left = vol - ((vol * sep * sep) >> 16);
	sep -= 257;
	right = vol - ((vol * sep * sep) >> 16);
	left = left < 0 ? 0 : left > 127 ? 127 : left;
	right = right < 0 ? 0 : right > 127 ? 127 : right;
	return SND_VOLUME(left * 2, right * 2);
}

static uint32_t step_of(int id, int pitch)
{
	return (uint32_t)(((uint64_t)snd_step(lump_sound[id].rate) * pitch_step[pitch & 255]) >> 16);
}

/* The voice a handle names, or -1 when that sound is over or its voice has another. */
static int slot_of(int handle)
{
	int slot = handle & 15;

	return ready && handle > 0 && slot < VOICES && playing[slot] == handle ? slot : -1;
}

void I_SetChannels(void)
{
	int n;

	pitch_step[128] = 0x10000;
	for (n = 129; n < 256; n++)
		pitch_step[n] = (uint32_t)(((uint64_t)pitch_step[n - 1] * 66250) >> 16);   /* times 2^(1/64) */
	for (n = 127; n >= 0; n--)
		pitch_step[n] = (uint32_t)(((uint64_t)pitch_step[n + 1] << 16) / 66250);
	if (snd_open()) {
		fprintf(stderr, "I_SetChannels: no sound card\n");
		return;
	}
	wad_at = snd_rom(fileno(lumpinfo[0].handle), "IWAD", 4);
	for (n = 0; n < VOICES && wad_at; n++)
		if ((voice[n] = snd_claim()) < 0)
			wad_at = 0;
	ready = wad_at != 0;
	if (!ready)
		fprintf(stderr, "I_SetChannels: no sound (the game's data is not in the ROM, or no voices are free)\n");
}

void I_SetSfxVolume(int volume)
{
	snd_SfxVolume = volume;
}

int I_GetSfxLumpNum(sfxinfo_t *sfx)
{
	char name[9];

	/* the shareware data lacks the second game's effects: those take the pistol's */
	sprintf(name, "ds%s", sfx->name);
	return W_GetNumForName(W_CheckNumForName(name) == -1 ? "dspistol" : name);
}

int I_StartSound(int id, int vol, int sep, int pitch, int priority)
{
	static int serial;
	int n, slot = -1, oldest = 0;

	if (!ready || !sound_of(id))
		return 0;
	/* the port's rule: these are heard once at a time */
	if (id == sfx_sawup || id == sfx_sawidl || id == sfx_sawful || id == sfx_sawhit || id == sfx_stnmov || id == sfx_pistol)
		for (n = 0; n < VOICES; n++)
			if (playing[n] && effect[n] == id && snd_playing(voice[n]))
				slot = n;
	/* else a voice that has ended, or the one that started longest ago */
	for (n = 0; n < VOICES && slot < 0; n++) {
		if (!playing[n] || !snd_playing(voice[n]))
			slot = n;
		else if (started[n] < started[oldest])
			oldest = n;
	}
	if (slot < 0)
		slot = oldest;
	serial = serial % 0x7ffff + 1;
	playing[slot] = serial << 4 | slot;
	effect[slot] = id;
	started[slot] = gametic;
	snd_play(voice[slot], SND_PCM8 | SND_SMOOTH, lump_sound[id].address, lump_sound[id].length, 0,
		 step_of(id, pitch), volume_of(vol, sep));
	return playing[slot];
}

void I_StopSound(int handle)
{
	int slot = slot_of(handle);

	if (slot < 0)
		return;
	snd_stop(voice[slot]);
	playing[slot] = 0;
}

int I_SoundIsPlaying(int handle)
{
	int slot = slot_of(handle);

	return slot >= 0 && snd_playing(voice[slot]);
}

/* Called every tic for every sound that is heard: only what has changed is written. */
void I_UpdateSoundParams(int handle, int vol, int sep, int pitch)
{
	int slot = slot_of(handle);
	uint32_t volume, step;

	if (slot < 0)
		return;
	volume = volume_of(vol, sep);
	step = step_of(effect[slot], pitch);
	if (SND_VOICE[voice[slot]].volume != volume)
		SND_VOICE[voice[slot]].volume = volume;
	if (SND_VOICE[voice[slot]].step != step)
		SND_VOICE[voice[slot]].step = step;
}

void I_InitSound(void)
{
}

void I_ShutdownSound(void)
{
}

/*
 * Music. A song is a MUS score: notes going down and up on sixteen channels, 140 ticks a
 * second, played on the instruments of the WAD's GENMIDI lump (OPL register values). Here a
 * score becomes tracks for the card's FM voices, once, when the game registers it; playing it
 * then costs the game nothing.
 */
#define MUSIC_VOICES 12
#define MAX_NOTES 8192
#define TONES 175
#define NOTE_OPEN 0xffffff        /* held until the score says otherwise */

static int music_voice[MUSIC_VOICES];
static int music_ready, music_playing, music_paused;
static char *track_memory;        /* in sound memory: each voice's header and notes, then the instruments */
static uint32_t track_at, tones_at;
static uint32_t track_of[MUSIC_VOICES];   /* each voice's track, as the device addresses it */
static uint32_t song_samples;
static uint32_t note_pitch[128];  /* turns per output sample, times 2^32 */
static const unsigned char *genmidi;

static snd_note notes[MAX_NOTES];
static unsigned char note_voice[MAX_NOTES];
static int note_count;

/* Levels fallen per sample (16.16) at an OPL decay or release rate: 512 levels take 39.28 s at
 * rate 1 and half as long for each rate above. */
static uint32_t fall_rate(int rate, uint32_t out)
{
	uint32_t ms = rate ? 39281 >> (rate - 1) : 0;

	return rate ? (uint32_t)(512ull * 65536 * 1000 / ((uint64_t)(ms ? ms : 1) * out)) : 0;
}

static void make_operator(snd_operator *op, const unsigned char *r, uint32_t out)
{
	static const unsigned char twice[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };
	int attack = r[1] >> 4, decay = r[1] & 15, sustain = r[2] >> 4, release = r[2] & 15;

	op->flags = twice[r[0] & 15] | SND_OP_WAVE(r[3] & 3) | (r[0] & 0x20 ? SND_OP_HOLDS : 0);
	op->level = (r[5] & 63) * 4;
	op->attack = attack == 0 ? NOTE_OPEN : attack == 15 ? 0 : (out * 2826 / 1000) >> (attack - 1);
	op->sustain = sustain == 15 ? 496 : sustain * 16;
	op->decay = fall_rate(decay, out);
	op->decay_samples = op->decay ? (op->sustain << 16) / op->decay : NOTE_OPEN;
	/* a note that would never die away is given two seconds to */
	op->release = release ? fall_rate(release, out) : 512 * 65536 / (2 * out);
}

/* The instruments, and the notes' pitches, for the card's output rate. */
static int music_setup(void)
{
	static const uint32_t lowest[12] = { 535809, 567670, 601425, 637188, 675077, 715219, 757748, 802806, 850544, 901120,
					     954703, 1011473 };   /* Hz times 65536 of notes 0 to 11 */
	uint32_t out = SND_RATE, bytes = MUSIC_VOICES * 16 + MAX_NOTES * sizeof(snd_note) + TONES * sizeof(snd_tone);
	snd_tone *tone;
	int n, lump = W_CheckNumForName("GENMIDI");

	if (music_ready || !ready || lump < 0 || W_LumpLength(lump) < 8 + TONES * 36 || !out)
		return music_ready;
	genmidi = (const unsigned char *)W_CacheLumpNum(lump, PU_STATIC) + 8;
	track_memory = snd_memory(bytes, &track_at);
	if (!track_memory)
		return 0;
	for (n = 0; n < MUSIC_VOICES; n++)
		if ((music_voice[n] = snd_claim()) < 0)
			return 0;
	tones_at = track_at + MUSIC_VOICES * 16 + MAX_NOTES * sizeof(snd_note);
	tone = (snd_tone *)(track_memory + (tones_at - track_at));
	for (n = 0; n < TONES; n++) {
		const unsigned char *r = genmidi + 36 * n + 4;

		memset(&tone[n], 0, sizeof tone[n]);
		make_operator(&tone[n].shaper, r, out);
		make_operator(&tone[n].sounder, r + 7, out);
		tone[n].shaper.flags |= SND_OP_FEEDBACK((r[6] >> 1) & 7) | (r[6] & 1 ? SND_OP_BOTH : 0);
	}
	for (n = 0; n < 128; n++)
		note_pitch[n] = (uint32_t)(((uint64_t)(lowest[n % 12] << (n / 12)) << 16) / out);
	return music_ready = 1;
}

/* The score as notes in the order they start, each given to one of the voices. */
static void convert_score(const unsigned char *mus)
{
	const unsigned char *p = mus + (mus[6] | mus[7] << 8), *end = p + (mus[4] | mus[5] << 8);
	uint32_t out = SND_RATE, now = 0, part = 0, tail = out / 4;
	uint32_t free_at[MUSIC_VOICES];           /* when each voice's last note has died away */
	int sounding[MUSIC_VOICES];               /* the note it holds, or -1 */
	int key_of[MUSIC_VOICES];                 /* channel << 8 | note, of that */
	int patch[16], level[16], pan[16], struck[16], n, v;

	note_count = 0;
	for (n = 0; n < 16; n++)
		patch[n] = 0, level[n] = 100, pan[n] = 64, struck[n] = 127;
	for (v = 0; v < MUSIC_VOICES; v++)
		free_at[v] = 0, sounding[v] = -1;
	while (p < end) {
		int event = *p++, kind = (event >> 4) & 7, channel = event & 15;

		if (kind == 0) {
			int key = channel << 8 | (*p++ & 127);

			for (v = 0; v < MUSIC_VOICES; v++)
				if (sounding[v] >= 0 && key_of[v] == key) {
					notes[sounding[v]].held = now - notes[sounding[v]].start;
					sounding[v] = -1;
					free_at[v] = now + tail;
				}
		} else if (kind == 1) {
			int note = *p++, tone, pitch, best = 0, loud;
			const unsigned char *r;

			if (note & 128)
				struck[channel] = *p++ & 127;
			note &= 127;
			/* channel 15 is the drums: a note there picks the instrument */
			tone = channel == 15 ? 128 + note - 35 : patch[channel];
			if (tone >= 0 && tone < TONES && note_count < MAX_NOTES) {
				r = genmidi + 36 * tone;
				pitch = channel == 15 || (r[0] & 1) ? r[3] : note + (short)(r[18] | r[19] << 8);
				/* a voice that is silent, else one let go longest ago, else the oldest note */
				for (v = 1; v < MUSIC_VOICES; v++) {
					int idle = sounding[v] < 0, best_idle = sounding[best] < 0;

					if (idle != best_idle ? idle : idle ? free_at[v] < free_at[best]
					    : notes[sounding[v]].start < notes[sounding[best]].start)
						best = v;
				}
				if (sounding[best] >= 0)
					notes[sounding[best]].held = now - notes[sounding[best]].start;
				loud = struck[channel] * level[channel] * 2 / 127;
				notes[note_count].start = now;
				notes[note_count].held = NOTE_OPEN;
				notes[note_count].pitch = note_pitch[pitch & 127];
				notes[note_count].tone = tone;
				notes[note_count].left = loud * (pan[channel] > 64 ? 127 - pan[channel] : 64) / 64;
				notes[note_count].right = loud * (pan[channel] < 64 ? pan[channel] : 64) / 64;
				note_voice[note_count] = best;
				sounding[best] = note_count++;
				key_of[best] = channel << 8 | note;
			}
		} else if (kind == 2 || kind == 3) {
			p++;   /* the pitch wheel is not followed */
		} else if (kind == 4) {
			int what = *p++, value = *p++ & 127;

			if (what == 0)
				patch[channel] = value;
			else if (what == 3)
				level[channel] = value;
			else if (what == 4)
				pan[channel] = value;
		} else if (kind == 6) {
			break;
		}
		if (event & 128) {
			uint32_t ticks = 0;

			do
				ticks = ticks * 128 + (*p & 127);
			while (*p++ & 128);
			/* 140 ticks a second, without a 64-bit division an event */
			now += ticks * (out / 140);
			part += ticks * (out % 140);
			now += part / 140;
			part %= 140;
		}
	}
	for (v = 0; v < MUSIC_VOICES; v++)
		if (sounding[v] >= 0)
			notes[sounding[v]].held = now - notes[sounding[v]].start;
	song_samples = now ? now : 1;
}

/* The notes into sound memory, a voice's after its header. */
static void write_tracks(void)
{
	char *to = track_memory;
	int n, v;

	for (v = 0; v < MUSIC_VOICES; v++) {
		snd_track *head = (snd_track *)to;
		snd_note *first = (snd_note *)(to + sizeof *head);
		uint32_t count = 0;

		track_of[v] = track_at + (uint32_t)(to - track_memory);
		for (n = 0; n < note_count; n++)
			if (note_voice[n] == v)
				first[count++] = notes[n];
		head->count = count;
		to = (char *)(first + count);
	}
}

static uint32_t music_volume(void)
{
	int loud = music_paused ? 0 : snd_MusicVolume * 4;   /* of 256: a dozen voices share the mix */

	return SND_VOLUME(loud, loud);
}

void I_InitMusic(void)
{
}

void I_ShutdownMusic(void)
{
}

void I_SetMusicVolume(int volume)
{
	int v;

	snd_MusicVolume = volume;
	for (v = 0; v < MUSIC_VOICES && music_playing; v++)
		SND_VOICE[music_voice[v]].volume = music_volume();
}

int I_RegisterSong(void *data)
{
	const unsigned char *mus = data;

	if (!music_setup() || memcmp(mus, "MUS\x1a", 4) != 0)
		return 0;
	convert_score(mus);
	write_tracks();
	fprintf(stderr, "I_RegisterSong: %d notes, %u seconds\n", note_count, song_samples / SND_RATE);
	return 1;
}

/* Every voice starts on the same sample when their keys change in one frame of the machine. */
void I_PlaySong(int handle, int looping)
{
	int v;

	if (!music_ready || handle != 1)
		return;
	music_paused = 0;
	for (v = 0; v < MUSIC_VOICES; v++)
		snd_play_aux(music_voice[v], SND_FM | (looping ? SND_LOOPED : 0), track_of[v], song_samples, 0, 0x10000,
			     music_volume(), tones_at);
	music_playing = 1;
}

/* Paused, the tune's clock stands still and it is not heard. */
void I_PauseSong(int handle)
{
	int v;

	music_paused = 1;
	for (v = 0; v < MUSIC_VOICES && music_playing; v++) {
		SND_VOICE[music_voice[v]].step = 0;
		SND_VOICE[music_voice[v]].volume = 0;
	}
}

void I_ResumeSong(int handle)
{
	int v;

	music_paused = 0;
	for (v = 0; v < MUSIC_VOICES && music_playing; v++) {
		SND_VOICE[music_voice[v]].step = 0x10000;
		SND_VOICE[music_voice[v]].volume = music_volume();
	}
}

void I_StopSong(int handle)
{
	int v;

	for (v = 0; v < MUSIC_VOICES && music_playing; v++)
		snd_stop(music_voice[v]);
	music_playing = 0;
}

void I_UnRegisterSong(int handle)
{
}

int I_QrySongPlaying(int handle)
{
	return music_playing;
}
