#!/usr/bin/env python3
"""gpu_vm against the lean tick, flat on the card: the same command, a cold boot each, through this repository's queue.
    python h2h.py MACHINE LOAD [--tick FILE] [--tag TEXT] [--out FILE] [--image DIR] [--env A=B ...]    MACHINE: gvm or rv
Time for the work is the wall between the guest's own marks, from the per-pass log (--frame-log, microseconds since the
start), so it holds tick, commit, device and readback alike. What ended passes comes from that log's stall word."""
import argparse, json, os, re, struct, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.abspath(os.path.join(HERE, '..', '..'))
# bin and the image are not in a worktree: the main checkout is the worktree list's first line
MAIN = subprocess.run(['git', '-C', TREE, 'worktree', 'list', '--porcelain'], capture_output=True, text=True).stdout.split('\n')[0].split(' ', 1)[1]
GVM = os.environ.get('GPU_VM_TREE', os.path.join(os.path.dirname(MAIN), 'GPU-VM'))
LOGS = os.path.join(HERE, 'logs')
PRE = 'nano-X -p & sleep 2; '
TD = 'TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 '
A = 'echo H2H"" A; '
B = '; echo H2H"" B'
LOADS = {   # the command, seconds at most, the lines kept as the work's proof
    'boot': (A + 'echo H2H"" B', 60, ''),
    'gzip': ('seq 1 20000 > /tmp/g; ' + A + 'gzip -c /tmp/g > /dev/null; gzip -c /tmp/g > /dev/null' + B, 120, ''),
    'jpeg': (A + 'NXVIEW_SUM=1 nxview --decode /usr/share/picture-fox.jpg /tmp/b.ppm' + B, 120, r'nxview: .*sum'),
    'png': (A + 'NXVIEW_SUM=1 nxview --decode /usr/share/picture-fox.png /tmp/a.ppm' + B, 120, r'nxview: .*sum'),
    'mp3': (A + 'media_test audio /usr/share/play-test.mp3 /tmp/o.raw 63' + B, 120, r'audio .*sum|sum'),
    'mp3_1': (A + 'media_test audio /usr/share/play-test.mp3 /tmp/o.raw 0' + B, 200, r'audio .*sum|sum'),
    'flac': (A + 'media_test audio /usr/share/play-test.flac /tmp/o.raw 63' + B, 120, r'audio .*sum|sum'),
    'mpeg': (A + 'media_test video "/root/Videos/Big Buck Bunny.mpg" 8' + B, 170, r'sum'),
    'avi': (A + 'media_test video "/root/Videos/Big Buck Bunny.avi" 16' + B, 170, r'sum'),
    'tdawn': (PRE + A + TD + 'TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL' + B, 280, r'done at frame'),
    # (play alone: frames 1 to 1299; with --env "RVC_MARK=: mark " the work is what lies between them, a level's load outside it)
    'tdawn_play': (PRE + TD + 'TDAWN_MARK=1,1299 TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL' + B, 280, r'done at frame'),
    'doom': (PRE + A + 'DOOM_VIEW=told DOOM_HASH_TICS=100 doom -1 -timedemo demo1' + B, 150, r'timed \d+'),
    # no test's sum in what is timed, at the players' worker counts (experiments/rv_acct/NOTES.md)
    'jpeg_ns': (A + 'nxview --decode /usr/share/picture-fox.jpg /tmp/b.ppm' + B, 120, r'nxview: '),
    'png_ns': (A + 'nxview --decode /usr/share/picture-fox.png /tmp/a.ppm' + B, 120, r'nxview: '),
    'mpeg24': (A + 'MEDIA_TEST_NOSUM=1 media_test video "/root/Videos/Big Buck Bunny.mpg" 24' + B, 170, r'video: '),
    'avi64': (A + 'MEDIA_TEST_NOSUM=1 media_test video "/root/Videos/Big Buck Bunny.avi" 64' + B, 170, r'video: '),
    'shell': (A + 'i=0; while [ $i -lt 4000 ]; do i=$((i+1)); done' + B, 90, ''),
}
ap = argparse.ArgumentParser()
ap.add_argument('machine'); ap.add_argument('load'); ap.add_argument('--tick', default=''); ap.add_argument('--tag', default='')
ap.add_argument('--image', default=''); ap.add_argument('--out', default=os.path.join(HERE, 'results.jsonl'))
ap.add_argument('--env', action='append', default=[]); ap.add_argument('--extra', default='')
a = ap.parse_args()
cmd, secs, proof = LOADS[a.load]
os.makedirs(LOGS, exist_ok=True)
stem = os.path.join(LOGS, f'{a.machine}_{a.load}' + (f'_{a.tag}' if a.tag else ''))
log, frames = stem + '.uart', stem + '.frames'
for f in (log, frames):
    if os.path.exists(f): os.remove(f)
if a.machine == 'gvm':
    exe = [os.path.join(GVM, 'bin', 'vm.exe'), a.image or r'build\gvm_apps\img', '--ticks', '8192', '--no-viz', '--no-sound', '--no-stdin']
    prompt, cwd = '@gpuvm:~# ', GVM
    if a.tick: exe += ['--tick', a.tick]
else:
    exe = [os.path.join(MAIN, 'bin', 'rvc_harness.exe'), '--d3d11', '--rvc', a.tick or r'experiments\rvc_lean', '--define', 'NO_PAGING',
           '--image', a.image or os.path.join(MAIN, 'build', 'images', 'nommu'), '--no-desktop', '--no-viz', '--no-stdin']
    prompt, cwd = '/ # ', MAIN
exe += a.extra.split()
exe += ['--frame-log', frames, '--seconds', str(secs), '--stats-after', '0', '--uart-log', log, '--expect', prompt, '--send', cmd + '\\n', '--until', 'H2H B']
env = dict(os.environ, RVC_MARK='H2H ', GVM_MARK='H2H ', RVC_STATS_EVERY='1', GVM_STATS_EVERY='1', GPU_QUEUE_WHO='rv_vm h2h')
for e in a.env:
    k, v = e.split('=', 1); env[k] = v
q = os.path.join(TREE, 'tools', 'gpu_queue.ps1')   # (this repository's: GPU-VM's copy has another mutex's name on main)
for tr in range(4000):
    p = subprocess.run(['pwsh', '-NoProfile', '-File', q] + exe, cwd=cwd, capture_output=True, text=True, errors='replace', env=env)
    out = p.stdout + p.stderr
    if 'refused' not in out.lower(): break
    time.sleep(2)
text = open(log, encoding='latin-1').read() if os.path.exists(log) else ''
marks = re.findall(r'mark \d+: tick ([\d.]+) ms a frame over (\d+) frames', out)
r = dict(machine=a.machine, load=a.load, tag=a.tag, tick=a.tick, date=time.strftime('%m-%d %H:%M'), marks=marks, done='--until matched' in out)
WHY = {'gvm': {0: 'budget', 1: 'sync', 2: 'wait', 3: 'full', 5: 'halt'},
       'rv': {0: 'budget', 1: 'exit call', 2: 'csr full', 3: 'full', 6: 'uart', 7: 'fence.i', 8: 'copy', 9: 'wfi/pause', 10: 'fill'}}[a.machine]
if os.path.exists(frames) and len(marks) >= 1:
    raw = open(frames, 'rb').read()
    rec = list(struct.iter_unpack('<4I', raw[:len(raw) // 16 * 16]))
    n0 = int(marks[0][1])
    n1 = int(marks[1][1]) if len(marks) > 1 else len(rec) - n0
    part = rec[n0:n0 + n1] if n1 else []
    # a boot is the time to the first mark, the shell's prompt and one echo
    def describe(ps, us0):
        if not ps: return {}
        ins = sum(x[1] for x in ps)
        why = {}
        for x in ps:
            k = WHY.get(x[2] & 0xff, str(x[2] & 0xff))
            w = why.setdefault(k, [0, 0]); w[0] += 1; w[1] += x[1]
        return dict(wall_ms=round((ps[-1][3] - us0) / 1000, 1), passes=len(ps), core0=ins, core0_a_pass=round(ins / len(ps), 1),
                    why={k: dict(share=round(100 * v[0] / len(ps), 1), a_pass=round(v[1] / v[0])) for k, v in sorted(why.items(), key=lambda e: -e[1][0])})
    r['boot'] = describe(rec[:n0], 0)
    r['work'] = describe(part, rec[n0 - 1][3] if n0 else 0)
    r['tick_ms'] = round(float(marks[1][0]) * int(marks[1][1]), 1) if len(marks) > 1 else None
    wk = re.search(r'workers: (?:(\d+) )?instructions=(\d+)', out)
r['proof'] = re.findall(r'[^\n]*(?:' + proof + r')[^\n]*', text)[-4:] if proof else []
r['oops'] = len(re.findall(r'Oops|unhandled signal|Kernel panic|Segmentation', text))
r['stats'] = [l[:200] for l in out.split('\n') if re.match(r'(STATS (workers|gpu)|\[vm\] \d+ workers|\[harness\] \d+ workers)', l)][-3:]
open(a.out, 'a').write(json.dumps(r) + '\n')
print(json.dumps({k: r[k] for k in r if k not in ('marks', 'date')}))
if not r['done']: print('NOT DONE:', out[-800:], '\nCONSOLE:', text[-500:])
