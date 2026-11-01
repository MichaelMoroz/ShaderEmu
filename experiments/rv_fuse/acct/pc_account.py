#!/usr/bin/env python3
"""Where a tick's time goes, instruction by instruction, with no UI: the raw PC samples of a GPU Trace (acct/pc_samples.py) against the trace's code blocks (vaRange) and its SASS.
    python pc_account.py TRACE [--sms 36] [--top 60] [--out listing.txt] [--reason]
Samples are per warp per SM, so a sample's weight is time a warp of this shader was at that instruction. b: bits 0-5 the warp, 8-13 the reason, 16 ready/waiting (read from what the card says, see RESULTS.md)."""
import argparse, collections, json, os, re, subprocess, sys
import numpy as np
NGFX = os.path.expandvars(r'%USERPROFILE%\.cargo\bin\ngfx-trace.exe')
NVDISASM = r'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\nvdisasm.exe'
ap = argparse.ArgumentParser(); ap.add_argument('trace'); ap.add_argument('--sms', type=int, default=36); ap.add_argument('--top', type=int, default=60); ap.add_argument('--out', default=''); ap.add_argument('--kp', default='', help='known plaintext DXIL for a long shader')
a = ap.parse_args()
stem = os.path.splitext(a.trace)[0]
npy = stem + '.pcs.npy'
if not os.path.exists(npy):
    subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pc_samples.py'), a.trace, npy, '--sms', str(a.sms)], check=True, stdout=subprocess.DEVNULL)
x = np.load(npy)
q = subprocess.run([NGFX, 'query', a.trace, 'devices.0.ShaderProfilerReport.pcCountersInfo.pcSamplingSession.codeBlocks', '--compact', '--limit', '64'], capture_output=True, text=True)
blocks = [b['shaderInstance'] for b in json.loads(q.stdout)]
print('samples', len(x), 'SMs', len(np.unique(x[:, 3])))
tot = []
for i, b in enumerate(blocks):
    lo, n = int(b['vaRange']['address']), int(b['vaRange']['size'])
    m = (x[:, 0] >= lo) & (x[:, 0] < lo + n)
    tot.append(int(m.sum()))
    print(f"block {i} {b['type'][19:]} regs {b['commonProgramMeta']['registersPerThread']} va {lo:#x} size {n} samples {tot[-1]} instr-index {b['instructions']['idx']}")
outside = len(x) - sum(tot)
print('samples outside the blocks', outside)
i = int(np.argmax(tot))
b = blocks[i]
lo, n = int(b['vaRange']['address']), int(b['vaRange']['size'])
m = (x[:, 0] >= lo) & (x[:, 0] < lo + n)
s = x[m]
sass = f'{stem}.b{i}.sass'
if not os.path.exists(sass):
    # the shader selector counts the same shaders in the same order
    args = [NGFX, 'sass', a.trace, str(i), '--compact', '--nvdisasm', NVDISASM, '--output', sass]
    if a.kp: args += ['--known-plaintext', a.kp]
    p = subprocess.run(args, capture_output=True, text=True)
    if p.returncode: sys.exit(p.stderr)
lines = open(sass, errors='replace').read().split('\n')
INS = re.compile(r'\s*/\*([0-9a-f]{4,})\*/\s*(.*)')
at, dxil = {}, 0
for l in lines:
    k = re.search(r'line (\d+)', l) if l.lstrip().startswith('//') else None
    if k: dxil = int(k.group(1))
    g = INS.match(l)
    if g: at[int(g.group(1), 16)] = (g.group(2).strip(), dxil)
print('hottest block', i, 'instructions', len(at), 'samples', len(s))
off = (s[:, 0] - lo).astype(np.int64)
ready = (s[:, 2] >> 16) & 1
reason = (s[:, 2] >> 8) & 0x3f
warp = s[:, 2] & 0x3f
print('warps seen', len(np.unique(warp)), collections.Counter(warp.tolist()).most_common(6))
print('reason share', [(int(k), round(100 * v / len(s), 1)) for k, v in collections.Counter(reason.tolist()).most_common(12)])
print('bit 16 share', round(100 * float(ready.mean()), 1))
c = collections.Counter(off.tolist())
inst = sorted(at)
ops = collections.Counter()
rs = collections.defaultdict(collections.Counter)
for o, r in zip(off.tolist(), reason.tolist()): rs[o][r] += 1
for o, v in c.items():
    if o in at:
        ops[re.sub(r'^@!?\w+\s+', '', at[o][0]).split()[0].split('.')[0]] += v
print('by opcode', [(k, round(100 * v / len(s), 2)) for k, v in ops.most_common(30)])
print('top instructions (share of the shader):')
for o, v in c.most_common(a.top):
    t = at.get(o, ('?', 0))
    print(f'{100 * v / len(s):6.2f}% {o:#7x} L{t[1]:<6} {rs[o].most_common(2)} {t[0][:90]}')
if a.out:
    with open(a.out, 'w') as f:
        for o in inst:
            f.write(f'{100 * c.get(o, 0) / len(s):7.3f} {o:#7x} L{at[o][1]:<6} {rs[o].most_common(2) if o in rs else ""} {at[o][0]}\n')
