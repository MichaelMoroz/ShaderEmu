"""Where a guest spends its instructions, from the harness's --pc-log samples.

    python tools/pc_profile.py PC.LOG kernel=vmlinux.nm server=nano-X.nm [-n 40]

Each .nm file is `nm -n` output of one program. The log has one sample per emulator frame:
the pc the frame ended at and the instructions it ran, which are all charged to that pc. Run
with a small --ticks for finer samples. Programs must not overlap in address: link one of two
static programs elsewhere (-Wl,-Ttext-segment=...).
"""
import bisect, collections, sys
import numpy as np


def main():
    args = sys.argv[1:]
    top = 40
    if '-n' in args:
        i = args.index('-n')
        top = int(args[i + 1])
        del args[i:i + 2]
    samples = np.fromfile(args[0], dtype=np.uint32).reshape(-1, 2)
    symbols = []
    for spec in args[1:]:
        name, path = spec.split('=', 1)
        for line in open(path, encoding='latin1'):
            part = line.split()
            if len(part) == 3 and part[1] in 'TtWw':
                symbols.append((int(part[0], 16), name, part[2]))
    symbols.sort()
    starts = [s[0] for s in symbols]
    by_function, by_program = collections.Counter(), collections.Counter()
    for pc, count in samples:
        i = bisect.bisect_right(starts, int(pc)) - 1
        # a pc far past the last symbol before it belongs to something without symbols
        if i < 0 or int(pc) - starts[i] > 0x4000:
            program, function = 'unknown', '0x%08x' % (int(pc) & ~0xfff)
        else:
            program, function = symbols[i][1], symbols[i][2]
        by_function[(program, function)] += int(count)
        by_program[program] += int(count)
    total = int(samples[:, 1].sum())
    print('%d samples, %d instructions' % (len(samples), total))
    for program, count in by_program.most_common():
        print('  %-10s %5.1f%%' % (program, 100.0 * count / total))
    print()
    for (program, function), count in by_function.most_common(top):
        print('%5.1f%%  %-8s %s' % (100.0 * count / total, program, function))


if __name__ == '__main__':
    main()
