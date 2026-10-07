// Sound test card: every kind of voice, started, changed and stopped at fixed samples of the
// device's clock, for half a second. It is checked against tools/sound_reference.py, which
// mixes the same voices from the harness's capture (docs/sound.md).

#include "../common/sound.h"

#define WAVE 256
#define RING 4096

static int16_t sine[WAVE];
static int16_t pair[2 * WAVE];        // stereo: a sine on the left, a ramp on the right
static uint8_t bytes[1 + 4000];       // 8-bit, from an odd address on
static uint8_t byte_pair[2 * 600];
static int16_t ring[RING];
#define CODED 3000
static uint8_t coded[CODED / 2];      // ADPCM: a sweep, four bits a sample
static uint32_t marks[(CODED + 31) / 32];

static const int16_t steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
    1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// FM: two instruments and a tune of six notes that goes round. The first is a plucked string
// (a shaper that bends itself, levels falling away), the second two held tones heard together.
#define HZ(f) ((uint32_t)((f) * 89478.485))   // turns per sample at 48,000 a second, times 2^32
static snd_tone tones[2] = {
    {{2 | SND_OP_FEEDBACK(3), 24, 0, 200, 0x0600, 200 * 65536 / 0x0600, 0x0100},
     {2, 0, 30, 48, 0x0300, 48 * 65536 / 0x0300, 0x0080}},
    {{4 | SND_OP_WAVE(1) | SND_OP_HOLDS | SND_OP_BOTH, 40, 900, 16, 0x0040, 16 * 65536 / 0x0040, 0x0200},
     {1 | SND_OP_WAVE(2) | SND_OP_HOLDS, 8, 300, 0, 0, 0, 0x0400}},
};
static struct {
    snd_track head;
    snd_note notes[6];
} tune = {{6}, {
    {0, 2500, HZ(220.0), 0, 256, 128},
    {3000, 1500, HZ(277.18), 0, 128, 256},
    {5000, 400, HZ(329.63), 0, 256, 256},
    {6000, 5000, HZ(110.0), 1, 200, 200},
    {12000, 3000, HZ(440.0), 0, 256, 60},
    {15500, 300, HZ(880.0), 1, 60, 256},
}};

// IMA ADPCM of a sine that rises in pitch, with the decoder's state noted every 32 samples.
static void make_coded(void) {
    int value = 0, index = 0;
    uint32_t phase = 0;
    for (int i = 0; i < CODED; i++) {
        if ((i & 31) == 0) marks[i >> 5] = (uint32_t)(value & 0xffff) | (uint32_t)index << 16;
        phase += 3 + i / 40;
        int want = sine[phase & (WAVE - 1)] * 2, size = steps[index], off = want - value, code = 0, change = size >> 3;
        if (off < 0) code = 8, off = -off;
        if (off >= size) code |= 4, off -= size, change += size;
        if (off >= size >> 1) code |= 2, off -= size >> 1, change += size >> 1;
        if (off >= size >> 2) code |= 1, change += size >> 2;
        value += code & 8 ? -change : change;
        value = value < -32768 ? -32768 : value > 32767 ? 32767 : value;
        index += code & 4 ? 2 * (code & 3) + 2 : -1;
        index = index < 0 ? 0 : index > 88 ? 88 : index;
        coded[i >> 1] |= (uint8_t)(code << (4 * (i & 1)));
    }
}

// What the stream plays at sample i; the reference model has the same line.
static inline int16_t stream_sample(uint32_t i) {
    return (int16_t)(((i * 2654435761u) >> 19) - 4096);
}

static void make_waves(void) {
    for (int i = 0; i < WAVE; i++) {
        // Bhaskara's sine: 16x(pi - x) / (5 pi^2 - 4x(pi - x)), with pi = 128 steps
        int x = i & 127, p = x * (128 - x);
        int s = 12000 * 16 * p / (5 * 128 * 128 - 4 * p);
        sine[i] = (int16_t)(i < 128 ? s : -s);
        pair[2 * i] = sine[i];
        pair[2 * i + 1] = (int16_t)((i - 128) * 60);
    }
    uint32_t noise = 12345;
    for (int i = 0; i < 4000; i++) {
        noise = noise * 1664525u + 1013904223u;
        // a decaying burst of noise around the middle value
        bytes[1 + i] = (uint8_t)(128 + (((int)(noise >> 24) - 128) * (4000 - i)) / 8000);
    }
    for (int i = 0; i < 600; i++) {
        byte_pair[2 * i] = (uint8_t)(128 + ((i % 100) - 50));
        byte_pair[2 * i + 1] = (uint8_t)(128 + ((i % 60) < 30 ? 40 : -40));
    }
}

int main(void) {
    make_waves();
    make_coded();
    uint32_t rate = snd_init();
    if (rate == 0) {
        uart_puts("sound: no device\r\n");
        for (;;) cpu_wait();
    }
    uint32_t start = SND_CLOCK, written = 0;
    int event = 0, streaming = 0;
    for (;;) {
        uint32_t now = SND_CLOCK - start;
        // one event at a time, each once its sample has come
        static const uint32_t when[] = {0, 2000, 4000, 6000, 9000, 10000, 12000, 14000, 15000, 16000, 20000, 24000};
        while (event < 12 && now >= when[event]) {
            switch (event++) {
            case 0:
                snd_play(0, SND_PCM16 | SND_LOOPED | SND_SMOOTH, (uint32_t)sine, WAVE, 0, snd_step(WAVE * 440), SND_VOLUME(160, 0));
                snd_play(2, SND_PCM16 | SND_STEREO | SND_LOOPED | SND_SMOOTH, (uint32_t)pair, WAVE, 64, snd_step(WAVE * 55), SND_VOLUME(90, 90));
                snd_play(5, SND_PCM8 | SND_STEREO | SND_LOOPED, (uint32_t)byte_pair, 600, 100, snd_step(8000), SND_VOLUME(60, 120));
                snd_play_aux(7, SND_FM | SND_LOOPED, (uint32_t)&tune, 17000, 0, 0x10000, SND_VOLUME(90, 90), (uint32_t)tones);
                break;
            case 1:
                snd_play(1, SND_PCM8, (uint32_t)(bytes + 1), 4000, 0, snd_step(11025), SND_VOLUME(0, 256));
                snd_play_aux(6, SND_ADPCM | SND_SMOOTH, (uint32_t)coded, CODED, 0, snd_step(22050), SND_VOLUME(200, 100), (uint32_t)marks);
                break;
            case 2:
                snd_play(3, SND_STREAM, (uint32_t)ring, RING, 0, snd_step(22050), SND_VOLUME(128, 128));
                streaming = 1;
                break;
            case 3: SND_VOICE[0].step = snd_step(WAVE * 660); break;
            case 4: SND_VOICE[0].volume = SND_VOLUME(40, 200); break;
            case 5: snd_stop(2); break;
            case 6: streaming = 0; break;   // the stream runs dry
            case 7: snd_play(1, SND_PCM8 | SND_SMOOTH, (uint32_t)(bytes + 1), 4000, 0, snd_step(5000), SND_VOLUME(200, 50)); break;
            case 8: streaming = 1; break;
            case 9:
                snd_play(4, SND_PCM16, (uint32_t)sine, WAVE, 0, snd_step(WAVE * 20), SND_VOLUME(256, 256));   // ends by itself
                snd_play_aux(6, SND_ADPCM | SND_LOOPED, (uint32_t)coded, CODED - 1, 1001, snd_step(30000), SND_VOLUME(80, 160), (uint32_t)marks);
                break;
            case 10: SND_MASTER = 128; break;
            case 11:
                uart_puts(snd_playing(0) && !snd_playing(4) && !snd_playing(2) ? "sound: done\r\n" : "sound: FAILED\r\n");
                for (;;) cpu_wait();
            }
        }
        if (streaming) {
            // keep half a ring ahead of where the device reads
            uint32_t upto = SND_STATE[3].position + RING / 2;
            if (SND_STATE[3].seen != SND_VOICE[3].key) upto = RING / 2;
            for (; (int32_t)(upto - written) > 0; written++) ring[written & (RING - 1)] = stream_sample(written);
            SND_VOICE[3].loop = written;
        }
        cpu_wait();
    }
}
