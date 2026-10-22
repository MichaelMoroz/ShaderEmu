#!/bin/bash
# The tick bench's real programs (gvmbench's cases coremark, gzip, gunzip; cases.h) into build/tick_real: CoreMark as
# gpu_vm/llvm/test/bench/run.sh builds it, and gzloop.c with zlib over bench_real's input. Kept apart from
# build/gvm_bench so that the code the tick is measured on stays the same while the compiler changes. In WSL:
#   bash gpu_vm/tick/real/build.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
BIN=${GVM_LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}
Z=${THIRD:-$HOME/gpu_vm/third}/zlib
OUT=$ROOT/build/tick_real
mkdir -p "$OUT"
OUT=$OUT bash "$ROOT/gpu_vm/llvm/test/bench/run.sh" -O2 > "$OUT/coremark.txt"
cat "$Z"/*.c | head -c 262144 > "$OUT/input"
gzip -6 -n -c "$OUT/input" > "$OUT/input.gz"
RTDIR=$ROOT/build/gvm_exec
CC="$BIN/clang --target=gpuvm-unknown-elf -nostdlibinc -I$ROOT/gpu_vm/llvm/test/exec/rt/include -I$Z -w -O2"
objs=()
for f in adler32 crc32 deflate inffast inflate inftrees trees zutil; do
    $CC -c -o "$OUT/$f.o" "$Z/$f.c"
    objs+=("$OUT/$f.o")
done
$CC -DGZ_INPUT="\"$OUT/input\"" -DGZ_PACKED="\"$OUT/input.gz\"" -c -o "$OUT/gzloop.o" "$HERE/gzloop.c"
"$BIN/ld.lld" -e _start -o "$OUT/gzloop.elf" "$RTDIR/crt0.o" "${objs[@]}" "$OUT/gzloop.o" "$RTDIR/rt/librt.a"
"$ROOT/build/gvm_cpu/gvm_cpu" --stats "$OUT/gzloop.elf" 2>&1 | tail -1
cat "$OUT/coremark.txt"
