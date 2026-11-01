#!/usr/bin/env python3
"""A program's hot RISC-V code as HLSL inside the tick: python aot.py SNAP PCSDIR OUT.hlsl [--words N] [--min-run K] [--stats]
    SNAP      a snapshot in whose RAM the code lies (a word that differs from the profile's is left out)
    PCSDIR    an RVC_PROF run's pcs.txt
    --words   the hottest N words of code are translated
A fragment is a stretch of words that ends at a memory op, a control op or a word not translated. The tick's loop dispatches
on the fragment's number; a memory op is made by one shared path after the switch, so a fragment's code holds no load."""
import argparse, collections, os, struct, sys
ap = argparse.ArgumentParser()
ap.add_argument('snap'); ap.add_argument('pcs'); ap.add_argument('out')
ap.add_argument('--words', type=int, default=4000); ap.add_argument('--maxlen', type=int, default=24)
ap.add_argument('--map-rows', type=int, default=2048); ap.add_argument('--core0', action='store_true', help='only core 0 enters')
ap.add_argument('--inline', type=int, default=0, help='the memory ops of the N hottest words of memory made in the fragment, which runs on'); ap.add_argument('--dispatch', default='switch'); ap.add_argument('--rev', action='store_true'); ap.add_argument('--enter', type=int, default=0, help='only the hottest N words are ever entered: the rest is code never run'); ap.add_argument('--kernel-only', action='store_true', help='only the kernel: safe under any program')
a = ap.parse_args()
HDR, W = 32, 2048
def sx(v, b): return (v & ((1 << b) - 1)) - ((v >> (b - 1) & 1) << b)
snap = open(a.snap, 'rb')
def word(pc):
    snap.seek(HDR + 64 * W * 16 + (pc & 0x7fffffff))
    return struct.unpack('<I', snap.read(4))[0]
cnt, wds = collections.Counter(), {}
for l in open(os.path.join(a.pcs, 'pcs.txt')):
    if l[0] == '#': continue
    f = l.split()
    pc, w, n = int(f[2], 16), int(f[3], 16), int(f[4])
    if pc < 0x82000000 or pc & 3 or (a.kernel_only and pc >= 0x82800000): continue
    # (several programs may have lain at one address: the word in the snapshot decides)
    if pc not in wds:
        wds[pc] = word(pc)
    if wds[pc] == w: cnt[pc] += n
total = sum(cnt.values()); all_n = total
hot = [pc for pc, _ in cnt.most_common(a.words)]
H = set(hot)
covered = sum(cnt[p] for p in hot)

def kind(w):
    o, f3, f7 = w & 0x7f, (w >> 12) & 7, w >> 25
    if o == 0x13:
        if f3 == 1 and f7 != 0: return None
        if f3 == 5 and f7 not in (0, 0x20): return None
        return 'alui'
    if o == 0x33:
        if f7 == 0 or (f7 == 0x20 and f3 in (0, 5)): return 'alur'
        if f7 == 1 and f3 == 0: return 'mul'
        return None
    if o == 0x37: return 'lui'
    if o == 0x17: return 'auipc'
    if o == 0x03: return 'load' if f3 in (0, 1, 2, 4, 5) else None
    if o == 0x23: return 'store' if f3 < 3 else None
    if o == 0x07: return 'flw' if f3 == 2 else None
    if o == 0x27: return 'fsw' if f3 == 2 else None
    if o == 0x63: return 'branch' if (f3 & 6) != 2 else None
    if o == 0x6f: return 'jal'
    if o == 0x67: return 'jalr' if f3 == 0 else None
    return None
ctl = {'branch', 'jal', 'jalr'}; mem = {'load', 'store', 'flw', 'fsw'}
def imm_b(w): return sx(((w >> 31) << 12) | ((w & 0x80) << 4) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e), 13)
def imm_j(w): return sx(((w >> 31) << 20) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe), 21)
def imm_s(w): return sx(((w >> 25) << 5) | ((w >> 7) & 31), 12)
T = {pc for pc in H if kind(wds[pc])}
# heads: where control comes from elsewhere: a direct target, the word after a call or a memory op or a control op, the word after one not translated
heads = set()
targets = set()
for pc in T:
    w = wds[pc]; k = kind(w)
    if k == 'branch': heads.add((pc + imm_b(w)) & 0xffffffff); heads.add(pc + 4)
    if k == 'jal': heads.add((pc + imm_j(w)) & 0xffffffff); heads.add(pc + 4)
    if k == 'jalr': heads.add(pc + 4)
    if k in mem: heads.add(pc + 4)
for pc in T:
    if pc - 4 not in T: heads.add(pc)
heads &= T
# a function's first word, reached by a jalr of what the profile cannot name, and a return point: every word that follows a non-sequential entry is already a head.
INL = set([p for p in hot if kind(wds[p]) in mem and p in T][:a.inline])
frag_of = {}
frags = []
order = sorted(heads, key=lambda p: -cnt[p])
# a tree of tests on the fragment's number: the driver makes a switch a row of compares (gzip at 3,193 cases: 16 ms a pass), so the numbers are a Huffman tree's leaves in order
import heapq
tot = sum(cnt[p] for p in order) or 1
heap = [(max(cnt[p], tot >> 22), i, p) for i, p in enumerate(order)]
heapq.heapify(heap); k = len(order)
while len(heap) > 1:
    x = heapq.heappop(heap); y = heapq.heappop(heap)
    heapq.heappush(heap, (x[0] + y[0], k, (x, y))); k += 1
root = heap[0] if heap else None
def number(node, depth):
    if isinstance(node[2], tuple):
        number(node[2][0], depth + 1); number(node[2][1], depth + 1)
    else:
        frag_of[node[2]] = len(frags) + 2; frags.append(node[2]); depths[node[2]] = depth
depths = {}
if root: number(root, 0)
ENTER = set(hot[:a.enter]) if a.enter else None
def nid(pc):
    pc &= 0xffffffff
    return 0 if ENTER is not None and pc not in ENTER else frag_of.get(pc, 0)
def u(v): return '0x%08xu' % (v & 0xffffffff)
out = []
stat = collections.Counter()
def gen(head):
    pc = head; st = []; r = {}; n = 0; dirty = set(); tn = [0]
    def rd(i):
        if i == 0: return '0u'
        if i in r: return r[i]
        return f'xr[{i}]'
    def wr(i, ex):
        if i == 0: return
        tn[0] += 1; name = f't{tn[0]}'
        st.append(f'uint {name} = {ex};')
        r[i] = name; dirty.add(i)
    tail = None
    while True:
        if pc not in T:
            tail = f'pc = {u(pc)}; a = 0;'; stat['exit'] += 1; break
        if pc != head and pc in heads and pc in frag_of and n >= a.maxlen:
            tail = f'pc = {u(pc)}; a = {nid(pc)};'; break
        w = wds[pc]; k = kind(w); f3 = (w >> 12) & 7; d = (w >> 7) & 31; x, y = rd((w >> 15) & 31), rd((w >> 20) & 31); im = sx(w >> 20, 12)
        if k in ('alui', 'alur'):
            reg = k == 'alur'; alt = (w >> 30) & 1
            Y = y if reg else u(im); sh = y if reg else str((w >> 20) & 31) + 'u'
            ex = {0: f'{x} - {Y}' if reg and alt else f'{x} + {Y}', 1: f'{x} << {sh}', 2: f'asint({x}) < asint({Y}) ? 1u : 0u', 3: f'{x} < {Y} ? 1u : 0u', 4: f'{x} ^ {Y}',
                  5: f'asuint(asint({x}) >> {sh})' if alt else f'{x} >> {sh}', 6: f'{x} | {Y}', 7: f'{x} & {Y}'}[f3]
            wr(d, ex); n += 1; pc += 4
        elif k == 'mul': wr(d, f'{x} * {y}'); n += 1; pc += 4
        elif k == 'lui': wr(d, u(w & 0xfffff000)); n += 1; pc += 4
        elif k == 'auipc': wr(d, u((w & 0xfffff000) + pc)); n += 1; pc += 4
        elif k in ('load', 'store') and pc in INL and pc + 4 in T:
            tn[0] += 1; ad = f't{tn[0]}'
            for i in sorted(dirty): st.append(f'xr[{i}] = {r[i]};')
            dirty.clear()
            if n: st.append(f'n += {n};'); stat['words'] += n; n = 0
            st.append(f'uint {ad} = {x} + {u(im if k == "load" else imm_s(w))}; pc = {u(pc)};')
            if k == 'load':
                sz = 1 << (f3 & 3); up = 24 - 8 * (f3 & 3) - (8 if (f3 & 3) == 2 else 0)
                st.append(f'[branch] if (!({ad} != 0 && ({ad} & 3) + {sz} <= 4 && ({ad} ^ 0x80000000) < RAM_MAX)) {{ a = 0; break; }}')
                tn[0] += 1; v = f't{tn[0]}'
                st.append(f'uint {v} = ram_word(c, {ad} & 0x7ffffffc) >> (({ad} & 3) * 8) << {up};')
                wr(d, f'{v} >> {up}' if f3 & 4 else f'asuint(asint({v}) >> {up})')
            else:
                sz = 1 << f3; mk = {0: 0xff, 1: 0xffff, 2: 0xffffffff}[f3]
                st.append(f'[branch] if (!(({ad} & 0x80000000) != 0 && ({ad} & 3) + {sz} <= 4)) {{ a = 0; break; }}')
                st.append(f'ram_store(c, {ad} & 0x7ffffffc, {y} << (({ad} & 3) * 8), {u(mk)} << (({ad} & 3) * 8));')
                st.append(f'[branch] if (budget == 0) {{ n++; pc = {u(pc + 4)}; a = {nid(pc + 4)}; break; }}')
            n += 1; pc += 4; stat['inlined'] += 1
        elif k == 'load' or k == 'flw':
            dst = (32 + d) if k == 'flw' else (d if d else 64)
            tail = f'ma = {x} + {u(im)}; mi = {16 | f3}; md = {dst}; nx = {nid(pc + 4)}; pc = {u(pc)};'; stat['load'] += 1; break
        elif k == 'store' or k == 'fsw':
            val = f'xr[{32 + ((w >> 20) & 31)}]' if k == 'fsw' else y
            tail = f'ma = {x} + {u(imm_s(w))}; mi = {8 | f3}; md = {val}; nx = {nid(pc + 4)}; pc = {u(pc)};'; stat['store'] += 1; break
        elif k == 'branch':
            xv, yv = x, y
            c = {0: f'{xv} == {yv}', 1: f'{xv} != {yv}', 4: f'asint({xv}) < asint({yv})', 5: f'asint({xv}) >= asint({yv})', 6: f'{xv} < {yv}', 7: f'{xv} >= {yv}'}[f3]
            tp = (pc + imm_b(w)) & 0xffffffff; fp = pc + 4
            n += 1
            tail = f'bool tk = {c}; pc = tk ? {u(tp)} : {u(fp)}; a = tk ? {nid(tp)} : {nid(fp)};'; stat['branch'] += 1; break
        elif k == 'jal':
            wr(d, u(pc + 4)); n += 1; tp = (pc + imm_j(w)) & 0xffffffff
            tail = f'pc = {u(tp)}; a = {nid(tp)};'; stat['jal'] += 1; break
        elif k == 'jalr':
            nxt = f'({x} + {u(im)})'
            tn[0] += 1; st.append(f'uint t{tn[0]} = {nxt};')
            wr(d, u(pc + 4)); n += 1
            tail = f'pc = t{tn[0] - (1 if d else 0)}; a = 1;'; stat['jalr'] += 1; break
        if pc in heads and n >= 1:
            # the next word is entered from elsewhere too: the fragment can run on through it (a head is only a place to come in)
            if n >= a.maxlen:
                tail = f'pc = {u(pc)}; a = {nid(pc)};'; break
    for i in sorted(dirty): st.append(f'xr[{i}] = {r[i]};')
    if n: st.append(f'n += {n};')
    st.append(tail)
    stat['words'] += n
    return st
def emit(node):
    if isinstance(node[2], tuple):
        l, r = node[2]
        first = lowest(r)
        return f'[branch] if (a < {first}) {{ ' + emit(l) + ' } else { ' + emit(r) + ' }'
    return chr(10) + ' '.join(gen(node[2])) + chr(10)
def lowest(node):
    while isinstance(node[2], tuple): node = node[2][0]
    return frag_of[node[2]]
sys.setrecursionlimit(100000)
if a.dispatch == 'tree':
    out = emit(root).split(chr(10)) if root else ['']
else:
    frs = sorted(order, key=lambda p: cnt[p], reverse=not a.rev)
    out = ['[forcecase] switch (a) {'] + [f'case {frag_of[p]}: {{ ' + ' '.join(gen(p)) + ' break; }' for p in frs] + ['default: a = 0; break; }']
wd = sum(cnt[p] * depths[p] for p in order) / tot
# the map: row = hash, two ways (pc, fragment); the hottest first, a third of a row's lost to the words that need no map as well
R = a.map_rows
rows = [[0, 0, 0, 0] for _ in range(R)]
def hsh(pc): return ((pc >> 2) * 0x9e3779b1 & 0xffffffff) >> (32 - (R.bit_length() - 1))
lost = 0
for pc in order:
    if ENTER is not None and pc not in ENTER: continue
    x = rows[hsh(pc)]
    if x[1] == 0: x[0], x[1] = pc, frag_of[pc]
    elif x[3] == 0: x[2], x[3] = pc, frag_of[pc]
    else: lost += 1
with open(a.out, 'w', newline='\n') as f:
    f.write(f'// made by experiments/rv_why/aot.py: {len(frags)} fragments of the hottest {len(hot)} words, {100 * covered / all_n:.1f}% of the profile\n')
    f.write(f'#define AOT_BITS {R.bit_length() - 1}\nstatic const uint4 AOT_MAP[{R}] = {{\n')
    for x in rows: f.write('uint4(%#x,%d,%#x,%d),' % tuple(x) + '\n')
    f.write('};\n#define AOT_CASES \\\n')
    for l in out: f.write(l + ' \\\n')
    f.write('\n')
print(f'{all_n/1e6:.1f} M instructions in the profile, {len(wds)} words; hottest {len(hot)}: {100*covered/all_n:.1f}%; translated {len(T)} words ({100*sum(cnt[p] for p in T)/all_n:.1f}%); {len(frags)} fragments, map lost {lost}; depth mean {wd:.1f} max {max(depths.values())}', dict(stat))
