#!/bin/bash
# A profile of one load on rvc_cpu with the card's write cache: runprof.sh NAME 'COMMAND' SECONDS [RVC_CPU_ARGS...]
# Writes pcs.txt, edges.txt, timeline.bin under $OUT/NAME. EXE and IMG may be set in the environment.
OUT=${OUT:-C:/Users/micha/agents_isa/prof}
EXE=${EXE:-D:/Development/ShaderX86/bin/rvc_cpu.exe}
IMG=${IMG:-D:/Development/ShaderX86/build/images/nommu}
name=$1; cmd=$2; secs=$3; shift 3
mkdir -p "$OUT/$name"
export MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 RVC_THREADS=8 RVC_WCACHE=1 RVC_PROF="$OUT/$name" RVC_PROF_MEM=0
"$EXE" --image "$IMG" --quiet --expect "/ # " --send "echo FL-''GO; $cmd; echo LX-''DONE\n" \
    --count-from FL-GO --until LX-DONE --seconds "$secs" "$@" > "$OUT/$name/out.txt" 2>&1
echo "$name exit $?"
tail -4 "$OUT/$name/out.txt"
