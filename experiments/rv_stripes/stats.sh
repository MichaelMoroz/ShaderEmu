#!/bin/bash
# Read-backs of unlimited passes, each load (WSL).
cd "$(dirname "$0")" && gcc -O2 -o ~/stripes_trace/sim sim.c -Wno-unused-result || exit 1
cd ~/stripes_trace
for l in gzip png mp3_1 jpeg boot tdawn; do ( ./sim $l.bin > $l.stats ) & done; wait
for l in gzip png mp3_1 jpeg boot tdawn; do echo "== $l"; cat $l.stats; done
