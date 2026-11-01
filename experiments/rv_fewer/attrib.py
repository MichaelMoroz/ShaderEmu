#!/usr/bin/env python3
"""Instructions by the function a line was written in, through what a whole-program build inlined (addr2line -f -i).
Run in WSL:  python3 attrib.py PROF_DIR ELF BASE [CLASS] [TOP] > out.txt
Self: the innermost frame of each pc. Lines: the same frame's file:line. Inlined-in: every frame's sum (inclusive of
what is inlined into it, not of calls)."""
import collections, os, subprocess, sys
prof, elf, base = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
cls = sys.argv[4] if len(sys.argv) > 4 else '0'
top = int(sys.argv[5]) if len(sys.argv) > 5 else 70
import struct
_b = open(elf, 'rb').read()
_ph, _n = struct.unpack_from('<I', _b, 28)[0], struct.unpack_from('<H', _b, 44)[0]
_t = next(struct.unpack_from('<8I', _b, _ph + 32 * i) for i in range(_n) if struct.unpack_from('<I', _b, _ph + 32 * i)[0] == 1 and struct.unpack_from('<I', _b, _ph + 32 * i + 24)[0] & 1)
_mem = bytearray(_t[2] + _t[5] + 16); _mem[_t[2]:_t[2] + _t[4]] = _b[_t[1]:_t[1] + _t[4]]
code = struct.unpack('<%dI' % (len(_mem) // 4), bytes(_mem[:len(_mem) // 4 * 4])) + (0,) * 0x10000
cnt = collections.Counter(); total = 0
for l in open(os.path.join(prof, 'pcs.txt')):
    if l[0] == '#': continue
    f = l.split()
    if f[0] != cls: continue
    n = int(f[4]); total += n; pc = int(f[2], 16)
    # (what lies there in this run may be another program's: only a pc with the ELF's own word is the program's)
    if base <= pc < base + 0x5c0000 and code[(pc - base) // 4] == int(f[3], 16): cnt[pc - base] += n
addrs = sorted(cnt)
A2L = os.path.expanduser('/home/micha/shaderemu-linux/toolchain/bin/riscv32-linux-addr2line')
p = subprocess.run([A2L, '-f', '-i', '-C', '-a', '-e', elf], input='\n'.join('%x' % a for a in addrs), capture_output=True, text=True)
self_f = collections.Counter(); lines = collections.Counter(); incl = collections.Counter()
cur = None; frames = []
def flush():
    if cur is None or not frames: return
    n = cnt[cur]
    fn, ln = frames[0]
    self_f[fn] += n; lines[os.path.basename(ln.split(' ')[0]) + ' ' + fn] += n
    for g in set(x[0] for x in frames): incl[g] += n
t = p.stdout.split('\n'); i = 0
while i < len(t):
    s = t[i]
    if s.startswith('0x'):
        flush(); cur = int(s, 16); frames = []; i += 1; continue
    if i + 1 < len(t) and s: frames.append((s[:70], t[i + 1])); i += 2; continue
    i += 1
flush()
print(f'class {cls}: {total:,}; in the program {sum(cnt.values()):,}')
print('--- by function (innermost frame)')
for k, v in self_f.most_common(top): print(f'{100*v/total:6.2f}% {v:>11,} {k}')
print('--- with what is inlined into it')
for k, v in incl.most_common(top): print(f'{100*v/total:6.2f}% {v:>11,} {k}')
print('--- by line')
for k, v in lines.most_common(top + 40): print(f'{100*v/total:6.2f}% {v:>11,} {k}')
