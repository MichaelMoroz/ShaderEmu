"""report.py [A B]: tools/rv_compare/REPORT.md from results.jsonl, machine B against machine A (before, after), each
part's metrics as the mean of its rounds, their spread, and the ratio. Written after every part."""
import json, os, statistics, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parts import PARTS

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, 'REPORT.md')
PACE = {'frame time median': ('ms', 'low', 0), 'frame time p95': ('ms', 'low', 0), 'frame time p99': ('ms', 'low', 0),
        'hitches a second': ('hitches/s', 'low', 0)}

HEAD = """# The RISC-V machine, machine against machine, every application on the card

`tools/rv_compare/`. Every part runs on the card (RTX 5070 Laptop, D3D11, fxc2)
as a VRChat world runs it: `rvc_harness --unity 11.1,1.4,0.75`, the guest's clock 11.1 ms a host frame (90 Hz), as many
rounds (a tick of 8,192 instructions and its commit) a frame as fit 9.7 ms of the card, each round costing at least
0.75 ms (Unity's CPU a round in RISC-V's world). Under it the guest's clock is the world's, so every time and rate
below is what a player in the world sees.

- **The start of every part**: the machine's shell snapshot with `nano-X -p` up, made on the processor
  (`cmp.py shell MACHINE`). The command is typed after `echo GO`; times are from that line.
- **How it is measured**: the console's lines and the count of GPU lists drawn are logged by host frame
  (`RVC_LINE_LOG`, `RVC_FRAMES_LOG`). "start to first frame" is GO to the first list drawn; "frames" are lists drawn
  a second (90 is the ceiling: one a host frame); other numbers are the programs' own prints, on the guest's clock.
  A run longer than 33 s of the guest goes on in sections of under a minute, saved and loaded.
- **Frame pacing** (every part that draws): the gaps between frames drawn over the second half of the run (or from
  the part's own mark), median, 95th and 99th percentile in ms, and hitches (a gap over twice the median) a second.
- **mean**: of the rounds run (each round a whole run of the part); **spread**: (largest - smallest) / mean over the
  rounds, in percent. **ratio**: how many times better B is than A (A / B for a time, B / A for a rate): above 1 B
  is better; 3% either way is level.
- **card ms**: the card's time a host frame, the run's mean (the budget is 9.7); **rounds**: rounds a frame.
  **ratio at the budget**: the ratio with each side's card ms a frame brought to the budget (A's card ms over B's
  times the ratio): a machine whose rounds overshot the budget is not paid for it.
- Machines: `tools/rv_compare/machines.py`; `before` is frozen in the backport's `build/before` (its README.md).

"""


def fmt(v):
    if v is None:
        return '-'
    return f'{v:,.2f}' if abs(v) < 100 else f'{v:,.0f}'


def mean(v):
    v = [x for x in v if x is not None]
    return sum(v) / len(v) if v else None


def spread(v):
    v = [x for x in v if x is not None]
    if len(v) < 2 or not mean(v):
        return None
    return 100 * (max(v) - min(v)) / abs(mean(v))


def spoiled():
    """spoiled.txt: `MACHINE:PART:ROUND reason` a line, a round left out of the means (a card that was slower)"""
    p = os.path.join(HERE, 'spoiled.txt')
    out = {}
    for l in open(p) if os.path.exists(p) else []:
        if l.strip() and not l.startswith('#'):
            key, _, why = l.strip().partition(' ')
            m, part, rnd = key.split(':')
            out[(m, part, int(rnd))] = why
    return out


def load():
    res = {}   # (machine, part) -> {round: row}
    p = os.path.join(HERE, 'results.jsonl')
    bad = spoiled()
    if os.path.exists(p):
        for l in open(p):
            r = json.loads(l)
            if (r['machine'], r['part'], r.get('round', 0)) not in bad:
                res.setdefault((r['machine'], r['part']), {})[r.get('round', 0)] = r
    return res


def main(A='before', B='after'):
    res = load()
    rows = [f'| group | program and part | metric | unit | better | {A}: mean | {A}: each round | {A}: spread (%) | '
            f'{B}: mean | {B}: each round | {B}: spread (%) | ratio ({B} x {A}) | ratio at the budget ({B} x {A}) | card ms a frame {A} / {B} (ms) | '
            f'rounds a frame {A} / {B} (rounds) | runs {A} / {B} (guest s, sections) |',
            '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    wins = losses = level = 0
    spreads = {A: [], B: []}
    parts_done = {A: 0, B: 0}
    for name, P in PARTS.items():
        a, b = res.get((A, name), {}), res.get((B, name), {})
        for m, x in ((A, a), (B, b)):
            parts_done[m] += bool(x)
        if not a and not b:
            continue
        ar, br = [a[k] for k in sorted(a)], [b[k] for k in sorted(b)]
        for metric, (unit, better, _) in list(P['metrics'].items()) + list(PACE.items()):
            av = [r.get('metrics', {}).get(metric) for r in ar]
            bv = [r.get('metrics', {}).get(metric) for r in br]
            if all(v is None for v in av + bv):
                continue
            am, bm = mean(av), mean(bv)
            ratio = None
            if am and bm:
                ratio = am / bm if better == 'low' else bm / am
                wins, losses, level = wins + (ratio > 1.03), losses + (ratio < 0.97), level + (0.97 <= ratio <= 1.03)
            # at the budget ("fps at budget"): a rate scaled by the budget over the card ms a frame the
            # run took, a time by the inverse: the harness's rounds overshoot or undershoot 9.7 ms on a bursty program
            ca, cb = mean([r.get('card_ms_frame') for r in ar]), mean([r.get('card_ms_frame') for r in br])
            at_budget = ratio * (ca / cb) if ratio and ca and cb else None
            for m, v in ((A, av), (B, bv)):
                if spread(v) is not None:
                    spreads[m].append(spread(v))
            card = lambda rs: fmt(mean([r.get('card_ms_frame') for r in rs]))
            rnd = lambda rs: fmt(mean([r.get('rounds_frame') for r in rs]))
            runs = lambda rs: ', '.join(f"{r.get('guest_s', '-')}/{r.get('sections', '-')}" for r in rs) or '-'
            each = lambda v: ', '.join(fmt(x) for x in v) or '-'
            err = ' '.join(r.get('error', '') for r in ar + br if r.get('error'))
            rows.append(f"| {P['group']} | {name}: {P['what']} | {metric} | {unit} | {better}er | {fmt(am)} | {each(av)} | "
                        f"{fmt(spread(av))} | {fmt(bm)} | {each(bv)} | {fmt(spread(bv))} | {'-' if ratio is None else f'{ratio:.2f}'} | "
                        f"{'-' if at_budget is None else f'{at_budget:.2f}'} | {card(ar)} / {card(br)} | {rnd(ar)} / {rnd(br)} | {runs(ar)} / {runs(br)}{' ' + err if err else ''} |")
    s = HEAD
    s += (f'**Parts measured: {A} {parts_done[A]}, {B} {parts_done[B]} of {len(PARTS)}. '
          f'{B} better than {A} in {wins} metrics, worse in {losses}, level in {level} (3% either way).**\n\n')
    for m in (A, B):
        if spreads[m]:
            v = sorted(spreads[m])
            s += (f'- Run-to-run spread on {m} over {len(v)} metrics with two rounds or more: median {statistics.median(v):.1f}%, '
                  f'75th percentile {v[int(0.75 * len(v))]:.1f}%, largest {v[-1]:.1f}%.\n')
    bad = spoiled()
    if bad:
        s += f'- Rounds left out of the means ({len(bad)}, `spoiled.txt`): ' + '; '.join(f'{m} {p} round {r}' for m, p, r in bad) + '.\n'
    s += '\n'
    notes = os.path.join(HERE, 'notes.md')
    if os.path.exists(notes):
        s += open(notes, encoding='utf-8').read() + '\n'
    s += '## Every metric\n\n' + '\n'.join(rows) + '\n'
    open(OUT, 'w', encoding='utf-8', newline='\n').write(s)
    print(f'{OUT}: {len(rows) - 2} rows')


if __name__ == '__main__':
    main(*sys.argv[1:3])
