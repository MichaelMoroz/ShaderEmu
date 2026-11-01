#!/usr/bin/env python3
"""gpu_vm's write cache in gvm_cpu (the reference, not the card), core 0's passes in a load's window, for several shapes in parallel.
    python model.py LOADS SHAPES [--budget N] [--out FILE]
LOADS a,b; a shape is NB,WAYS[,3] (buckets a table, ways; 3: three tables nb, nb/2, nb/4). Runs in WSL."""
import json, os, re, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
HERE = os.path.dirname(os.path.abspath(__file__))
sys.argv, rest = sys.argv[:3], sys.argv[3:]
budget = rest[rest.index('--budget') + 1] if '--budget' in rest else '8192'
out = rest[rest.index('--out') + 1] if '--out' in rest else os.path.join(HERE, 'model.jsonl')
GVM = '/mnt/d/Development/GPU-VM'
PRE = 'nano-X -p & sleep 2; '
TD = 'TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 '
A, B = 'echo H2H"" A; ', '; echo H2H"" B'
L = {'boot': None,
     'gzip': 'seq 1 20000 > /tmp/g; ' + A + 'gzip -c /tmp/g > /dev/null; gzip -c /tmp/g > /dev/null' + B,
     'jpeg': A + 'nxview --decode /usr/share/picture-fox.jpg /tmp/b.ppm' + B,
     'png': A + 'nxview --decode /usr/share/picture-fox.png /tmp/a.ppm' + B,
     'mp3_1': A + 'media_test audio /usr/share/play-test.mp3 /tmp/o.raw 0' + B,
     'mp3': A + 'media_test audio /usr/share/play-test.mp3 /tmp/o.raw 63' + B,
     'flac': A + 'media_test audio /usr/share/play-test.flac /tmp/o.raw 63' + B,
     'mpeg': A + 'media_test video "/root/Videos/Big Buck Bunny.mpg" 8' + B,
     'tdawn': PRE + A + TD + 'TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL' + B,
     'doom': PRE + A + 'DOOM_VIEW=told DOOM_HASH_TICS=100 doom -1 -timedemo demo1' + B}


def run(load, shape):
    p = shape.split(',')
    win = "--profile-from 0 --profile-until '@gpuvm:~# '" if load == 'boot' else "--profile-at 'H2H A' --profile-until 'H2H B'"
    send = '--expect ' + shlex.quote('@gpuvm:~# ') + ' --send ' + shlex.quote(((L[load] + '; ') if L[load] else '') + 'poweroff -f\n')
    envs = 'GVM_FAST=0 ' + ('GVM_WC3=1 ' if len(p) > 2 else '')
    c = (f'cd {GVM} && {envs} timeout 1500 build/gvm_cpu/gvm_cpu --cores 256 --budget {budget} --ram 192 --rom build/gvm_apps/img/rootfs.romfs '
         f'--dtb build/gvm_apps/img/gvm.dtb --wc {p[0]},{p[1]} --stats {win} {send} build/gvm_apps/img/vmlinux </dev/null 2>&1 | grep -a "gvm_cpu: \\|done at frame"')
    os.makedirs(os.path.join(HERE, 'logs'), exist_ok=True)
    sh = os.path.join(HERE, 'logs', f'm_{load}_{shape.replace(",", "_")}.sh')
    open(sh, 'w', newline=chr(10)).write(c + chr(10))
    w = subprocess.run(['wsl', 'bash', '/mnt/' + sh[0].lower() + sh[2:].replace(os.sep, '/')], capture_output=True, text=True, errors='replace')
    t = w.stdout + w.stderr
    m = re.search(r'core 0: (\d+) passes, (\d+) bundles a pass; ended: budget (\d+), sync (\d+), wait (\d+), full (\d+)', t)
    r = dict(load=load, shape=shape, budget=budget, entries=sum(int(p[0]) // d for d in ((1, 2, 4) if len(p) > 2 else (1, 1))) * int(p[1]))
    if m:
        n, bp, bu, sy, wa, fu = map(int, m.groups())
        r.update(passes=n, bundles=bp, budget_ends=bu, sync=sy, wait=wa, full=fu, full_pct=round(100 * fu / n, 1))
        r['ends'] = {e: (int(a), int(b), float(c)) for e, a, b, c in re.findall(r'wc end (\d+): (\d+) passes, (\d+) bundles, ([\d.]+) texels', t)}
    else:
        r['error'] = t[-300:]
    with open(out, 'a') as f: f.write(json.dumps(r) + '\n')
    print(load, shape, r.get('entries'), 'passes', r.get('passes'), 'b/p', r.get('bundles'), 'full%', r.get('full_pct'), r.get('ends'), r.get('error', ''), flush=True)


jobs = [(l, s) for l in sys.argv[1].split(',') for s in sys.argv[2].split(';')]
with ThreadPoolExecutor(12) as ex: list(ex.map(lambda j: run(*j), jobs))
