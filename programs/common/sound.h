// The sound card (docs/sound.md): up to 32 voices, each playing samples from memory at its own
// pitch and volume, mixed by the device. A program fills in a voice and changes its key; the
// device answers in words of its own, a frame later at the earliest.
#pragma once

// A Linux program maps the registers first (linux/userland/shaderemu_sound.h) and says where.
#ifndef SND_BASE
#include "platform.h"
#define SND_BASE 0x87000000u
#define SND_BARE
#endif

#define SND_ENABLE (*(volatile uint32_t*)(SND_BASE + 0x220))   // not 0: the device mixes
#define SND_MASTER (*(volatile uint32_t*)(SND_BASE + 0x224))   // volume of the mix, 0..256
#define SND_COUNT  (*(volatile uint32_t*)(SND_BASE + 0x228))   // voices the device looks at, from voice 0
#define SND_CLOCK  (*(volatile uint32_t*)(SND_BASE + 0x230))   // output samples played so far
#define SND_RATE   (*(volatile uint32_t*)(SND_BASE + 0x234))   // output samples a second; 0 until the device has answered
#define SND_MAX_VOICES 32

typedef struct {
    uint32_t key;       // a number that changes to start (bit 0 set) or stop (clear) the voice
    uint32_t kind;      // what the samples are, plus flags; read when the key changes, as are the next three and aux
    uint32_t address;
    uint32_t length;    // in samples (both channels of a stereo one count as one)
    uint32_t loop;      // SND_LOOPED: where the end goes back to. SND_STREAM: samples written so far, read all the time
    uint32_t step;      // samples per output sample, 16.16; read all the time
    uint32_t volume;    // SND_VOLUME(left, right); read all the time
    uint32_t aux;
} snd_voice;
#define SND_VOICE ((volatile snd_voice*)(SND_BASE + 0x800))

// What the device keeps for each voice. Read only.
typedef struct {
    uint32_t seen;       // the key it has acted on
    uint32_t state;      // bit 0: playing
    uint32_t position;   // the sample it is at
    uint32_t fraction, address, length, loop, aux;
} snd_voice_state;
#define SND_STATE ((volatile const snd_voice_state*)(SND_BASE + 0xc00))

enum {
    SND_PCM8 = 1,         // unsigned bytes, at any address
    SND_PCM16,            // signed 16-bit, at an even address
    SND_STREAM,           // 16-bit, in a ring of `length` samples (a power of two) the program keeps writing
    SND_ADPCM,            // IMA ADPCM, 4 bits a sample (low half of a byte first), mono; `aux` is its checkpoints:
                          // a word for every 32 samples, the decoder's value | step index << 16 before them
    SND_FM,               // a track of FM notes (snd_note) played by the instruments at `aux` (snd_tone); `length` is
                          // the tune's in output samples, and `step` 0x10000 plays it at its own speed
    SND_LOOPED = 0x100,
    SND_SMOOTH = 0x200,   // interpolate between samples
    SND_STEREO = 0x400,   // left and right interleaved
};
#define SND_VOLUME(left, right) ((uint32_t)(left) | (uint32_t)(right) << 16)   // 0..256 each

// An FM track: its header, then its notes in the order they start. All of it, and the
// instruments, must lie on 16-byte boundaries.
typedef struct {
    uint32_t count, unused[3];
} __attribute__((aligned(16))) snd_track;
typedef struct {
    uint32_t start, held;   // in output samples: when the key goes down, and for how long
    uint32_t pitch;         // the note's turns per output sample, times 2^32
    uint32_t tone;          // which instrument
    uint32_t left, right;   // 0..256
    uint32_t unused[2];
} __attribute__((aligned(16))) snd_note;
// One of an instrument's two operators. Levels are in steps of 1/32 of a halving; 512 is silence.
typedef struct {
    uint32_t flags;         // twice its pitch multiplier, plus the SND_OP bits
    uint32_t level;         // how far down it is at its loudest
    uint32_t attack;        // samples to get there
    uint32_t sustain;       // the level it falls to while the key is down
    uint32_t decay;         // levels fallen per sample, 16.16
    uint32_t decay_samples; // samples until the sustain level is reached
    uint32_t release;       // levels per sample after that (unless SND_OP_HOLDS) and once the key is up
    uint32_t unused;
} __attribute__((aligned(16))) snd_operator;
typedef struct {
    snd_operator shaper, sounder;   // the first bends the second's phase, or with SND_OP_BOTH is heard beside it
} snd_tone;
#define SND_OP_WAVE(n) ((n) << 8)       // 0 sine, 1 half sine, 2 its absolute, 3 quarter pulses
#define SND_OP_HOLDS 0x400              // stays at the sustain level while the key is down
#define SND_OP_FEEDBACK(n) ((n) << 12)  // the shaper's: how much of itself bends it, 0..7
#define SND_OP_BOTH 0x8000              // the shaper's: both are heard

#ifdef SND_BARE
// Turns the device on and waits for it. Returns its output rate, or 0 if the host has none.
static inline uint32_t snd_init(void) {
    SND_MASTER = 256;
    SND_COUNT = SND_MAX_VOICES;
    SND_ENABLE = 1;
    for (int wait = 0; wait < 1000 && SND_RATE == 0; wait++) cpu_wait();
    return SND_RATE;
}
#endif

// The step that plays samples recorded at `rate` a second at their own speed.
static inline uint32_t snd_step(uint32_t rate) {
    uint32_t out = SND_RATE;
    return out ? ((rate / out) << 16) + ((rate % out) << 16) / out : 0;
}

// `samples` is a physical address: the device does not see a program's own memory.
static inline void snd_play_aux(int v, uint32_t kind, uint32_t samples, uint32_t length, uint32_t loop, uint32_t step, uint32_t volume,
                                uint32_t aux) {
    volatile snd_voice* voice = &SND_VOICE[v];
    voice->aux = aux;
    voice->kind = kind;
    voice->address = samples;
    voice->length = length;
    voice->loop = loop;
    voice->step = step;
    voice->volume = volume;
    voice->key = ((voice->key >> 1) + 1) << 1 | 1;
}

static inline void snd_play(int v, uint32_t kind, uint32_t samples, uint32_t length, uint32_t loop, uint32_t step, uint32_t volume) {
    snd_play_aux(v, kind, samples, length, loop, step, volume, 0);
}

static inline void snd_stop(int v) {
    SND_VOICE[v].key = ((SND_VOICE[v].key >> 1) + 1) << 1;
}

// True from the moment a voice is started until the device has played it to its end.
static inline int snd_playing(int v) {
    return SND_STATE[v].seen != SND_VOICE[v].key ? (int)(SND_VOICE[v].key & 1) : (int)(SND_STATE[v].state & 1);
}
