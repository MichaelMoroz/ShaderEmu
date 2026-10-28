"""The ISA study's runs (docs/plans/riscv-isa.md): each part on each variant's private image (build/images/isa-VAR,
programs built with VAR's -march), through prof_parts.py, rvc_cpu running the variant's instructions (RVC_ISA).

    python tools/rv_compare/isa_runs.py [--mode plain|prof] [--parts a,b] [--vars base,zb,...] [--out DIR]

Prints one line a run (core 0's and all cores' instructions, passes ended full, the window's guest seconds), then a
table against base. A run's counts are in OUT/PART_VAR.MODE/stderr.txt; prof runs also in pcs.txt."""
import argparse, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
SE = os.path.dirname(os.path.dirname(HERE))
ENV = {'base': '', 'zb': 'zb,zbs', 'fma': '', 'zfinx': 'zfinx', 'all': 'zb,zbs,zfinx', 'lay': ''}
# parts whose window is not a fixed piece of work run a fixed one here (nxray runs to the wall otherwise)
PART_ARGS = {'nxray': ['--cmd', 'RAY_FRAMES=8 nxray']}
PARTS = ['doom', 'quake-timedemo', 'quake', 'quake3', 'tdawn', 'nxray', 'qjs', 'cc']


def counts(d):
    t = open(os.path.join(d, 'stderr.txt'), errors='replace').read()
    m = re.search(r'COUNT from .*?: core 0 (\d+), all cores (\d+), .*?(\d+) passes, ([\d.]+) s of guest time', t)
    w = re.search(r'write cache: (\d+) passes of core 0 and (\d+) of workers ended full', t)
    r = dict(core0=int(m.group(1)), all=int(m.group(2)), passes=int(m.group(3)), guest=float(m.group(4))) if m else {}
    if w:
        r['full0'], r['fullw'] = int(w.group(1)), int(w.group(2))
    pcs = os.path.join(d, 'pcs.txt')
    if os.path.exists(pcs):
        st = [0, 0]; nt = [0, 0]
        for line in open(pcs):
            if line[0] == '#':
                continue
            p = line.split()
            c = int(p[0])
            st[c] += int(p[6]); nt[c] += int(p[7])
        r['stores0'], r['storesw'], r['newtex0'], r['newtexw'] = st[0], st[1], nt[0], nt[1]
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='plain')
    ap.add_argument('--parts', default=','.join(PARTS))
    ap.add_argument('--vars', default='base,zb,fma,zfinx')
    ap.add_argument('--out', default=os.path.join(SE, 'build', 'isa', 'runs'))
    ap.add_argument('--again', action='store_true', help='run again a run that has its counts')
    a = ap.parse_args()
    exe = os.path.join(SE, 'bin', 'rvc_cpu.exe')
    res = {}
    for part in a.parts.split(','):
        for v in a.vars.split(','):
            d = os.path.join(a.out, f'{part}_{v}.{a.mode}')
            if a.again or not os.path.exists(os.path.join(d, 'stderr.txt')) or not counts(d):
                img = os.path.join(SE, 'build', 'images', f'isa-{v}')
                cmd = [sys.executable, os.path.join(HERE, 'prof_parts.py'), part, '--mode', a.mode, '--image', img, '--exe', exe,
                       '--tag', f'_{v}', '--out', a.out, '--env', f'RVC_ISA={ENV[v]}'] + PART_ARGS.get(part, [])
                for _ in range(90):   # (refused, exit 75, while a GPU harness holds the card: wait for it)
                    r = subprocess.run(cmd, capture_output=True, text=True)
                    if 'exit 75' not in r.stdout:
                        break
                    time.sleep(20)
            res[(part, v)] = counts(d)
            print(part, v, res[(part, v)], flush=True)
    print()
    keys = ['core0', 'all', 'full0', 'fullw', 'stores0', 'storesw', 'newtex0', 'newtexw']
    for part in a.parts.split(','):
        b = res.get((part, 'base'), {})
        for v in a.vars.split(','):
            r = res.get((part, v), {})
            cells = []
            for k in keys:
                if k in r and k in b and b[k]:
                    cells.append(f'{k} {r[k]} ({100.0 * (r[k] - b[k]) / b[k]:+.2f}%)')
                elif k in r:
                    cells.append(f'{k} {r[k]}')
            print(f'{part:15} {v:6} ' + '; '.join(cells))


if __name__ == '__main__':
    sys.exit(main())
