// Tiberian Dawn's sound on the machine's sound card (docs/sound.md), in place of the game's
// mixer. Nothing is decoded here: a sound the game plays is looked up, by how its AUD file
// begins, in the index tools/make_tdawn_sound.py made, and a voice is pointed at its samples
// where they lie in the ROM.
#include "audio.h"
#include "file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "shaderemu_sound.h"
}

void (*Audio_Focus_Loss_Function)(void) = nullptr;
bool StreamLowImpact = false;
SFX_Type SoundType;
Sample_Type SampleType;

namespace
{
const int kVoices = 8, kKeyBytes = 44;
const char kPack[] = "/usr/share/tdawn-sound.pak";

struct Sound {
    uint32_t key, kind, rate, samples, data, marks;
};
Sound* sounds;
uint32_t sound_count, pack_at;   // where the pack's first byte is, as the device addresses it
int voice[kVoices + 1];           // the last one is the tune's
int score_volume = 255;          // what the game set for music, 0 to 255
int tune_volume;                 // and what it asked of the tune that is playing
const void* held[kVoices];       // the sample each voice was last started with
int rank[kVoices];               // and its priority

// The sound whose AUD file begins as these bytes do (rate, sizes, coding, then data).
const Sound* Find(const void* sample)
{
    const unsigned char* p = (const unsigned char*)sample;
    uint32_t size = p[2] | p[3] << 8 | p[4] << 16 | (uint32_t)p[5] << 24, key = 0x811c9dc5;
    int count = size + 12 < (uint32_t)kKeyBytes ? (int)size + 12 : kKeyBytes;
    for (int n = 0; n < count; n++) {
        key = (key ^ p[n]) * 0x01000193;
    }
    uint32_t low = 0, high = sound_count;
    while (low < high) {
        uint32_t mid = (low + high) / 2;
        if (sounds[mid].key < key) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low < sound_count && sounds[low].key == key ? &sounds[low] : nullptr;
}

bool Playing(int slot)
{
    return slot >= 0 && slot < kVoices && held[slot] != nullptr && snd_playing(voice[slot]);
}

uint32_t Tune_Volume()
{
    uint32_t loud = (uint32_t)(tune_volume * score_volume) / 255;
    return SND_VOLUME(loud, loud);
}
} // namespace

bool Audio_Init(int bits_per_sample, bool stereo, int rate, bool reverse_channels)
{
    uint32_t head[2];
    int file = open(kPack, O_RDONLY);
    if (file < 0 || read(file, head, 8) != 8 || memcmp(head, "TDSN", 4) != 0 || snd_open() != 0) {
        return false;
    }
    sound_count = head[1];
    sounds = (Sound*)malloc(sound_count * sizeof(Sound));
    pack_at = snd_rom(file, "TDSN", 4);
    bool ok = pack_at != 0 && read(file, sounds, sound_count * sizeof(Sound)) == (ssize_t)(sound_count * sizeof(Sound));
    close(file);
    for (int n = 0; n <= kVoices && ok; n++) {
        ok = (voice[n] = snd_claim()) >= 0;
    }
    if (ok) {
        SoundType = SFX_ALFX;
        SampleType = SAMPLE_SB;
    }
    return ok;
}

int Play_Sample(void const* sample, int priority, int volume, signed short panloc)
{
    const Sound* sound = SampleType != SAMPLE_NONE && sample != nullptr ? Find(sample) : nullptr;
    // TDAWN_SOUND_LOG=1 says what the game asked for
    static const bool log = getenv("TDAWN_SOUND_LOG") != nullptr;
    if (log) {
        fprintf(stderr, "tdsound: priority %d volume %d: %s\n", priority, volume, sound ? "plays" : "not in the pack");
    }
    if (sound == nullptr) {
        return -1;
    }
    // a voice that has ended, or else the least important one if this matters more
    int slot = -1, least = 0;
    for (int n = 0; n < kVoices && slot < 0; n++) {
        if (!Playing(n)) {
            slot = n;
        } else if (rank[n] < rank[least]) {
            least = n;
        }
    }
    if (slot < 0) {
        if (rank[least] >= priority) {
            return -1;
        }
        slot = least;
    }
    // volume is 0 to 255; a sound to one side loses the other side
    uint32_t loud = volume < 0 ? 0 : volume > 255 ? 256 : volume + (volume >> 7);
    uint32_t left = (loud * (0x8000 - (panloc > 0 ? panloc : 0))) >> 15, right = (loud * (0x8000 + (panloc < 0 ? panloc : 0))) >> 15;
    held[slot] = sample;
    rank[slot] = priority;
    snd_play_aux(voice[slot], (sound->kind == 4 ? SND_ADPCM : SND_PCM8) | SND_SMOOTH, pack_at + sound->data, sound->samples, 0,
                 snd_step(sound->rate), SND_VOLUME(left, right), sound->marks ? pack_at + sound->marks : 0);
    return slot;
}

void Stop_Sample(int handle)
{
    if (handle == kVoices && snd_playing(voice[kVoices])) {
        snd_stop(voice[kVoices]);
    }
    if (handle >= 0 && handle < kVoices && held[handle] != nullptr) {
        snd_stop(voice[handle]);
        held[handle] = nullptr;
    }
}

bool Sample_Status(int handle)
{
    if (handle == kVoices) {
        return snd_playing(voice[kVoices]);
    }
    return Playing(handle);
}

bool Is_Sample_Playing(void const* sample)
{
    for (int n = 0; n < kVoices; n++) {
        if (held[n] == sample && Playing(n)) {
            return true;
        }
    }
    return false;
}

void Stop_Sample_Playing(void const* sample)
{
    for (int n = 0; n < kVoices; n++) {
        if (held[n] == sample) {
            Stop_Sample(n);
        }
    }
}

void Fade_Sample(int handle, int ticks)
{
    Stop_Sample(handle);
}

// A sound in a file of its own: only its beginning is needed, to look it up by.
void* Load_Sample(char const* filename)
{
    if (SampleType == SAMPLE_NONE || filename == nullptr || !Find_File(filename)) {
        return nullptr;
    }
    void* begins = calloc(1, kKeyBytes);
    int handle = Open_File(filename, 1);
    if (handle != -1) {
        Read_File(handle, begins, kKeyBytes);
        Close_File(handle);
    }
    return begins;
}

void Free_Sample(void const* sample)
{
    Stop_Sample_Playing(sample);
    free((void*)sample);
}

// Music: the game streams a file; here the file's beginning names a sound in the pack, and one
// voice of its own plays it. The handle is that voice's.
int File_Stream_Sample_Vol(char const* filename, int volume, bool real_time_start)
{
    unsigned char begins[kKeyBytes] = {};
    if (SampleType == SAMPLE_NONE || filename == nullptr || !Find_File(filename)) {
        return -1;
    }
    int handle = Open_File(filename, 1);
    if (handle == -1) {
        return -1;
    }
    Read_File(handle, begins, kKeyBytes);
    Close_File(handle);
    const Sound* sound = Find(begins);
    if (sound == nullptr) {
        return -1;
    }
    tune_volume = volume < 0 ? 0 : volume > 255 ? 256 : volume + (volume >> 7);
    snd_play_aux(voice[kVoices], (sound->kind == 4 ? SND_ADPCM : SND_PCM8) | SND_SMOOTH, pack_at + sound->data, sound->samples, 0,
                 snd_step(sound->rate), Tune_Volume(), sound->marks ? pack_at + sound->marks : 0);
    return kVoices;
}

void Sound_Callback(void)
{
}

void Sound_End(void)
{
}

int Set_Score_Vol(int volume)
{
    int before = score_volume;
    score_volume = volume < 0 ? 0 : volume > 255 ? 255 : volume;
    if (SampleType != SAMPLE_NONE) {
        SND_VOICE[voice[kVoices]].volume = Tune_Volume();
    }
    return before;
}

int Get_Digi_Handle(void)
{
    return 1;
}

void Restore_Sound_Buffers(void)
{
}

bool Set_Primary_Buffer_Format(void)
{
    return true;
}

bool Start_Primary_Sound_Buffer(bool forced)
{
    return true;
}

void Stop_Primary_Sound_Buffer(void)
{
}
