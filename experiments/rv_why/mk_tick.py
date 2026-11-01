#!/usr/bin/env python3
"""A copy of experiments/rvc_lean with translated code in its fast loop: python mk_tick.py NAME GEN [--loop FILE] [--switch ATTR]
Makes experiments/rvc_lean_NAME (ignored by git). The folder's stamped sources hold only the hook: aot_gen.inc, which no stamp hashes,
holds the code and the loop, so that each load's and shape's translation needs no WARP run of its own (each is checked by the load's
own state sum on the card). The hook and a kernel-only translation passed mctest-3 on WARP (de732bc8cfb8a1ca's shape)."""
import argparse, os, shutil, sys
HERE = os.path.dirname(os.path.abspath(__file__))
EXP = os.path.dirname(HERE)
ap = argparse.ArgumentParser()
ap.add_argument('name'); ap.add_argument('gen'); ap.add_argument('--switch', default='forcecase'); ap.add_argument('--loop', default=os.path.join(HERE, 'aot_loop.inc'))
a = ap.parse_args()
dst = os.path.join(EXP, 'rvc_lean_' + a.name)
stamp = open(os.path.join(dst, 'warp_ok')).read() if os.path.exists(os.path.join(dst, 'warp_ok')) else ''
shutil.rmtree(dst, ignore_errors=True)
shutil.copytree(os.path.join(EXP, 'rvc_lean'), dst, ignore=shutil.ignore_patterns('warp_ok', '*.md', 'tests'))
if stamp: open(os.path.join(dst, 'warp_ok'), 'w').write(stamp)
with open(os.path.join(dst, 'aot_gen.inc'), 'w', newline='\n') as f:
    f.write(open(a.gen).read()); f.write(f'\n#define AOT_ATTR [{a.switch}]\n'); f.write(open(a.loop).read())
p = os.path.join(dst, 'tick.hlsl')
t = open(p, newline='').read().replace('\r\n', '\n')
old = '\n// ---- The fast loop:'
assert t.count(old) == 1
t = t.replace(old, '\n#include "aot_gen.inc"\n' + old)
old = '[branch] if (!same) { fetch(pc);'
assert t.count(old) == 1
t = t.replace(old, '[branch] if (!same) { aot_run(c, n); [branch] if (n >= budget) break; fetch(pc);')
open(p, 'w', newline='\n').write(t)
print(dst)
