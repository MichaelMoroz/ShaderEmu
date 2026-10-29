#!/bin/bash
# three profiles of the played script's parts, in parallel: IMG TAG
IMG=$1; TAG=$2; cd /e/Development/Emu/ShaderEmu-ofps
export MSYS_NO_PATHCONV=1 RVC_ADFAULT=1 RVC_WCACHE=1 RVC_SOUND=1
CMD="nano-X -p & sleep 2; OPENTTD_PLAY=1 ${EXTRA:-} openttd -g /usr/share/openttd/busy.sav\n"
one() { for i in 1 2 3 4 5 6 7 8; do timeout 55 python tools/prof.py run "E:/Development/Emu/ShaderEmu-ofps/build/prof/$TAG-$1" --exe "E:/Development/Emu/ShaderEmu-rvbp/bin/rvc_cpu.exe" -- --image build/images/$IMG --quiet --ips 3700000 --pass-ms 0.25 --uart-log build/prof/$TAG-$1.uart --expect "/ # " --send "$CMD" --count-from "$2" --until "$3" --wall 50 > build/prof/$TAG-$1.out 2>&1; grep -q "exit 75" build/prof/$TAG-$1.out || break; sleep 6; done; }
rm -rf build/prof/$TAG-*
one scroll "PL-GO" "play scroll:" &
one zoom "play scroll tick" "play zoomed out:" &
one windows "play zoomed out tick" "play windows:" &
wait
for p in scroll zoom windows; do tail -1 build/prof/$TAG-$p.out; timeout 30 python tools/prof.py report "E:/Development/Emu/ShaderEmu-ofps/build/prof/$TAG-$p" --elf ${ELF:-/home/micha/osim/work/src/openttd-build/openttd} --top 60 > build/prof/$TAG-$p.report 2>&1 & done; wait
