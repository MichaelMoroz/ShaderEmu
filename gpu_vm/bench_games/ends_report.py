"""Why the card's passes ended, from profile_card.py's logs (GVM_ENDS_LOG, and GVM_MBOX_LOG for the pcs).

    python gpu_vm/bench_games/ends_report.py BASE [--top 20]

BASE is OUTDIR/GAME.TAG (BASE.ends, BASE.mbox). Prints, for core 0 and for the busiest worker of each sample, how often
a pass ended for each reason and the mean bundles of those passes; with BASE.mbox, core 0's commonest pcs at a sync,
a full log and a wait.
"""
import argparse, collections, os, struct

WHY = {0: 'budget', 1: 'sync', 2: 'wait', 3: 'full', 4: 'parked', 5: 'halt'}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('base')
    ap.add_argument('--top', type=int, default=20)
    a = ap.parse_args()
    raw = open(a.base + '.ends', 'rb').read()
    n = len(raw) // 1024
    c0 = collections.defaultdict(lambda: [0, 0])
    wk = collections.defaultdict(lambda: [0, 0])
    for f in range(n):
        row = struct.unpack_from('<256I', raw, f * 1024)
        w, b = row[0] & 0xff, row[0] >> 8
        c0[w][0] += 1; c0[w][1] += b
        busy = max(range(1, 256), key=lambda c: row[c] >> 8)
        w, b = row[busy] & 0xff, row[busy] >> 8
        wk[w][0] += 1; wk[w][1] += b
    for name, h in (('core 0', c0), ('busiest worker', wk)):
        tot = sum(v[0] for v in h.values()) or 1
        allb = sum(v[1] for v in h.values())
        print(f'{name}: {tot} passes sampled, {allb / tot:.0f} bundles a pass')
        print('  why      passes (%)  bundles a pass')
        for w, (k, b) in sorted(h.items(), key=lambda x: -x[1][0]):
            print(f'  {WHY.get(w, w):8} {100 * k / tot:9.1f}  {b / max(k, 1):14.0f}')
    if os.path.exists(a.base + '.mbox'):
        rec = 2 + 512 + 6 * 256
        raw = open(a.base + '.mbox', 'rb').read()
        pcs = collections.defaultdict(collections.Counter)
        wpcs = collections.defaultdict(collections.Counter)
        for i in range(len(raw) // (rec * 4)):
            e, pc = struct.unpack_from('<2I', raw, (i * rec + 2) * 4)[0], struct.unpack_from('<I', raw, (i * rec + 258) * 4)[0]
            if e == 0 and pc == 0:
                continue
            pcs[e & 0xff][pc] += 1
            row = struct.unpack_from('<512I', raw, (i * rec + 2) * 4)
            for c in range(1, 256):   # every worker that ran this pass: its end and pc
                if row[c] >> 8:
                    wpcs[row[c] & 0xff][row[256 + c]] += 1
        for w in (1, 3, 2):
            if not pcs[w]:
                continue
            tot = sum(pcs[w].values())
            print(f'core 0 at a {WHY[w]} end: {tot} passes, the commonest pcs')
            for pc, k in pcs[w].most_common(a.top):
                print(f'  {pc:08x} {100 * k / tot:5.1f}%')
        for w in (2, 3, 1):
            if not wpcs[w]:
                continue
            tot = sum(wpcs[w].values())
            print(f'the workers at a {WHY[w]} end: {tot} ends, the commonest pcs')
            for pc, k in wpcs[w].most_common(a.top):
                print(f'  {pc:08x} {100 * k / tot:5.1f}%')


if __name__ == '__main__':
    main()
