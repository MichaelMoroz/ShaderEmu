#!/usr/bin/env bash
# The second home's game, flat, into a root: flat_home2.sh ROOT
set -euo pipefail
ROOT=$1; B=$HOME/agents/fewer_second_home
export PATH=$HOME/shaderemu-linux/toolchain/bin:$PATH
T=$B/flat_tmp; rm -rf "$T"; mkdir -p "$T/usr/lib/tdawn"
cp "$B/game" "$T/usr/lib/tdawn/game"
python3 linux/nommu/flat.py "$T" > /dev/null
cp "$T/usr/lib/tdawn/game" "$ROOT/usr/lib/tdawn/game"
sha1sum "$ROOT/usr/lib/tdawn/game"; ls -l "$ROOT/usr/lib/tdawn/game" | cut -c20-
# what differs from the first home's build
python3 - "$B/nm/work/src/tdawn-build/vanillatd" "$HOME/agents/fewer/nm/work/src/tdawn-build/vanillatd" <<'PY'
import subprocess,sys,collections,os
nm=os.path.expanduser('~/shaderemu-linux/toolchain/bin/riscv32-linux-nm')
def r(f):
    d=collections.Counter()
    for l in subprocess.run([nm,'-S','-C',f],capture_output=True,text=True).stdout.split('\n'):
        p=l.split(None,3)
        if len(p)==4 and p[2] in 'tT': d[p[3]]+=int(p[1],16)
    return d
a,b=r(sys.argv[1]),r(sys.argv[2])
print('functions',len(a),len(b),'bytes',sum(a.values()),sum(b.values()))
for x in sorted(set(a)|set(b),key=lambda x:-abs(a[x]-b[x]))[:14]: print(a[x],b[x],x[:64])
PY
