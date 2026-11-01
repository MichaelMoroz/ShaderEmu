#!/usr/bin/env python3
"""gpu_vm's bundles on a load, from gvm_cpu --profile (the reference, not the card): how many bundles of which fast-loop path,
how many leave the fast loop, how far a taken bundle goes, and code bytes touched.
    python gvmprof.py run LOAD...      runs them in WSL, logs/LOAD.prof and .stats
    python gvmprof.py show LOAD...     the table"""
import json, os, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
HERE = os.path.dirname(os.path.abspath(__file__))
GVM = '/mnt/d/Development/GPU-VM'
PRE = 'nano-X -p & sleep 2; '
TD = 'TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 '
A, B = 'echo H2H"" A; ', '; echo H2H"" B'
L = {'gzip': 'seq 1 20000 > /tmp/g; ' + A + 'gzip -c /tmp/g > /dev/null; gzip -c /tmp/g > /dev/null' + B,
     'png': A + 'MEDIA_TEST_NOSUM=1 nxview --decode /usr/share/picture-fox.png /tmp/a.ppm' + B,
     'tdawn': PRE + A + TD + 'TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL' + B,
     'tdinline': PRE + A + TD + 'TDAWN_SCENE=inline TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL' + B,
     'shell': A + 'i=0; while [ $i -lt 4000 ]; do i=$((i+1)); done' + B,
     }
LOGS = os.path.join(HERE, 'logs')
def wsl(p): return '/mnt/' + p[0].lower() + p[2:].replace(os.sep, '/')
def run(load):
    os.makedirs(LOGS, exist_ok=True)
    prof = wsl(os.path.join(LOGS, load + '.prof'))
    send = '--expect ' + shlex.quote('@gpuvm:~# ') + ' --send ' + shlex.quote(L[load] + '; poweroff -f\n')
    c = (f'cd {GVM} && GVM_FAST=0 timeout 3000 build/gvm_cpu/gvm_cpu --cores 256 --budget 8192 --ram 192 --rom build/gvm_apps/img/rootfs.romfs '
         f'--dtb build/gvm_apps/img/gvm.dtb --stats --profile {prof} --profile-at "H2H A" --profile-until "H2H B" {send} build/gvm_apps/img/vmlinux </dev/null 2>&1 | grep -a "gvm_cpu: \\|done at frame" > {prof}.stats')
    sh = os.path.join(LOGS, load + '.sh')
    open(sh, 'w', newline='\n').write(c + '\n')
    subprocess.run(['wsl', 'bash', wsl(sh)])
    print(load, 'done', flush=True)
PATHS = {0: 'general', 1: 'alu', 2: 'memf', 3: 'load', 4: 'load2', 5: 'store', 6: 'mem2', 7: 'int'}
def show(load):
    n0 = 0; byp = {}; ops = 0; mem = 0; st = 0; cc = {}; addrs = {}; hot = []
    for l in open(os.path.join(LOGS, load + '.prof')):
        if l[0] == '#': continue
        f = l.split()
        pc, w0, w1, w2, ctl = (int(x, 16) for x in f[:5]); n = int(f[5])
        if not n: continue
        n0 += n
        p = ctl & 7; byp[p] = byp.get(p, 0) + n
        k = (ctl >> 3) & 3; ops += k * n
        for w, on in ((w0, k > 0), (w1, k > 1)):
            cls = (w >> 18) & 3
            if on and cls == 2:
                mem += n
                if (w >> 15) & 1: st += n
        c = (ctl >> 7) & 15; cc[c] = cc.get(c, 0) + n
        addrs[pc >> 12] = addrs.get(pc >> 12, 0) + n
        hot.append((n, pc))
    hot.sort(reverse=True)
    cum = 0; marks = {}
    for i, (n, pc) in enumerate(hot):
        cum += n
        for q in (.5, .9, .99):
            if q not in marks and cum >= q * n0: marks[q] = i + 1
    print(f'{load}: core 0 bundles {n0/1e6:.1f} M, ops {ops/1e6:.1f} M ({ops/n0:.2f} a bundle), memory ops {100*mem/ops:.1f}%, stores {100*st/ops:.1f}%')
    print('  paths:', ' '.join(f'{PATHS[p]} {100*v/n0:.1f}%' for p, v in sorted(byp.items())))
    print('  cc:', ' '.join(f'{c}:{100*v/n0:.1f}%' for c, v in sorted(cc.items())))
    print(f'  distinct bundles {len(hot)}; 50/90/99% of runs in {marks[.5]}/{marks[.9]}/{marks[.99]} bundles')
if sys.argv[1] == 'run':
    with ThreadPoolExecutor(6) as ex: list(ex.map(run, sys.argv[2:]))
else:
    for l in sys.argv[2:]: show(l)
