#!/usr/bin/env bash
# Builds nxperf, the machine's benchmark (docs/apps/bench.md): the processor, memory, the worker
# cores, the lanes, numerical kernels, libmedia and the GPU, each with a check.
#   wsl -- bash /mnt/c/.../linux/bench/build.sh      (after linux/nanox/build.sh and linux/libmedia/build.sh)
# In the guest: nano-X -p & nxperf, or nxperf --cli [NAME|GROUP ...]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
MEDIA=$WORK/src/libmedia/libmedia.a
OUT=${BENCH_OUT:-$REPO/build/images/linux/root}   # (BENCH_OUT: a private image's overlay)
BUILD=$WORK/src/bench-build
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
[ -f "$MEDIA" ] || { echo "run linux/libmedia/build.sh first"; exit 1; }
mkdir -p "$BUILD" "$OUT/usr/bin" "$OUT/usr/share"
INC="-idirafter $REPO/programs/linux/include -I$MW/src/include -I$REPO/linux/userland -I$REPO/programs/mc -I$REPO/linux/libmedia -I$REPO/linux/apps -I$HERE"
# single-precision constants (the machine has no doubles); a copy loop stays a loop, not a call of memcpy
OPT="-O2 -fsingle-precision-constant -fno-math-errno -fno-tree-loop-distribute-patterns"
rv32-cc $OPT -fno-pie -Wall -Wno-unused-function $INC -c "$HERE/perf_tests.c" -o "$BUILD/perf_tests.o"
rv32-cc $OPT -fno-pie -Wall -Wno-unused-function -DSEGL_FLOAT $INC -c "$HERE/perf_gpu.c" -o "$BUILD/perf_gpu.o"
rv32-cc $OPT -fno-pie -Wall -Wno-unused-function $INC -c "$HERE/perf_score.c" -o "$BUILD/perf_score.o"
rv32-cc -O2 -fno-pie -Wall -Wno-unused-function $INC -c "$HERE/nxperf.c" -o "$BUILD/nxperf.o"
# the OpenGL library with floats, and the workers' library
rv32-cc -O2 -fno-pie -w -DSEGL_FLOAT $INC -c "$REPO/programs/linux/gles.c" -o "$BUILD/gles.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/mc/mcw.c" -o "$BUILD/mcw.o"
rv32-cc -O2 -fno-pie -w $INC -c "$REPO/programs/mc/mcwx.c" -o "$BUILD/mcwx.o"
rv32-cc -O2 -no-pie "$BUILD/nxperf.o" "$BUILD/perf_tests.o" "$BUILD/perf_gpu.o" "$BUILD/perf_score.o" "$BUILD/gles.o" "$MEDIA" "$BUILD/mcwx.o" "$BUILD/mcw.o" \
    "$MW/src/lib/libnano-X.a" -lm -Wl,--gc-sections -o "$BUILD/nxperf"
riscv32-linux-nm -n "$BUILD/nxperf" > "$WORK/src/nxperf.nm"
riscv32-linux-strip -o "$OUT/usr/bin/nxperf" "$BUILD/nxperf"
echo "Utilities/Benchmark=nxperf" > "$OUT/usr/share/nxapps.22-bench"
ls -l "$OUT/usr/bin/nxperf"
