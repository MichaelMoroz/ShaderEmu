#!/bin/bash
# The study's four loads profiled on rvc_cpu (runprof.sh): shell, doom, quake, nxray.
here=$(dirname "$0")
OUT=${OUT:-C:/Users/micha/agents_isa/prof}
bash $here/runprof.sh shell 'i=0; while [ $i -lt 3000 ]; do i=$((i+1)); done' 60 | head -1
bash $here/runprof.sh doom 'nano-X -p & sleep 2; DOOM_VIEW=told doom -1 -timedemo demo1' 400 | head -1
bash $here/runprof.sh quake 'nano-X -p & sleep 2; QUAKE_STATS_MS=5000 quake +map e1m1 & sleep 40' 400 | head -1
bash $here/runprof.sh nxray 'nano-X -p & sleep 2; RAY_FRAMES=8 nxray' 400 | head -1
for n in shell doom quake nxray; do
    echo "== $n"; grep -E "^COUNT|write cache|^\[cpu\] [0-9]+ passes" "$OUT/$n/out.txt" | cut -c1-200
done
