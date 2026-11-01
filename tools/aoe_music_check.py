"""Age of Empires' music in a run's console (AOE_MUSIC_LOG=2 AOE_SOUND_LOG=1, on rvc_cpu with RVC_SOUND=1; docs/games/aoe.md, "Music"): are the
notes on the voices' tracks those of the trial's MIDI file, and what came when?
   python tools/aoe_music_check.py LOG IMAGE [--wav FILE] [--seconds S] [--from-song N]
   python tools/aoe_music_check.py --files IMAGE      what the trial's songs ask for of the card, from the files alone
This reads each MIDI file by itself, not by the port's reader, works out what each note must be from the file and the bank, and compares them with
the words the machine wrote for the card, which the log holds: start, pitch, instrument, loudness and length, and the voices' own words.

--wav writes what those words sound like: music and effects, mixed by tools/sound_reference.py's FM and its integers from the tracks and instruments
in the log and the effects' samples in the image's ROM, at the card's clock values in the log. rvc_cpu mixes nothing, so this is the reference model's
mix of the machine's words, not a capture of the shader's output. The model agrees with the shader to the sample where the two were compared."""
import argparse, collections, os, re, struct, sys, wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sound_reference as card

RATE = 48000
TONES = 175
TAIL_S = 0.5


def read_length(d, i):
    v = 0
    while True:
        c = d[i]
        i += 1
        v = v << 7 | (c & 127)
        if not c & 128:
            return v, i


def midi_events(path):
    """Every event of a Standard MIDI File in the order it sounds: (tick, track, order, status or meta, a, b)."""
    d = open(path, 'rb').read()
    assert d[:4] == b'MThd', path
    head = struct.unpack('>I', d[4:8])[0]
    fmt, count, division = struct.unpack('>HHH', d[8:14])
    i = 8 + head
    events = []
    for track in range(count):
        assert d[i:i + 4] == b'MTrk', path
        size = struct.unpack('>I', d[i + 4:i + 8])[0]
        p, end = i + 8, i + 8 + size
        tick = status = order = 0
        while p < end:
            delta, p = read_length(d, p)
            tick += delta
            first = d[p]
            if first & 0x80:
                p += 1
                s = first
            else:
                s = status
            if s == 0xFF:
                kind = d[p]
                n, p = read_length(d, p + 1)
                events.append((tick, track, order, 0xFF, kind, d[p:p + n]))
                p += n
            elif s in (0xF0, 0xF7):
                n, p = read_length(d, p)
                p += n
            else:
                n = 1 if s & 0xF0 in (0xC0, 0xD0) else 2
                events.append((tick, track, order, s, d[p], d[p + 1] if n > 1 else 0))
                p += n
                status = s
            order += 1
        i = end
    events.sort(key=lambda e: e[:3])
    return division, fmt, count, events


def expected_notes(path, bank):
    """What the file and the bank say each voice's note must be; and what the file has that the card does not follow."""
    division, _, _, events = midi_events(path)
    tempo, base_tick, base_us = 500000, 0, 0
    patch, level, expr, pan = [0] * 16, [100] * 16, [127] * 16, [64] * 16
    notes, open_keys = [], collections.defaultdict(list)
    ignored = collections.Counter()
    now = 0
    for tick, _, _, s, a, b in events:
        now = (base_us + (tick - base_tick) * tempo // division) * RATE // 1000000
        if s == 0xFF:
            if a == 0x51 and len(b) == 3:
                base_us += (tick - base_tick) * tempo // division
                base_tick, tempo = tick, b[0] << 16 | b[1] << 8 | b[2]
            continue
        kind, c = s & 0xF0, s & 15
        if kind == 0xC0:
            patch[c] = a
        elif kind == 0xE0:
            ignored['pitch wheel'] += 1
        elif kind == 0xB0:
            if a == 7:
                level[c] = b
            elif a == 11:
                expr[c] = b
            elif a == 10:
                pan[c] = b
            elif a == 121:
                expr[c] = 127
            elif a == 64:
                ignored['sustain pedal'] += 1
            elif a not in (120, 123):
                ignored['controller %d' % a] += 1
        elif kind == 0x90 and b:
            tone = 128 + a - 35 if c == 9 else patch[c]
            if not 0 <= tone < TONES:
                ignored['drums outside the bank'] += 1
                continue
            r = bank[8 + 36 * tone:8 + 36 * (tone + 1)]
            flags = r[0] | r[1] << 8
            loud = b * level[c] * expr[c] * 2 // (127 * 127)
            for k in range(2 if flags & 4 else 1):
                offset = struct.unpack_from('<h', r, 18 + 16 * k)[0]
                key = r[3] if c == 9 or flags & 1 else a + offset
                key = max(0, min(127, key))
                hz = 440 * 2 ** ((key - 69) / 12)
                if k == 1:
                    hz *= 2 ** (((r[2] // 2) - 64) / (32 * 12))
                n = dict(start=now, tone=tone + TONES * k, left=loud * ((127 - pan[c]) if pan[c] > 64 else 64) // 64,
                         right=loud * (pan[c] if pan[c] < 64 else 64) // 64, turns=hz / RATE * 2 ** 32, end=None, key=key, channel=c)
                notes.append(n)
                open_keys[c, a].append(n)
        elif kind in (0x80, 0x90):
            for n in open_keys.pop((c, a), []):
                n['end'] = now
    for ns in open_keys.values():
        for n in ns:
            n['end'] = now
    return notes, now, ignored


def most_at_once(notes):
    """The most notes sounding together (keys down), which is what the voices must hold."""
    changes = []
    for n in notes:
        changes += [(n['start'], 1), (n['end'], -1)]
    changes.sort(key=lambda c: (c[0], c[1]))
    now = most = 0
    for _, c in changes:
        now += c
        most = max(most, now)
    return most


def data_file(image, name):
    folder = os.path.join(image, 'overlay', 'usr', 'share', 'aoe')
    base = re.split(r'[\\/]', name)[-1].lower()
    for f in os.listdir(folder):
        if f.lower() == base:
            return os.path.join(folder, f)
    return None


class Song:
    def __init__(self, name, line):
        self.name = name
        self.line = line
        self.notes_told, self.samples, self.cut = [int(x) for x in re.search(r'(\d+) notes, (\d+) samples, (\d+) cut', line).groups()]
        self.clock = int(re.search(r'clock (\d+)', line)[1])
        self.voices = []
        self.tracks = collections.defaultdict(list)
        self.ended = None


def read_log(path):
    songs, tones, sounds, other = [], {}, [], collections.Counter()
    opened = ended = None
    with open(path, encoding='latin-1') as f:
        for line in f:
            i = line.find('aoemus: ')
            if i >= 0:
                t = line[i + 8:].strip()
                if t.startswith('n '):
                    w = t.split()
                    if songs and len(w) == 8:
                        songs[-1].tracks[int(w[1])].append([int(x, 16) for x in w[2:]])
                    else:
                        other['torn note lines'] += 1
                elif t.startswith('t '):
                    w = t.split()
                    if len(w) == 18:
                        tones[int(w[1])] = [int(x, 16) for x in w[2:]]
                elif t.startswith('voice '):
                    m = re.match(r'voice (\d+) key (\w+) kind (\w+) track (\w+) length (\d+) volume (\w+) tones (\w+) count (\d+)', t)
                    if m and songs:
                        songs[-1].voices.append(dict(voice=int(m[1]), key=int(m[2], 16), kind=int(m[3], 16), track=int(m[4], 16), length=int(m[5]),
                                                     volume=int(m[6], 16), tones=int(m[7], 16), count=int(m[8])))
                elif t.startswith('play ') and re.search(r'clock \d+, level', t):   # (a run's last line may be torn)
                    songs.append(Song(t[5:].split(',')[0], t))
                elif t.startswith('ended '):
                    if songs:
                        songs[-1].ended = t
                elif t.startswith('open:'):
                    opened = t
                elif t.startswith('playing '):
                    other['looks'] += 1
                    other['voices on, last'] = int(re.search(r'(\d+) voices on', t)[1])
                    other['position, last'] = int(re.search(r' at (\d+) of', t)[1])
                continue
            m = re.search(r'aoesnd: play \d+ at ([0-9a-f]+), (\d+) bytes, (\d+) Hz, (\d+) bits, voice (\d+), volume ([0-9a-f]+), at \d+ ms, clock (\d+)', line)
            if m:
                sounds.append(dict(at=int(m[1], 16), bytes=int(m[2]), hz=int(m[3]), bits=int(m[4]), volume=int(m[6], 16), clock=int(m[7])))
    return songs, tones, sounds, other, opened


def check_song(song, image, bank):
    path = data_file(image, song.name)
    if not path:
        print('%s: no such file in the image' % song.name)
        return 1
    want, end, ignored = expected_notes(path, bank)
    if not song.tracks:
        print('%s: its notes are not in the log (AOE_MUSIC_LOG=2)' % song.name)
        return 1
    got = [n for v in sorted(song.tracks) for n in song.tracks[v]]
    bad = 0
    # by start, instrument and loudness; the pitch and the length are compared on the pair found
    left = collections.defaultdict(list)
    for n in want:
        left[n['start'], n['tone'], n['left'], n['right']].append(n)
    pitch_worst = 0
    cut = whole = longer = 0
    for w in got:
        wants = left.get((w[0], w[3], w[4], w[5]))
        if not wants:
            bad += 1
            if bad <= 5:
                print('  a note the file has not: start %d held %d pitch %x tone %d left %d right %d' % tuple(w[:6]))
            continue
        pick = min(wants, key=lambda n: abs(n['turns'] - w[2]))
        wants.remove(pick)
        pitch_worst = max(pitch_worst, abs(pick['turns'] / w[2] - 1))
        length = pick['end'] - pick['start']
        if w[1] == length:
            whole += 1
        elif w[1] < length:
            cut += 1
        else:
            longer += 1
    missing = sum(len(v) for v in left.values())
    in_order = all(all(a[0] <= b[0] for a, b in zip(t, t[1:])) for t in song.tracks.values())
    overlap = sum(a[0] + a[1] > b[0] for t in song.tracks.values() for a, b in zip(t, t[1:]))
    tones_used = len({n[3] for n in got})
    words_ok = bool(song.voices) and all(v['kind'] == card.FM and v['key'] & 1 and v['length'] == song.samples + RATE // 2 and v['tones'] == song.voices[0]['tones'] for v in song.voices)
    fails = bad or missing or longer or not in_order or overlap or pitch_worst > 0.002 or not words_ok or len(got) != song.notes_told
    print('%s: %d notes on %d voices, %d of %d of the file found, %d instruments; pitch off by %.3f%% at most; held as the file says %d, cut by a later note %d (the log says %d), longer %d; '
          '%d not in the file, %d of the file missing; %.1f s (the file %.1f); voices\' words %s%s' % (
              os.path.basename(path), len(got), len(song.tracks), len(want) - missing, len(want), tones_used, pitch_worst * 100, whole, cut, song.cut, longer, bad, missing,
              song.samples / RATE, end / RATE, 'right' if words_ok else 'WRONG', '' if not ignored else '; not followed: ' + ', '.join('%s %d' % kv for kv in ignored.most_common())))
    return 1 if fails else 0


def show_files(image):
    bank = open(os.path.join(image, 'overlay', 'usr', 'share', 'aoe', 'genmidi.op2'), 'rb').read()
    folder = os.path.join(image, 'overlay', 'usr', 'share', 'aoe')
    print('song            bytes  notes  seconds  most at once  channels  not followed')
    for f in sorted(os.listdir(folder)):
        if f.lower().endswith('.mid'):
            notes, end, ignored = expected_notes(os.path.join(folder, f), bank)
            print('%-13s %7d %6d %8.1f %13d %9d  %s' % (f, os.path.getsize(os.path.join(folder, f)), len(notes), end / RATE, most_at_once(notes), len({n['channel'] for n in notes}),
                                                         ', '.join('%s %d' % kv for kv in ignored.most_common())))


class Words:
    """Sound memory as the log gives it, to tools/sound_reference.py's fm()."""

    def __init__(self, base, size):
        self.base = base
        self.words = np.zeros(size // 4, np.int64)

    def put(self, address, values):
        self.words[(address - self.base) // 4:(address - self.base) // 4 + len(values)] = values

    def words_at(self, address):
        a = np.asarray(address, dtype=np.int64)
        return self.words[(a - self.base) // 4]


def write_wav(path, songs, tones, sounds, image, seconds, from_song):
    songs = [s for s in songs if s.tracks and s.voices][from_song:]
    if not songs:
        print('nothing to mix')
        return
    start = songs[0].clock
    total = int(seconds * RATE)
    left, right = np.zeros(total, np.int64), np.zeros(total, np.int64)
    base = min(v['track'] for s in songs for v in s.voices) & ~0xfff
    aux = songs[0].voices[0]['tones']
    memory = Words(base, (aux - base) + 2 * TONES * 64 + 4096)
    for k, w in tones.items():
        memory.put(aux + 64 * k, w)
    for i, song in enumerate(songs):
        if song.clock - start >= total:
            break
        stop = re.search(r'at clock (\d+)', song.ended)[1] if song.ended else None
        last = int(stop) if stop else song.clock + song.samples + RATE // 2
        if i + 1 < len(songs):
            last = min(last, songs[i + 1].clock)
        at, count = song.clock - start, min(last - song.clock, total - (song.clock - start))
        for chunk in range(0, count, RATE):
            t = np.arange(chunk, min(chunk + RATE, count), dtype=np.int64)
            for voice in song.voices:
                n = song.voices.index(voice)
                track = song.tracks[n]
                memory.put(voice['track'], [len(track), 0, 0, 0])
                flat = [x for note in track for x in note + [0, 0]]
                memory.put(voice['track'] + 16, flat)
                v = card.Voice()
                v.pos, v.address, v.aux = t, voice['track'], aux
                sl, sr = card.fm(v, memory)
                left[at + t] += sl * (voice['volume'] & 0xffff)
                right[at + t] += sr * (voice['volume'] >> 16)
    music_peak = int(max(np.abs(left).max(), np.abs(right).max())) >> 8
    rom = np.fromfile(os.path.join(image, 'rootfs.bin'), dtype=np.uint8)
    used = 0
    for e in sounds:
        at = e['clock'] - start
        if at < 0 or at >= total:
            continue
        step = e['hz'] / RATE
        raw = rom[e['at'] - 0x40000000:e['at'] - 0x40000000 + e['bytes']]
        wav = (raw.astype(np.int64) - 128) * 256 if e['bits'] == 8 else np.frombuffer(raw[:len(raw) & ~1].tobytes(), dtype='<i2').astype(np.int64)
        n = min(int(len(wav) / step), total - at)
        s = wav[np.minimum((np.arange(n) * step).astype(np.int64), len(wav) - 1)]
        left[at:at + n] += s * (e['volume'] & 0xffff)
        right[at:at + n] += s * (e['volume'] >> 16)
        used += 1
    mix = np.stack([np.clip(left >> 8, -32768, 32767), np.clip(right >> 8, -32768, 32767)], axis=1).astype('<i2')
    clipped = int(np.count_nonzero(np.abs(mix).max(axis=1) >= 32767))
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(mix.tobytes())
    each = [float(np.abs(mix[i:i + RATE]).max()) for i in range(0, total, RATE)]
    print('%s: %.1f s, %d songs, %d effects; peak of the music alone %d, of the mix %d, %d samples at the limit; seconds with sound %d of %d' % (
        path, total / RATE, i + 1, used, music_peak, int(np.abs(mix.astype(np.int64)).max()), clipped, sum(p > 64 for p in each), len(each)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log', nargs='?')
    ap.add_argument('image')
    ap.add_argument('--files', action='store_true')
    ap.add_argument('--wav')
    ap.add_argument('--seconds', type=float, default=60)
    ap.add_argument('--from-song', type=int, default=0)
    args = ap.parse_args()
    if args.files:
        show_files(args.image)
        return 0
    bank = open(os.path.join(args.image, 'overlay', 'usr', 'share', 'aoe', 'genmidi.op2'), 'rb').read()
    songs, tones, sounds, other, opened = read_log(args.log)
    print(opened or 'no music was opened')
    fails = 0 if songs else 1
    for s in songs:
        fails += check_song(s, args.image, bank)
    print('order: ' + ', '.join(re.split(r'[\\/]', s.name)[-1] + (' ended' if s.ended else '') for s in songs))
    # instruments: each written as the bank says, by fm's own reading of the words
    if tones:
        wrong = 0
        for k, w in tones.items():
            r = bank[8 + 36 * (k % TONES) + 4 + 16 * (k // TONES):]
            wrong += (w[8] & 0xff) != [1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30][r[7] & 15] or (w[0] & 0xff) != [1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30][r[0] & 15] or w[1] != (r[5] & 63) * 4 or w[9] != (r[12] & 63) * 4
        print('instruments: %d of %d, %d not as the bank says' % (len(tones), 2 * TONES, wrong))
        fails += wrong
    print('the card\'s words: %d looks while playing, the last with %s voices on, the first voice at sample %s; effects played beside the music: %d' % (
        other['looks'], other['voices on, last'], other['position, last'], sum(1 for e in sounds if songs and e['clock'] >= songs[0].clock)))
    if other['torn note lines']:
        print('%d note lines torn by other output' % other['torn note lines'])
    if args.wav:
        write_wav(args.wav, songs, tones, sounds, args.image, args.seconds, args.from_song)
    print('PASS' if not fails else 'FAIL (%d)' % fails)
    return 1 if fails else 0


sys.exit(main())
