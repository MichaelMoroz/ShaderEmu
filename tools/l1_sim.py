"""How long frames would run under other layouts of the write cache, from the harness's
--l1-log (see tools/l1_study.py, which does the same for caches of words).

    python tools/l1_sim.py FILE.l1 [--budget 16384] [--limit FRAMES] [--step N]

Record with a small --ticks (`--ticks 32 --fixed-dt 0.0000625`): each record is then what 32
instructions stored, the records in order are the guest's stores in order, and a simulated
frame is as many of them as its cache has room for. A simulated frame runs until its budget,
until the logged run itself ended a frame early there (wfi, a memory operation, a fence) or
until a store finds no room: the stall this is about.

Printed per layout: frames, instructions a frame, the share of frames a full cache ended, how
full the cache was when it did, and the time for the run at two costs of a frame.

A layout is: what an entry holds (a word, or a RAM texel of four words), buckets, entries a
bucket, and one or two hash functions (with two, a new entry goes to the emptier bucket, or
to the first with room). A log from the texel cache has one address a texel, so the word
layouts are only right on a log that has words (one from before it).
"""
import sys
import numpy as np

M32 = 0xffffffff


def h_current(k, bits):      # word index: bits 0-6 and 9-10 (address bits 2-8 and 11-12)
    return (k & 127) | (((k >> 9) & 3) << 7)


def h_low(k, bits):
    return k & ((1 << bits) - 1)


def h_fold(k, bits):
    return (k ^ (k >> bits)) & ((1 << bits) - 1)


def h_fold3(k, bits):
    return (k ^ (k >> bits) ^ (k >> (2 * bits))) & ((1 << bits) - 1)


def h_mul(k, bits):
    return ((k * 0x9E3779B1) & M32) >> (32 - bits)


def h_mul2(k, bits):
    return ((k * 0x85EBCA6B) & M32) >> (32 - bits)


def h_rot(k, bits):          # a second choice that is cheap in a shader: the low bits, shifted, xor higher ones
    return ((k >> 3) ^ (k << (bits - 3)) ^ (k >> bits)) & ((1 << bits) - 1)


class Layout:
    def __init__(self, name, unit, bits, ways, hashes, stash=0, first=False):
        self.name, self.unit, self.bits, self.ways, self.hashes, self.stash = name, unit, bits, ways, hashes, stash
        self.first = first   # take the first table with room instead of the emptier bucket
        self.reset()
        self.frames = self.stalls = self.instr = 0
        self.fill_at_stall = []

    def reset(self):
        self.have = set()
        self.fill = {}
        self.extra = 0
        self.now = 0

    def add(self, keys):
        """False when a key finds no room."""
        have, fill, ways = self.have, self.fill, self.ways
        for k in keys:
            if k in have:
                continue
            best, room = None, 0
            for t, h in enumerate(self.hashes):
                b = (t, h(k, self.bits))
                r = ways - fill.get(b, 0)
                if r > room:
                    best, room = b, r
                    if self.first:
                        break
            if best is None:
                if self.extra < self.stash:
                    self.extra += 1
                    have.add(k)
                    continue
                return False
            fill[best] = fill.get(best, 0) + 1
            have.add(k)
        return True

    def end(self, stalled):
        self.frames += 1
        self.instr += self.now
        if stalled:
            self.stalls += 1
            self.fill_at_stall.append(len(self.have))
        self.reset()


def layouts():
    out = []
    # words, 2048 of them: 1024 texels of two (address, value) pairs, as now
    out.append(Layout('now: word, 512 x 4, address bits 2-8,11-12', 4, 9, 4, [h_current]))
    out.append(Layout('word, 512 x 4, low bits', 4, 9, 4, [h_low]))
    out.append(Layout('word, 512 x 4, folded', 4, 9, 4, [h_fold]))
    out.append(Layout('word, 512 x 4, multiplied', 4, 9, 4, [h_mul]))
    out.append(Layout('word, 1024 x 2, folded', 4, 10, 2, [h_fold]))
    out.append(Layout('word, 256 x 8, folded', 4, 8, 8, [h_fold]))
    out.append(Layout('word, 2 x 512 x 2, low | multiplied', 4, 9, 2, [h_low, h_mul]))
    out.append(Layout('word, 2 x 512 x 2, low | rotated', 4, 9, 2, [h_low, h_rot]))
    out.append(Layout('word, 2 x 512 x 2, folded | multiplied', 4, 9, 2, [h_fold, h_mul]))
    out.append(Layout('word, 2 x 512 x 2, low | rotated, 8 spare', 4, 9, 2, [h_low, h_rot], stash=8))
    out.append(Layout('word, 2 x 512 x 2, low then rotated', 4, 9, 2, [h_low, h_rot], first=True))
    out.append(Layout('word, 2 x 512 x 2, now then rotated', 4, 9, 2, [h_current, h_rot], first=True))
    out.append(Layout('word, 2 x 512 x 2, low then multiplied', 4, 9, 2, [h_low, h_mul], first=True))
    out.append(Layout('word, any 2048 (no hash could do better)', 4, 0, 2048, [lambda k, b: 0]))
    # RAM texels: a tag and four words an entry; 1024 state texels hold 819 of them
    out.append(Layout('texel, 256 x 3 = 768, low bits', 16, 8, 3, [h_low]))
    out.append(Layout('texel, 256 x 3 = 768, folded', 16, 8, 3, [h_fold]))
    out.append(Layout('texel, 2 x 128 x 3 = 768, low | multiplied', 16, 7, 3, [h_low, h_mul]))
    out.append(Layout('texel, 2 x 128 x 3 = 768, low | rotated', 16, 7, 3, [h_low, h_rot]))
    out.append(Layout('texel, 2 x 128 x 3 = 768, low then rotated', 16, 7, 3, [h_low, h_rot], first=True))
    out.append(Layout('texel, 2 x 256 x 2 = 1024, low then rotated', 16, 8, 2, [h_low, h_rot], first=True))
    out.append(Layout('texel, 2 x 64 x 3 = 384, low then rotated', 16, 6, 3, [h_low, h_rot], first=True))
    out.append(Layout('texel, 2 x 32 x 3 = 192, low then rotated', 16, 5, 3, [h_low, h_rot], first=True))
    out.append(Layout('texel, 2 x 16 x 3 = 96, low then rotated', 16, 4, 3, [h_low, h_rot], first=True))
    out.append(Layout('texel, 2 x 256 x 1 = 512, low | rotated', 16, 8, 1, [h_low, h_rot]))
    out.append(Layout('texel, any 819', 16, 0, 819, [lambda k, b: 0]))
    return out


def main():
    args = sys.argv[1:]
    budget, limit, step = 16384, None, None
    for name in ('--budget', '--limit', '--step'):
        if name in args:
            i = args.index(name)
            v = int(args[i + 1])
            del args[i:i + 2]
            if name == '--budget':
                budget = v
            elif name == '--step':
                step = v
            else:
                limit = v
    d = np.fromfile(args[0], dtype=np.uint32)
    frames, i = [], 0
    while i + 3 <= len(d) and i + 3 + int(d[i + 2]) <= len(d):
        count, n = int(d[i]), int(d[i + 2])
        frames.append((count, d[i + 3:i + 3 + n]))
        i += 3 + n
        if limit and len(frames) >= limit:
            break
    if step is None:
        step = int(np.median([f[0] for f in frames]))
    total = sum(f[0] for f in frames)
    words = sum(len(f[1]) for f in frames)
    print('%s: %d records of %d instructions, %d instructions, %.1f%% of them stores of a new word'
          % (args[0], len(frames), step, total, 100.0 * words / total))

    ls = layouts()
    keysets = {}
    for count, addrs in frames:
        early = count < step
        if count > 4 * step:      # a gap in the log (the harness skipped frames): everything starts again
            for L in ls:
                L.end(False)
            continue
        for unit in (4, 16):
            keysets[unit] = [int(a) // unit for a in addrs]
        for L in ls:
            if not L.add(keysets[L.unit]):
                # the store that found no room ends the frame; it is written by the commit
                L.now += count
                L.end(True)
                continue
            L.now += count
            if early or L.now >= budget:
                L.end(False)

    print('%-46s %7s %8s %7s %9s %9s %9s' % ('layout', 'frames', 'instr/fr', 'stalls', 'fill', '0.083 ms', '1 ms'))
    base = None
    for L in ls:
        if L.now:
            L.end(False)
        fill = np.mean(L.fill_at_stall) if L.fill_at_stall else 0
        # seconds: 0.27 us an instruction, and a frame's fixed cost in the harness or a slower host
        t1 = L.instr * 0.27e-6 + L.frames * 0.083e-3
        t2 = L.instr * 0.27e-6 + L.frames * 1e-3
        if base is None:
            base = (t1, t2)
        print('%-46s %7d %8.0f %6.1f%% %9.0f %8.1f%% %8.1f%%' % (L.name, L.frames, L.instr / L.frames,
              100.0 * L.stalls / L.frames, fill, 100 * (base[0] / t1 - 1), 100 * (base[1] / t2 - 1)))


if __name__ == '__main__':
    main()
