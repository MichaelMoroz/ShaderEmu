#!/bin/bash
# The oracle's ceiling and the filter's sizes (WSL); output more.txt.
cd "$(dirname "$0")" && gcc -O2 -o ~/stripes_trace/oracle oracle.c -Wno-unused-result && H=$PWD; cd ~/stripes_trace
F=""; for sil in 0 1; do for ff in 256 1024 4096 16384; do F="$F s=4,e=168,r=128,p=l,sil=$sil,f=$ff s=8,e=168,r=128,p=l,sil=$sil,f=$ff"; done; F="$F s=4,e=168,r=128,p=l,sil=$sil,f=4096,counted=1,cap=70"; done
for l in gzip png mp3_1 jpeg boot tdawn; do ( ( ./oracle $l.bin 0 32 64 128 168 224; ./oracle $l.bin 1 32 64 128 168 224; ./sim $l.bin $F ) > $l.more ) & done; wait
for l in gzip png mp3_1 jpeg boot tdawn; do echo "== $l"; cat $l.more; done > "$H/more.txt"
