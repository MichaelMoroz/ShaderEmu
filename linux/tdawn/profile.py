"""What a played game ran, for the compiler (docs/games/tdawn.md, "What the compiler is told").
    profile.py make OUT.profile LOG ...   counting runs' 'GC' lines (host.c), summed, as one text file
    profile.py put IN.profile BUILD       that file as GCC 13's .gcda files where a build in BUILD looks
"""
import os
import struct
import sys

VALUES = (3, 4)    # counter kinds that keep values met (three words a place in memory): topn, indirect calls
TIME = 7           # and the order functions were first run in: the first run's is kept


def read(lines, files, relative):
    """files: name -> [version, stamp, checksum, kinds, {ident: fn}, [idents]]; fn: [lineno, cfg, {kind: [count, {at: n}]}].
    For the kinds of VALUES, n is [times, {value: times}]."""
    cur = fn = ctr = place = None
    seen = fresh = False
    for line in lines:
        at = line.find('GC ')
        if at < 0:
            continue
        p = line[at:].split()
        if p[1] == 'F' and len(p) == 7:
            name = p[2].split('-build/', 1)[-1] if relative else p[2]
            cur = files.setdefault(name, [int(p[3], 16), int(p[4], 16), int(p[5], 16), int(p[6], 16), {}, []])
            if cur[1] != int(p[4], 16):
                sys.exit('runs of two builds: ' + name)
            seen = True
        elif p[1] == 'f' and len(p) == 5 and cur:
            ident = int(p[2], 16)
            fresh = ident not in cur[4]
            if fresh:
                cur[4][ident] = [int(p[3], 16), int(p[4], 16), {}]
                cur[5].append(ident)
            fn = cur[4][ident]
        elif p[1] == 'k' and len(p) == 4 and fn:
            kind = int(p[2])
            ctr = fn[2].setdefault(kind, [int(p[3]), {}])
            if kind == TIME and not fresh:
                ctr = [0, {}]    # (a later run's order is not added to the first's)
        elif p[1] == 'c' and len(p) == 4 and ctr:
            ctr[1][int(p[2], 16)] = ctr[1].get(int(p[2], 16), 0) + int(p[3], 16)
        elif p[1] == 't' and len(p) == 4 and ctr:
            place = ctr[1].setdefault(int(p[2], 16), [0, {}])
            place[0] += int(p[3], 16)
        elif p[1] == 'v' and len(p) == 4 and place:
            place[1][int(p[2], 16)] = place[1].get(int(p[2], 16), 0) + int(p[3], 16)
    return seen


def make(out, logs):
    files, runs = {}, 0
    for log in logs:
        runs += read(open(log, encoding='latin-1'), files, True)
    if not runs:
        sys.exit('no counting run in the logs: nothing written')
    with open(out, 'w', newline='\n') as f:
        f.write('GC runs %d\n' % runs)
        for name in sorted(files):
            version, stamp, checksum, kinds, fns, order = files[name]
            f.write('GC F %s %x %x %x %x\n' % (name, version, stamp, checksum, kinds))
            for ident in order:
                lineno, cfg, ctrs = fns[ident]
                f.write('GC f %x %x %x\n' % (ident, lineno, cfg))
                for kind in sorted(ctrs):
                    count, values = ctrs[kind]
                    f.write('GC k %d %d\n' % (kind, count))
                    for at in sorted(values):
                        if kind in VALUES:
                            f.write('GC t %x %x\n' % (at, values[at][0]))
                            for value, times in sorted(values[at][1].items(), key=lambda kv: -kv[1])[:32]:
                                f.write('GC v %x %x\n' % (value, times))
                        else:
                            f.write('GC c %x %x\n' % (at, values[at]))
    print('%d runs, %d files, %d functions' % (runs, len(files), sum(len(v[4]) for v in files.values())))


def put(profile, build):
    files = {}
    lines = open(profile, encoding='latin-1').read().split('\n')
    runs = int(lines[0].split()[2])
    read(lines[1:], files, False)
    for name, (version, stamp, checksum, kinds, fns, order) in files.items():
        most = max([max(c[1].values()) for f in fns.values() for k, c in f[2].items() if k == 0 and c[1]] or [0])
        out = [struct.pack('<IIII', 0x67636461, version, stamp, checksum),
               struct.pack('<IIII', 0xa1000000, 8, runs, min(most, 0xffffffff))]
        for ident in order:
            lineno, cfg, ctrs = fns[ident]
            out.append(struct.pack('<IIIII', 0x01000000, 12, ident, lineno, cfg))
            for kind in range(8):
                if not kinds >> kind & 1:
                    continue
                tag = 0x01a10000 + (kind << 17)
                count, values = ctrs.get(kind, [0, {}])
                if kind in VALUES:
                    # on disk a place is: how often, how many values, then each value and how often
                    words = []
                    for at in range(count // 3):
                        times, met = values.get(at, [0, {}])
                        words += [times, len(met)] + [w for pair in met.items() for w in pair]
                    out.append(struct.pack('<Ii', tag, 8 * len(words)) + b''.join(struct.pack('<Q', w) for w in words))
                elif count and values:
                    out.append(struct.pack('<Ii', tag, 8 * count))
                    out.append(b''.join(struct.pack('<Q', values.get(i, 0)) for i in range(count)))
                else:
                    out.append(struct.pack('<Ii', tag, -8 * count))    # (all zero: only their number)
        out.append(struct.pack('<I', 0))
        path = os.path.join(build, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'wb').write(b''.join(out))
    # A .gcda the profile does not hold is an older count of a source changed since: GCC, told to use
    # mismatched counts, met Red Alert's tab.cpp.gcda of a counting run before the window's-size layout
    # and died (an internal compiler error) in every build from 3 November 2026 on. Only the profile's own.
    kept = {os.path.normpath(os.path.join(build, n)) for n in files}
    stale = 0
    for folder, _, names in os.walk(build):
        for n in names:
            p = os.path.normpath(os.path.join(folder, n))
            if n.endswith('.gcda') and p not in kept:
                os.remove(p)
                stale += 1
    print('%d .gcda files in %s%s' % (len(files), build, ', %d stale removed' % stale if stale else ''))


if __name__ == '__main__':
    if len(sys.argv) >= 4 and sys.argv[1] == 'make':
        make(sys.argv[2], sys.argv[3:])
    elif len(sys.argv) == 4 and sys.argv[1] == 'put':
        put(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
