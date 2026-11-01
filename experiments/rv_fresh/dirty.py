#!/usr/bin/env python3
"""The texels a stretch of real code writes, by the model, for fresh12: python dirty.py SNAP OUT.bin"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import snapsim as S
sn = S.Snap(sys.argv[1]); x, pc = S.regs(sn); r, *_ = S.run(sn, x, pc, 300000)
ts = sorted({(a & 0x7fffffff) >> 4 for a in sn.writes})
open(sys.argv[2], 'wb').write(b''.join(struct.pack('<I', t) for t in ts))
print('dirty texels', len(ts), 'ran', r['done'], r['stop'])
