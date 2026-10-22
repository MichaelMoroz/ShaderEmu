"""Passes of a gvm_gpu --trace-pass run of the bench's Linux (run_gvm.ps1 -Trace): for the boot and each program
init.c ran (by the guest's times init.c prints, TIME's 100 kHz), the passes, bundles a pass and why passes ended.
    python passes.py TRACE.log"""
import re, sys, collections

text = open(sys.argv[1], encoding='utf-8', errors='replace').read()
passes = [(int(t), int(b), e) for t, b, e in re.findall(r'\[pass \d+\] TIME (\d+) pc \w+ bundles (\d+) end (\S+)', text)]
init = re.search(r'BENCH-INIT \(guest ([\d.]+) s\)', text)
# CLOCK_MONOTONIC is TIME less the passes before the kernel's clock began: the first pass after init's line
off = int(re.compile(r'\[pass \d+\] TIME (\d+)').search(text, init.end()).group(1)) / 1e5 - float(init.group(1))
segs = [('boot', -off, float(init.group(1)))]
for name, start in re.findall(r'BENCH-START (\S+) \(guest ([\d.]+) s\)', text):
    dur = float(re.search(r'BENCH-END %s: exit \d+, guest ([\d.]+) s' % re.escape(name), text).group(1))
    segs.append((name, float(start), float(start) + dur))
print('%-14s %8s %11s %8s  %s' % ('', 'passes', 'bundles', 'a pass', 'ended by'))
for name, a, b in segs:
    sel = [(n, e) for t, n, e in passes if (a + off) * 1e5 <= t < (b + off) * 1e5]
    ends = collections.Counter(e for _, e in sel)
    total = sum(n for n, _ in sel)
    print('%-14s %8d %11d %8.0f  %s' % (name, len(sel), total, total / max(len(sel), 1),
          ', '.join('%s %d (%.0f%%)' % (k, v, 100 * v / len(sel)) for k, v in ends.most_common())))
