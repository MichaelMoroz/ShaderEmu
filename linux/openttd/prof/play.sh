#!/bin/bash
# one played run on rvc_cpu at the card's speed: IMG TAG "ENV" [PLAY]
IMG=$1; TAG=$2; ENVS=$3; PLAY=${4:-1}; cd /e/Development/Emu/ShaderEmu-ofps; S=build/prof
export MSYS_NO_PATHCONV=1 RVC_ADFAULT=1 RVC_WCACHE=1 RVC_SOUND=1
: > build/prof/$TAG.uart
for i in 1 2 3 4 5 6 7 8; do
  timeout 52 /e/Development/Emu/ShaderEmu-rvbp/bin/rvc_cpu.exe --image build/images/$IMG --quiet --ips 3700000 --pass-ms 0.25 --uart-log $S/$TAG.uart --expect "/ # " --send "nano-X -p & sleep 2; OPENTTD_PLAY=$PLAY $ENVS openttd -g /usr/share/openttd/busy.sav\n" --until "PL-DONE" --wall 45 > $S/$TAG.err 2>&1
  grep -q refused $S/$TAG.err || break; rm -f $S/$TAG.uart; sleep 5
done
python - "$S/$TAG.uart" <<'PY'
import re,sys
t=open(sys.argv[1],encoding='latin-1').read()
rnd=0
for l in t.splitlines():
    m=re.match(r'openttd: play (scroll|zoomed out|windows|windows closed): (\d+) pictures in ([\d.]+) s, ([\d.]+) a second, (\d+) ticks',l)
    if m: print(f"  {m[1]:15s} {m[4]:>6s} pictures/s  {int(m[5])/float(m[3]):.2f} ticks/s")
    m=re.match(r'openttd: play round (\d+): .* ([\d.]+) a second, (\d+) ticks',l)
    if m: print(f"  round {m[1]}: {m[2]} pictures/s")
PY
