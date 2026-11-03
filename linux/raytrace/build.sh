#!/usr/bin/env bash
# Builds nxray, the ray tracer that shares its tiles between the worker cores (docs/apps/raytrace.md),
# for the image.   wsl -- bash /mnt/c/.../linux/raytrace/build.sh        (after linux/nanox/build.sh)
# In the guest: nano-X -p & nxray
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
OUT=$REPO/build/images/linux/root
BUILD=$WORK/src/raytrace-build
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
mkdir -p "$BUILD" "$OUT/usr/bin" "$OUT/usr/share"
INC="-idirafter $REPO/programs/linux/include -I$MW/src/include -I$REPO/linux/userland -I$REPO/programs/mc -I$REPO/linux/apps"
# single-precision constants: a float times 0.5 is a double multiplication otherwise, and the
# machine has no doubles. No errno from sqrt: it is the machine's instruction then.
# -O3 with loops unrolled: 148.6 to 140.3 million instructions of four pictures on every core, the same sums
# (rvc_cpu, 3 November 2026; docs/system/flags.md)
rv32-cc -O3 -fno-tree-vectorize -fsched-pressure -funroll-loops -fno-pie -Wall -Wno-unused-function -fsingle-precision-constant -fno-math-errno $INC -c "$HERE/nxray.c" -o "$BUILD/nxray.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/linux/gles.c" -o "$BUILD/gles.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/mc/mcw.c" -o "$BUILD/mcw.o"
rv32-cc -O2 -no-pie "$BUILD/nxray.o" "$BUILD/gles.o" "$BUILD/mcw.o" "$MW/src/lib/libnano-X.a" -lm -o "$BUILD/nxray"
riscv32-linux-strip -o "$OUT/usr/bin/nxray" "$BUILD/nxray"
echo "Other/Rays (every core)=nxray" > "$OUT/usr/share/nxapps.82-raytrace"
ls -l "$OUT/usr/bin/nxray"
