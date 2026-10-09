"""Where inside a function a guest's instructions go, from the harness's --pc-log.

    python tools/pc_hot_in.py PC.LOG PROGRAM.nm NAME [NAME ...] [-n 12]

For each function whose (mangled) name contains NAME: its share of all instructions and the
addresses in it that the most samples ended at. Take a log with a small --ticks, and read the
addresses in `objdump -d` of the unstripped program.
"""
import bisect, collections, sys
import numpy as np


def main():
    args = sys.argv[1:]
    top = 12
    if '-n' in args:
        i = args.index('-n')
        top = int(args[i + 1])
        del args[i:i + 2]
    samples = np.fromfile(args[0], dtype=np.uint32).reshape(-1, 2)
    symbols = []
    for line in open(args[1], encoding='latin1'):
        part = line.split()
        if len(part) == 3 and part[1] in 'TtWw':
            symbols.append((int(part[0], 16), part[2]))
    symbols.sort()
    starts = [s[0] for s in symbols]
    total = int(samples[:, 1].sum())
    for want in args[2:]:
        hot = collections.Counter()
        for pc, count in samples:
            i = bisect.bisect_right(starts, int(pc)) - 1
            if i >= 0 and want in symbols[i][1]:
                hot[(symbols[i][1], symbols[i][0], int(pc))] += int(count)
        print('%s: %.2f%% of %d instructions' % (want, 100.0 * sum(hot.values()) / total, total))
        for (name, start, pc), count in hot.most_common(top):
            print('   %5.2f%%  %08x  %s+0x%x' % (100.0 * count / total, pc, name[-40:], pc - start))


if __name__ == '__main__':
    main()
