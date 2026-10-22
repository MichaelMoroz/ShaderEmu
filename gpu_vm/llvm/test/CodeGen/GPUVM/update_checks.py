#!/usr/bin/env python3
# Writes a CodeGen test's CHECK lines again from llc's output (update_llc_test_checks.py has no gpuvm scrubber):
# CHECK-LABEL for each function, a CHECK for each instruction, comments and directives left out. Look at the
# difference before committing it (gpu_vm/llvm/README.md, "Test").
#   python3 update_checks.py TEST.ll [LLC]
import re, subprocess, sys

path = sys.argv[1]
llc = sys.argv[2] if len(sys.argv) > 2 else 'llc'
src = [l for l in open(path).read().splitlines() if not re.match(r'^; CHECK(-LABEL|-NEXT)?:', l)]
run = next(l for l in src if l.startswith('; RUN:'))
args = run.split('llc', 1)[1].split('<')[0].split()
asm = subprocess.run([llc] + args, input='\n'.join(src) + '\n', capture_output=True, text=True, check=True).stdout
body, cur = {}, None
for line in asm.splitlines():
    m = re.match(r'^([A-Za-z_][\w.$]*):', line)
    if m:
        cur = m.group(1)
        body[cur] = []
        continue
    if cur is None:
        continue
    if 'End function' in line:
        cur = None
        continue
    t = line.split('#')[0].strip()
    if t and not t.startswith('.'):
        body[cur].append(re.sub(r'\s+', ' ', t))
out = []
for line in src:
    out.append(line)
    m = re.match(r'define .*@([\w.$]+)\(', line)
    if m and m.group(1) in body:
        out.append(f'; CHECK-LABEL: {m.group(1)}:')
        out += [f'; CHECK: {t}' for t in body[m.group(1)]]
open(path, 'w').write('\n'.join(out) + '\n')
