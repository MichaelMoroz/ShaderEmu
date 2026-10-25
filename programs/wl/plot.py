"""A page of wl's results (programs/wl/README.md): gpu_vm's units a second against the RISC-V machine's, a log-log
panel a mode with the line x = y, and each kernel's ratio. Reads the JSON run.py writes; several rounds are averaged.

    python programs/wl/plot.py OUT.html RUN_DIR [RUN_DIR ...] [--gvm c8_v0001] [--title "..."] [--note "..."]
"""
import argparse, glob, json, os, re, statistics

HERE = os.path.dirname(os.path.abspath(__file__))
GROUPS = [('kernels_games.h', 'Game code'), ('kernels_render.h', 'Rendering, physics, emulation'), ('kernels_system.h', 'System and libraries')]


def kernels():
    """name -> (group, description), from the kernel lists and each kernel's wl_kernel line"""
    var_group = {}
    for f, g in GROUPS:
        for v in re.findall(r'K\((\w+)\)', open(os.path.join(HERE, f)).read()):
            var_group[v] = g
    out = {}
    for f in glob.glob(os.path.join(HERE, 'k_*.c')):
        for v, name, desc in re.findall(r'const wl_kernel (\w+) = \{"([^"]+)", "([^"]+)"', open(f).read()):
            out[name] = (var_group.get(v, 'Other'), desc, os.path.basename(f))
    return out


def load(dirs, gvm):
    """(mode, name) -> {machine: [rates]}, and the notes and commands seen"""
    rates, notes, info = {}, [], {}
    for d in dirs:
        for f in sorted(glob.glob(os.path.join(d, '*.json'))):
            j = json.load(open(f))
            m = 'rv' if j['machine'] == 'rv' else ('gvm' if j['machine'] == gvm else None)
            if not m:
                continue
            info.setdefault(m, set()).add(j['machine'] + (' ' + j['extra'] if j.get('extra') and m == 'gvm' else ''))
            for r in j['rows']:
                rates.setdefault((r['mode'], r['name']), {}).setdefault(m, []).append(r['rate'])
            notes += [f"{j['machine']}: {n}" for n in j.get('notes', [])]
    return rates, notes, {k: sorted(v) for k, v in info.items()}


def main():
    a = argparse.ArgumentParser()
    a.add_argument('out')
    a.add_argument('dirs', nargs='+')
    a.add_argument('--gvm', default='c8_v0001')
    a.add_argument('--title', default='gpu_vm against RISC-V, workload by workload')
    a.add_argument('--note', action='append', default=[], help='a line under the table (a finding, a caveat)')
    a = a.parse_args()
    ks = kernels()
    rates, notes, info = load(a.dirs, a.gvm)
    rows = []
    for (mode, name), by in sorted(rates.items()):
        if 'rv' not in by or 'gvm' not in by:
            continue
        g, desc, src = ks.get(name, ('Other', '', ''))
        rv, gv = statistics.mean(by['rv']), statistics.mean(by['gvm'])
        rows.append(dict(mode=mode, name=name, group=g, desc=desc, src=src, rv=rv, gvm=gv, ratio=gv / rv if rv else 0,
                         nrv=len(by['rv']), ngvm=len(by['gvm'])))
    data = dict(rows=rows, notes=a.note + notes, info=info, groups=[g for _, g in GROUPS], title=a.title)
    page = open(os.path.join(HERE, 'plot_page.html')).read().replace('/*DATA*/null', json.dumps(data))
    open(a.out, 'w', encoding='utf-8').write(page)
    print(f'{a.out}: {len(rows)} points')


if __name__ == '__main__':
    main()
