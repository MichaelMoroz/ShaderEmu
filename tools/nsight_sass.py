"""The card's own code (SASS) of every shader of an Nsight Graphics GPU Trace (GPU-VM docs/nsight.md, "SASS"): the trace keeps
the driver's ELF of each shader; ngfx-trace (github.com/xirreal/nsight-gpu-trace, built with cargo) takes it out and
CUDA's nvdisasm disassembles it. Writes NAME.sN.sass beside the trace and prints each shader's kind, registers and
instructions, and for the pixel shaders their opcodes and local memory (LDL/STL: arrays and spills).

    python tools/nsight_sass.py logs/nsight/s1dxil/*.ngfx-gputrace [--top 30]

A few seconds a shader. The trace must be of DXIL (GPU-VM tools/nsight.ps1 -Dxil: tools/gpu_trace.ps1 of rvc_trace12 is DXBC, which has none): of a DXBC one there is no code."""
import argparse
import collections
import glob
import json
import os
import re
import subprocess
import sys

NGFX_TRACE = os.path.expandvars(r'%USERPROFILE%\.cargo\bin\ngfx-trace.exe')
NVDISASM = r'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.8\bin\nvdisasm.exe'
INSTR = re.compile(r'\s*/\*([0-9a-f]{4,})\*/\s+(@!?U?P\w+\s+)?([A-Z0-9_]+)')


def opcodes(path):
    c = collections.Counter()
    with open(path) as f:
        for line in f:
            m = INSTR.match(line)
            if m:
                c[m.group(3)] += 1
    return c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('trace')
    ap.add_argument('--top', type=int, default=30, help='opcodes listed for each pixel shader')
    a = ap.parse_args()
    trace = (glob.glob(a.trace) or [a.trace])[0]
    stem = os.path.splitext(trace)[0]
    for i in range(64):
        out = f'{stem}.s{i}.sass'
        p = subprocess.run([NGFX_TRACE, 'sass', trace, str(i), '--compact', '--nvdisasm', NVDISASM, '--output', out],
                           capture_output=True, text=True, timeout=50)
        if p.returncode:
            if 'out of range' not in p.stderr:
                sys.exit(p.stderr.strip())
            break
        s = json.loads(p.stdout)
        s = s.get('shader', s)
        c = opcodes(out)
        print(f"s{i}: {s.get('shader_type')}, {s.get('registers_per_thread')} registers a thread, {sum(c.values())} "
              f"instructions ({s.get('size')} bytes), LDL {c['LDL']}, STL {c['STL']}: {out}")
        if s.get('shader_type') == 'fragment':
            print('   ' + ', '.join(f'{k} {v}' for k, v in c.most_common(a.top)))


if __name__ == '__main__':
    main()
