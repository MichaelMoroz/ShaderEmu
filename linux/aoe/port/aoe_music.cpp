// The game's music: Windows' MCI sequencer and the synthesizer's mixer line, which is what the game's own TMusic_System asks for
// (music.cpp, unchanged), on the sound card's FM voices (docs/games/aoe.md, "Music"). The game says what plays and when, what comes
// next and how loud; this reads a MIDI file, makes it into notes (linux/userland/shaderemu_fm.h, shared with OpenTTD) on a worker while
// the game goes on, starts the voices, and tells the game the song's end as Windows did: MM_MCINOTIFY to its window.
#include <fcntl.h>
#include "mmsystem.h"
#include "aoe_port.h"
#undef open
#undef fopen
extern "C" {
#include "shaderemu_sound.h"
#include "shaderemu_fm.h"
typedef void (*mcw_fn)(uint32_t, uint32_t);
void mcw_post(int k, mcw_fn fn, uint32_t a0, uint32_t a1);
int mcw_done(int k);
int mcw_wait(int k);
void mcw_touch(void* memory, unsigned bytes);
}
int aoe_view_second_worker(void);   // aoe_view.cpp: the worker a song is made on, or 0: here
extern int aoe_clock_still;         // win32.cpp: looks at the clock do not move AOE_TICK_MS's clock

enum {
    VOICES = 16,                    // what the effects leave of the card's 32 (sound.cpp's VOICES)
    LOUD = 48,                      // of 256, a voice at the mixer's full level: as OpenTTD's, under the effects' 160
    MAX_NOTES = 16384,              // the trial's largest song has 6,138 (tools/aoe_music_check.py --files says each)
    MAX_FILE = 65536,
    MIXER_FULL = 65535,
    MAX_TRACKS = 32,
};
static const unsigned TRACKS_BYTES = VOICES * sizeof(snd_track) + MAX_NOTES * sizeof(snd_note);
// All in sound memory, which needs a store once and lies apart from what the game writes: the card's tracks and instruments, and the job's
// file, bank and notes. (In the program's zeros it would be 1.4 MB that the view's touch of its data pays at every start.)
static const unsigned AT_TONES = TRACKS_BYTES, AT_BANK = AT_TONES + 2 * FM_TONES * sizeof(snd_tone), AT_FILE = AT_BANK + ((FM_BANK_BYTES + 15) & ~15u),
                      AT_NOTES = AT_FILE + MAX_FILE, MEMORY = AT_NOTES + MAX_NOTES * sizeof(fm_note);

// What a worker makes. The core that makes it alone touches this until it is joined.
struct SongJob {
    const uint8_t* file;
    unsigned length, from_ms, out;
    char* memory;
    unsigned memory_at;
    int tones_made;
    unsigned ok, notes, samples, stolen, instructions;
    unsigned track_of[VOICES];
    unsigned pitches[128];
    fm_tunes tunes;
} __attribute__((aligned(16)));
static SongJob job __attribute__((aligned(16)));

enum { SONG_NONE, SONG_WANTED, SONG_OUT, SONG_PLAYING };
static struct {
    int state, ready, log, cut_s;
    int voice[VOICES];
    char* memory;
    unsigned memory_at, out;
    char name[300];
    unsigned from_ms, samples, started, paused_at;
    int paused;
    unsigned pending, owed;     // MM_MCINOTIFYs the song's end will send, and those a stop owes at the next look
    unsigned level;             // the mixer's: 0 to MIXER_FULL
    int worker;
    unsigned songs, looks, look_instructions, ask_instructions;
    unsigned told_at;
} m;

static inline unsigned my_instructions(void) { unsigned n; __asm__ volatile("rdinstret %0" : "=r"(n)); return n; }
static unsigned word32(const uint8_t* p) { return (unsigned) p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// ---------------------------------------------------------------- a Standard MIDI File made into the voices' tracks, on any core
struct Track { const uint8_t* at; const uint8_t* end; unsigned tick; int status; };
static int read_length(const uint8_t** p, const uint8_t* end, unsigned* out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        if (*p >= end) return 0;
        uint8_t c = *(*p)++;
        v = v << 7 | (c & 127);
        if (!(c & 128)) { *out = v; return 1; }
    }
    return 0;
}
static int next_delta(Track* t) {
    unsigned d;
    if (t->at >= t->end || !read_length(&t->at, t->end, &d)) { t->at = t->end; return 0; }
    t->tick += d;
    return 1;
}
static void make_song(SongJob* j) {
    const uint8_t* f = j->file;
    if (j->length < 14 || memcmp(f, "MThd", 4)) return;
    unsigned head = word32(f + 4), division = f[12] << 8 | f[13], track_count = f[10] << 8 | f[11];
    if (head < 6 || (division & 0x8000) || !division) return;   // (time in frames: the trial has none)
    Track tracks[MAX_TRACKS];
    int n = 0;
    const uint8_t* p = f + 8 + head, * end = f + j->length;
    while (p + 8 <= end && n < MAX_TRACKS && n < (int) track_count) {
        unsigned size = word32(p + 4);
        if (p + 8 + size > end) size = end - p - 8;
        if (!memcmp(p, "MTrk", 4)) {
            Track* t = &tracks[n];
            t->at = p + 8; t->end = p + 8 + size; t->tick = 0; t->status = 0;
            if (next_delta(t)) n++;
        }
        p += 8 + size;
    }

    fm_song s;
    s.bank = (const uint8_t*) j->memory + AT_BANK;
    s.pitches = j->pitches;
    s.tunes = &j->tunes;
    s.voices = VOICES;
    s.tail = j->out / 4;
    s.max_notes = MAX_NOTES;
    s.notes = (fm_note*) (j->memory + AT_NOTES);
    s.cap = MAX_NOTES;
    s.grow = 0;
    s.owner = 0;
    s.now = 0;
    fm_song_begin(&s);

    // the time of a tick goes by the last change of tempo: base_us at base_tick, tempo microseconds a beat
    unsigned long long base_us = 0, from = (unsigned long long) j->from_ms * j->out / 1000;
    unsigned base_tick = 0, tempo = 500000, at_tick = 0;
    unsigned long long at_samples = 0;
    for (;;) {
        Track* t = 0;
        for (int i = 0; i < n; i++) if (tracks[i].at < tracks[i].end && (!t || tracks[i].tick < t->tick)) t = &tracks[i];
        if (!t) break;
        if (t->tick != at_tick || t->tick == 0) {
            at_tick = t->tick;
            at_samples = (base_us + (unsigned long long) (at_tick - base_tick) * tempo / division) * j->out / 1000000;
        }
        int heard = at_samples >= from;
        s.now = heard ? (unsigned) (at_samples - from) : 0;
        int status = *t->at;
        if (status & 0x80) t->at++; else status = t->status;
        if (status == 0xFF) {
            unsigned size;
            if (t->at >= t->end) break;
            int type = *t->at++;
            if (!read_length(&t->at, t->end, &size) || t->at + size > t->end) { t->at = t->end; continue; }
            if (type == 0x51 && size == 3) {
                base_us += (unsigned long long) (at_tick - base_tick) * tempo / division;
                base_tick = at_tick;
                tempo = t->at[0] << 16 | t->at[1] << 8 | t->at[2];
            }
            t->at += size;
            t->status = 0;
        } else if (status == 0xF0 || status == 0xF7) {
            unsigned size;
            if (!read_length(&t->at, t->end, &size) || t->at + size > t->end) { t->at = t->end; continue; }
            t->at += size;
            t->status = 0;
        } else if (status >= 0x80 && status < 0xF0) {
            int size = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 1 : 2;
            if (t->at + size > t->end) { t->at = t->end; continue; }
            fm_message(&s, status, t->at[0], size > 1 ? t->at[1] : 0, heard);
            t->at += size;
            t->status = status;
        } else {
            t->at = t->end;   // (not a Standard MIDI File past here)
            continue;
        }
        next_delta(t);
    }
    if (at_samples >= from) s.now = (unsigned) (at_samples - from);
    fm_song_end(&s);
    fm_write_tracks(&s, j->memory, j->memory_at, j->track_of);
    j->notes = s.count;
    j->stolen = s.stolen;
    j->samples = s.now > 1 ? s.now : 1;
    j->ok = 1;
}
static void song_job(uint32_t a0, uint32_t) {
    SongJob* j = (SongJob*) a0;
    unsigned began = my_instructions();
    if (!j->tones_made) {
        fm_make_pitches(j->pitches, j->out);
        fm_make_tones((snd_tone*) (j->memory + AT_TONES), (const uint8_t*) j->memory + AT_BANK, j->out);
        j->tones_made = 1;
    }
    make_song(j);
    j->instructions = my_instructions() - began;
}

// ---------------------------------------------------------------- the voices
static unsigned loud_now(void) { unsigned v = LOUD * m.level / MIXER_FULL; return SND_VOLUME(v, v); }

static int music_open(void) {
    if (m.ready) return m.ready > 0;
    m.ready = -1;
    m.log = 0;
    const char* e = getenv("AOE_MUSIC_LOG");
    if (e) m.log = atoi(e);
    e = getenv("AOE_MUSIC_CUT");
    if (e) m.cut_s = atoi(e);
    const char* off = getenv("AOE_MUSIC");
    const char* sound = getenv("AOE_SOUND");
    if ((off && off[0] == '0') || (sound && sound[0] == '0') || getenv("AOE_NOWINDOW") || snd_open()) {
        if (m.log) fprintf(stderr, "aoemus: no music\n");
        return 0;
    }
    int fd = aoe_open("genmidi.op2", O_RDONLY);
    if (fd < 0) { fprintf(stderr, "aoe: no genmidi.op2, so no music (an image made before the music)\n"); return 0; }
    m.memory = (char*) snd_memory(MEMORY, &m.memory_at);
    if (!m.memory) { close(fd); fprintf(stderr, "aoe: no sound memory for music\n"); return 0; }
    int got = read(fd, m.memory + AT_BANK, FM_BANK_BYTES);
    close(fd);
    if (got != FM_BANK_BYTES || memcmp(m.memory + AT_BANK, "#OPL_II#", 8)) { fprintf(stderr, "aoe: genmidi.op2 is not a bank\n"); return 0; }
    for (int i = 0; i < VOICES; i++) {
        if ((m.voice[i] = snd_claim()) < 0) { fprintf(stderr, "aoe: %d voices are all there is for music: no music\n", i); return 0; }
    }
    m.worker = aoe_view_second_worker();
    if (m.worker) {
        mcw_touch(m.memory, MEMORY);    // a store by this core first, to every page the worker writes
        mcw_touch(&job, sizeof job);
    }
    m.ready = 1;
    if (m.log) fprintf(stderr, "aoemus: open: voices %d to %d, memory %08x, %u bytes, %s\n", m.voice[0], m.voice[VOICES - 1], m.memory_at, MEMORY, m.worker ? "songs on a worker" : "songs on the first core");
    return 1;
}

static void song_start(void) {
    if (job.ok != 1) { m.state = SONG_NONE; fprintf(stderr, "aoe: %s is not a song\n", m.name); return; }
    m.samples = job.samples + m.out / 2;    // (a last note's release, before the game is told)
    for (int i = 0; i < VOICES; i++) snd_play_aux(m.voice[i], SND_FM, job.track_of[i], m.samples, 0, 0x10000, loud_now(), m.memory_at + AT_TONES);
    m.started = SND_CLOCK;
    m.state = SONG_PLAYING;
    m.paused = 0;
    m.told_at = 0;
    m.songs++;
    if (m.log) {
        fprintf(stderr, "aoemus: play %s, %u notes, %u samples, %u cut, %u instructions on the %s, %u asking, clock %u, level %u\n", m.name, job.notes, job.samples, job.stolen,
                job.instructions, m.worker ? "worker" : "first core", m.ask_instructions, m.started, m.level);
        if (m.log > 1) {
            // the card's own words, as it will read them (tools/aoe_music_check.py)
            for (int i = 0; i < VOICES; i++) {
                const unsigned* t = (const unsigned*) (m.memory + (job.track_of[i] - m.memory_at));
                fprintf(stderr, "aoemus: voice %d key %x kind %x track %x length %u volume %x tones %x count %u\n", m.voice[i], SND_VOICE[m.voice[i]].key, SND_VOICE[m.voice[i]].kind, SND_VOICE[m.voice[i]].address, SND_VOICE[m.voice[i]].length, SND_VOICE[m.voice[i]].volume, SND_VOICE[m.voice[i]].aux, t[0]);
                for (unsigned k = 0; k < t[0]; k++) { const unsigned* w = t + 4 + 8 * k; fprintf(stderr, "aoemus: n %d %x %x %x %x %x %x\n", i, w[0], w[1], w[2], w[3], w[4], w[5]); }
            }
            static int tones_told;
            if (!tones_told) {
                tones_told = 1;
                const unsigned* t = (const unsigned*) (m.memory + AT_TONES);
                for (int k = 0; k < 2 * FM_TONES; k++, t += 16) {
                    fprintf(stderr, "aoemus: t %d", k);
                    for (int w = 0; w < 16; w++) fprintf(stderr, " %x", t[w]);
                    fprintf(stderr, "\n");
                }
            }
        }
    }
}

// The file read here and the notes made on a worker, once the card has said its rate.
static void song_ask(void) {
    if (!SND_RATE) return;
    unsigned began = my_instructions();
    if (!m.out) m.out = SND_RATE;
    int fd = aoe_open(m.name, O_RDONLY);
    int got = fd < 0 ? -1 : read(fd, m.memory + AT_FILE, MAX_FILE);
    if (fd >= 0) close(fd);
    if (got <= 0 || got == MAX_FILE) { m.state = SONG_NONE; fprintf(stderr, "aoe: %s could not be read whole\n", m.name); return; }
    job.file = (const uint8_t*) m.memory + AT_FILE;
    job.length = got;
    job.from_ms = m.from_ms;
    job.out = m.out;
    job.memory = m.memory;
    job.memory_at = m.memory_at;
    job.ok = 0;
    m.ask_instructions = my_instructions() - began;
    if (m.worker) {
        mcw_post(m.worker, song_job, (uint32_t) &job, 0);
        m.state = SONG_OUT;
    } else {
        song_job((uint32_t) &job, 0);
        song_start();
    }
}

static void stop_voices(void) {
    if (m.state == SONG_OUT) { mcw_wait(m.worker); }    // (it writes the tracks)
    if (m.state == SONG_PLAYING) for (int i = 0; i < VOICES; i++) snd_stop(m.voice[i]);
    m.state = SONG_NONE;
    m.paused = 0;
    m.owed += m.pending;    // as Windows: a notification the stop aborts still comes
    m.pending = 0;
}

static unsigned position_ms(void) {
    if (m.state != SONG_PLAYING || !m.out) return m.from_ms;
    return m.from_ms + ((m.paused ? m.paused_at : SND_CLOCK) - m.started) / (m.out / 1000);
}

static void tell_game(unsigned why) {
    // A sent message, not one of the queue: a message in the queue is a look that does not move AOE_TICK_MS's clock and the game's idle
    // work not done, so it would change a run's state sums, and a song's end comes when the card says.
    HWND w = aoe_main_window();
    if (!w) return;
    aoe_clock_still++;
    SendMessage(w, MM_MCINOTIFY, why, 1);
    aoe_clock_still--;
}

// Every look for messages, on the first core. What the card has told: a song made, a song ended.
void aoe_music_look(void) {
    if (!m.state && !m.owed) return;
    unsigned began = m.log ? my_instructions() : 0;
    while (m.owed) { m.owed--; tell_game(MCI_NOTIFY_ABORTED); }
    if (m.state == SONG_WANTED) song_ask();
    if (m.state == SONG_OUT && mcw_done(m.worker)) song_start();
    if (m.state == SONG_PLAYING && !m.paused) {
        unsigned played = SND_CLOCK - m.started;
        if (played >= m.samples || (m.cut_s && played >= (unsigned) m.cut_s * m.out)) {
            for (int i = 0; i < VOICES; i++) snd_stop(m.voice[i]);
            m.state = SONG_NONE;
            if (m.log) fprintf(stderr, "aoemus: ended %s at clock %u after %u of %u samples, %u looks at %u instructions\n", m.name, SND_CLOCK, played, m.samples, m.looks, m.looks ? m.look_instructions / m.looks : 0);
            unsigned n = m.pending;
            m.pending = 0;
            while (n--) tell_game(MCI_NOTIFY_SUCCESSFUL);
        } else if (m.log && played - m.told_at >= 5 * m.out) {
            // what the card's own words say: how many voices it has as playing, and where the first of them is
            int on = 0;
            for (int i = 0; i < VOICES; i++) on += snd_playing(m.voice[i]) != 0;
            m.told_at = played;
            fprintf(stderr, "aoemus: playing %s, clock %u, %d voices on, voice %d at %u of %u\n", m.name, SND_CLOCK, on, m.voice[0], SND_STATE[m.voice[0]].position, m.samples);
        }
    }
    if (m.log) { m.looks++; m.look_instructions += my_instructions() - began; }
}

// ---------------------------------------------------------------- Windows
extern "C" {
DWORD mciSendCommandA(UINT device, UINT what, DWORD flags, DWORD parms) {
    switch (what) {
        case MCI_OPEN: {
            MCI_OPEN_PARMSA* o = (MCI_OPEN_PARMSA*) parms;
            // (the CD has no drive: the game then goes to the sequencer, as with no CD in Windows)
            if ((flags & MCI_OPEN_TYPE_ID) && (DWORD) o->lpstrDeviceType != MCI_DEVTYPE_SEQUENCER) return 1;
            if (!(flags & MCI_OPEN_ELEMENT) || !music_open()) return 1;
            strncpy(m.name, o->lpstrElementName, sizeof m.name - 1);
            m.from_ms = 0;
            o->wDeviceID = 1;
            return 0;
        }
        case MCI_SEEK:
            m.from_ms = ((MCI_SEEK_PARMS*) parms)->dwTo;
            return 0;
        case MCI_PLAY:
            if (m.ready <= 0) return 1;
            m.pending++;   // (the game counts every play, its resume's too, which asks for no notice)
            if (m.state == SONG_PLAYING && m.paused) {
                unsigned now = SND_CLOCK;
                m.started += now - m.paused_at;
                m.paused = 0;
                for (int i = 0; i < VOICES; i++) SND_VOICE[m.voice[i]].step = 0x10000;
                return 0;
            }
            if (m.state != SONG_NONE) return 0;
            m.state = SONG_WANTED;
            song_ask();
            return 0;
        case MCI_PAUSE:
            if (m.state == SONG_PLAYING && !m.paused) {
                m.paused = 1;
                m.paused_at = SND_CLOCK;
                for (int i = 0; i < VOICES; i++) SND_VOICE[m.voice[i]].step = 0;
            }
            return 0;
        case MCI_STOP:
        case MCI_CLOSE:
            if (m.ready > 0) stop_voices();
            return 0;
        case MCI_STATUS: {
            MCI_STATUS_PARMS* s = (MCI_STATUS_PARMS*) parms;
            s->dwReturn = s->dwItem == MCI_STATUS_POSITION ? position_ms() : 0;
            return 0;
        }
    }
    return 1;
}
BOOL mciGetErrorStringA(DWORD, LPSTR s, UINT n) { if (s && n) s[0] = 0; return 1; }

// One mixer with one line, the synthesizer's, and its volume, 0 to 65535: the game's slider and its default of half are by this
UINT mixerGetNumDevs(void) { return 1; }
UINT mixerOpen(HMIXER* h, UINT, DWORD, DWORD, DWORD) { *h = (HMIXER) 1; if (!m.level) m.level = MIXER_FULL; return 0; }
UINT mixerClose(HMIXER) { return 0; }
UINT mixerGetLineInfoA(HMIXER, MIXERLINEA* l, DWORD) { l->dwLineID = 1; return 0; }
UINT mixerGetLineControlsA(HMIXER, MIXERLINECONTROLSA* c, DWORD) {
    c->pamxctrl->dwControlID = 1;
    c->pamxctrl->Bounds.dwMinimum = 0;
    c->pamxctrl->Bounds.dwMaximum = MIXER_FULL;
    return 0;
}
UINT mixerGetControlDetailsA(HMIXER, MIXERCONTROLDETAILS* d, DWORD) { ((MIXERCONTROLDETAILS_UNSIGNED*) d->paDetails)->dwValue = m.level; return 0; }
UINT mixerSetControlDetails(HMIXER, MIXERCONTROLDETAILS* d, DWORD) {
    m.level = ((MIXERCONTROLDETAILS_UNSIGNED*) d->paDetails)->dwValue;
    if (m.state == SONG_PLAYING) for (int i = 0; i < VOICES; i++) SND_VOICE[m.voice[i]].volume = loud_now();
    if (m.log) fprintf(stderr, "aoemus: mixer %u\n", m.level);
    return 0;
}
}
