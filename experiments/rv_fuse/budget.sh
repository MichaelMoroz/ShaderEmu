#!/bin/bash
# How a longer pass in RISC-V instructions (8,192 trips of fused words) meets the write cache: rvc_cpu with the card's
# cache, --ticks over 8192, each load: passes, instructions a pass, passes ended full. No profile: plain speed.
export MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 RVC_THREADS=8 RVC_WCACHE=1
EXE=${EXE:-$(cd "$(dirname "$0")/../.." && pwd -W)/bin/rvc_cpu.exe}
IMG=${IMG:-$(cd "$(dirname "$0")/../.." && pwd -W)/build/images/nommu}
one() {
    name=$1; cmd=$2; secs=$3; ticks=$4
    o=$("$EXE" --image "$IMG" --quiet --ticks "$ticks" --expect "/ # " --send "echo FL-''GO; $cmd; echo LX-''DONE\n" --count-from FL-GO --until LX-DONE --seconds "$secs" 2>&1)
    c=$(echo "$o" | grep '^COUNT')
    f=$(echo "$o" | grep 'write cache:')
    echo "$name ticks=$ticks | $c" | sed 's/COUNT from "FL-GO": //' | cut -c1-140
    echo "    $f" | cut -c1-110
}
for t in 8192 10000 11000 12000 16384; do
    one shell 'i=0; while [ $i -lt 3000 ]; do i=$((i+1)); done' 60 $t
    one doom 'nano-X -p & sleep 2; DOOM_VIEW=told doom -1 -timedemo demo1' 400 $t
    one quake 'nano-X -p & sleep 2; QUAKE_STATS_MS=5000 quake +map e1m1 & sleep 40' 400 $t
    one nxray 'nano-X -p & sleep 2; RAY_FRAMES=8 nxray' 400 $t
done
