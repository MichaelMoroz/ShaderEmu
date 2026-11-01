"""Age of Empires played by real input (tools/game_par.py aoe-play, docs/games/aoe.md, "Playing"): from a run's console, for each
act, how long from the input to the game's having it, to its having handled it, and to the picture that shows it, and the frames a
second and core 0's instructions a frame until the next act.
   python tools/aoe_play.py LOG"""
import re
import sys


def main(path):
    acts, rec = [], []
    for line in open(path, encoding='latin-1', errors='replace'):
        m = re.match(r'act (\S+) (\d+)\.(\d+) ', line)
        if m:
            acts.append((m[1], int(m[2]) * 1000 + int(m[3]) * 10))
        m = re.match(r'L (\d+) (\d+) (\d+) (\S) (-?\d+) (-?\d+)', line)
        if m:
            rec.append((int(m[1]), int(m[2]), int(m[3]), m[4], int(m[5]), int(m[6])))
    print(f'{len(acts)} acts, {len(rec)} records')
    print(f'{"act":18} {"came":>6} {"taken":>6} {"queue":>5} {"pict1":>6} {"pict2":>6} {"panel":>6} {"fps":>5} {"k/frame":>8} {"moves":>6} {"mv queue":>8} {"worst frame":>11}')
    for i, (name, t0) in enumerate(acts):
        t1 = acts[i + 1][1] if i + 1 < len(acts) else t0 + 5000
        span = [r for r in rec if t0 <= r[0] < t1]
        pics = [r for r in span if r[3] == 'P']
        came = next((r for r in span if r[3] == 'B'), None) or next((r for r in span if r[3] == 'M'), None)
        taken = next((r for r in span if r[3] in 'dm' and came and r[0] >= came[0]), None)
        after = [r for r in pics if taken and r[0] >= taken[0]]
        panel = next((r for r in span if r[3] == 'R' and taken and r[0] >= taken[0]), None)
        shown = next((r for r in pics if panel and r[0] >= panel[0]), None)
        ms = lambda r: f'{r[0] - t0}' if r else '-'
        moves = [r for r in span if r[3] == 'm']
        fps = (len(pics) - 1) * 1000 / (pics[-1][0] - pics[0][0]) if len(pics) > 1 and pics[-1][0] > pics[0][0] else 0
        per = (pics[-1][1] - pics[0][1]) / (len(pics) - 1) / 1000 if len(pics) > 1 else 0
        worst = max((b[0] - a[0] for a, b in zip(pics, pics[1:])), default=0)
        print(f'{name:18} {ms(came):>6} {ms(taken):>6} {taken[4] if taken else 0:>5} {ms(after[0] if after else None):>6} '
              f'{ms(after[1] if len(after) > 1 else None):>6} {ms(shown):>6} {fps:>5.1f} {per:>8.0f} {len(moves):>6} '
              f'{max((r[4] for r in moves), default=0):>8} {worst:>11}')
    # (V: the view's place, hundredths of a tile, at each step of a drag of the map by the right button: "Controls")
    for i, (name, t0) in enumerate(acts):
        t1 = acts[i + 1][1] if i + 1 < len(acts) else t0 + 5000
        steps = [r for r in rec if r[3] == 'V' and t0 <= r[0] < t1]
        if steps:
            print(f'{name}: the map dragged, {len(steps)} steps in {steps[-1][0] - steps[0][0]} ms, the view to {steps[-1][4]} {steps[-1][5]}')
    pics = [r for r in rec if r[3] == 'P' and acts and r[0] >= acts[0][1]]
    if len(pics) > 1:
        span = pics[-1][0] - pics[0][0]
        for a, b, what in (('w', 'x', 'waiting for the walk'), ('j', 'k', 'waiting for the raster')):
            wait, at, n = 0, None, 0
            for r in rec:
                if r[0] < pics[0][0] or r[0] > pics[-1][0]:
                    continue
                if r[3] == a:
                    at = r[0]
                elif r[3] == b and at is not None:
                    wait += r[0] - at
                    n += 1
                    at = None
            print(f'{what}: {wait} ms of {span} ({100 * wait / span:.0f}%), {wait / max(1, len(pics) - 1):.1f} ms a frame, {n} times')
        print(f'{(len(pics) - 1) * 1000 / span:.1f} frames a second over the acts, {span / (len(pics) - 1):.0f} ms a frame')


main(sys.argv[1])
