"""A batch of tick variants judged by the games as the Unity world runs them (GPU-VM's docs/MEASURE.md).

    python gpu_vm/bench_games/grid.py BATCH --from JSON|BATCH:NAME --grid k=a,b [k=a,b ...] --games nxray,quake3
                                      [--refs rv,nt_v0008] [--unity 11.1,1.4] [--rounds 1] [--ticks N] [--only v0001,v0003]

Makes every combination of the grid over the point (builder/search.py gen into build/tick_gen/BATCH, a folder that
must not exist yet unless --resume), checks each on WARP (search.py check), then runs bench_games/run.py bench --unity
on the games for the references and every checked variant, and prints each game's fps with the variant's parameters
that differ from the point, and its ratio to the first reference. The first reference runs again at the end: a game
more than 8% off its first run is warned of (the card changed speed). Each harness run is one game on one machine
(under 5 minutes); the batch's table is also BATCH/grid.md. The fps are at the frame's card time (fps x F - U over the card ms
a frame the run took: the harness's rounds, as the world's steady mode, overshoot or undershoot it on a bursty game);
a cell marked * took over 108% or under 80% of it. --raw: the fps as run.
"""
import argparse, glob, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SEARCH = os.path.join(ROOT, 'gpu_vm', 'tick', 'builder', 'search.py')
RUN = os.path.join(HERE, 'run.py')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('batch')
    ap.add_argument('--from', dest='from_', required=True)
    ap.add_argument('--grid', nargs='+', default=[])
    ap.add_argument('--games', default='nxray,quake3')
    ap.add_argument('--refs', default='rv')
    ap.add_argument('--unity', default='11.1,1.4')
    ap.add_argument('--rounds', type=int, default=1)
    ap.add_argument('--ticks', type=int, default=0, help="the pass's bundles (the harness's --ticks; 0: its default)")
    ap.add_argument('--only', default='', help='these variants of the batch only')
    ap.add_argument('--raw', dest='at_budget', action='store_false', help='the fps as run, not at the card time of the frame')
    ap.add_argument('--resume', action='store_true', help='the batch exists: make no new variants, check and bench what is there')
    a = ap.parse_args()
    d = os.path.join(ROOT, 'build', 'tick_gen', a.batch)
    if os.path.exists(d) and not a.resume:
        sys.exit(f'grid: {d} exists (another batch?): give a new name, or --resume')
    if not a.resume:
        subprocess.run([sys.executable, SEARCH, 'gen', a.batch, '--from', a.from_, '--grid'] + a.grid, check=True, cwd=ROOT)
    subprocess.run([sys.executable, SEARCH, 'check', a.batch, '-j', '3', '--timeout', '280'], check=True, cwd=ROOT)
    base = json.load(open(a.from_))['params'] if a.from_.endswith('.json') else \
        json.load(open(os.path.join(ROOT, 'build', 'tick_gen', *a.from_.split(':')) + '.json'))['params']
    names, diff = [], {}
    for f in sorted(glob.glob(os.path.join(d, 'v*.json'))):
        n = os.path.basename(f)[:-5]
        if a.only and n not in a.only.split(','):
            continue
        if not os.path.exists(os.path.join(d, n + '.hlsl.warp_ok')):
            print(f'grid: {n} failed on WARP: left out')
            continue
        p = json.load(open(f))['params']
        names.append(n)
        diff[n] = ' '.join(f'{k}={v}' for k, v in sorted(p.items()) if base.get(k) != v) or '(the point)'
    machines = [m for m in a.refs.split(',') if m] + [f'{a.batch}_{n}' for n in names]
    env = dict(os.environ)
    if a.ticks:
        env['GVM_EXTRA'] = (env.get('GVM_EXTRA', '') + f' --ticks {a.ticks}').strip()
    p = subprocess.run([sys.executable, RUN, 'bench', '--machine', ','.join(machines), '--games', a.games,
                        '--rounds', str(a.rounds), '--unity', a.unity], cwd=ROOT, env=env, capture_output=True, text=True)
    fps, over = {}, set()
    budget = float(a.unity.split(',')[0]) - (float(a.unity.split(',')[1]) if ',' in a.unity else 1.4)
    for line in p.stdout.splitlines():
        c = [x.strip() for x in line.split('|')]
        if len(c) > 5 and c[1] in a.games.split(',') and c[2] in machines:
            m = re.search(r'([\d.]+) ms card/frame', line)   # (fps at the frame's card time: fps x budget / the card ms it took)
            try:
                f = float(c[4])
            except ValueError:
                continue
            ms = float(m.group(1)) if m else budget
            fps.setdefault((c[1], c[2]), []).append(f * budget / ms if a.at_budget else f)
            if ms > 1.08 * budget or ms < 0.8 * budget:
                over.add((c[1], c[2]))
    games = a.games.split(',')
    ref = machines[0]
    # the first reference again at the end: a card that changed speed during the batch (it happened: every machine 3x
    # slower for half an hour) shows as the two runs disagreeing
    p2 = subprocess.run([sys.executable, RUN, 'bench', '--machine', ref, '--games', a.games, '--rounds', '1', '--unity', a.unity],
                        cwd=ROOT, env=env, capture_output=True, text=True)
    drift = []
    for line in p2.stdout.splitlines():
        c = [x.strip() for x in line.split('|')]
        if len(c) > 5 and c[1] in games and c[2] == ref and fps.get((c[1], ref)):
            try:
                v, r = float(c[4]), sum(fps[(c[1], ref)]) / len(fps[(c[1], ref)])
            except ValueError:
                continue
            m = re.search(r'([\d.]+) ms card/frame', line)
            v = v * budget / float(m.group(1)) if a.at_budget and m else v
            if abs(v - r) > 0.08 * r:
                drift.append(f'{c[1]} {r:.1f} then {v:.1f}')
    if drift:
        print(f'grid: WARNING: {ref} at the batch\'s end differs from its start by more than 8%: ' + ', '.join(drift) + ' (the card\'s speed changed: run again)')
    unit = 'fps at budget' if a.at_budget else 'fps'
    out = ['| machine | what differs | ' + ' | '.join(f'{g}, {unit}' for g in games) + ' |', '|---|---|' + '---|' * len(games)]
    for m in machines:
        cells = []
        for g in games:
            v = fps.get((g, m))
            r = fps.get((g, ref))
            if not v:
                cells.append('-')
                continue
            mean = sum(v) / len(v)
            cells.append(f'{mean:.1f}' + (f' ({mean / (sum(r) / len(r)):.2f}x)' if r and m != ref else '') + (' *' if (g, m) in over else ''))
        n = m[len(a.batch) + 1:] if m.startswith(a.batch + '_') else ''
        out.append(f'| {m} | {diff.get(n, "reference")} | ' + ' | '.join(cells) + ' |')
    table = '\n'.join(out)
    print(table)
    with open(os.path.join(d, 'grid.md'), 'a') as f:
        f.write(f'\n{" ".join(sys.argv[1:])}\n\n{table}\n')
    with open(os.path.join(d, 'grid_run.txt'), 'a') as f:
        f.write(p.stdout + p.stderr)


if __name__ == '__main__':
    main()
