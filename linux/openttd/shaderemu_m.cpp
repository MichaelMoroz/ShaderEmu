/* OpenTTD's music on the sound card's FM voices (shaderemu_m.h), as Doom plays its scores
 * (linux/nanox/doom_sound.c): MidiFile reads a song, its notes are given to the voices and
 * written as their tracks (shaderemu_fm.h, shared with Age of Empires), and the instruments are DMXOPL's General MIDI bank (genmidi.op2).
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
#include "shaderemu_fm.h"
#include <mcwx.h>
}
uint8_t *SeWorkerHeap(uint32_t &size); /* linkgraphjob.cpp */
uint32_t SeWorkerArena(uint8_t *lo, uint32_t size, jmp_buf *jb);

#include "../safeguards.h"

static FMusicDriver_ShaderEmu iFMusicDriver_ShaderEmu;

static const int VOICES = 20;                 ///< (the mixer has 8 of the card's 32)
static const uint32_t MAX_NOTES = 24576;
static const int TONES = FM_TONES;

static bool _ready, _playing, _looping;
static int _voice[VOICES];
static uint8_t _bank[8 + TONES * 36];
static char *_memory;                         ///< each voice's header and notes, then the instruments
static uint32_t _memory_at, _tones_at, _out;
static uint32_t _song_samples, _started_at;
static uint32_t _note_pitch[128];             ///< turns per output sample, times 2^32
static uint8_t _volume = 127;

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
	fm_tunes tunes;
};
static SongJob _job;

std::optional<std::string_view> MusicDriver_ShaderEmu::Start(const StringList &)
{
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
	_tones_at = _memory_at + VOICES * 16 + MAX_NOTES * sizeof(snd_note);
	fm_make_tones(reinterpret_cast<snd_tone *>(_memory + (_tones_at - _memory_at)), _bank, _out);
	fm_make_pitches(_note_pitch, _out);
	_ready = true;
	return std::nullopt;
}

void MusicDriver_ShaderEmu::Stop()
{
	this->StopSong();
}

/** The song's notes as tracks in the sound memory, on any core: it stores to j and the tracks. The notes in the order they start grow in a vector. */
static void MakeTracks(const MidiFile &file, SongJob &j)
{
	std::vector<fm_note> notes(1024);
	fm_song s{};
	s.bank = _bank;
	s.pitches = _note_pitch;
	s.tunes = &j.tunes;
	s.voices = VOICES;
	s.tail = _out / 4;
	s.max_notes = MAX_NOTES;
	s.notes = notes.data();
	s.cap = static_cast<uint32_t>(notes.size());
	s.owner = &notes;
	s.grow = [](fm_song *song) {
		auto &v = *static_cast<std::vector<fm_note> *>(song->owner);
		v.resize(v.size() * 2);
		song->notes = v.data();
		song->cap = static_cast<uint32_t>(v.size());
	};
	fm_song_begin(&s);

	int64_t from = -1;
	for (const MidiFile::DataBlock &block : file.blocks) {
		if (j.override_end > 0 && block.ticktime >= static_cast<uint32_t>(j.override_end)) break;
		bool heard = block.ticktime >= static_cast<uint32_t>(j.override_start);
		if (heard && from < 0) from = block.realtime;
		if (heard) s.now = static_cast<uint32_t>((block.realtime - from) * _out / 1000000);

		const uint8_t *data = block.data.data();
		size_t remaining = block.data.size();
		uint8_t status = 0;
		while (remaining > 0) {
			if (data[0] & 0x80) {
				status = *data++;
				remaining--;
			}
			int size = 0;
			switch (status & 0xF0) {
				case MIDIST_PROGCHG: case MIDIST_CHANPRESS: size = 1; break;
				case MIDIST_NOTEOFF: case MIDIST_NOTEON: case MIDIST_POLYPRESS: case MIDIST_PITCHBEND: case MIDIST_CONTROLLER: size = 2; break;
				default: size = static_cast<int>(remaining); break; // a system message: the rest of the block
			}
			if (static_cast<size_t>(size) > remaining) break;
			fm_message(&s, status, size > 0 ? data[0] : 0, size > 1 ? data[1] : 0, heard);
			data += size;
			remaining -= size;
		}
	}
	fm_song_end(&s);
	j.samples = std::max<uint32_t>(s.now, 1);
	fm_write_tracks(&s, _memory, _memory_at, j.track_of);
	j.notes = s.count;
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
