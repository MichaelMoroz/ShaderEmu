"""How concentrated a guest's instructions are, from the harness's --pc-log samples: how many
places in the code cover half, 80% and 90% of what ran, and how large they are.

    python tools/pc_hot.py PC.LOG [PC.LOG ...] [--gap 32] [--top 25]

Sample finely: `--ticks 29 --fixed-dt 0.00005664` is a sample every 29 instructions with the
guest's clock running as fast per instruction as at `--ticks 2048 --fixed-dt 0.004` (a small
--ticks alone makes timer interrupts 70 times as frequent). Each sample's instructions are
charged to the pc the frame ended at.

A "region" is a run of sampled addresses with no gap larger than --gap bytes: roughly a loop
or a function's hot part. Addresses are the guest's own (virtual under Linux), so two programs
at the same addresses count as one.
"""
import sys
import numpy as np


def cover(weights, total, shares=(0.5, 0.8, 0.9, 0.95)):
    """For each share, how many of the heaviest items reach it."""
    order = np.sort(weights)[::-1]
    cum = np.cumsum(order)
    return [int(np.searchsorted(cum, s * total) + 1) for s in shares]


def main():
    args = sys.argv[1:]
    gap, top = 32, 25
    for name in ('--gap', '--top'):
        if name in args:
            i = args.index(name)
            value = int(args[i + 1])
            del args[i:i + 2]
            if name == '--gap':
                gap = value
            else:
                top = value
    for path in args:
        samples = np.fromfile(path, dtype=np.uint32).reshape(-1, 2)
        samples = samples[samples[:, 1] > 0]
        pcs, inverse = np.unique(samples[:, 0], return_inverse=True)
        weights = np.bincount(inverse, weights=samples[:, 1].astype(np.float64))
        total = weights.sum()
        print('%s: %d samples, %d instructions, %d distinct addresses' % (path, len(samples), total, len(pcs)))

        n = cover(weights, total)
        print('  addresses for 50/80/90/95%% of instructions: %d / %d / %d / %d' % tuple(n))

        # regions: consecutive sampled addresses no more than `gap` bytes apart
        breaks = np.nonzero(np.diff(pcs.astype(np.int64)) > gap)[0] + 1
        starts = np.concatenate(([0], breaks))
        ends = np.concatenate((breaks, [len(pcs)]))
        r_weight = np.add.reduceat(weights, starts)
        r_lo = pcs[starts]
        r_hi = pcs[ends - 1]
        r_size = (r_hi.astype(np.int64) - r_lo.astype(np.int64)) // 4 + 1
        order = np.argsort(r_weight)[::-1]
        cum = np.cumsum(r_weight[order])
        sizes = np.cumsum(r_size[order])
        print('  %d regions (gap %d bytes)' % (len(order), gap))
        for share in (0.5, 0.8, 0.9, 0.95):
            k = int(np.searchsorted(cum, share * total) + 1)
            print('    %2d%% of instructions: %5d regions, %6d instructions of code' % (share * 100, k, sizes[k - 1]))
        for k in (16, 64, 256, 1024):
            if k <= len(order):
                print('    the %4d heaviest regions: %5.1f%%, %6d instructions of code' % (k, 100 * cum[k - 1] / total, sizes[k - 1]))
        print('  heaviest regions:')
        for i in order[:top]:
            print('    %5.2f%%  0x%08x-0x%08x  %4d instructions' % (100 * r_weight[i] / total, r_lo[i], r_hi[i], r_size[i]))
        print()


if __name__ == '__main__':
    main()
