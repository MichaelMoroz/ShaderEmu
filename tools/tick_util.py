"""Tick utilization of a run, from `rvc_harness --frame-log FILE` (tools/tick_util.ps1 runs both).

    python tools/tick_util.py FILE [--ticks 8192] [--top N] [--json OUT] kernel=vmlinux.nm [name=prog.nm ...]

Utilization is the instructions busy passes ran over what they could have run. A pass that
ended early is listed by why; a wfi or pause by where the guest was: the kernel's idle loop
(nothing to run: counted apart, not a loss), a rest the program asked the kernel for (the GPU
driver's rest call, docs/machine/multicore.md: it had nothing to do; counted apart as the idle passes
are, on a line of its own), a wait in the GPU driver, elsewhere in the kernel, or a program's
own pause, which stays a busy pass. Symbol files are `nm -n` output.
"""
import bisect, json, sys
import numpy as np

STALLS = {0: 'none', 1: 'exit call', 2: 'CSR cache full', 3: 'write cache full', 6: 'UART', 7: 'fence.i',
          8: 'parallel copy', 9: 'wfi/pause', 10: 'parallel fill'}
IDLE = ('arch_cpu_idle', 'default_idle_call', 'do_idle', 'cpu_startup_entry', 'cpuidle_idle_call')
REST = 'rest: asked for by the program'    # (only a pass that ended in the driver's rest call)
APART = ('wfi: kernel idle', REST)         # passes with nothing to run: not busy ones

args = sys.argv[1:]
path = args.pop(0)
ticks, top, out, syms = 8192, 12, None, []
while args:
    a = args.pop(0)
    if a == '--ticks': ticks = int(args.pop(0))
    elif a == '--top': top = int(args.pop(0))
    elif a == '--json': out = args.pop(0)
    else: syms.append(a.split('=', 1))

tables = {}
for name, f in syms:
    t = []
    for line in open(f):
        p = line.split()
        if len(p) == 3 and p[1] in 'tTwW':
            t.append((int(p[0], 16), p[2]))
    t.sort()
    tables.setdefault('kernel' if name == 'kernel' else 'user', []).append((name, [x[0] for x in t], [x[1] for x in t]))

KERNEL_LO, KERNEL_HI = 1, 0
for name, addrs, names in tables.get('kernel', []):
    if addrs and addrs[0] < 0xc0000000:
        KERNEL_LO, KERNEL_HI = addrs[0], addrs[-1] + 0x4000

def where(pc):
    # (the no-MMU kernel lies at the start of RAM and programs above it: by its own names' range, past the MMU's 0xc0000000 too)
    kind = 'kernel' if pc >= 0xc0000000 or KERNEL_LO <= pc < KERNEL_HI else 'user'
    best = None
    for name, addrs, names in tables.get(kind, []):
        i = bisect.bisect_right(addrs, pc) - 1
        if i >= 0 and (best is None or pc - addrs[i] < best[0]):
            best = (pc - addrs[i], name, names[i])
    return (kind, best[1], best[2]) if best and best[0] < 0x4000 else (kind, '?', hex(pc & ~0xfff))

d = np.fromfile(path, dtype=np.uint32).reshape(-1, 4)
rows = {}      # reason -> [passes, lost]
places = {}    # (reason, place) -> [passes, lost]
total_i = int(d[:, 1].astype(np.int64).sum())
full = busy_i = 0
for pc, n, stall, us in d:
    pc, n, stall = int(pc), int(n), int(stall)
    if n >= ticks:
        full += 1
        busy_i += n
        continue
    kind, prog, sym = where(pc)
    reason = STALLS.get(stall, str(stall))
    if stall == 9:
        if kind == 'kernel':
            reason = ('wfi: kernel idle' if sym in IDLE else REST if sym == 'shaderemu_gpu_rest' else
                      'wfi: GPU driver wait' if 'shaderemu_gpu' in sym else 'wfi: kernel, ' + sym)
        else:
            reason = 'pause: program'
    elif kind == 'kernel' and sym == 'zeroed_idle':
        reason = 'wfi: kernel idle'    # (the idle loop clearing memory ahead: docs/machine/boot.md)
    r = rows.setdefault(reason, [0, 0]); r[0] += 1; r[1] += ticks - n
    if reason not in APART:
        busy_i += n
    p = places.setdefault((reason, prog + ':' + sym), [0, 0]); p[0] += 1; p[1] += ticks - n

passes = len(d)
idle = rows.get('wfi: kernel idle', [0, 0])
rest = rows.get(REST, [0, 0])
busy = passes - idle[0] - rest[0]
lost = sum(v[1] for k, v in rows.items() if k not in APART)
print('%s: %d passes, %.2f s, %d instructions; %d full, %d idle, %d rested, %d busy' %
      (path.replace('\\', '/').split('/')[-1], passes, (int(d[-1, 3]) - int(d[0, 3])) / 1e6, total_i, full, idle[0], rest[0], busy))
print('UTILIZATION %.1f%%: busy passes ran %.0f instructions each of %d; lost to early ends %d (idle and rested passes apart)' %
      (100.0 * busy_i / max(busy * ticks, 1), busy_i / max(busy, 1), ticks, lost))
if rest[0]:
    print('REST %d passes (%.1f%% of all): the program asked the kernel for a rest, having run %.0f instructions of the pass' %
          (rest[0], 100.0 * rest[0] / passes, ticks - rest[1] / rest[0]))
print('%-34s %8s %7s %12s %7s' % ('reason', 'passes', 'share', 'instr lost', 'of busy'))
for k, v in sorted(rows.items(), key=lambda kv: -kv[1][1]):
    print('%-34s %8d %6.1f%% %12d %6.1f%%' % (k, v[0], 100.0 * v[0] / passes, v[1], 0.0 if k in APART else 100.0 * v[1] / max(busy * ticks, 1)))
print('where (top %d by instructions lost):' % top)
for (reason, place), v in sorted(places.items(), key=lambda kv: -kv[1][1])[:top]:
    print('  %6d passes %10d lost  %-24s %s' % (v[0], v[1], reason, place))
if out:
    json.dump({'passes': passes, 'full': full, 'rested': rest[0],'instructions': total_i, 'rows': rows,
               'places': {k[0] + ' @ ' + k[1]: v for k, v in places.items()}}, open(out, 'w'), indent=1)

# The worker cores, when the harness wrote FILE.workers: a pass's instructions on them, how
# many ran, how many ran the whole pass, how many were awake with no job (under 64
# instructions: apart, as idle). Why a worker's pass was short the log does not say.
import os
if os.path.exists(path + '.workers'):
    w = np.fromfile(path + '.workers', dtype=np.uint32).reshape(-1, 4).astype(np.int64)
    ran, cores, whole, waited = (int(w[:, k].sum()) for k in range(4))
    if cores:
        busy_passes = int((w[:, 1] > 0).sum())
        print('WORKERS %.1f%%: %d instructions in %d worker passes (%.1f workers a pass in the %d passes any worked), %.1f%% of them '
              'whole; %d more worker passes only waited for a job' %
              (100.0 * ran / (cores * ticks), ran, cores, cores / max(busy_passes, 1), busy_passes, 100.0 * whole / cores, waited))
        part = cores - whole
        if part:
            print('  the %d that were not whole ran %.0f instructions each' % (part, (ran - whole * ticks) / part))
