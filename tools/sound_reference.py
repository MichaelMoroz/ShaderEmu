"""Reference model of the sound card (docs/sound.md): mixes what the harness captured.

  rvc_harness ... --fixed-dt 0.004 --sound-capture out.wav --save-state out.snap
  python tools/sound_reference.py out.wav out.snap [--rom rootfs.bin] [--stream-test]

out.wav.frames holds, per mix, the host's cursor and the machine's control row after that
frame. From those and the samples in the snapshot's RAM (and the ROM) this computes every
sample again, in the integers the shader uses (experiments/rvc_opt/src/sound.h), and compares:
the device's own words frame by frame, then the samples. Exit code 0 when all are equal.

Sample data must not have changed during the run, except a stream's: --stream-test takes
those from the test card's formula (programs/sound/sound.c).
"""
import argparse
import math
import struct
import sys

import numpy as np

RING = 16384
SNAP_HEADER = 32
RAM_AT = SNAP_HEADER + 64 * 2048 * 16
PCM8, PCM16, STREAM, ADPCM, FM = 1, 2, 3, 4, 5
STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
         130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060,
         1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
         7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
LOOPED, SMOOTH, STEREO = 0x100, 0x200, 0x400
M32 = 0xffffffff


class Memory:
    def __init__(self, snap, rom, stream_test):
        self.ram = np.memmap(snap, dtype=np.uint8, mode='r', offset=RAM_AT)
        self.rom = np.fromfile(rom, dtype=np.uint8) if rom else np.zeros(0, np.uint8)
        self.stream_test = stream_test

    def bytes_at(self, address):
        """Bytes at an array of addresses, RAM or ROM; zero outside both."""
        address = address.astype(np.int64)
        out = np.zeros(address.shape, np.int64)
        for base, data in ((0x80000000, self.ram), (0x40000000, self.rom)):
            off = address - base
            ok = (off >= 0) & (off < len(data))
            out[ok] = data[off[ok]]
        return out

    def words_at(self, address):
        """32-bit words at an array of addresses that are multiples of four."""
        a = np.asarray(address, dtype=np.int64)
        return self.bytes_at(a) | self.bytes_at(a + 1) << 8 | self.bytes_at(a + 2) << 16 | self.bytes_at(a + 3) << 24

    def halves_at(self, address):
        v = self.bytes_at(address) | self.bytes_at(address + 1) << 8
        return (v ^ 0x8000) - 0x8000


class Voice:
    """A voice at one sample, or at many: pos, frac and on may be arrays."""

    def copy(self):
        v = Voice()
        v.__dict__.update(self.__dict__)
        return v

    def move(self, k):
        """snd_move: on by k output samples (an array, or a number)."""
        k = np.asarray(k, dtype=np.uint64)
        adv = k * np.uint64(self.pitch) + np.asarray(self.frac, dtype=np.uint64)
        pos = (np.asarray(self.pos, dtype=np.uint64) + (adv >> np.uint64(16))) & np.uint64(M32)
        frac = adv & np.uint64(0xffff)
        on = np.broadcast_to(np.asarray(self.on, dtype=bool), pos.shape).copy()
        pos = pos.astype(np.int64)
        frac = frac.astype(np.int64)
        if self.kind & 0xff == STREAM:
            starved = ((self.restart - pos) & M32) - (((self.restart - pos) & 0x80000000) << 1) <= 0
            pos = np.where(starved, self.restart, pos)
            frac = np.where(starved, 0, frac)
        else:
            over = pos >= self.length
            if self.kind & LOOPED and self.restart < self.length:
                span = self.length - self.restart
                pos = np.where(over, self.restart + (pos - self.restart) % span, pos)
            else:
                on &= ~over
        # a voice that is off does not move
        was = np.broadcast_to(np.asarray(self.on, dtype=bool), pos.shape)
        self.pos = np.where(was, pos, self.pos)
        self.frac = np.where(was, frac, self.frac)
        self.on = on & was

    def sounds(self):
        dry = (self.kind & 0xff == STREAM) & (np.asarray(self.pos) == self.restart)
        return np.asarray(self.on, dtype=bool) & ~dry

    def read(self, mem):
        """snd_read at pos: left and right, 16 bits signed."""
        p = np.asarray(self.pos, dtype=np.int64)
        kind = self.kind & 0xff
        if kind == STREAM:
            if mem.stream_test:
                s = (((p * 2654435761) & M32) >> 19) - 4096
                return s, s
            p = p & (self.length - 1)
        first = 2 * p if self.kind & STEREO else p
        second = first + 1 if self.kind & STEREO else first
        if kind == PCM8:
            return (mem.bytes_at(self.address + first) - 128) * 256, (mem.bytes_at(self.address + second) - 128) * 256
        return mem.halves_at((self.address + 2 * first) & M32), mem.halves_at((self.address + 2 * second) & M32)


def adpcm(v, mem):
    """snd_adpcm: the samples at pos and after it, decoded from the checkpoint before them."""
    p = np.asarray(v.pos, dtype=np.int64)
    at = (v.aux + 4 * (p >> 5)) & M32
    mark = mem.bytes_at(at) | mem.bytes_at(at + 1) << 8 | mem.bytes_at(at + 2) << 16
    value = ((mark & 0xffff) ^ 0x8000) - 0x8000
    index = (mark >> 16) & 0xff
    last = np.minimum(p + 1, v.length - 1)
    steps = np.array(STEPS, dtype=np.int64)
    x = np.zeros(p.shape, np.int64)
    y = np.zeros(p.shape, np.int64)
    for j in range(33):
        n = (p & ~31) + j
        active = n <= last
        code = (mem.bytes_at((v.address + (n >> 1)) & M32) >> (4 * (n & 1))) & 15
        size = steps[np.minimum(index, 88)]
        change = (size >> 3) + np.where(code & 4, size, 0) + np.where(code & 2, size >> 1, 0) + np.where(code & 1, size >> 2, 0)
        moved = np.clip(value + np.where(code & 8, -change, change), -32768, 32767)
        value = np.where(active, moved, value)
        index = np.where(active, np.clip(index + np.where(code & 4, 2 * (code & 3) + 2, -1), 0, 88), index)
        x = np.where(n == p, value, x)
        y = np.where(active, value, y)
    return x, y


SINE = [int(round(4095 * math.sin((i + 0.5) * math.pi / 512))) for i in range(256)]
LOUD = [int(round(4096 * 2 ** (-i / 32))) for i in range(32)]


def wave(phase, shape):
    """snd_wave: a sine of 4095; 2^32 of phase is a turn."""
    at = (phase & M32) >> 22
    n = at & 255
    v = np.array(SINE, dtype=np.int64)[np.where(at & 256, 255 - n, n)]
    if shape == 3:
        return np.where(at & 256, 0, v)
    return np.where(at & 512, 0 if shape == 1 else v if shape == 2 else -v, v)


def scale(k, rate):
    return (k * rate) >> 16


def held_level(a, b, t):
    after = np.minimum(np.maximum(t - a[2], 0), 0xffffff)
    falling = np.minimum(scale(after, b[0]), a[3])
    later = a[3] + (0 if a[0] & 0x400 else scale(np.minimum(np.maximum(after - b[1], 0), 0xffffff), b[2]))
    return np.where(t <= a[2], 0, np.where(after <= b[1], falling, later))


def envelope(a, b, t, held):
    down = held_level(a, b, np.minimum(t, held)) + a[1]
    down = down + np.where(t > held, scale(np.minimum(np.maximum(t - held, 0), 0xffffff), b[2]), 0)
    loud = np.array(LOUD, dtype=np.int64)[down & 31] >> np.minimum(down >> 5, 62)
    rising = np.minimum(t, held)
    loud = np.where(rising < a[2], loud * rising // np.maximum(a[2], 1), loud)
    return np.where(down >= 512, 0, loud)


def fm(v, mem):
    """snd_fm: the voice's track at pos, left and right."""
    t = np.asarray(v.pos, dtype=np.int64)
    track = (v.address & 0x7ffffff0) | 0x80000000
    count = min(int(mem.words_at(np.array([track]))[0]), 65535)
    out = np.zeros(t.shape, np.int64)
    if count == 0:
        return out, out
    lo = np.zeros(t.shape, np.int64)
    hi = np.full(t.shape, count, np.int64)
    for _ in range(16):
        mid = (lo + hi) >> 1
        wide = hi - lo > 1
        before = mem.words_at(track + 16 + 32 * mid) <= t
        lo = np.where(wide & before, mid, lo)
        hi = np.where(wide & ~before, mid, hi)
    left = np.zeros(t.shape, np.int64)
    right = np.zeros(t.shape, np.int64)
    for n in np.unique(lo):
        pick = lo == n
        note = [int(x) for x in mem.words_at(track + 16 + 32 * int(n) + 4 * np.arange(4))]
        gain = [int(x) for x in mem.words_at(track + 32 + 32 * int(n) + 4 * np.arange(2))]
        tone = ((v.aux & 0x7ffffff0) | 0x80000000) + 64 * note[3]
        ma, mb, ca, cb = ([int(x) for x in mem.words_at(tone + 16 * k + 4 * np.arange(4))] for k in range(4))
        since = t[pick] - note[0]
        on = since >= 0
        since = np.maximum(since, 0)
        loud = envelope(ca, cb, since, note[1])
        mm, cm = ma[0] & 0xff, ca[0] & 0xff
        mp = ((note[2] >> 1) * since if mm == 1 else ((note[2] * since) & M32) * (mm >> 1)) & M32
        cp = ((note[2] >> 1) * since if cm == 1 else ((note[2] * since) & M32) * (cm >> 1)) & M32
        bend = envelope(ma, mb, since, note[1])
        back = (ma[0] >> 12) & 7
        first = 0 if back == 0 else (wave(mp, (ma[0] >> 8) & 3) * bend) >> 12
        m = (wave(mp + (first << (13 + back)), (ma[0] >> 8) & 3) * bend) >> 12
        if ma[0] & 0x8000:
            s = ((wave(cp, (ca[0] >> 8) & 3) * loud) >> 12) + m
        else:
            s = (wave(cp + (m << 22), (ca[0] >> 8) & 3) * loud) >> 12
        s = np.where(on & (loud != 0), s, 0)
        left[pick] = (s * min(gain[0], 256)) >> 6
        right[pick] = (s * min(gain[1], 256)) >> 6
    return left, right


def voice_at(row, device, n, cursor):
    """snd_voice_at: voice n at the cursor, from the guest's words and the device's."""
    a, b = row[0x80 + 2 * n], row[0x81 + 2 * n]
    c, d = device[0xc0 + 2 * n], device[0xc1 + 2 * n]
    v = Voice()
    v.key, v.pitch, v.volume = int(a[0]), int(b[1]), int(b[2])
    moved = 0
    if a[0] != c[0]:
        v.on = bool(a[0] & 1)
        v.kind, v.address, v.length, v.restart, v.aux = int(a[1]), int(a[2]), int(a[3]), int(b[0]), int(b[3])
        v.pos, v.frac = 0, 0
    else:
        v.on = bool(c[1] & 1)
        v.kind, v.address, v.length, v.restart, v.aux = int(c[1]) >> 8, int(d[0]), int(d[1]), int(d[2]), int(d[3])
        v.pos, v.frac = int(c[2]), int(c[3])
        moved = (cursor - int(device[0x23][0])) & M32
    if v.kind & 0xff == STREAM:
        v.restart = int(b[0])
    if v.on:
        v.move(moved)
        v.pos, v.frac, v.on = int(v.pos), int(v.frac), bool(v.on)
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('wav')
    ap.add_argument('snapshot')
    ap.add_argument('--rom')
    ap.add_argument('--stream-test', action='store_true')
    ap.add_argument('--rate', type=int, default=48000)
    args = ap.parse_args()

    mem = Memory(args.snapshot, args.rom, args.stream_test)
    frames = np.fromfile(args.wav + '.frames', dtype=np.uint32).reshape(-1, 1 + 256 * 4)
    wav = open(args.wav, 'rb').read()
    got = np.frombuffer(wav[44:], dtype=np.int16).reshape(-1, 2).astype(np.int64)

    device = np.zeros((256, 4), np.int64)   # the device's words as the model has them
    want = []
    word_errors = 0
    for f in range(len(frames)):
        cursor = int(frames[f][0])
        row = frames[f][1:].reshape(256, 4).astype(np.int64)
        guest = row[0x22]
        count = min(int(guest[2]), 32) if guest[0] else 0
        span = (int(frames[f + 1][0]) - cursor) & M32 if f + 1 < len(frames) else 0
        k = np.arange(min(span, RING), dtype=np.int64)
        left = np.zeros(len(k), np.int64)
        right = np.zeros(len(k), np.int64)
        after = device.copy()
        for n in range(count):
            v = voice_at(row, device, n, cursor)
            after[0xc0 + 2 * n] = [v.key, (v.kind << 8) & M32 | int(v.on), v.pos, v.frac]
            after[0xc1 + 2 * n] = [v.address, v.length, v.restart, v.aux]
            if not v.on or len(k) == 0:
                continue
            m = v.copy()
            m.move(k)
            if v.kind & 0xff == ADPCM:
                sl, both = adpcm(m, mem)
                if v.kind & SMOOTH:
                    sl = sl + (((both - sl) * (np.asarray(m.frac) >> 4)) >> 12)
                sr = sl
            elif v.kind & 0xff == FM:
                sl, sr = fm(m, mem)
            else:
                sl, sr = m.read(mem)
            if v.kind & SMOOTH and v.kind & 0xff < ADPCM:
                nxt = m.copy()
                nxt.frac, nxt.pitch = 0, 0x10000
                nxt.move(1)
                tl, tr = nxt.read(mem)
                has = nxt.sounds()
                part = np.asarray(m.frac) >> 4
                sl = sl + (((np.where(has, tl, sl) - sl) * part) >> 12)
                sr = sr + (((np.where(has, tr, sr) - sr) * part) >> 12)
            live = m.sounds()
            left += np.where(live, sl * min(v.volume & 0xffff, 256), 0)
            right += np.where(live, sr * min(v.volume >> 16, 256), 0)
        if guest[0]:
            after[0x23][0], after[0x23][1] = cursor, args.rate
        master = min(int(guest[1]), 256)
        want.append(np.stack([np.clip(((left >> 8) * master) >> 8, -32768, 32767),
                              np.clip(((right >> 8) * master) >> 8, -32768, 32767)], axis=1))
        if span > RING:
            want.append(np.zeros((span - RING, 2), np.int64))
        device = after
        # the device's words after this frame's control pass must be the model's
        bad = np.argwhere(row[0xc0:0x100] != device[0xc0:0x100])
        if len(bad) or row[0x23][0] != device[0x23][0] or row[0x23][1] != device[0x23][1]:
            if word_errors < 5:
                where = '0x%03x word %d' % ((0xc0 + bad[0][0]) * 16, bad[0][1]) if len(bad) else 'the clock'
                print('frame %d (cursor %d): the device and the model disagree at %s' % (f, cursor, where))
            word_errors += 1

    want = np.concatenate(want) if want else np.zeros((0, 2), np.int64)
    n = min(len(want), len(got))
    diff = np.abs(want[:n] - got[:n])
    differing = int(np.count_nonzero(diff.max(axis=1))) if n else 0
    print('%d mixes, %d samples (%d captured), peak %d' % (len(frames), len(want), len(got), int(np.abs(got).max()) if len(got) else 0))
    print('device words: %d frames differ' % word_errors)
    print('samples: %d differ, largest difference %d' % (differing, int(diff.max()) if n else 0))
    if differing:
        first = int(np.argmax(diff.max(axis=1) > 0))
        print('first at sample %d: model %s, device %s' % (first, want[first].tolist(), got[first].tolist()))
        print('last at sample %d of %d' % (int(np.nonzero(diff.max(axis=1))[0][-1]), n))
    ok = word_errors == 0 and differing == 0 and len(want) == len(got) and len(got) > 0
    print('MATCH' if ok else 'DIFFERENT')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
