#!/usr/bin/env python3
"""Lead 3: gpu_vm's passes ended full under another form of write cache, in gvm_cpu's model (a private copy of ../GPU-VM/ref under C:/Users/micha/agents_isa/gvmref).
    python wcform.py build
    python wcform.py run LOADS SHAPES    a shape NB,WAYS[,3][,s]: 3 three tables, s: a store of what is there already takes no entry, as lean's does
Prints passes, full share and what a pass does; appends to wcform.jsonl. Runs in WSL like rv_vm/model.py: no git, no card."""
import json, os, re, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
HERE = os.path.dirname(os.path.abspath(__file__))
R = r'C:\Users\micha\agents_isa\gvmref'
WR = '/mnt/c/Users/micha/agents_isa/gvmref'
GVM = '/mnt/d/Development/GPU-VM'
PRE = 'nano-X -p & sleep 2; '
TD = 'TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 '
A, B = 'echo H2H"" A; ', '; echo H2H"" B'
L = {'gzip': 'seq 1 20000 > /tmp/g; ' + A + 'gzip -c /tmp/g > /dev/null; gzip -c /tmp/g > /dev/null' + B,
     'png': A + 'nxview --decode /usr/share/picture-fox.png /tmp/a.ppm' + B,
     'jpeg': A + 'nxview --decode /usr/share/picture-fox.jpg /tmp/b.ppm' + B,
     'tdawn': PRE + A + TD + 'TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL' + B}
def wsl(p): return '/mnt/' + p[0].lower() + p[2:].replace(os.sep, '/')
def sh(name, text):
    p = os.path.join(HERE, 'logs', name + '.sh'); os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, 'w', newline='\n').write(text + '\n')
    return subprocess.run(['wsl', 'bash', wsl(p)], capture_output=True, text=True, errors='replace')
if sys.argv[1] == 'build':
    f = os.path.join(R, 'ref', 'gvm_ref.h'); s = open(f, newline='').read().replace('\r\n', '\n')
    if 'gvm__same' not in s:
        s = s.replace('static void gvm__store(gvm_machine *m, gvm_core *c, uint32_t a, uint32_t bytes, uint32_t v) {',
                      '''static int gvm_silent = -1;
static int gvm__same(gvm_machine *m, gvm_core *c, uint32_t a, uint32_t bytes, uint32_t v) {
    if (gvm_silent < 0) gvm_silent = getenv("GVM_WC_SILENT") != NULL;
    if (!gvm_silent || !c->wl.nb) return 0;
    for (uint32_t i = 0; i < bytes; i++) {
        uint32_t b = a + i, word = b >> 2, sh = (b & 3) * 8, *e = gvm__wl_find(&c->wl, word, 0), old = e ? *e : m->ram[word];
        if (((old >> sh) & 0xffu) != ((v >> (8 * i)) & 0xffu)) return 0;
    }
    return 1;
}
static void gvm__store(gvm_machine *m, gvm_core *c, uint32_t a, uint32_t bytes, uint32_t v) {''')
        s = s.replace('if (c->wl.nb) { gvm__wc_put(&c->wl, a >> 4, 1);', 'if (c->wl.nb && !gvm__same(m, c, a, bytes, v)) { gvm__wc_put(&c->wl, a >> 4, 1);')
        s = s.replace('if (addr < GVM_OPS && !gvm__room(c, addr, by)) {', 'if (addr < GVM_OPS && !gvm__same(m, c, addr, by, c->x[rd]) && !gvm__room(c, addr, by)) {')
        if '#include <stdlib.h>' not in s: s = '#include <stdlib.h>\n' + s
        open(f, 'w', newline='\n').write(s)
    print(sh('wc_build', f'cd {WR}/ref && make OUT=../out 2>&1 | tail -3').stdout)
    sys.exit()
def run(load, shape):
    p = shape.split(',')
    three = '3' in p[2:]; sil = 's' in p[2:]
    send = '--expect ' + shlex.quote('@gpuvm:~# ') + ' --send ' + shlex.quote(L[load] + '; poweroff -f\n')
    envs = 'GVM_FAST=0 ' + ('GVM_WC3=1 ' if three else '') + ('GVM_WC_SILENT=1 ' if sil else '')
    c = (f'cd {GVM} && {envs} timeout 1500 {WR}/out/gvm_cpu --cores 256 --budget 8192 --ram 192 --rom build/gvm_apps/img/rootfs.romfs --dtb build/gvm_apps/img/gvm.dtb '
         f'--wc {p[0]},{p[1]} --stats --profile-at "H2H A" --profile-until "H2H B" {send} build/gvm_apps/img/vmlinux </dev/null 2>&1 | grep -a "gvm_cpu: \\|done at frame"')
    w = sh(f'wc_{load}_{shape.replace(",", "_")}', c); t = w.stdout + w.stderr
    m = re.search(r'core 0: (\d+) passes, (\d+) bundles a pass; ended: budget (\d+), sync (\d+), wait (\d+), full (\d+)', t)
    r = dict(load=load, shape=shape, entries=sum(int(p[0]) // d for d in ((1, 2, 4) if three else (1, 1))) * int(p[1]))
    if m:
        n, bp, bu, sy, wa, fu = map(int, m.groups())
        r.update(passes=n, bundles=bp, budget=bu, sync=sy, wait=wa, full=fu, full_pct=round(100 * fu / n, 1))
        e = re.search(r'wc end 0: \d+ passes, \d+ bundles, ([\d.]+) texels', t); r['texels_budget'] = float(e[1]) if e else None
    else: r['error'] = t[-300:]
    with open(os.path.join(HERE, 'wcform.jsonl'), 'a') as f: f.write(json.dumps(r) + '\n')
    print(load, shape, r['entries'], 'passes', r.get('passes'), 'b/p', r.get('bundles'), 'full%', r.get('full_pct'), 'texels at budget', r.get('texels_budget'), r.get('error', ''), flush=True)
jobs = [(l, s) for l in sys.argv[2].split(',') for s in sys.argv[3].split(';')]
with ThreadPoolExecutor(6) as ex: list(ex.map(lambda j: run(*j), jobs))
