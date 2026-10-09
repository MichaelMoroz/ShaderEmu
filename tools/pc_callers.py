"""Who calls the functions a guest spends its instructions in, from the harness's --ra-log.

    python tools/pc_callers.py RA.LOG game=ralert.nm [kernel=vmlinux.nm] [-n 40] [-f NAME]

The log has three words a machine frame: the pc it ended at, the return address register and
the instructions it ran, all charged to that pc. The return address is the caller only while
a function that calls nothing runs (memset, a small loop): for others it is whatever they
called last. So read the lines for leaf functions, and take the rest as a hint.
-f NAME lists the callers of functions whose name contains NAME.
"""
import bisect, collections, sys
import numpy as np


def main():
    args = sys.argv[1:]
    top, only = 40, None
    for flag in ('-n', '-f'):
        if flag in args:
            i = args.index(flag)
            if flag == '-n':
                top = int(args[i + 1])
            else:
                only = args[i + 1]
            del args[i:i + 2]
    samples = np.fromfile(args[0], dtype=np.uint32).reshape(-1, 3)
    symbols = []
    for spec in args[1:]:
        name, path = spec.split('=', 1)
        for line in open(path, encoding='latin1'):
            part = line.split()
            if len(part) == 3 and part[1] in 'TtWw':
                symbols.append((int(part[0], 16), part[2]))
    symbols.sort()
    starts = [s[0] for s in symbols]

    def name_of(address):
        i = bisect.bisect_right(starts, int(address)) - 1
        if i < 0 or int(address) - starts[i] > 0x4000:
            return '0x%08x' % (int(address) & ~0xfff)
        return symbols[i][1]

    pairs = collections.Counter()
    for pc, ra, count in samples:
        function = name_of(pc)
        if only is None or only in function:
            pairs[(function, name_of(ra))] += int(count)
    total = int(samples[:, 2].sum())
    print('%d samples, %d instructions' % (len(samples), total))
    for (function, caller), count in pairs.most_common(top):
        print('%5.2f%%  %-44s from %s' % (100.0 * count / total, function[:44], caller))


if __name__ == '__main__':
    main()
