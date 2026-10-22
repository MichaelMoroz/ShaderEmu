#!/usr/bin/env bash
# The HLSL compiler for gpu_vm (what linux/tcc/build.sh and linux/nommu/root.sh build of it for RISC-V): its IR run by
# programs/hlsl/hlsl_interp.c (HLSL_INTERP, on for __gpuvm__: no back end makes gpu_vm code), libhlsl.a, hlslc,
# hlslview (linux/tcc/examples/fractal.c) and hlsl_bench. Run by build.sh as ../gpu_vm/userland/apps/hlsl.sh.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
source "$REPO/linux/userland/toolchain.sh"
H=$REPO/programs/hlsl M=$REPO/programs/mc GL=$REPO/programs/linux MW=$WORK/src/microwindows/src
B=$WORK/src/hlsl; OUT=$REPO/build/images/linux/root/usr/bin
mkdir -p "$B" "$OUT"
for f in hlsl hlsl_parse hlsl_gen hlsl_back hlsl_draw hlsl_interp; do
    rv32-cc -O2 -Wall -Wno-unused-function -I"$H" -I"$M" -c "$H/$f.c" -o "$B/$f.o"
done
rm -f "$B/libhlsl.a"; riscv32-linux-ar rcs "$B/libhlsl.a" "$B"/hlsl*.o
rv32-cc -O2 -w -I"$M" -c "$M/mcw.c" -o "$B/mcw.o"; rv32-cc -O2 -w -I"$M" -c "$M/mcwx.c" -o "$B/mcwx.o"
rv32-cc -O2 -w -c -DSEGL_FLOAT -idirafter "$GL/include" -I"$MW/include" "$GL/gles.c" -o "$B/gles.o"
rv32-cc -O2 -Wall -I"$H" "$H/hlslc.c" "$B/libhlsl.a" "$B/mcw.o" "$B/mcwx.o" -o "$B/hlslc"
rv32-cc -O2 -w -I"$H" -I"$M" "$H/test/hlsl_bench.c" "$B/libhlsl.a" "$B/mcw.o" "$B/mcwx.o" -lm -o "$B/hlsl_bench"
rv32-cc -O2 -w -I"$H" -I"$M" -idirafter "$GL/include" -I"$MW/include" "$REPO/linux/tcc/examples/fractal.c" "$B/gles.o" \
    "$MW/lib/libnano-X.a" "$B/libhlsl.a" "$B/mcw.o" "$B/mcwx.o" -lm -o "$B/hlslview"
for p in hlslc hlsl_bench hlslview; do riscv32-linux-strip -o "$OUT/$p" "$B/$p"; done
mkdir -p "$OUT/../share"; cp "$REPO/linux/tcc/examples/fractal.hlsl" "$OUT/../share/example-fractal.hlsl"
ls -l "$OUT/hlslc" "$OUT/hlsl_bench" "$OUT/hlslview"
