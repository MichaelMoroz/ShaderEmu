#!/usr/bin/env python3
"""The lean tick with the last K texels a jump fetched kept in registers: a jump back to one does not wait for the texture (the wait of a jump's fetch is the largest single wait of the tick).
   python make_jt.py DIR K   (K 1 to 4)"""
import os, shutil, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
d, K = sys.argv[1], int(sys.argv[2])
if os.path.exists(d): shutil.rmtree(d)
shutil.copytree(os.path.join(ROOT, 'experiments', 'rvc_lean'), d)
p = os.path.join(d, 'tick.hlsl')
s = open(p, encoding='utf-8', newline='').read()
crlf = '\r\n' in s
s = s.replace('\r\n', '\n')
old = "    [branch] if (t == fw_t + 1) fw0 = fw1; else fw0 = TEX(RAM_ADDR(t));   // (an if: a ?: would read the texel either way)\n"
assert s.count(old) == 1
tests = ''
for k in range(K):
    tests += f"        [branch] if (t == jt_t{k}) fw0 = jt_v{k}; else\n"
shift = ''.join(f"            jt_t{k} = jt_t{k - 1}; jt_v{k} = jt_v{k - 1};\n" for k in range(K - 1, 0, -1))
new = "    [branch] if (t == fw_t + 1) fw0 = fw1; else {\n" + tests + "        {\n            fw0 = TEX(RAM_ADDR(t));\n" + shift + "            jt_t0 = t; jt_v0 = fw0;\n        }\n    }\n"
s = s.replace(old, new)
decl = ''.join(f"static uint jt_t{k} = 0xffffffff; static uint4 jt_v{k} = 0;\n" for k in range(K))
s = s.replace("static uint4 fw0, fw1;\n", "static uint4 fw0, fw1;\n" + decl)
open(p, 'w', encoding='utf-8', newline='').write(s.replace('\n', '\r\n') if crlf else s)
print('wrote', d)
