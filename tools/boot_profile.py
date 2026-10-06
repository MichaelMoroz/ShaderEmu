"""Where a run's time and instructions went, from `rvc_harness --frame-log FILE`.

    python tools/boot_profile.py FILE [--ticks N] [--window SECONDS] [name=symbols.nm ...]

Prints instructions/s per window and their median, frames and instructions by the reason a
frame ended early, and (given `nm -n` symbol files) the functions frames most often ended in.
"""
import bisect, sys
import numpy as np

STALLS = {0: 'none', 1: 'exit call', 2: 'CSR cache full', 3: 'write cache full', 4: 'reserved', 5: 'entry point',
          6: 'UART', 7: 'fence', 8: 'memory copy', 9: 'wfi/pause', 10: 'memory fill'}

args = sys.argv[1:]
ticks, window, syms = 16384, 0.1, []
path = args.pop(0)
while args:
    a = args.pop(0)
    if a == '--ticks':
        ticks = int(args.pop(0))
    elif a == '--window':
        window = float(args.pop(0))
    else:
        syms.append(a.split('=', 1))

d = np.fromfile(path, dtype=np.uint32).reshape(-1, 4)
pc, instr, stall, us = d[:, 0], d[:, 1].astype(np.int64), d[:, 2], d[:, 3].astype(np.float64) / 1e6
total, seconds = int(instr.sum()), float(us[-1] - us[0])
print('%d frames, %.2f s, %d instructions, %.0fk instructions/s overall, %.0f per frame' %
      (len(d), seconds, total, total / seconds / 1e3, total / len(d)))

edges = np.arange(us[0], us[-1], window)
idx = np.searchsorted(us, edges)
cum = np.concatenate([[0], np.cumsum(instr)])
rates = (cum[idx[1:]] - cum[idx[:-1]]) / window
if len(rates):
    q = np.percentile(rates, [10, 25, 50, 75, 90]) / 1e6
    print('instructions/s per %.2f s window: median %.2fM (10%% %.2fM, 25%% %.2fM, 75%% %.2fM, 90%% %.2fM)' %
          (window, q[2], q[0], q[1], q[3], q[4]))
    step = max(1, len(rates) // 40)
    print('timeline (M/s): ' + ' '.join('%.1f' % (rates[i:i + step].mean() / 1e6) for i in range(0, len(rates), step)))

short = instr < ticks
print('frames that ended early: %d of %d, carrying %.1f%% of instructions' %
      (short.sum(), len(d), 100.0 * instr[short].sum() / total))
for code in np.unique(stall[short]):
    m = short & (stall == code)
    print('  %-16s %6d frames  %5.1f%% of instructions  %6.0f per frame' %
          (STALLS.get(int(code), str(code)), m.sum(), 100.0 * instr[m].sum() / total, instr[m].mean()))

if syms:
    table = []
    for name, f in syms:
        for line in open(f):
            p = line.split()
            if len(p) == 3 and p[1] in 'tTwW':
                table.append((int(p[0], 16), name, p[2]))
    table.sort()
    addrs = [t[0] for t in table]
    counts = {}
    for a, n, s in zip(pc, instr, stall):
        i = bisect.bisect_right(addrs, int(a)) - 1
        key = (table[i][1], table[i][2], STALLS.get(int(s), str(s)) if n < ticks else 'full') if i >= 0 else ('?', hex(int(a)), '')
        c = counts.setdefault(key, [0, 0])
        c[0] += 1
        c[1] += int(n)
    print('where frames ended:')
    for key, c in sorted(counts.items(), key=lambda kv: -kv[1][0])[:25]:
        print('  %6d frames %5.1f%% instr  %-8s %-32s %s' % (c[0], 100.0 * c[1] / total, key[0], key[1], key[2]))
