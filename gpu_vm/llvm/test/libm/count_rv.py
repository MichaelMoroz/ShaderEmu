#!/usr/bin/env python3
# RISC-V instructions a call of qkern.c's kernels: qemu-riscv32 runs the program one instruction a block with every
# block logged (-one-insn-per-tb -d exec,nochain), and the instructions between two calls of qk_mark are counted.
#   count_rv.py PROGRAM NM
import re, subprocess, sys, tempfile, os

prog, nm = sys.argv[1], sys.argv[2]
syms = {}
for line in subprocess.run([nm, prog], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16)
mark = syms['qk_mark']
with tempfile.TemporaryDirectory() as d:
    log = os.path.join(d, 'trace.log')
    subprocess.run(['qemu-riscv32', '-one-insn-per-tb', '-d', 'exec,nochain', '-D', log, prog], check=True,
                   stdout=subprocess.DEVNULL)
    pat = re.compile(r'Trace \d+: 0x[0-9a-f]+ \[[0-9a-f]+/([0-9a-f]+)/')
    counts, n, started = [], 0, False
    with open(log) as f:
        for line in f:
            m = pat.search(line)
            if not m:
                continue
            pc = int(m.group(1), 16)
            if pc == mark:
                if started:
                    counts.append(n)
                started, n = True, 0
            n += 1
names = ["VectorNormalize", "MatrixMultiply", "AnglesToAxis", "vectoangles", "TransformVerts (64)",
         "SinTable (1024)", "GammaTable (256)"]
calls = [64, 64, 64, 64, 64, 1, 1]
for name, c, k in zip(names, counts, calls):
    print(f"{name:22s} {c / k:10.1f} instructions a call")
