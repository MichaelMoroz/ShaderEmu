"""rvc_cpu's clock fitted to a card, so that a program's speed is read in seconds and not a card's minutes
(docs/machine/cpu-harness.md, "What it is not"). What a program does with the clock changes what it runs, so the model
goes into the run (--pass-model), not over its counts.

    python tools/calibrate.py card GAME [GAME ...]   the games on the card through game_par.py, once each: build/calibrate/card.jsonl
    python tools/calibrate.py fit [--copy-ms MS]     ms a pass = fixed + core 0's + the workers' + a GPU list's, by least squares
                                                     over the card's runs; each game also left out and predicted; tools/card_model.json.
                                                     A pass that makes a copy's chunk from RAM (256 KB) has its own fixed part, MS
                                                     (the model's fifth value): nxperf's memcpy on the card says it (docs/apps/bench.md)
    python tools/calibrate.py check GAME [GAME ...]  the games under the model on rvc_cpu against their card runs

A card's runs and its model are that card's (and that fxc2's): fit again for another.
"""
import json, os, re, subprocess, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import game_par

OUT = os.path.join(ROOT, 'build', 'calibrate')
CARD = os.path.join(OUT, 'card.jsonl')
MODEL = os.path.join(ROOT, 'tools', 'card_model.json')
LINE = re.compile(r'\(card\): ([\d.]+) frames a second \((\d+) frames, ([\d.]+) s from the mark\); (\d+) passes a second, ([\d.]+) a frame; '
                  r'core 0 ([\d.]+)M a second, workers ([\d.]+)M')


def samples():
    rows = {}
    for line in open(CARD):
        r = json.loads(line)
        rows.setdefault(r['game'], []).append(r)
    return {g: {k: float(np.mean([x[k] for x in v])) for k in ('fps', 'passes_s', 'passes', 'core0', 'workers')} for g, v in rows.items()}


def counts(name):
    """What the game runs and draws a pass, from a plain rvc_cpu run (its GPU lists are not on the card's line)."""
    log = os.path.join(OUT, name + '.log')
    r = game_par.run(name, game_par.GAMES[name], None, log, None, 0, False)
    return r


def cmd_card(names):
    os.makedirs(OUT, exist_ok=True)
    for n in names:
        p = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'game_par.py'), n, '--card', '--limit', '240'], cwd=ROOT, capture_output=True, text=True, errors='replace')
        m = LINE.search(p.stdout)
        print(p.stdout.strip().splitlines()[0][:170] if p.stdout.strip() else p.stderr[-170:], flush=True)
        if m:
            with open(CARD, 'a') as f:
                f.write(json.dumps(dict(game=n, fps=float(m[1]), frames=int(m[2]), seconds=float(m[3]), passes_s=float(m[4]), passes=float(m[5]),
                                        core0=float(m[6]), workers=float(m[7]))) + '\n')


def design(s, lists_a_pass, copies_a_pass=0.0):
    k = 1000.0 / s['passes_s']    # thousands of instructions a pass, from millions a second
    return [1.0 - copies_a_pass, s['core0'] * k, s['workers'] * k, lists_a_pass]


def nonnegative(a, b):
    """Least squares with no negative cost: a term that comes out below 0 is left out and the rest fitted again."""
    use = list(range(a.shape[1]))
    while True:
        x = np.zeros(a.shape[1])
        x[use] = np.linalg.lstsq(a[:, use], b, rcond=None)[0]
        bad = [i for i in use if x[i] < 0]
        if not bad:
            return x
        use.remove(min(bad, key=lambda i: x[i]))


def cmd_fit(copy_ms=None):
    sam = samples()
    lists, copies = {}, {}
    for g in sam:
        r = counts(g)
        if not r or 'lists' not in r:
            print(f'{g}: no counts from rvc_cpu, left out')
            continue
        lists[g] = r['lists'] / r['passes']
        copies[g] = r.get('copy_passes', 0.0) / r['passes'] if copy_ms is not None else 0.0
    names = list(lists)
    a = np.array([design(sam[g], lists[g], copies[g]) for g in names])
    b = np.array([1000.0 / sam[g]['passes_s'] - (copy_ms or 0.0) * copies[g] for g in names])
    for g in names:
        if copies[g]: print(f'{g}: {copies[g] * 100:.2f}% of passes make a copy')
    x = nonnegative(a, b)
    print(f'ms a pass = {x[0]:.4f} + {x[1]:.4f} a thousand of core 0 + {x[2]:.5f} a thousand of the workers + {x[3]:.4f} a GPU list ({len(names)} games)')
    print(f"{'game':16} {'card ms':>8} {'fitted':>8} {'left out':>9} {'error':>7}")
    worst = 0.0
    for i, g in enumerate(names):
        keep = [k for k in range(len(names)) if k != i]
        y = nonnegative(a[keep], b[keep]) if len(keep) >= 2 else x
        out = float(a[i] @ y)
        e = (out / b[i] - 1) * 100
        worst = max(worst, abs(e))
        print(f'{g:16} {b[i]:8.3f} {float(a[i] @ x):8.3f} {out:9.3f} {e:+6.1f}%')
    print(f'the worst left out: {worst:.1f}%')
    model = [float(v) for v in x] + ([float(copy_ms)] if copy_ms is not None else [])
    json.dump(dict(model=model, games=names, worst_left_out=worst), open(MODEL, 'w'), indent=1)
    print('wrote', os.path.relpath(MODEL, ROOT))


def cmd_check(names):
    sam = samples()
    game_par.PASS_MODEL = ','.join(f'{v:.6f}' for v in json.load(open(MODEL))['model'])
    print(f"{'game':16} {'card fps':>9} {'model fps':>10} {'error':>7} {'card passes':>12} {'model passes':>13}")
    for n in names:
        r = game_par.run(n, game_par.GAMES[n], None, os.path.join(OUT, n + '.model.log'), None, 0, False)
        if not r:
            continue
        c = sam.get(n)
        if c:
            print(f"{n:16} {c['fps']:9.1f} {r['fps']:10.1f} {(r['fps'] / c['fps'] - 1) * 100:+6.1f}% {c['passes']:12.2f} {r['passes']:13.2f}")
        else:
            print(f"{n:16} {'':9} {r['fps']:10.1f} {'':7} {'':12} {r['passes']:13.2f}")


if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in ('card', 'fit', 'check'):
        sys.exit(__doc__)
    copy_ms = float(sys.argv[sys.argv.index('--copy-ms') + 1]) if '--copy-ms' in sys.argv else None
    {'card': lambda: cmd_card(sys.argv[2:]), 'fit': lambda: cmd_fit(copy_ms), 'check': lambda: cmd_check(sys.argv[2:])}[sys.argv[1]]()
