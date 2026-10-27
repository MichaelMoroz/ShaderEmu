"""Several sides on one game in turn, each its fps at the budget against the first: a batch's variants on the games,
a few a call so that one call stays under 5 minutes (a side is about a minute).

    python gpu_vm/bench_games/many.py --game quake3 --sides img-as:gvm-as@u1025a_v0002 img-as:gvm-as@u1025c26_v0000 ...
                                      [--unity 11.1,1.4] [--again]

--again runs the first side once more at the end (the card's drift over the call). Prints a line a side as it ends.
"""
import argparse, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ab import bench


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', required=True)
    ap.add_argument('--sides', nargs='+', required=True, help='IMAGE:CKPT@MACHINE')
    ap.add_argument('--unity', default='11.1,1.4')
    ap.add_argument('--again', action='store_true')
    a = ap.parse_args()
    sides = a.sides + ([a.sides[0]] if a.again else [])
    first = None
    for s in sides:
        spec, _, machine = s.partition('@')
        img, ckpt = spec.split(':')
        res = bench(machine, a.game, a.unity, img, ckpt).get(a.game)
        if not res:
            print(f'{s}: no number', flush=True)
            continue
        fb, f, ms = res
        first = first or fb
        print(f'{a.game} {s}: {fb:.1f} fps at budget ({f:.1f} fps, {ms:.2f} ms card/frame), {fb / first:.3f} of the first', flush=True)


if __name__ == '__main__':
    main()
