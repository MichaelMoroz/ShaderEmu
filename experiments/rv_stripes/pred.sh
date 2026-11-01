#!/bin/bash
# What decides which store is kept by all, at sizes that fit the array (WSL); output pred.txt.
cd "$(dirname "$0")" && gcc -O2 -o ~/stripes_trace/sim sim.c -Wno-unused-result || exit 1
H=$PWD; cd ~/stripes_trace
P=""
for sil in 0 1; do for shape in "s=4,e=76,r=128" "s=8,e=48,r=128" "s=4,e=116,r=64" "s=8,e=77,r=64" "s=8,e=168,r=128"; do
  P="$P $shape,p=l,sil=$sil,stk=512 $shape,p=l,sil=$sil,stk=4096"
  for pr in "pred=2048,g=0" "pred=2048,g=2" "pred=2048,g=4" "pred=8192,g=0" "pred=8192,g=2"; do P="$P $shape,p=l,sil=$sil,$pr $shape,p=l,sil=$sil,$pr,stk=512 $shape,p=l,sil=$sil,$pr,stk=512,age=256"; done
done; done
for l in gzip png mp3_1 jpeg boot tdawn; do ( ./sim $l.bin $P > $l.pred ) & done; wait
for l in gzip png mp3_1 jpeg boot tdawn; do echo "== $l"; cat $l.pred; done > "$H/pred.txt"
