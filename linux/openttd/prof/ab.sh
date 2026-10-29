#!/bin/bash
cd /e/Development/Emu/ShaderEmu-ofps
export RVCMP_MACHINES='om3=E:\Development\Emu\ShaderEmu-omus\build\images\om3@E:\Development\Emu\ShaderEmu-rvbp\build\after,of8=E:\Development\Emu\ShaderEmu-ofps\build\images\of8@E:\Development\Emu\ShaderEmu-rvbp\build\after'
for i in $(seq 1 80); do
  out=$(timeout 600 python tools/rv_compare/run.py --machines om3,of8 --rounds 2 --parts openttd-play,openttd --sections 1 --pause 3 2>&1)
  echo "$(date +%T) $i: $(echo "$out" | tail -2 | tr '\n' ' ' | cut -c1-200)"
  echo "$out" | grep -qi "nothing\|all done\|no jobs" && break
  sleep 4
done
echo AB-END
