#ifndef SOUND_H
#define SOUND_H

// The sound card (docs/sound.md): voices the guest sets up, mixed here one pixel an output
// sample. Included by gpu.h, whose ram() and texture_word() it uses; programs/common/sound.h
// has the same numbers for C. Addresses below are RAM texel indices.
#define SND_GUEST  0x700022u   // the guest's: enable, master volume (0..256), voices in use
#define SND_DEVICE 0x700023u   // the device's: sample clock, output rate
#define SND_VOICES 0x700080u   // two texels a voice: key, kind, address, length; loop, step, volume, aux
#define SND_STATE  0x7000c0u   // the device's two: seen, state, position, fraction; address, length, loop, aux
#define SND_MAX_VOICES 32u
#define SND_RING 16384u        // samples the host keeps ahead: the mix target's pixels, 128 a row

// A voice's kind: what its samples are (low byte), plus flags.
#define SND_PCM8 1u         // unsigned bytes
#define SND_PCM16 2u        // signed 16-bit, at an even address
#define SND_STREAM 3u       // 16-bit, a ring of `length` samples (a power of two); `loop` is how many the guest has written
#define SND_ADPCM 4u        // IMA ADPCM, 4 bits a sample, low half of a byte first; `aux` is its table of checkpoints
#define SND_FM 5u           // a track of FM notes at `address`, instruments at `aux`; `length` is the tune's, in output samples
#define SND_LOOPED 0x100u   // at the end go back to `loop`
#define SND_SMOOTH 0x200u   // interpolate between samples
#define SND_STEREO 0x400u   // left and right interleaved

// The host's words for this frame: the sample its ring starts at, whether it mixed, its rate.
#ifndef SOUND_CURSOR
#define SOUND_CURSOR _SoundCursor
#define SOUND_MIXED _SoundMixed
#define SOUND_RATE _SoundRate
#endif

struct snd_voice {
    uint key, kind, address, length, restart, aux, pitch, volume;
    uint pos, frac;   // the sample it is at, and 16 bits of fraction
    bool on;
};

// Moves a voice on by k output samples: k times its 16.16 step, then the loop or the end.
void snd_move(inout snd_voice v, uint k) {
    uint kl = k & 0xffff, kh = k >> 16, pl = v.pitch & 0xffff, ph = v.pitch >> 16;
    uint low = kl * pl + v.frac;
    v.pos += (low >> 16) + kl * ph + kh * pl + ((kh * ph) << 16);
    v.frac = low & 0xffff;
    if ((v.kind & 0xff) == SND_STREAM) {
        // nothing written yet from here on: it waits at the guest's write index
        if ((int)(v.restart - v.pos) <= 0) {
            v.pos = v.restart;
            v.frac = 0;
        }
    } else if (v.pos >= v.length) {
        if ((v.kind & SND_LOOPED) != 0 && v.restart < v.length) v.pos = v.restart + (v.pos - v.restart) % (v.length - v.restart);
        else v.on = false;
    }
}

// Voice n as it is at sample `cursor`. A key the device has not seen yet starts it there (or
// stops it); otherwise it has moved on from where the device left it at its clock.
snd_voice snd_voice_at(uint n, uint cursor) {
    uint4 a = ram(SND_VOICES + 2 * n), b = ram(SND_VOICES + 2 * n + 1);
    uint4 c = ram(SND_STATE + 2 * n), d = ram(SND_STATE + 2 * n + 1);
    snd_voice v;
    v.key = a.r;
    v.pitch = b.g;
    v.volume = b.b;
    uint moved = 0;
    if (a.r != c.r) {
        v.on = (a.r & 1) != 0;
        v.kind = a.g;
        v.address = a.b;
        v.length = a.a;
        v.restart = b.r;
        v.aux = b.a;
        v.pos = 0;
        v.frac = 0;
    } else {
        v.on = (c.g & 1) != 0;
        v.kind = c.g >> 8;
        v.address = d.r;
        v.length = d.g;
        v.restart = d.b;
        v.aux = d.a;
        v.pos = c.b;
        v.frac = c.a;
        moved = cursor - ram(SND_DEVICE).r;
    }
    if ((v.kind & 0xff) == SND_STREAM) v.restart = b.r;   // the write index is read live
    if (v.on) snd_move(v, moved);
    return v;
}

// True when a voice has a sample to give where it is.
bool snd_sounds(snd_voice v) {
    return v.on && !((v.kind & 0xff) == SND_STREAM && v.pos == v.restart);
}

int snd_half(uint address) {
    int h = (int)((texture_word(address & ~3u) >> (8 * (address & 2))) & 0xffff);
    return (h ^ 0x8000) - 0x8000;
}
int snd_byte(uint address) {
    return ((int)((texture_word(address & ~3u) >> (8 * (address & 3))) & 0xff) - 128) * 256;
}

// Sample p of a voice, left and right, as 16 bits signed.
int2 snd_read(snd_voice v, uint p) {
    uint kind = v.kind & 0xff;
    if (kind == SND_STREAM) p &= v.length - 1;
    bool stereo = (v.kind & SND_STEREO) != 0;
    uint first = stereo ? 2 * p : p, second = stereo ? first + 1 : first;
    if (kind == SND_PCM8) return int2(snd_byte(v.address + first), snd_byte(v.address + second));
    return int2(snd_half(v.address + 2 * first), snd_half(v.address + 2 * second));
}

// ADPCM is decoded from the checkpoint before the sample: a word for every 32 samples, the
// decoder's value (16 bits) and step index (8) as they are before the first of them.
static const int snd_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// Sample p of an ADPCM voice and the one after it (the same again at the very end).
int2 snd_adpcm(snd_voice v, uint p) {
    uint mark = texture_word(v.aux + 4 * (p >> 5));
    int value = (int)((mark & 0xffff) ^ 0x8000) - 0x8000, index = (int)((mark >> 16) & 0xff);
    uint last = min(p + 1, v.length - 1), word = 0;
    int2 both = 0;
    [loop]
    for (uint n = p & ~31u; n <= last; n++) {
        uint at = v.address + (n >> 1);
        if (n == (p & ~31u) || (at & 3) == 0 && (n & 1) == 0) word = texture_word(at & ~3u);
        uint code = (word >> (8 * (at & 3) + 4 * (n & 1))) & 15;
        int size = snd_steps[min(index, 88)], change = size >> 3;
        if (code & 4) change += size;
        if (code & 2) change += size >> 1;
        if (code & 1) change += size >> 2;
        value = clamp(value + ((code & 8) ? -change : change), -32768, 32767);
        index = clamp(index + ((code & 4) ? 2 * (int)(code & 3) + 2 : -1), 0, 88);
        if (n == p) both.x = value;
        both.y = value;
    }
    return both;
}

// FM (SND_FM): a voice plays a track of notes, each two sine operators, the first bending the
// second's phase, with envelopes worked out from how long the note has been on (docs/sound.md).
// Levels are in steps of 1/32 of a halving, 512 of them being silence.
static const int snd_sine[256] = {
    13, 38, 63, 88, 113, 138, 163, 188, 213, 239, 264, 289, 314, 339, 364, 389,
    414, 439, 464, 489, 514, 539, 564, 588, 613, 638, 663, 688, 712, 737, 762, 787,
    811, 836, 860, 885, 909, 934, 958, 983, 1007, 1032, 1056, 1080, 1104, 1128, 1153, 1177,
    1201, 1225, 1249, 1273, 1296, 1320, 1344, 1368, 1391, 1415, 1439, 1462, 1485, 1509, 1532, 1555,
    1579, 1602, 1625, 1648, 1671, 1694, 1717, 1739, 1762, 1785, 1807, 1830, 1852, 1875, 1897, 1919,
    1941, 1964, 1986, 2007, 2029, 2051, 2073, 2094, 2116, 2137, 2159, 2180, 2201, 2223, 2244, 2265,
    2285, 2306, 2327, 2348, 2368, 2389, 2409, 2429, 2449, 2470, 2490, 2509, 2529, 2549, 2569, 2588,
    2608, 2627, 2646, 2665, 2684, 2703, 2722, 2741, 2759, 2778, 2796, 2815, 2833, 2851, 2869, 2887,
    2904, 2922, 2940, 2957, 2974, 2992, 3009, 3026, 3043, 3059, 3076, 3093, 3109, 3125, 3141, 3157,
    3173, 3189, 3205, 3221, 3236, 3251, 3267, 3282, 3297, 3311, 3326, 3341, 3355, 3370, 3384, 3398,
    3412, 3426, 3439, 3453, 3466, 3480, 3493, 3506, 3519, 3532, 3544, 3557, 3569, 3581, 3594, 3606,
    3617, 3629, 3641, 3652, 3663, 3675, 3686, 3696, 3707, 3718, 3728, 3739, 3749, 3759, 3769, 3778,
    3788, 3798, 3807, 3816, 3825, 3834, 3843, 3851, 3860, 3868, 3876, 3884, 3892, 3900, 3908, 3915,
    3922, 3929, 3936, 3943, 3950, 3957, 3963, 3969, 3975, 3981, 3987, 3993, 3998, 4004, 4009, 4014,
    4019, 4023, 4028, 4033, 4037, 4041, 4045, 4049, 4053, 4056, 4059, 4063, 4066, 4069, 4071, 4074,
    4076, 4079, 4081, 4083, 4085, 4087, 4088, 4089, 4091, 4092, 4093, 4093, 4094, 4095, 4095, 4095};
static const int snd_loud[32] = {
    4096, 4008, 3922, 3838, 3756, 3676, 3597, 3520, 3444, 3371, 3298, 3228, 3158, 3091, 3025, 2960,
    2896, 2834, 2774, 2714, 2656, 2599, 2543, 2489, 2435, 2383, 2332, 2282, 2233, 2186, 2139, 2093};

// A sine of 4095 at a phase of which 2^32 is a turn; `wave` 1 to 3 are the OPL's cut shapes.
int snd_wave(uint phase, uint wave) {
    uint at = phase >> 22, n = at & 255;
    int v = snd_sine[(at & 256) != 0 ? 255 - n : n];
    if (wave == 3) return (at & 256) != 0 ? 0 : v;
    if ((at & 512) != 0) v = wave == 1 ? 0 : wave == 2 ? v : -v;
    return v;
}

// (k * rate) >> 16, for k below 2^24 and rate below 2^20.
uint snd_scale(uint k, uint rate) {
    uint kl = k & 0xffff, kh = k >> 16, rl = rate & 0xffff, rh = rate >> 16;
    return ((kl * rl) >> 16) + kl * rh + kh * rl + ((kh * rh) << 16);
}

// How far down an operator is `t` samples into a held note: its two texels are flags, level,
// attack samples, sustain level; decay rate, samples of decay, release rate.
uint snd_held(uint4 a, uint4 b, uint t) {
    if (t <= a.b) return 0;
    uint after = min(t - a.b, 0xffffffu);
    if (after <= b.g) return min(snd_scale(after, b.r), a.a);
    return a.a + ((a.r & 0x400) != 0 ? 0 : snd_scale(min(after - b.g, 0xffffffu), b.b));
}

// An operator's loudness, 0 to 4096, t samples into a note let go after `held` samples.
int snd_envelope(uint4 a, uint4 b, uint t, uint held) {
    uint down = snd_held(a, b, min(t, held)) + a.g;
    if (t > held) down += snd_scale(min(t - held, 0xffffffu), b.b);
    if (down >= 512) return 0;
    int loud = snd_loud[down & 31] >> (down >> 5);
    uint rising = min(t, held);
    if (rising < a.b) loud = (int)((uint)loud * rising / a.b);
    return loud;
}

// The voice's track at sample t: a count, then two texels a note in order of their starts
// (start, samples held, phase step, instrument; left and right gain).
int2 snd_fm(snd_voice v, uint t) {
    uint track = texel_of(v.address), count = min(ram(track).r, 65535u);
    if (count == 0) return 0;
    uint lo = 0, hi = count;
    for (uint step = 0; step < 16; step++) {
        uint mid = (lo + hi) >> 1;
        if (hi - lo > 1) {
            if (ram(track + 1 + 2 * mid).r <= t) lo = mid;
            else hi = mid;
        }
    }
    uint4 note = ram(track + 1 + 2 * lo), gain = ram(track + 2 + 2 * lo);
    if (note.r > t) return 0;
    uint since = t - note.r, tone = texel_of(v.aux) + 4 * note.a;
    uint4 ma = ram(tone), mb = ram(tone + 1), ca = ram(tone + 2), cb = ram(tone + 3);
    int loud = snd_envelope(ca, cb, since, note.g);
    if (loud == 0) return 0;
    // an operator runs at the note's pitch times half its multiplier
    uint mm = ma.r & 0xff, cm = ca.r & 0xff;
    uint mp = mm == 1 ? (note.b >> 1) * since : note.b * since * (mm >> 1);
    uint cp = cm == 1 ? (note.b >> 1) * since : note.b * since * (cm >> 1);
    int bend = snd_envelope(ma, mb, since, note.g);
    uint back = (ma.r >> 12) & 7;
    int first = back == 0 ? 0 : (snd_wave(mp, (ma.r >> 8) & 3) * bend) >> 12;
    int m = (snd_wave(mp + (uint)(first << (13 + back)), (ma.r >> 8) & 3) * bend) >> 12;
    int s;
    if ((ma.r & 0x8000) != 0) s = ((snd_wave(cp, (ca.r >> 8) & 3) * loud) >> 12) + m;
    else s = (snd_wave(cp + (uint)(m << 22), (ca.r >> 8) & 3) * loud) >> 12;
    return (int2(s, s) * int2(min(gain.r, 256u), min(gain.g, 256u))) >> 6;
}

// SoundMix: the pixel's sample is the one with its number (mod SND_RING) at or after the cursor.
float2 sound_mix(uint2 pixel) {
    uint4 guest = ram(SND_GUEST);
    uint cursor = SOUND_CURSOR;
    uint k = (pixel.y * 128 + pixel.x - cursor) & (SND_RING - 1);
    uint count = guest.r == 0 ? 0 : min(guest.b, SND_MAX_VOICES);
    int2 sum = 0;
    [loop]
    for (uint n = 0; n < count; n++) {
        snd_voice v = snd_voice_at(n, cursor);
        if (v.on) snd_move(v, k);
        [branch]
        if (snd_sounds(v)) {
            int2 s;
            [branch]
            if ((v.kind & 0xff) == SND_ADPCM) {
                int2 both = snd_adpcm(v, v.pos);
                s = both.x;
                if ((v.kind & SND_SMOOTH) != 0) s += ((both.y - both.x) * (int)(v.frac >> 4)) >> 12;
            } else if ((v.kind & 0xff) == SND_FM) {
                s = snd_fm(v, v.pos);
            } else {
                s = snd_read(v, v.pos);
            }
            [branch]
            if ((v.kind & SND_SMOOTH) != 0 && (v.kind & 0xff) < SND_ADPCM) {
                // towards the sample after this one, wherever the loop or the end puts it
                snd_voice next = v;
                next.frac = 0;
                next.pitch = 0x10000;
                snd_move(next, 1);
                int2 t = s;
                if (snd_sounds(next)) t = snd_read(next, next.pos);
                s += ((t - s) * (int)(v.frac >> 4)) >> 12;
            }
            sum += s * int2(min(v.volume & 0xffff, 256u), min(v.volume >> 16, 256u));
        }
    }
    int2 mixed = clamp(((sum >> 8) * (int)min(guest.g, 256u)) >> 8, -32768, 32767);
    return float2(mixed) / 32768.0;
}

// For GPUControl, in a frame the host mixed: the device's words move up to the cursor.
uint4 sound_control(uint index, uint4 keep) {
    if (SOUND_MIXED == 0 || index < SND_DEVICE || index >= SND_STATE + 2 * SND_MAX_VOICES) return keep;
    uint4 guest = ram(SND_GUEST);
    if (guest.r == 0) return keep;
    if (index == SND_DEVICE) return uint4(SOUND_CURSOR, SOUND_RATE, keep.b, keep.a);
    if (index < SND_STATE || (index - SND_STATE) >> 1 >= min(guest.b, SND_MAX_VOICES)) return keep;
    snd_voice v = snd_voice_at((index - SND_STATE) >> 1, SOUND_CURSOR);
    if (((index - SND_STATE) & 1) == 0) return uint4(v.key, (v.kind << 8) | (v.on ? 1 : 0), v.pos, v.frac);
    return uint4(v.address, v.length, v.restart, v.aux);
}

#endif
