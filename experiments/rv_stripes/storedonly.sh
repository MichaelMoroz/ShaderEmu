#!/bin/bash
# Windows of about 8,000 instructions, by each load's ops a bundle (gvm_cpu's totals); output storedonly.txt.
cd "$(dirname "$0")" && gcc -O2 -o ~/stripes_trace/storedonly storedonly.c -Wno-unused-result || exit 1
H=$PWD; cd ~/stripes_trace
run() { for s in 0 1; do ./storedonly $1.bin $2 $s > $1.so$s & done; }
run gzip 6557; run png 6107; run mp3_1 5161; run jpeg 5128; run boot 5755; run tdawn 6154; wait
for l in gzip png mp3_1 jpeg boot tdawn; do cat $l.so0 $l.so1; done > "$H/storedonly.txt"
