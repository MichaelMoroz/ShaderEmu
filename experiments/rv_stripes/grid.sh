#!/bin/bash
# The grid over each load's trace (WSL); one job a load and part. Output grid.txt beside this script.
cd "$(dirname "$0")" && gcc -O2 -o ~/stripes_trace/sim sim.c -Wno-unused-result || exit 1
H=$PWD; cd ~/stripes_trace
MONO=""; for e in 168 224 368 512 720; do for sil in 0 1; do MONO="$MONO s=1,e=$e,sil=$sil"; done; done
ST=""
for sil in 0 1; do
  for s in 2 4 8; do for e in 168 224; do
    ST="$ST s=$s,e=$e,r=0,sil=$sil"
    for r in 16 32 64 128; do ST="$ST s=$s,e=$e,r=$r,p=f,sil=$sil s=$s,e=$e,r=$r,p=l,sil=$sil"; done
  done; done
done
for l in gzip png mp3_1 jpeg boot tdawn; do ( ./sim $l.bin $MONO $ST > $l.grid ) & done; wait
for l in gzip png mp3_1 jpeg boot tdawn; do echo "== $l"; cat $l.grid; done > "$H/grid.txt"
