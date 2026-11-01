#!/usr/bin/env python3
"""The tick's blob in a shader cache folder: the largest with eight outputs.   python pick_tick.py FOLDER"""
import glob, os, struct, sys
best = None
for f in glob.glob(os.path.join(sys.argv[1], '*')):
    b = open(f, 'rb').read()
    if b[:4] != b'DXBC': continue
    for i in range(struct.unpack('<I', b[28:32])[0]):
        o = struct.unpack('<I', b[32 + 4 * i:36 + 4 * i])[0]
        if b[o:o + 4] in (b'OSGN', b'OSG5', b'OSG1') and struct.unpack('<I', b[o + 8:o + 12])[0] == 8 and len(b) > 100000:
            if not best or len(b) > best[0]: best = (len(b), f)
print(best[1])
