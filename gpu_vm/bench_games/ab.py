"""Two images (and their checkpoints) on one tick, in turn: a change to the programs measured at equal card time.
A side may name its own tick (IMAGE:CKPT@MACHINE): an ISA change, whose programs need their tick.

    python gpu_vm/bench_games/ab.py --machine u1025a_v0002 --games quake1 --a img-q4:gvm-c25 --b img-uop:gvm-uop
                                    [--unity 11.1,1.4] [--rounds 3]

Each round runs A then B (run.py bench --unity, every run through tools/gpu_queue.ps1) and prints the game's fps at
the budget (fps x budget / the frame's card ms, as grid.py's "fps at budget") of each; the last lines are the means and
B over A.
"""
import argparse, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
RUN = os.path.join(HERE, 'run.py')


def bench(machine, games, unity, img, ckpt):
    env = dict(os.environ, GVM_IMAGE=os.path.join(ROOT, 'build', 'gvm_apps', img), GVM_CKPT=ckpt)
    p = subprocess.run([sys.executable, RUN, 'bench', '--machine', machine, '--games', games, '--rounds', '1',
                        '--unity', unity], cwd=ROOT, env=env, capture_output=True, text=True)
    budget = float(unity.split(',')[0]) - float(unity.split(',')[1])
    out = {}
    for line in p.stdout.splitlines():
        c = [x.strip() for x in line.split('|')]
        if len(c) > 5 and c[1] in games.split(',') and c[2] == machine:
            m = re.search(r'([\d.]+) ms card/frame', line)
            try:
                f = float(c[4])
            except ValueError:
                continue
            ms = float(m.group(1)) if m else budget
            out[c[1]] = (f * budget / ms, f, ms)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--machine', required=True, help="the tick of a side that names none")
    ap.add_argument('--games', default='quake1')
    ap.add_argument('--a', required=True, help='IMAGE:CKPT[@MACHINE]')
    ap.add_argument('--b', required=True, help='IMAGE:CKPT[@MACHINE]')
    ap.add_argument('--unity', default='11.1,1.4')
    ap.add_argument('--rounds', type=int, default=3)
    a = ap.parse_args()
    sums = {}
    for r in range(a.rounds):
        for side in ('a', 'b'):
            spec, _, machine = getattr(a, side).partition('@')
            img, ckpt = spec.split(':')
            res = bench(machine or a.machine, a.games, a.unity, img, ckpt)
            for g, (fb, f, ms) in res.items():
                sums.setdefault((g, side), []).append(fb)
                print(f'round {r + 1} {side} {img} {g}: {fb:.1f} fps at budget ({f:.1f} fps, {ms:.2f} ms card/frame)', flush=True)
    for g in a.games.split(','):
        va, vb = sums.get((g, 'a')), sums.get((g, 'b'))
        if va and vb:
            ma, mb = sum(va) / len(va), sum(vb) / len(vb)
            print(f'{g}: A {ma:.1f}, B {mb:.1f} fps at budget, B/A {mb / ma:.3f}')


if __name__ == '__main__':
    main()
