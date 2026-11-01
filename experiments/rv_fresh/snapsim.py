#!/usr/bin/env python3
"""Real code on the model, from a harness snapshot's RAM and core 0's registers, to the first instruction or address the fresh tick lacks:
    python snapsim.py SNAPSHOT [N] [--no-m] [--out REGS_AND_WRITES.json]
Says what it ran by kind, how long the runs in the hot loop are (arithmetic in a row, what the fresh tick does without leaving it), and why it stopped.
A snapshot: a 32-byte header, then 2048-wide texels of 16 bytes; RAM and the devices from row 64 (texel = (address & 0x7fffffff) >> 4); core 0's texel lin at (lin % 64, lin // 64),
x1 to x31 and the pc in texels 29 to 36 (experiments/rvc_lean/tick.hlsl, state_out)."""
import argparse, collections, json, mmap, struct, sys
M = 0xffffffff
def sx(v, b): return (v & ((1 << b) - 1)) - ((v >> (b - 1) & 1) << b)
HDR, W = 32, 2048
class Snap:
    def __init__(s, path):
        s.f = open(path, 'rb'); s.m = mmap.mmap(s.f.fileno(), 0, access=mmap.ACCESS_READ); s.writes = {}
        s.w, s.h = struct.unpack_from('<II', s.m, 8)
        s.size = len(s.m)
    def off(s, a): return HDR + (64 * W) * 16 + (a & 0x7fffffff)
    def r8(s, a):
        if a in s.writes: return s.writes[a]
        o = s.off(a)
        return s.m[o] if o < s.size else 0
    def r32(s, a):
        if a in s.writes or a + 1 in s.writes or a + 2 in s.writes or a + 3 in s.writes or (a & 3):
            return s.r8(a) | s.r8(a + 1) << 8 | s.r8(a + 2) << 16 | s.r8(a + 3) << 24
        o = s.off(a)
        return struct.unpack_from('<I', s.m, o)[0] if o + 4 <= s.size else 0
    def w8(s, a, v): s.writes[a] = v & 255
    def state(s, lin): return struct.unpack_from('<4I', s.m, HDR + lin * 16)
def regs(sn):
    x = [0] * 32
    for lin in range(29, 37):
        t = sn.state(lin); k = 4 * (lin - 29)
        x[k + 1], x[k + 2], x[k + 3] = t[0], t[1], t[2]
        if lin == 36: pc = t[3]
        else: x[k + 4] = t[3]
    return x, pc
RAM_LO = 0x82000000; RAM_HI = 0x92000000
def run(sn, x, pc, n):
    kinds = collections.Counter(); runs = collections.Counter(); cur = 0; done = 0; stop = None; sub = collections.Counter()
    res = None
    for done in range(n):
        if not RAM_LO <= pc < RAM_HI: stop = 'pc outside RAM %08x' % pc; break
        w = sn.r32(pc); op = w & 0x7f; f3 = w >> 12 & 7; rd = w >> 7 & 31; a = x[w >> 15 & 31]; b = x[w >> 20 & 31]; nx = (pc + 4) & M; v = None; k = None; f7 = w >> 25
        if op == 0x13 or (op == 0x33 and f7 != 1):
            y = b if op == 0x33 else sx(w >> 20, 12) & M; s = y & 31; alt = w >> 30 & 1
            sa, sy = sx(a, 32), sx(y, 32)
            v = [((a - y) if (op == 0x33 and alt) else (a + y)), a << s, sa < sy, a < y, a ^ y, (sa >> s) if alt else (a >> s), a | y, a & y][f3]; k = 'alu'
        elif op == 0x33:
            sa, sb = sx(a, 32), sx(b, 32); k = 'muldiv'
            if f3 == 0: v = a * b
            elif f3 == 1: v = (sa * sb) >> 32
            elif f3 == 2: v = (sa * b) >> 32
            elif f3 == 3: v = (a * b) >> 32
            elif f3 == 4: v = M if b == 0 else (a if (sa == -2**31 and sb == -1) else int(abs(sa) // abs(sb)) * (1 if (sa < 0) == (sb < 0) else -1))
            elif f3 == 5: v = M if b == 0 else a // b
            elif f3 == 6: v = a if b == 0 else (0 if (sa == -2**31 and sb == -1) else (abs(sa) % abs(sb)) * (-1 if sa < 0 else 1))
            else: v = a if b == 0 else a % b
        elif op == 0x37: v = w & 0xfffff000; k = 'lui'
        elif op == 0x17: v = pc + (w & 0xfffff000); k = 'auipc'
        elif op == 0x6f: v = pc + 4; nx = (pc + sx((w >> 31 << 20) | (w & 0xff000) | ((w >> 20 & 1) << 11) | (w >> 20 & 0x7fe), 21)) & M; k = 'jal'
        elif op == 0x67: v = pc + 4; nx = (a + sx(w >> 20, 12)) & M & ~1; k = 'jalr'
        elif op == 0x63:
            t = {0: a == b, 1: a != b, 4: sx(a, 32) < sx(b, 32), 5: sx(a, 32) >= sx(b, 32), 6: a < b, 7: a >= b}.get(f3)
            if t is None: stop = 'bad branch %08x' % w; break
            if t: nx = (pc + sx((w >> 31 << 12) | ((w >> 7 & 1) << 11) | (w >> 20 & 0x7e0) | (w >> 7 & 0x1e), 13)) & M
            k = 'branch'
        elif op == 0x03:
            at = (a + sx(w >> 20, 12)) & M; k = 'load'
            if not RAM_LO <= at < RAM_HI: stop = 'load outside RAM %08x at pc %08x' % (at, pc); break
            size = 1 << (f3 & 3)
            if (at & 3) + size > 4: sub['load across a word'] += 1
            if f3 == 0: v = sx(sn.r8(at), 8) & M
            elif f3 == 1: v = sx(sn.r8(at) | sn.r8(at + 1) << 8, 16) & M
            elif f3 == 2: v = sn.r32(at)
            elif f3 == 4: v = sn.r8(at)
            elif f3 == 5: v = sn.r8(at) | sn.r8(at + 1) << 8
            else: stop = 'bad load %08x' % w; break
        elif op == 0x23:
            at = (a + sx((w >> 25 << 5) | (w >> 7 & 31), 12)) & M; k = 'store'
            if not RAM_LO <= at < RAM_HI: stop = 'store outside RAM %08x at pc %08x' % (at, pc); break
            size = 1 << (f3 & 3)
            if (at & 3) + size > 4: sub['store across a word'] += 1
            for i in range(size): sn.w8(at + i, b >> (8 * i))
        elif op == 0x2f and f3 == 2:
            f5 = w >> 27; k = 'atomic'
            if not RAM_LO <= a < RAM_HI: stop = 'atomic outside RAM'; break
            if f5 == 2: v = sn.r32(a); res = a
            elif f5 == 3:
                ok = res == a; v = 0 if ok else 1; res = None
                if ok:
                    for i in range(4): sn.w8(a + i, b >> (8 * i))
            else:
                old = sn.r32(a)
                nv = {0: old + b, 1: b, 4: old ^ b, 8: old | b, 12: old & b}.get(f5)
                if nv is None: stop = 'atomic %08x' % w; break
                for i in range(4): sn.w8(a + i, nv >> (8 * i))
                v = old
        elif op == 0x0f: k = 'fence'
        elif (w & 0xfffff07f) in (0xc0002073, 0xc8102073, 0xc0102073): v = 0; k = 'rdtime'
        else:
            stop = 'lacks %08x (opcode %02x) at pc %08x' % (w, op, pc); break
        kinds[k] += 1
        if k == 'alu' or k == 'lui':
            cur += 1
        else:
            runs[min(cur, 40)] += 1; cur = 0
            if k == 'branch' and nx == (pc + 4) & M: sub['branch not taken'] += 1
        if v is not None and rd: x[rd] = int(v) & M
        pc = nx
    else:
        done = n
    return dict(done=done, stop=stop, pc=pc), kinds, runs, sub
if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('snap'); ap.add_argument('n', nargs='?', type=int, default=300000); ap.add_argument('--out', default=''); a = ap.parse_args()
    sn = Snap(a.snap); x, pc = regs(sn)
    print('snapshot %dx%d, pc %08x, sp %08x, priv word %08x' % (sn.w, sn.h, pc, x[2], sn.state(8)[2]))
    r, kinds, runs, sub = run(sn, x, pc, a.n)
    print('ran', r['done'], 'stop:', r['stop'], 'pc %08x' % r['pc'])
    tot = max(1, sum(kinds.values()))
    print(' '.join('%s %.1f%%' % (k, 100 * v / tot) for k, v in kinds.most_common()))
    leaves = sum(runs.values())
    print('leaves the hot loop: every %.2f instructions (%.1f%% of them); run of arithmetic before a way out: ' % (tot / max(1, leaves), 100 * leaves / tot) +
          ' '.join('%d:%.0f%%' % (k, 100 * runs[k] / max(1, leaves)) for k in range(0, 8)))
    print(dict(sub))
    if a.out:
        json.dump(dict(x=x, pc=r['pc'], done=r['done'], writes={str(k): v for k, v in sn.writes.items()}), open(a.out, 'w'))
