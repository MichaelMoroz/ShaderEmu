"""The whole comparison, section by section (each a harness run under a minute), resuming where it stopped.

    python tools/rv_compare/run.py [--machines before,after] [--rounds 2] [--parts a,b] [--groups game,app]
                                   [--sections N] [--pause S]

Rounds alternate: round 0 runs the parts in their order and, on each part, the machines in the order given; round 1
runs the parts backwards and the machines the other way round; and so on. A (machine, part, round) with a row in
results.jsonl is done; skip.txt lists `PART` or `MACHINE:PART` to leave out. After every section its output is checked
(frames run, the card's time read) and the next waits PAUSE seconds; three failed sections in a row stop the run.
--sections N stops after N sections (a call that must end in time). Every section's line goes to DATA/run.log too."""
import argparse, json, os, sys, time, traceback
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cmp
from parts import PARTS


def jobs(machines, rounds, parts):
    out = []
    for r in range(rounds):
        ps = parts if r % 2 == 0 else parts[::-1]
        ms = machines if r % 2 == 0 else machines[::-1]
        out += [(m, p, r) for p in ps for m in ms]
    return out


def done_set():
    d = set()
    if os.path.exists(cmp.RESULTS):
        for l in open(cmp.RESULTS):
            r = json.loads(l)
            d.add((r['machine'], r['part'], r.get('round', 0)))
    return d


def log(text):
    os.makedirs(cmp.DATA, exist_ok=True)
    line = time.strftime('%H:%M:%S ') + text
    print(line, flush=True)
    with open(os.path.join(cmp.DATA, 'run.log'), 'a') as f:
        f.write(line + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--machines', default='before,after')
    ap.add_argument('--rounds', type=int, default=2)
    ap.add_argument('--parts', default='')
    ap.add_argument('--groups', default='')
    ap.add_argument('--sections', type=int, default=0)
    ap.add_argument('--pause', type=float, default=5)
    a = ap.parse_args()
    machines = a.machines.split(',')
    parts = [p for p in (a.parts.split(',') if a.parts else PARTS) if not a.groups or PARTS[p]['group'] in a.groups.split(',')]
    skipf = os.path.join(cmp.HERE, 'skip.txt')
    skip = set(open(skipf).read().split()) if os.path.exists(skipf) else set()
    for m in machines:
        if not os.path.exists(cmp.MACHINES[m]['shell']):
            log(f'{m}: no shell snapshot, making it')
            if not cmp.make_shell(m):
                raise SystemExit(1)
    n = fails = 0
    for (m, p, r) in jobs(machines, a.rounds, parts):
        if p in skip or f'{m}:{p}' in skip:
            continue
        while (m, p, r) not in done_set():
            if a.sections and n >= a.sections:
                log('stopped after %d sections' % n)
                return
            n += 1
            try:
                res = cmp.run(m, p, r)
            except Exception:
                log(f'{m} {p} r{r}: ' + traceback.format_exc()[-600:])
                res, fails = None, fails + 1
            meta = os.path.join(cmp.LOGS, cmp.tag_of(m, p, r) + '.meta')
            s = json.load(open(meta))[-1] if os.path.exists(meta) else {}
            ok = s.get('frames', 0) > 0 and s.get('card_ms')
            log(f'{m} {p} r{r} section {len(json.load(open(meta))) - 1 if os.path.exists(meta) else "?"}: {s.get("frames")} frames, '
                f'{s.get("wall")} s, card {s.get("card_ms")} ms a frame, exit {s.get("code")}{", until matched" if s.get("until") else ""}'
                f'{"" if ok else " FAILED: " + (s.get("tail") or "")[-300:].replace(chr(10), " | ")}')
            fails = 0 if ok else fails + 1
            if res is not None or (s.get('done') and not ok):
                row = cmp.summary(res if res is not None else cmp.load_logs(m, p, r))
                cmp.record(row)
                import report
                report.main()
                log(f'{m} {p} r{r}: ' + json.dumps(row.get('metrics', row.get('error')))[:400])
            if fails >= 3:
                log('three failed sections in a row: stopped')
                raise SystemExit(2)
            time.sleep(a.pause)
    log('ALL DONE')


if __name__ == '__main__':
    main()
