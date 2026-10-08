"""What fills the write cache, from the harness's --l1-log, and what other caches would do.

    python tools/l1_study.py L1.LOG [--tail MILLIONS] [--budget INSTRUCTIONS]

The log has, for each machine frame: instructions, the stall that ended it, a count, and the
word addresses its write cache held (in no order). Frames are joined into one stream of
writes and each cache design is run over it: a design keeps filling until a word finds no
place (a stall, and the cache empties) or the frame's instructions are used up. Frames the
machine ended for another reason (a copy, a wait) end every design's frame too.
"""
import collections
import sys
import numpy as np

BUDGET = 16384
L1_STALL = 3


def frames(path, tail):
    data = np.fromfile(path, dtype=np.uint32)
    out, at = [], 0
    while at + 3 <= len(data):
        n = int(data[at + 2])
        # (the stall word is the last one's: it means this frame only if the frame was short)
        out.append((int(data[at]), int(data[at + 1]) if data[at] < 16384 else 0, data[at + 3:at + 3 + n]))
        at += 3 + n
    if tail:
        total, keep = 0, len(out)
        while keep > 0 and total < tail * 1000000:
            keep -= 1
            total += out[keep][0]
        out = out[keep:]
    return out


def now(a):
    """Upstream's set: address bits 2-8 and 11-12."""
    return ((a >> 2) & 127) | (((a >> 11) & 3) << 7)


def low(bits):
    return lambda a: (a >> 2) & ((1 << bits) - 1)


def fold(a):
    """Bits 2-10 xor bits 11-19: every bit up to 1 MiB takes part."""
    w = a >> 2
    return (w ^ (w >> 9)) & 511


def mult(a):
    return (((a >> 2) * 2654435761) >> 23) & 511


def mult_low(a):
    """The low seven bits as they are (a texel's four words stay neighbours), the rest hashed."""
    w = a >> 2
    return (w & 127) | (((((w >> 7) * 2654435761) >> 30) & 3) << 7)


class Design:
    def __init__(self, name, sets, ways, hash1, hash2=None, spill=0):
        self.name, self.sets, self.ways, self.hash1, self.hash2, self.spill = name, sets, ways, hash1, hash2, spill
        self.stalls = self.frames = self.second = 0
        self.reset()

    def reset(self):
        self.fill = collections.Counter()
        self.held = set()
        self.spilled = 0

    def put(self, a):
        """False when the word finds no place."""
        if a in self.held:
            return True
        s = self.hash1(a) % self.sets
        if self.fill[s] < self.ways:
            self.fill[s] += 1
        elif self.hash2 is not None and self.fill[self.sets + self.hash2(a) % self.sets] < self.ways:
            self.fill[self.sets + self.hash2(a) % self.sets] += 1
            self.second += 1
        elif self.spilled < self.spill:
            self.spilled += 1
            self.second += 1
        else:
            return False
        self.held.add(a)
        return True


def run(design, stream):
    """The frames a design would make of the stream; returns its instructions-per-frame list."""
    made, ran = [], 0
    design.reset()
    for instructions, stall, words in stream:
        # the frame's words arrive spread evenly over its instructions
        n = len(words)
        done = 0
        for i, a in enumerate(words):
            here = instructions * (i + 1) // n - done
            if ran + here > BUDGET:
                made.append(ran)
                ran = 0
                design.reset()
            if not design.put(int(a)):
                design.stalls += 1
                made.append(ran)
                ran = 0
                design.reset()
                design.put(int(a))
            ran += here
            done += here
        ran += instructions - done
        if stall != L1_STALL and stall != 0:
            made.append(ran)   # the machine's frame ended for a reason no cache changes
            ran = 0
            design.reset()
        elif ran >= BUDGET:
            made.append(ran)
            ran = 0
            design.reset()
    design.frames = len(made)
    return made


def main():
    args = sys.argv[1:]
    tail = 0
    if '--budget' in args:
        global BUDGET
        i = args.index('--budget')
        BUDGET = int(args[i + 1])
        del args[i:i + 2]
    if '--tail' in args:
        i = args.index('--tail')
        tail = int(args[i + 1])
        del args[i:i + 2]
    stream = frames(args[0], tail)
    total = sum(f[0] for f in stream)
    full = [f for f in stream if f[1] == L1_STALL]
    print('%d frames, %.1fM instructions; %d ended on a full set (%.1f%% of instructions, %d a frame, %d words held)' % (
        len(stream), total / 1e6, len(full), 100.0 * sum(f[0] for f in full) / max(total, 1),
        sum(f[0] for f in full) // max(len(full), 1), sum(len(f[2]) for f in full) // max(len(full), 1)))

    # how full the cache was when a set overflowed, and how the words were spread
    held = np.array([len(f[2]) for f in full])
    if len(held):
        print('words in the cache (of 2,048) at a stall: 10%% %d, median %d, 90%% %d' % tuple(np.percentile(held, [10, 50, 90])))
        used = np.array([len(set(now(int(a)) for a in f[2])) for f in full])
        print('sets in use (of 512) at a stall: 10%% %d, median %d, 90%% %d' % tuple(np.percentile(used, [10, 50, 90])))
        # the stride between words that share the fullest set
        strides, regions = collections.Counter(), collections.Counter()
        for _, _, words in full:
            by = collections.defaultdict(list)
            for a in words:
                by[now(int(a))].append(int(a))
            worst = max(by.values(), key=len)
            worst.sort()
            for x, y in zip(worst, worst[1:]):
                strides[y - x] += 1
            regions[worst[0] >> 20] += 1
        print('distance between neighbours in the fullest set: ' + ', '.join(
            '%s x%d' % (('%d KB' % (d >> 10)) if d >= 1024 else '%d B' % d, n) for d, n in strides.most_common(8)))
        print('where (MiB of RAM): ' + ', '.join('%d x%d' % (r, n) for r, n in regions.most_common(8)))

    designs = [
        Design('upstream: 512 sets x 4, bits 2-8 + 11-12', 512, 4, now),
        Design('512 x 4, bits 2-10', 512, 4, low(9)),
        Design('the shader: 512 x 4, bits 2-10 xor 11-19', 512, 4, fold),
        Design('512 x 4, multiplicative', 512, 4, mult),
        Design('512 x 4, low 7 bits + hashed rest', 512, 4, mult_low),
        Design('1024 x 2, bits 2-11 (one texel a lookup)', 1024, 2, low(10)),
        Design('1024 x 2, xor fold', 1024, 2, lambda a: ((a >> 2) ^ (a >> 12)) & 1023),
        Design('256 x 4 + second place 256 x 4 (fold)', 256, 4, low(8), lambda a: fold(a) & 255),
        Design('512 x 2 + second place 512 x 2 (fold)', 512, 2, low(9), fold),
        Design('upstream + 16 spilled words', 512, 4, now, spill=16),
        Design('upstream + 64 spilled words', 512, 4, now, spill=64),
        Design('bits 2-10 xor 11-19, + 16 spilled', 512, 4, fold, spill=16),
        Design('no limit but 2,048 words', 1, 2048, lambda a: 0),
    ]
    print()
    print('%-44s %8s %8s %10s %9s' % ('design', 'frames', 'stalls', 'instr/frame', '2nd place'))
    for d in designs:
        made = run(d, stream)
        print('%-44s %8d %8d %10d %8.1f%%' % (d.name, len(made), d.stalls, total // max(len(made), 1),
                                              100.0 * d.second / max(sum(len(f[2]) for f in stream), 1)))


main()
