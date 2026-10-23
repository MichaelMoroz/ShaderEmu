# fnmap.py GDB NM PROF OUT: a gvm_cpu profile's pcs mapped to functions by their first four bundles' op words, when the
# profiled build is not the ELF at hand (its layout differs); OUT is an nm-like list for density.py (BASE 0)
import struct, sys
gdb, nm, prof, out = sys.argv[1:5]
K = 4
b = open(gdb, 'rb').read(); T = 0x1010; TA = 0x10; N = 0x4223d0
funcs = []
for l in open(nm):
    p = l.split()
    if len(p) == 4 and p[2] in 'tT': funcs.append((int(p[0], 16), int(p[1], 16), p[3]))
def key(ws): return tuple(x for w in ws for x in (w[0], w[1]))
sig = {}
for a, sz, n in funcs:
    if sz < 16 * K or a % 16: continue
    o = T + a - TA
    ws = [struct.unpack('<4I', b[o + 16 * i:o + 16 * i + 16]) for i in range(K)]
    sig.setdefault(key(ws), []).append((n, sz))
rows = {}
for l in open(prof):
    if l.startswith('#'): continue
    p = l.split(); rows[int(p[0], 16)] = [int(x, 16) for x in p[1:5]]
starts = []
for pc in sorted(rows):
    if all(pc + 16 * i in rows for i in range(K)):
        k = key([rows[pc + 16 * i] for i in range(K)])
        if k in sig and len(sig[k]) == 1: starts.append((pc, sig[k][0][0], sig[k][0][1]))
with open(out, 'w') as f:
    for pc, n, sz in starts: f.write(f'{pc:08x} {sz:08x} t {n}\n')
print(len(starts), 'functions found')
