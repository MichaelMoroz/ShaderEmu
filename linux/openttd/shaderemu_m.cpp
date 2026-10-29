/* OpenTTD's music on the sound card's FM voices (shaderemu_m.h), as Doom plays its scores
 * (linux/nanox/doom_sound.c): MidiFile reads a song, its notes are given to the voices and
 * written as their tracks, and the instruments are DMXOPL's General MIDI bank (genmidi.op2).
 * A song is made on a worker core while the game goes on (docs/games/openttd.md, "Music"). */

#include "../stdafx.h"
#include "../fileio_func.h"
#include "../debug.h"
#include "../base_media_music.h"
#include "../gfx_func.h"
#include "midifile.hpp"
#include "midi.h"
#include "shaderemu_m.h"
#include <csetjmp>
extern "C" {
#include "shaderemu_sound.h"
#include <mcwx.h>
}
uint8_t *SeWorkerHeap(uint32_t &size); /* linkgraphjob.cpp */
uint32_t SeWorkerArena(uint8_t *lo, uint32_t size, jmp_buf *jb);

#include "../safeguards.h"

static FMusicDriver_ShaderEmu iFMusicDriver_ShaderEmu;

static const int VOICES = 20;                 ///< (the mixer has 8 of the card's 32)
static const uint32_t MAX_NOTES = 24576;
static const int TONES = 175;                 ///< 128 instruments and 47 drums, each with up to two voices
static const uint32_t NOTE_OPEN = 0xffffff;   ///< held until the song says otherwise

static bool _ready, _playing, _looping;
static int _voice[VOICES];
static uint8_t _bank[8 + TONES * 36];
static char *_memory;                         ///< each voice's header and notes, then the instruments
static uint32_t _memory_at, _tones_at, _out;
static uint32_t _song_samples, _started_at;
static uint32_t _note_pitch[128];             ///< turns per output sample, times 2^32
static uint8_t _volume = 127;

struct Note {
	snd_note note;
	int voice;
};

/**
 * A song made into the voices' tracks (MakeTracks), on a worker core or here. The core that makes it alone touches
 * it until it is joined: its inputs, its answer, and the second voices' tunings kept from song to song.
 */
struct alignas(16) SongJob {
	const uint8_t *bytes;          ///< the file, read by the first core
	uint32_t length;
	int override_start, override_end;
	uint8_t *heap;                 ///< the worker's heap (SeWorkerHeap)
	uint32_t heap_size;
	uint32_t ok;                   ///< 0: no song, 1: made, 2: the worker's heap ran out
	uint32_t notes, samples, used; ///< (used: of the worker's heap, bytes)
	uint32_t track_of[VOICES];
	double factor[TONES];
	bool made[TONES];
};
static SongJob _job;

/** Levels fallen per sample (16.16) at an OPL decay or release rate: 512 levels take 39.28 s at rate 1. */
static uint32_t FallRate(int rate)
{
	uint32_t ms = rate ? 39281 >> (rate - 1) : 0;
	return rate ? static_cast<uint32_t>(512ull * 65536 * 1000 / (static_cast<uint64_t>(ms ? ms : 1) * _out)) : 0;
}

static void MakeOperator(snd_operator &op, const uint8_t *r)
{
	static const uint8_t twice[16] = { 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30 };
	int attack = r[1] >> 4, decay = r[1] & 15, sustain = r[2] >> 4, release = r[2] & 15;

	op = {};
	op.flags = twice[r[0] & 15] | SND_OP_WAVE(r[3] & 3) | (r[0] & 0x20 ? SND_OP_HOLDS : 0);
	op.level = (r[5] & 63) * 4;
	op.attack = attack == 0 ? NOTE_OPEN : attack == 15 ? 0 : (_out * 2826 / 1000) >> (attack - 1);
	op.sustain = sustain == 15 ? 496 : sustain * 16;
	op.decay = FallRate(decay);
	op.decay_samples = op.decay ? (op.sustain << 16) / op.decay : NOTE_OPEN;
	/* a note that would never die away is given two seconds to */
	op.release = release ? FallRate(release) : 512 * 65536 / (2 * _out);
}

std::optional<std::string_view> MusicDriver_ShaderEmu::Start(const StringList &)
{
	static const uint32_t lowest[12] = { 535809, 567670, 601425, 637188, 675077, 715219, 757748, 802806, 850544, 901120,
			954703, 1011473 }; // Hz times 65536 of MIDI notes 0 to 11
	if (snd_open() < 0) return "no sound card";
	auto f = FioFOpenFile("genmidi.op2", "rb", BASESET_DIR);
	if (!f.has_value() || fread(_bank, 1, sizeof(_bank), *f) != sizeof(_bank) || std::string_view(reinterpret_cast<char *>(_bank), 8) != "#OPL_II#") {
		return "no genmidi.op2";
	}
	for (int i = 0; i < 1000 && SND_RATE == 0; i++) snd_wait();
	if ((_out = SND_RATE) == 0) return "the sound card gives no rate";

	uint32_t bytes = VOICES * 16 + MAX_NOTES * sizeof(snd_note) + 2 * TONES * sizeof(snd_tone);
	if ((_memory = static_cast<char *>(snd_memory(bytes, &_memory_at))) == nullptr) return "no sound memory";
	for (int &v : _voice) {
		if ((v = snd_claim()) < 0) return "too few voices";
	}
	/* tone t is an instrument's first voice, TONES + t its second */
	_tones_at = _memory_at + VOICES * 16 + MAX_NOTES * sizeof(snd_note);
	snd_tone *tone = reinterpret_cast<snd_tone *>(_memory + (_tones_at - _memory_at));
	for (int t = 0; t < 2 * TONES; t++) {
		const uint8_t *r = _bank + 8 + 36 * (t % TONES) + 4 + 16 * (t / TONES);
		MakeOperator(tone[t].shaper, r);
		MakeOperator(tone[t].sounder, r + 7);
		tone[t].shaper.flags |= SND_OP_FEEDBACK((r[6] >> 1) & 7) | (r[6] & 1 ? SND_OP_BOTH : 0);
	}
	for (int n = 0; n < 128; n++) _note_pitch[n] = static_cast<uint32_t>((static_cast<uint64_t>(lowest[n % 12] << (n / 12)) << 16) / _out);
	_ready = true;
	return std::nullopt;
}

void MusicDriver_ShaderEmu::Stop()
{
	this->StopSong();
}

/** The second voice's tuning factor of a tone, made once (a pow is 7,000 instructions here). */
static double FineTune(SongJob &j, int tone, uint8_t fine)
{
	if (!j.made[tone]) {
		j.factor[tone] = std::pow(2.0, ((fine / 2) - 64) / (32.0 * 12.0));
		j.made[tone] = true;
	}
	return j.factor[tone];
}

/** The song's notes in the order they start, each given to a voice. */
static std::vector<Note> Convert(const MidiFile &file, SongJob &j)
{
	std::vector<Note> notes;
	uint32_t free_at[VOICES] = {}, tail = _out / 4;
	int sounding[VOICES], key_of[VOICES] = {};
	uint8_t patch[16] = {}, level[16], expression[16], pan[16];
	std::fill_n(sounding, VOICES, -1);
	std::fill_n(level, 16, 100);
	std::fill_n(expression, 16, 127);
	std::fill_n(pan, 16, 64);

	int64_t from = -1;
	uint32_t now = 0;
	auto release = [&](int v) {
		notes[sounding[v]].note.held = now - notes[sounding[v]].note.start;
		sounding[v] = -1;
		free_at[v] = now + tail;
	};
	auto note_on = [&](int channel, int key, int velocity) {
		int tone = channel == 9 ? 128 + key - 35 : patch[channel];
		if (tone < 0 || tone >= TONES) return;
		const uint8_t *r = _bank + 8 + 36 * tone;
		int flags = r[0] | r[1] << 8, loud = velocity * level[channel] * expression[channel] * 2 / (127 * 127);
		for (int k = 0; k < ((flags & 4) ? 2 : 1) && notes.size() < MAX_NOTES; k++) {
			int offset = static_cast<int16_t>(r[18 + 16 * k] | r[19 + 16 * k] << 8);
			int pitch = Clamp((channel == 9 || (flags & 1)) ? r[3] : key + offset, 0, 127);
			uint32_t turns = _note_pitch[pitch];
			/* the second voice is tuned apart by the bank's fine tune, in 32nds of a semitone */
			if (k == 1) turns = static_cast<uint32_t>(turns * FineTune(j, tone, r[2]));
			/* a voice that is silent, else one let go longest ago, else the oldest note */
			int best = 0;
			for (int v = 1; v < VOICES; v++) {
				bool idle = sounding[v] < 0, best_idle = sounding[best] < 0;
				if (idle != best_idle ? idle : idle ? free_at[v] < free_at[best] : notes[sounding[v]].note.start < notes[sounding[best]].note.start) best = v;
			}
			if (sounding[best] >= 0) release(best);
			Note n{};
			n.note.start = now;
			n.note.held = NOTE_OPEN;
			n.note.pitch = turns;
			n.note.tone = tone + TONES * k;
			n.note.left = loud * (pan[channel] > 64 ? 127 - pan[channel] : 64) / 64;
			n.note.right = loud * (pan[channel] < 64 ? pan[channel] : 64) / 64;
			n.voice = best;
			sounding[best] = static_cast<int>(notes.size());
			key_of[best] = channel << 8 | key;
			notes.push_back(n);
		}
	};

	for (const MidiFile::DataBlock &block : file.blocks) {
		if (j.override_end > 0 && block.ticktime >= static_cast<uint32_t>(j.override_end)) break;
		bool heard = block.ticktime >= static_cast<uint32_t>(j.override_start);
		if (heard && from < 0) from = block.realtime;
		if (heard) now = static_cast<uint32_t>((block.realtime - from) * _out / 1000000);

		const uint8_t *data = block.data.data();
		size_t remaining = block.data.size();
		uint8_t status = 0;
		while (remaining > 0) {
			if (data[0] & 0x80) {
				status = *data++;
				remaining--;
			}
			int channel = status & 15, size = 0;
			switch (status & 0xF0) {
				case MIDIST_PROGCHG: case MIDIST_CHANPRESS: size = 1; break;
				case MIDIST_NOTEOFF: case MIDIST_NOTEON: case MIDIST_POLYPRESS: case MIDIST_PITCHBEND: case MIDIST_CONTROLLER: size = 2; break;
				default: size = static_cast<int>(remaining); break; // a system message: the rest of the block
			}
			if (static_cast<size_t>(size) > remaining) break;
			int a = size > 0 ? data[0] : 0, b = size > 1 ? data[1] : 0;
			switch (status & 0xF0) {
				case MIDIST_PROGCHG: patch[channel] = a & 127; break;
				case MIDIST_NOTEON:
					if (b > 0) {
						if (heard) note_on(channel, a & 127, b);
						break;
					}
					[[fallthrough]];
				case MIDIST_NOTEOFF:
					for (int v = 0; v < VOICES; v++) {
						if (sounding[v] >= 0 && key_of[v] == (channel << 8 | a)) release(v);
					}
					break;
				case MIDIST_CONTROLLER:
					if (a == MIDICT_CHANVOLUME) level[channel] = b;
					if (a == 11) expression[channel] = b;
					if (a == MIDICT_PAN) pan[channel] = b;
					if (a == 121) expression[channel] = 127;
					if (a == 120 || a == 123) {
						for (int v = 0; v < VOICES; v++) {
							if (sounding[v] >= 0 && (key_of[v] >> 8) == channel) release(v);
						}
					}
					break;
			}
			data += size;
			remaining -= size;
		}
	}
	for (int v = 0; v < VOICES; v++) {
		if (sounding[v] >= 0) release(v);
	}
	j.samples = std::max<uint32_t>(now, 1);
	return notes;
}

/** A loaded song's notes written as the voices' tracks in the sound memory, on any core: it stores to j and the tracks. */
static void MakeTracks(const MidiFile &file, SongJob &j)
{
	std::vector<Note> notes = Convert(file, j);

	/* each voice's notes after its header (counted first, then each placed: a pass over the notes a voice was a
	 * third of the song's start) */
	uint32_t count[VOICES] = {}, placed[VOICES] = {};
	snd_note *first[VOICES];
	for (const Note &n : notes) count[n.voice]++;
	char *to = _memory;
	for (int v = 0; v < VOICES; v++) {
		snd_track *head = reinterpret_cast<snd_track *>(to);
		first[v] = reinterpret_cast<snd_note *>(to + sizeof(*head));
		j.track_of[v] = _memory_at + static_cast<uint32_t>(to - _memory);
		*head = {};
		head->count = count[v];
		to = reinterpret_cast<char *>(first[v] + count[v]);
	}
	for (const Note &n : notes) first[n.voice][placed[n.voice]++] = n.note;
	j.notes = static_cast<uint32_t>(notes.size());
	j.ok = 1;
}

/** The song made on a worker, from the bytes the first core read: no file is read there. */
static void MakeOnWorker(SongJob &j)
{
	MidiFile file;
	if (!file.SeLoadData(j.bytes, j.length)) return;
	MakeTracks(file, j);
}

/** On a worker (a0 the job): what it allocates comes from a heap of the first core's, and a heap run out says so. */
static uint32_t SongRun(uint32_t at, uint32_t)
{
	SongJob &j = *reinterpret_cast<SongJob *>(static_cast<uintptr_t>(at));
	jmp_buf jb;
	SeWorkerArena(j.heap, j.heap_size, &jb);
	if (setjmp(jb) == 0) {
		MakeOnWorker(j);
	} else {
		j.ok = 2;
	}
	j.used = SeWorkerArena(nullptr, 0, nullptr);
	return 0;
}

/* The first core's: a song read and not yet given to a worker, or out on one. A song made here was 26 million
 * instructions of the first core's start to the game; on a worker the game goes on meanwhile. */
enum SongState { SONG_NONE, SONG_READ, SONG_OUT };
static SongState _song_state = SONG_NONE;
static MusicSongInfo _song;
static std::vector<uint8_t> _song_bytes;
static mcw_handle _song_h;
static int _song_tries;

/** OPENTTD_MUSIC=core: every song made by the first core, as before. */
static bool SongHere()
{
	static const bool here = getenv("OPENTTD_MUSIC") != nullptr && std::string_view(getenv("OPENTTD_MUSIC")) == "core";
	return here;
}

/** OPENTTD_MUSIC_SUM=1: each song's tracks' sum; =all: and every song of the set's, made one after another at the first. */
static const char *SongSums()
{
	static const char *sums = getenv("OPENTTD_MUSIC_SUM");
	return sums;
}

/** The voices started on the song made (j). */
static void SongStart(const SongJob &j, bool on_worker)
{
	_song_samples = j.samples;
	Debug(driver, 1, "shaderemu music: {}, {} notes, {} s{}", _song.songname, j.notes, _song_samples / _out,
			on_worker ? fmt::format(", made on a worker ({} KB of its heap)", j.used >> 10) : std::string{});
	if (SongSums() != nullptr) {
		const uint8_t *p = reinterpret_cast<const uint8_t *>(_memory);
		const size_t bytes = VOICES * sizeof(snd_track) + j.notes * sizeof(snd_note);
		uint32_t sum = 2166136261u;
		for (size_t i = 0; i < bytes; i++) sum = (sum ^ p[i]) * 16777619u;
		for (uint32_t t : j.track_of) sum = (sum ^ t) * 16777619u;
		sum = (sum ^ j.samples) * 16777619u;
		fmt::print(stderr, "shaderemu music: {} sum {:08x}, {} notes, {} samples, {}\n", _song.songname, sum, j.notes, j.samples,
				on_worker ? fmt::format("on a worker, {} KB of its heap", j.used >> 10) : std::string{"here"});
	}
	_looping = _song.loop;
	uint32_t loud = SND_VOLUME(_volume * 48 / 127, _volume * 48 / 127);
	/* (every voice starts on the same sample: their keys change in one frame of the machine) */
	for (int v = 0; v < VOICES; v++) {
		snd_play_aux(_voice[v], SND_FM | (_looping ? SND_LOOPED : 0), j.track_of[v], _song_samples, 0, 0x10000, loud, _tones_at);
	}
	_started_at = SND_CLOCK;
	_playing = true;
}

/** The song made here, as before: the file read by LoadSong. */
static void SongMakeHere()
{
	_song_state = SONG_NONE;
	MidiFile file;
	if (!file.LoadSong(_song)) return;
	_job.override_start = _song.override_start;
	_job.override_end = _song.override_end;
	MakeTracks(file, _job);
	SongStart(_job, false);
}

/** The first core: a song read given to a free worker, or one out joined once it has ended (wait: until then). */
static void SongService(bool wait = false)
{
	if (_song_state == SONG_NONE || TickOnWorker() || mcw_on_worker()) return;
	if (_song_state == SONG_READ) {
		uint32_t size = 0;
		uint8_t *heap = SeWorkerHeap(size);
		if (heap == nullptr) return SongMakeHere();
		mcw_handle h = 0;
		for (;;) {
			if (!VpFlushOut()) { /* (a lingering team is ended only while no flush's pieces are out) */
				static bool touched;
				if (!touched) {
					/* (a store by the first core first, to every page the worker writes) */
					mcw_touch(_memory, VOICES * sizeof(snd_track) + MAX_NOTES * sizeof(snd_note));
					mcw_touch(&_job, sizeof(_job));
					touched = true;
				}
				_job.bytes = _song_bytes.data();
				_job.length = static_cast<uint32_t>(_song_bytes.size());
				_job.override_start = _song.override_start;
				_job.override_end = _song.override_end;
				_job.heap = heap;
				_job.heap_size = size;
				_job.ok = 0;
				_job.used = 0;
				mcw_for_end();
				h = mcw_try_call(SongRun, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&_job)), 0);
			}
			if (h != 0 || !wait) break;
			mcw_rest();
		}
		if (h == 0) {
			/* (no worker free: asked again at the next look, and made here after a while) */
			if (++_song_tries >= 256) SongMakeHere();
			return;
		}
		_song_h = h;
		_song_state = SONG_OUT;
	}
	if (!wait && !mcw_ready(_song_h)) return;
	mcw_join(_song_h);
	_song_state = SONG_NONE;
	if (_job.ok == 1) {
		SongStart(_job, true);
	} else if (_job.ok == 2) {
		SongMakeHere(); /* (its heap ran out) */
	}
}

/** A song to be played: read here, and made on a worker (SongService) unless there is none. */
static void SongAsk(const MusicSongInfo &song)
{
	_song = song;
	_song_tries = 0;
	_song_state = SONG_NONE;
	if (song.filetype != MTT_STANDARDMIDI || SongHere() || TickOnWorker() || mcw_on_worker()) return SongMakeHere();
	auto f = FioFOpenFile(song.filename, "rb", BASESET_DIR);
	if (!f.has_value()) return;
	_song_bytes.clear();
	uint8_t buf[4096];
	for (size_t n; (n = fread(buf, 1, sizeof(buf), *f)) > 0;) _song_bytes.insert(_song_bytes.end(), buf, buf + n);
	_song_state = SONG_READ;
	SongService();
}

void MusicDriver_ShaderEmu::PlaySong(const MusicSongInfo &song)
{
	if (!_ready) return;
	this->StopSong();
	static bool all_done;
	if (SongSums() != nullptr && std::string_view(SongSums()) == "all" && !all_done) {
		/* every song of the set made at once, each to its end, for its sum */
		all_done = true;
		const MusicSet *set = BaseMusic::GetUsedSet();
		for (uint i = 0; set != nullptr && i < NUM_SONGS_AVAILABLE; i++) {
			if (set->songinfo[i].filename.empty()) continue;
			SongAsk(set->songinfo[i]);
			SongService(true);
			this->StopSong();
		}
	}
	SongAsk(song);
}

void MusicDriver_ShaderEmu::StopSong()
{
	/* (a song out on a worker, which writes the tracks, ends first) */
	if (_song_state == SONG_OUT) mcw_join(_song_h);
	_song_state = SONG_NONE;
	if (!_playing) return;
	for (int v = 0; v < VOICES; v++) snd_stop(_voice[v]);
	_playing = false;
}

bool MusicDriver_ShaderEmu::IsSongPlaying()
{
	SongService();
	if (_song_state != SONG_NONE) return true;
	return _playing && (_looping || SND_CLOCK - _started_at < _song_samples);
}

void MusicDriver_ShaderEmu::SetVolume(uint8_t vol)
{
	_volume = vol;
	if (!_playing) return;
	/* of 256: twenty voices share the mix */
	uint32_t loud = SND_VOLUME(vol * 48 / 127, vol * 48 / 127);
	for (int v = 0; v < VOICES; v++) SND_VOICE[_voice[v]].volume = loud;
}
