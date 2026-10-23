#!/usr/bin/env python3
"""Core 0's stores from gvm_cpu's GVM_STLOG (gpu_vm/memory_model.md): what kind of memory a pass's distinct stored
texels are, and how many passes x23's write cache would end full, as it is and with the stack kept out of it.

    python gpu_vm/cpu/stlog.py FILE [--globals LO,HI] [--window DOWN,UP] [--regions]

Made by `GVM_STLOG=FILE gvm_cpu --profile P --profile-at TEXT ...` (GVM_STLOG_MAX records, GVM_STLOG_LOADS=1 loads too).

A record is five words: the address, sp, core 0's bundles run, bytes | base register << 8 | supervisor << 16 |
mode << 20, the pc; address 0xffffffff is the end of a pass (why in the last word: 0 budget, 1 sync, 2 wait, 3 full).
"""
import argparse, array, collections, sys

NB = 32
BUDGET = 8192
KINDS = ('kernel stack', 'kernel other', 'user stack', 'user globals', 'user heap/other', 'devices')


def b1(t): return (t ^ (t >> 5) ^ (t >> 10)) & (NB - 1)
def b2(t): return NB + (((t >> 3) ^ (t << 2) ^ (t >> 5)) & (NB - 1))


class X23:
    """x23's write cache: two tables of NB buckets of four ways; a texel takes the first way of its two buckets that
    is its own or empty, else the pass ends (it goes to an overflow slot)."""
    def __init__(self, nb=NB, ways=4):
        self.nb, self.ways = nb, ways
        self.reset()

    def reset(self):
        self.b = [[] for _ in range(2 * self.nb)]
        self.n = 0

    def put(self, t):   # False: no room
        nb = self.nb
        for k in ((t ^ (t >> 5) ^ (t >> 10)) % nb, nb + (((t >> 3) ^ (t << 2) ^ (t >> 5)) % nb)):
            q = self.b[k]
            if t in q: return True
            if len(q) < self.ways:
                q.append(t); self.n += 1
                return True
        return False


def load(path):
    a = array.array('I')
    with open(path, 'rb') as f: a.frombytes(f.read())
    n = len(a) // 5
    return a, n


def kind(a, sp, info, glo):
    sup = (info >> 16) & 1
    reg = (info >> 8) & 255
    if a < 0x02000000: return 5
    near = reg in (2, 32) or (sp - 64 <= a < sp + 0x8000)
    if sup: return 0 if near else 1
    if near: return 2
    if glo and glo[0] <= a < glo[1]: return 3
    return 4


def run(a, n, glo, scratch=None, cache=lambda: X23()):
    """Simulated passes: (ended full, sync/wait, budget), bundles a pass, distinct texels by kind a pass.
    scratch: (down, up) texels about sp at the pass's start that a stack window holds instead of the cache."""
    wc = cache()
    seen = set()
    by_kind = [0] * len(KINDS)
    ends = collections.Counter()
    start = None
    sp0 = None
    passes_bundles = []
    in_scratch = 0
    def end(why, b):
        nonlocal start, sp0, seen
        ends[why] += 1
        passes_bundles.append(b - start)
        wc.reset(); seen = set(); start = b; sp0 = None
    for i in range(n):
        ad, sp, b, info = a[5 * i], a[5 * i + 1], a[5 * i + 2], a[5 * i + 3]
        if start is None: start = b
        while b - start >= BUDGET: end('budget', start + BUDGET)
        if ad == 0xffffffff:
            if info in (1, 2): end('sync/wait', b)
            continue
        if sp0 is None: sp0 = sp
        t = ad >> 4
        bytes_ = info & 255
        ts = (t,) if (ad & 15) + bytes_ <= 16 else (t, t + 1)
        for t in ts:
            if scratch and not ((info >> 16) & 1):
                d = (sp0 >> 4) - t
                if -scratch[1] < d <= scratch[0] and kind(ad, sp, info, glo) == 2:
                    if t not in seen: seen.add(t); in_scratch += 1
                    continue
            if not wc.put(t):
                end('full', b)
                wc.put(t)
            if t not in seen:
                seen.add(t); by_kind[kind(ad, sp, info, glo)] += 1
    np = sum(ends.values())
    return dict(passes=np, ends=ends, bundles=sum(passes_bundles) / max(1, np), kinds=[k / max(1, np) for k in by_kind],
                scratch=in_scratch / max(1, np))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--globals', help='LO,HI: the program\'s data and bss')
    ap.add_argument('--window', default='', help='DOWN,UP texels of a stack window about sp (several: ;)')
    ap.add_argument('--regions', action='store_true', help='user stores by 1 MB region and their sp offsets')
    ap.add_argument('--max', type=int, default=0, help='records at most')
    o = ap.parse_args()
    a, n = load(o.file)
    if o.max: n = min(n, o.max)
    glo = tuple(int(x, 0) for x in o.globals.split(',')) if o.globals else None
    if o.regions:
        reg = collections.Counter(); off = collections.Counter(); base = collections.Counter()
        for i in range(n):
            ad, sp, info = a[5 * i], a[5 * i + 1], a[5 * i + 3]
            if ad == 0xffffffff or (info >> 16) & 1: continue
            reg[ad >> 20] += 1; base[(info >> 8) & 255] += 1
            d = ad - sp
            off['<0' if d < 0 else '<256' if d < 256 else '<4K' if d < 4096 else '<32K' if d < 32768 else '<1M' if d < 1 << 20 else 'far'] += 1
        print('user stores by MB:', ', '.join(f'{k << 20:#010x} {v}' for k, v in reg.most_common(12)))
        print('base registers:', base.most_common(10))
        print('address - sp:', sorted(off.items()))
    r = run(a, n, glo)
    print(f'{n} records; x23 as it is: {r["passes"]} passes, {r["bundles"]:.0f} bundles a pass, ends {dict(r["ends"])}')
    print('  distinct texels a pass: ' + ', '.join(f'{k} {v:.1f}' for k, v in zip(KINDS, r['kinds'])) + f'; all {sum(r["kinds"]):.1f}')
    for w in filter(None, o.window.split(';')):
        d, u = (int(x) for x in w.split(','))
        r = run(a, n, glo, scratch=(d, u))
        print(f'stack window {d},{u}: {r["passes"]} passes, {r["bundles"]:.0f} bundles a pass, ends {dict(r["ends"])}, '
              f'{r["scratch"]:.1f} texels a pass in the window')
        print('  cache texels a pass: ' + ', '.join(f'{k} {v:.1f}' for k, v in zip(KINDS, r['kinds'])))


if __name__ == '__main__':
    main()
