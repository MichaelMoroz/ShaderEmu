#!/usr/bin/env python3
"""The same test programs on the lean tick, in the kernel's place at 0x82000000: python lean_on.py PROG[,PROG] [--frames 1500]
ns an instruction of whole frames (the commit and the devices in it, which the fresh bench has not), and the registers after a run are not checked here."""
import argparse, os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
SNAP = os.environ.get('AGENTS_ISA', r'C:\Users\micha\agents_isa')
sys.path.insert(0, HERE)
import progs
ap = argparse.ArgumentParser(); ap.add_argument('progs'); ap.add_argument('--frames', default='1500'); a = ap.parse_args()
real_lui = progs.lui
progs.lui = lambda d, im: real_lui(d, 0x82000 + im)   # data where lean has RAM
d = os.path.join(ROOT, 'logs', 'fresh_lean_img'); os.makedirs(d, exist_ok=True)
tree = open(os.path.join(SNAP, 'img', 'dts.bin'), 'rb').read()
for name in a.progs.split(','):
    prog = getattr(progs, name)()
    at, ta = 0x2000000, 0x3e00000
    open(os.path.join(d, 'linux_payload.bin'), 'wb').write(bytes(at) + prog + bytes(ta - at - len(prog)) + tree)
    open(os.path.join(d, 'rootfs.bin'), 'wb').write(b'-rom1fs-' + bytes(65536 - 8))
    open(os.path.join(d, 'dts.bin'), 'wb').write(tree)
    cmd = ['pwsh', '-NoProfile', '-File', os.path.join(ROOT, 'tools', 'gpu_queue.ps1'), os.path.join(ROOT, 'bin', 'rvc_harness.exe'), '--d3d11', '--rvc', os.path.join('experiments', 'rvc_lean'), '--define', 'NO_PAGING',
           '--image', d, '--no-viz', '--no-stdin', '--frames', a.frames, '--stats-after', '1']
    for _ in range(400):
        o = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace'); o = o.stdout + o.stderr
        if 'refused' not in o: break
    m = re.findall(r'^STATS seconds=([\d.]+) instructions=(\d+) ips=\d+ frames=(\d+) fps=[\d.]+ per_frame=([\d.]+)', o, re.M)
    if m:
        s, i, f, pf = m[-1]
        print(f'{name} lean ns={1e9 * float(s) / int(i):.1f} per_frame={pf}', flush=True)
    else:
        print(name, 'no stats:', o[-400:], flush=True)
