// The game's DirectSound on the machine's sound card (docs/machine/sound.md, docs/games/aoe.md, "Sound"): a wave is a voice pointed at
// its samples where they lie, in the trial's files unpacked into memory (or in the ROM), so a sound costs a few stores: nothing is
// mixed here. 16-bit samples must lie on an even address and the trial's DRS files have most waves on odd ones: such a wave is
// copied once, when it is loaded, to memory of its own. A buffer has a voice only while it plays.
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "dsound.h"
#include "aoe_port.h"
#undef open
#undef fopen
extern "C" {
#include "shaderemu_sound.h"
}
int __cdecl RESFILE_locate_resource(unsigned long rType, unsigned long rId, int& file, int& offset, unsigned char*& mapped_file, int& size);

enum { VOICES = 16, GAIN = 160 };   // of 256: room for a fight's sounds on top of each other before the mix clips. Music has the other 16 (aoe_music.cpp)
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
    // not in the ROM: the file in memory (tmpfs), mapped where it lies, whose address the card reads as it is (no MMU)
    if (!f->phys && !f->at) {
        struct stat st;
        void* p = fstat(f->fd, &st) == 0 && st.st_size > 0 ? mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, f->fd, 0) : MAP_FAILED;
        if (p != MAP_FAILED) f->at = p;
        else if (log_on) fprintf(stderr, "aoesnd: file %d not mapped (%s)\n", f->fd, strerror(errno));
    }
    if (!f->phys && f->at) f->phys = (unsigned) (uintptr_t) f->at;
    return f->phys;
}
// A file of its own (a wave), in the ROM or mapped where it lies in memory; kept mapped for the program's life
static unsigned file_phys(const char* path, int fd) {
    static struct { char path[64]; unsigned phys; } seen[16];
    static int count;
    for (int i = 0; i < count; i++) if (!strcmp(seen[i].path, path)) return seen[i].phys;
    unsigned phys = snd_rom(fd, "RIFF", 4);
    if (!phys) {
        struct stat st;
        void* p = fstat(fd, &st) == 0 && st.st_size > 0 ? mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
        if (p != MAP_FAILED) phys = (unsigned) (uintptr_t) p;
    }
    if (phys && count < 16 && strlen(path) < sizeof seen[0].path) { strcpy(seen[count].path, path); seen[count++].phys = phys; }
    return phys;
}
static unsigned copied_waves, copied_bytes;

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
    if (log_on) fprintf(stderr, "aoesnd: play %u at %08x, %u bytes, %d Hz, %d bits, voice %d, volume %08x, at %lu ms, clock %u\n", plays, phys, (unsigned) bytes, (int) wf.nSamplesPerSec, wf.wBitsPerSample, slot, volume_of(this), timeGetTime(), SND_CLOCK);
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
            if (bits == 16 && (phys & 1) && phys >= 0x80000000u) {
                // in memory (the trial's own DRS files): its samples copied once to an even address
                void* even = 0;
                if (posix_memalign(&even, 16, n ? n : 16)) return 0;
                memcpy(even, (const void*) (uintptr_t) phys, n);
                phys = (unsigned) (uintptr_t) even;
                copied_waves++, copied_bytes += n;
                if (log_on) fprintf(stderr, "aoesnd: copied to an even address, %u waves, %u bytes so far\n", copied_waves, copied_bytes);
            }
            if (bits == 16 && (phys & 1)) { fprintf(stderr, "aoe: a wave at an odd address (%08x) in the ROM\n", phys); return 0; }
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
        else if (!mapped && size > 0) {
            // a resource file read, not mapped, and not in the ROM (one of the trial's in memory that could not be mapped): the wave
            // read once into memory of its own
            void* copy = 0;
            if (!posix_memalign(&copy, 16, size) && pread(fd, copy, size, offset) == size) {
                b = wave_buffer((const unsigned char*) copy, size < 64 ? size : 64, (unsigned) (uintptr_t) copy);
                copied_waves++, copied_bytes += size;
                if (log_on) fprintf(stderr, "aoesnd: resource %ld read into memory, %u waves, %u bytes so far\n", id, copied_waves, copied_bytes);
            }
            if (!b) free(copy);
        }
    } else if (path && path[0]) {
        fd = _open(path, O_RDONLY);
        if (fd >= 0) {
            int n = read(fd, head, sizeof head);
            unsigned rom = n > 0 ? file_phys(path, fd) : 0;
            if (rom) b = wave_buffer(head, n, rom);
            close(fd);
        }
    }
    if (log_on) fprintf(stderr, "aoesnd: load %ld %s: %s\n", id, path ? path : "", !b ? "not played" : b->phys < 0x80000000u ? "in the ROM" : "in memory");
    return b;
}
