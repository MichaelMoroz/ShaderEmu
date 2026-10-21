#!/usr/bin/env bash
# mctest and the machine's elf2flt with the flat drivers, then the build's root copied and every
# program in it made flat: build/nommu/flatroot.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
NM=$HOME/nm; R=$NM/repo; W=$NM/work
mkdir -p "$NM"; exec 9>"$NM/build.lock"; flock 9   # one build of the shared copy at a time (sessions and agents share ~/nm)
export PATH="$W/bin:$HOME/shaderemu-linux/toolchain/bin:$PATH"
RT=$R/build/images/linux/root
rv32-cc -O2 -w -I"$R/programs/mc" "$R/programs/mc/mctest.c" "$R/programs/mc/mcw.c" "$R/programs/mc/mcwx.c" -o "$NM/mctest" 2>&1 | head -5
riscv32-linux-strip -o "$RT/usr/bin/mctest" "$NM/mctest" && echo "mctest built"
# the lanes' test (docs/machine/lanes.md)
rv32-cc -O2 -w -I"$R/programs/mc" "$R/programs/mc/lanetest.c" "$R/programs/mc/mcw.c" -o "$NM/lanetest" 2>&1 | head -5
riscv32-linux-strip -o "$RT/usr/bin/lanetest" "$NM/lanetest" && echo "lanetest built"
# the HLSL compiler, compiling and drawing a shader in the machine (docs/system/hlsl.md)
rv32-cc -O2 -w -fno-math-errno -I"$R/programs/hlsl" -I"$R/programs/mc" "$R/programs/hlsl/test/hlsl_bench.c" \
    "$R"/programs/hlsl/{hlsl,hlsl_parse,hlsl_gen,hlsl_back,hlsl_draw}.c "$R/programs/mc/mcw.c" "$R/programs/mc/mcwx.c" -o "$NM/hlsl_bench" 2>&1 | head -5
riscv32-linux-strip -o "$RT/usr/bin/hlsl_bench" "$NM/hlsl_bench" && echo "hlsl_bench built"
# the machine's own compiler: tcc links a library, elf2flt makes it a flat program
rv32-cc -O2 -Wall "$HERE/elf2flt.c" -o "$NM/elf2flt" && riscv32-linux-strip -o "$RT/usr/bin/elf2flt" "$NM/elf2flt" && echo "elf2flt built"
cp "$W/sysroot/lib/flatcrt.o" "$RT/usr/lib/flatcrt.o"
cp "$HERE/cc" "$RT/usr/bin/cc" && chmod 755 "$RT/usr/bin/cc"
rm -rf "$NM/flatroot"; cp -a "$RT" "$NM/flatroot"
python3 "$HERE/flat.py" "$NM/flatroot"
du -sh "$NM/flatroot"
mkdir -p "$REPO/build/nommu"; rm -rf "$REPO/build/nommu/flatroot"
cp -a "$NM/flatroot" "$REPO/build/nommu/flatroot"
