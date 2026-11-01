#!/usr/bin/env python3
"""The raw PC samples of an Nsight GPU Trace, with no UI: ngfx-trace's `extract` of gpus.0.SamplingData.PcSamplingPerSM.N.pcSamplesStorage (16 bytes a sample: u64 pc, u32 a, u32 b).
    python pc_samples.py TRACE OUT.npy [--sms 36]   writes the samples within large shaders (over 100 KB of code) of all SMs: columns pc, a, b, sm; how many each SM had in all goes to stdout."""
import argparse, json, os, subprocess, sys, tempfile
import numpy as np
NGFX = os.path.expandvars(r'%USERPROFILE%\.cargo\bin\ngfx-trace.exe')
ap = argparse.ArgumentParser(); ap.add_argument('trace'); ap.add_argument('out'); ap.add_argument('--sms', type=int, default=36); a = ap.parse_args()
q = subprocess.run([NGFX, 'query', a.trace, 'devices.0.ShaderProfilerReport.pcCountersInfo.pcSamplingSession.codeBlocks', '--compact', '--limit', '64'], capture_output=True, text=True)
blocks = [b['shaderInstance']['vaRange'] for b in json.loads(q.stdout)]
big = [(int(b['address']), int(b['address']) + int(b['size'])) for b in blocks if int(b['size']) > 3000]
rows = []
with tempfile.TemporaryDirectory() as d:
    for i in range(a.sms):
        f = os.path.join(d, f's{i}.bin')
        p = subprocess.run([NGFX, 'extract', a.trace, f'gpus.0.SamplingData.PcSamplingPerSM.{i}.pcSamplesStorage', f], capture_output=True, text=True)
        if p.returncode: print('sm', i, p.stderr[:120].strip()); break
        r = np.fromfile(f, dtype=np.dtype([('pc', '<u8'), ('a', '<u4'), ('b', '<u4')]))
        os.remove(f)
        m = np.zeros(len(r), bool)
        for lo, hi in big: m |= (r['pc'] >= lo) & (r['pc'] < hi)
        r = r[m]
        rows.append(np.stack([r['pc'], r['a'].astype(np.uint64), r['b'].astype(np.uint64), np.full(len(r), i, np.uint64)], 1))
        print('sm', i, len(m), 'in the tick', int(m.sum()), flush=True)
x = np.concatenate(rows)
np.save(a.out, x)
print(x.shape)

