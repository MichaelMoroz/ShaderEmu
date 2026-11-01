// The game's DirectSound on the machine's sound card (docs/machine/sound.md, docs/games/aoe.md, "Sound"): a wave is a voice pointed at
// its samples where they lie in the ROM, so a sound costs a few stores and no memory: nothing is copied or mixed here. make_data.py
// puts every wave of the DRS files on an even address, which 16-bit samples need. A buffer has a voice only while it plays.
#include <fcntl.h>
#include "dsound.h"
#include "aoe_port.h"
#undef open
#undef fopen
extern "C" {
#include "shaderemu_sound.h"
}
int __cdecl RESFILE_locate_resource(unsigned long rType, unsigned long rId, int& file, int& offset, unsigned char*& mapped_file, int& size);

enum { VOICES = 16, GAIN = 160 };   // of 256: room for a fight's sounds on top of each other before the mix clips
static int voices[VOICES];
static IDirectSoundBuffer* holder[VOICES];
static unsigned begun[VOICES], plays;
static int master = 256;
static IDirectSound device;
static int log_on;

// A file's first byte as the card addresses it, once for each file (the look reads the ROM's directory)
struct RomFile { const void* at; int fd; unsigned phys; };
static RomFile files[8];
static int file_count;
void aoe_sound_mapped(const void* at, long, int fd) {
    if (file_count < 8) { files[file_count].at = at; files[file_count].fd = dup(fd); file_count++; }   // (the game closes its handle at once)
}
static unsigned rom_of(const void* at, int fd) {
    RomFile* f = 0;
    for (int i = 0; i < file_count && !f; i++) if (at ? files[i].at == at : (!files[i].at && files[i].fd == fd)) f = &files[i];
    if (!f && !at && file_count < 8) { f = &files[file_count++]; f->at = 0; f->fd = fd; f->phys = 0; }
    if (!f) return 0;
    if (!f->phys) f->phys = snd_rom(f->fd, "Copyright", 9);
    return f->phys;
}

HRESULT aoe_DirectSoundCreate(GUID*, IDirectSound** out, IUnknown*) {
    const char* e = getenv("AOE_SOUND");
    log_on = getenv("AOE_SOUND_LOG") != 0;
    if ((e && e[0] == '0') || getenv("AOE_NOWINDOW") || snd_open()) {
        if (log_on) fprintf(stderr, "aoesnd: no card\n");
        return E_FAIL;
    }
    for (int i = 0; i < VOICES; i++) if ((voices[i] = snd_claim()) < 0) return E_FAIL;
    *out = &device;
    return DS_OK;
}
HRESULT IDirectSound::GetCaps(LPDSCAPS c) { c->dwFlags = 0; return DS_OK; }
HRESULT IDirectSound::DuplicateSoundBuffer(IDirectSoundBuffer*, IDirectSoundBuffer**) { return E_FAIL; }
static IDirectSoundBuffer* new_buffer(void) {
    IDirectSoundBuffer* b = new IDirectSoundBuffer;
    memset(&b->data, 0, (char*) &b->started + sizeof b->started - (char*) &b->data);
    b->voice = -1;
    return b;
}
// The primary buffer is the card itself. No other is made: a wave comes through aoe_sound_load.
HRESULT IDirectSound::CreateSoundBuffer(LPDSBUFFERDESC d, IDirectSoundBuffer** out, IUnknown*) {
    if (!(d->dwFlags & DSBCAPS_PRIMARYBUFFER)) return E_FAIL;
    *out = new_buffer();
    (*out)->primary = 1;
    return DS_OK;
}

// hundredths of a decibel to 0..256: halved every 6 dB
static int level(long hundredths) {
    static const unsigned short steps[6] = {256, 228, 203, 181, 162, 144};
    if (hundredths >= 0) return 256;
    long db = (-hundredths + 50) / 100;
    if (db >= 60) return 0;
    return steps[db % 6] >> (db / 6);
}
static unsigned volume_of(IDirectSoundBuffer* b) {
    int left = level(b->vol - (b->pan > 0 ? b->pan : 0)) * master >> 8, right = level(b->vol + (b->pan < 0 ? b->pan : 0)) * master >> 8;
    return SND_VOLUME(left * GAIN >> 8, right * GAIN >> 8);
}
static unsigned step_of(IDirectSoundBuffer* b) { return snd_step(b->freq ? b->freq : b->wf.nSamplesPerSec); }
static int voice_of(IDirectSoundBuffer* b) { return b->voice >= 0 && holder[b->voice] == b ? voices[b->voice] : -1; }

ULONG IDirectSoundBuffer::Release() {
    if (voice_of(this) >= 0) { snd_stop(voices[voice]); holder[voice] = 0; }
    delete this;
    return 0;
}
HRESULT IDirectSoundBuffer::Lock(DWORD, DWORD, LPVOID*, LPDWORD, LPVOID*, LPDWORD, DWORD) { return E_FAIL; }
HRESULT IDirectSoundBuffer::Unlock(LPVOID, DWORD, LPVOID, DWORD) { return E_FAIL; }
HRESULT IDirectSoundBuffer::Play(DWORD, DWORD, DWORD flags) {
    if (primary) return DS_OK;
    // its own voice again, or one that has ended, or the one begun longest ago
    int slot = voice_of(this) >= 0 ? voice : -1;
    for (int i = 0; i < VOICES && slot < 0; i++) if (!holder[i] || !snd_playing(voices[i])) slot = i;
    if (slot < 0) { slot = 0; for (int i = 1; i < VOICES; i++) if ((int) (begun[i] - begun[slot]) < 0) slot = i; }
    if (holder[slot] && holder[slot] != this) holder[slot]->voice = -1;
    holder[slot] = this; voice = slot; begun[slot] = ++plays;
    unsigned samples = wf.wBitsPerSample == 16 ? bytes / 2 : bytes;
    snd_play(voices[slot], (wf.wBitsPerSample == 16 ? SND_PCM16 : SND_PCM8) | SND_SMOOTH | (flags & DSBPLAY_LOOPING ? SND_LOOPED : 0), phys, samples, 0, step_of(this), volume_of(this));
    if (log_on) fprintf(stderr, "aoesnd: play %u at %08x, %u bytes, %d Hz, %d bits, voice %d, volume %08x, at %lu ms\n", plays, phys, (unsigned) bytes, (int) wf.nSamplesPerSec, wf.wBitsPerSample, slot, volume_of(this), timeGetTime());
    return DS_OK;
}
HRESULT IDirectSoundBuffer::Stop() {
    if (voice_of(this) >= 0) { snd_stop(voices[voice]); holder[voice] = 0; voice = -1; }
    return DS_OK;
}
HRESULT IDirectSoundBuffer::SetCurrentPosition(DWORD) { return DS_OK; }
HRESULT IDirectSoundBuffer::GetCurrentPosition(LPDWORD a, LPDWORD b) { if (a) *a = 0; if (b) *b = 0; return DS_OK; }
HRESULT IDirectSoundBuffer::GetStatus(LPDWORD s) {
    *s = !primary && voice_of(this) >= 0 && snd_playing(voices[voice]) ? DSBSTATUS_PLAYING : 0;
    return DS_OK;
}
HRESULT IDirectSoundBuffer::SetVolume(LONG v) { vol = v; if (voice_of(this) >= 0) SND_VOICE[voices[voice]].volume = volume_of(this); return DS_OK; }
HRESULT IDirectSoundBuffer::GetVolume(LONG* v) { *v = vol; return DS_OK; }
HRESULT IDirectSoundBuffer::SetPan(LONG p) { pan = p; if (voice_of(this) >= 0) SND_VOICE[voices[voice]].volume = volume_of(this); return DS_OK; }
HRESULT IDirectSoundBuffer::GetPan(LONG* p) { *p = pan; return DS_OK; }
HRESULT IDirectSoundBuffer::SetFrequency(DWORD f) { freq = f; if (voice_of(this) >= 0) SND_VOICE[voices[voice]].step = step_of(this); return DS_OK; }
HRESULT IDirectSoundBuffer::GetFrequency(LPDWORD f) { *f = freq; return DS_OK; }
HRESULT IDirectSoundBuffer::SetFormat(LPCWAVEFORMATEX) { return DS_OK; }

// AOE_SOUND_LOG=1, a frame at a time: when each voice was found ended
void aoe_sound_look(void) {
    static unsigned from[VOICES]; static IDirectSoundBuffer* told[VOICES];
    if (!log_on) return;
    for (int i = 0; i < VOICES; i++) {
        IDirectSoundBuffer* b = holder[i];
        if (!b) continue;
        if (told[i] != b || from[i] != begun[i]) { told[i] = b; from[i] = begun[i]; b->started = timeGetTime(); }
        else if (!snd_playing(voices[i])) {
            unsigned samples = b->wf.wBitsPerSample == 16 ? b->bytes / 2 : b->bytes;
            fprintf(stderr, "aoesnd: ended %u at %08x after %lu ms of %u\n", begun[i], b->phys, timeGetTime() - b->started, samples / (b->wf.nSamplesPerSec / 1000));
            holder[i] = 0; b->voice = -1;
        }
    }
}

// The sound driver's volume (the options' slider, where Windows had a mixer): -10000 to 0, in a line, for sounds to come and those playing.
void aoe_sound_master(long v) {
    master = (v + 10000) * 256 / 10000;
    for (int i = 0; i < VOICES; i++) if (holder[i]) SND_VOICE[voices[i]].volume = volume_of(holder[i]);
}

static IDirectSoundBuffer* wave_buffer(const unsigned char* h, int have, unsigned at) {
    // RIFF, then chunks: a format and the data
    if (have < 28 || memcmp(h, "RIFF", 4)) return 0;
    const unsigned char* format = 0;
    for (int o = 12; o + 8 <= have; ) {
        unsigned n = h[o + 4] | h[o + 5] << 8 | h[o + 6] << 16 | (unsigned) h[o + 7] << 24;
        if (!memcmp(h + o, "fmt ", 4)) format = h + o + 8;
        else if (!memcmp(h + o, "data", 4)) {
            if (!format || format[0] != 1 || format[2] != 1) return 0;   // PCM and mono: all the trial has
            unsigned phys = at + o + 8; int bits = format[14];
            if (bits == 16 && (phys & 1)) { fprintf(stderr, "aoe: a wave at an odd address (%08x): data of an older image\n", phys); return 0; }
            IDirectSoundBuffer* b = new_buffer();
            b->phys = phys; b->bytes = n;
            b->wf.nSamplesPerSec = format[4] | format[5] << 8 | format[6] << 16; b->wf.wBitsPerSample = bits;
            return b;
        }
        o += 8 + n + (n & 1);
    }
    return 0;
}
// A wave of a resource file, or a file of its own: TDigital::load (Sounddrv.cpp in aoe.patch). Nothing of the samples is read.
IDirectSoundBuffer* aoe_sound_load(long id, const char* path) {
    unsigned char head[64];
    IDirectSoundBuffer* b = 0;
    int fd, offset, size; unsigned char* mapped;
    if (id >= 0 && RESFILE_locate_resource(0x77617620 /* "wav " */, id, fd, offset, mapped, size)) {
        unsigned rom = 0;
        if (mapped) { memcpy(head, mapped + offset, sizeof head); rom = rom_of(mapped, -1); }
        else if (lseek(fd, offset, SEEK_SET) == offset && read(fd, head, sizeof head) == (int) sizeof head) rom = rom_of(0, fd);
        if (rom) b = wave_buffer(head, sizeof head, rom + offset);
    } else if (path && path[0]) {
        fd = _open(path, O_RDONLY);
        if (fd >= 0) {
            int n = read(fd, head, sizeof head);
            unsigned rom = n > 0 ? snd_rom(fd, "RIFF", 4) : 0;
            if (rom) b = wave_buffer(head, n, rom);
            close(fd);
        }
    }
    if (log_on) fprintf(stderr, "aoesnd: load %ld %s: %s\n", id, path ? path : "", b ? "in the ROM" : "not played");
    return b;
}
