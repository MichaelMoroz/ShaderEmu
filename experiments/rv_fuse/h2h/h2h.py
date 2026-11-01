#!/usr/bin/env python3
"""gpu_vm's tick and programs against ShaderEmu's on this card: the same command, a cold boot each, through the queue.
    python h2h.py MACHINE LOAD [--mode flat|world|vr] [--out results.jsonl]    MACHINE: gvm or rv
Wall seconds between the guest's own marks (work of a fixed size), each load's own figures, and the console kept.
world is --unity 11.1,1.4,R (Unity's frame with 9.7 ms for the machine), vr --unity 11.1,7.1,1.0 (4 ms of a 90 Hz frame)."""
import argparse, json, os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
GVM = r'D:\Development\GPU-VM'
RVIMG = r'D:\Development\ShaderEmu\build\images\nommu'
PRE = 'nano-X -p & sleep 2; '
LOADS = {   # the command, seconds at most
    'echo': ('echo H2H"" A; echo H2H"" B', 40),
    'shell': ('echo H2H"" A; i=0; while [ $i -lt 4000 ]; do i=$((i+1)); done; echo H2H"" B', 90),
    'seq': ('echo H2H"" A; seq 1 200000 > /dev/null; echo H2H"" B', 90),
    'gzip': ('seq 1 20000 > /tmp/g; echo H2H"" A; gzip -c /tmp/g > /dev/null; gzip -c /tmp/g > /dev/null; echo H2H"" B', 120),
    'doom': (PRE + 'echo H2H"" A; DOOM_VIEW=told DOOM_HASH_TICS=100 doom -1 -timedemo demo1; echo H2H"" B', 150),
    'quake': (PRE + 'echo H2H"" A; quake +timedemo demo1; echo H2H"" B', 170),
    'nxray': (PRE + 'echo H2H"" A; RAY_FRAMES=60 nxray; echo H2H"" B', 120),
}
ap = argparse.ArgumentParser()
ap.add_argument('machine'); ap.add_argument('load'); ap.add_argument('--mode', default='flat')
ap.add_argument('--out', default=os.path.join(HERE, 'results.jsonl')); ap.add_argument('--tick', default=''); ap.add_argument('--image', default='')
a = ap.parse_args()
cmd, secs = LOADS[a.load]
log = os.path.join(ROOT, 'logs', f'h2h_{a.machine}_{a.load}_{a.mode}.uart')
frames = os.path.join(ROOT, 'logs', f'h2h_{a.machine}_{a.load}_{a.mode}.frames')
if os.path.exists(log): os.remove(log)
unity = {'flat': [], 'world': ['--unity', '11.1,1.4,' + ('0.9' if a.machine == 'gvm' else '0.75')], 'vr': ['--unity', '11.1,7.1,1.0']}[a.mode]
if a.machine == 'gvm':
    exe = ['bin\\vm.exe', a.image or r'build\gvm_apps\img', '--ticks', '8192', '--no-viz', '--no-sound', '--no-stdin']
    prompt, cwd = '@gpuvm:~# ', GVM
    if a.tick: exe += ['--tick', a.tick]
else:
    exe = [os.path.join(ROOT, 'bin', 'rvc_harness.exe'), '--d3d11', '--rvc', a.tick or 'experiments\\rvc_opt', '--define', 'NO_PAGING', '--image', a.image or RVIMG,
           '--no-desktop', '--no-viz', '--no-stdin', '--frame-log', frames]
    prompt, cwd = '/ # ', ROOT
exe += unity + ['--seconds', str(secs), '--stats-after', '0', '--uart-log', log, '--expect', prompt, '--send', cmd + '\\n', '--until', 'H2H B']
os.environ['RVC_MARK'] = os.environ['GVM_MARK'] = 'H2H '
os.environ['RVC_STATS_EVERY'] = os.environ['GVM_STATS_EVERY'] = '1'
q = os.path.join(ROOT, 'tools', 'gpu_queue.ps1')
for tr in range(4000):
    p = subprocess.run(['pwsh', '-NoProfile', '-File', q] + exe, cwd=cwd, capture_output=True, text=True, errors='replace')
    out = p.stdout + p.stderr
    if 'refused' not in out.lower(): break
    time.sleep(2)
text = open(log, encoding='latin-1').read() if os.path.exists(log) else ''
marks = re.findall(r'mark \d+: tick ([\d.]+) ms a frame over (\d+) frames', out)
wall = round(float(marks[1][0]) * int(marks[1][1]), 1) if len(marks) >= 2 else None   # the card's tick ms of the part
frames_n = int(marks[1][1]) if len(marks) >= 2 else None
figs = re.findall(r'[^\n]*(?:\bfps\b|Iterations/Sec|timed \d+|raystat|pictures)[^\n]*', text)[-3:]
stats = [l[:220] for l in out.split('\n') if re.match(r'(STATS|\[vm\]|\[harness\]).*(IPS|ips|unity|tick [\d.]+ ms|workers)', l)][-6:]
util = ''
if a.machine == 'rv' and os.path.exists(frames):
    util = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'tick_util.py'), frames], capture_output=True, text=True, errors='replace').stdout[:900]
r = dict(machine=a.machine, load=a.load, mode=a.mode, tick=a.tick, wall=wall, frames=frames_n, marks=marks, figs=figs, stats=stats, util=util, date=time.strftime('%H:%M'))
open(a.out, 'a').write(json.dumps(r) + '\n')
print(json.dumps(r, indent=0)[:1600])
if wall is None: print('NO MARKS:', out[-800:], '\nCONSOLE:', text[-500:])
