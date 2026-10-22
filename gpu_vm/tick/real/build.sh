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
# many cores (gvmbench's mc_* cases, cases.h): copies of CoreMark and of gzloop, each linked at a place of its own (no
# copy shares a word with another), with the runtime's heap cut to 64 KB (neither uses it): CoreMark k at 0x02000000 +
# k * 0x30000 (64), gzloop k at 0x03000000 + k * 0x240000 (16); and CoreMark -fPIE for the case coremark_pie
mkdir -p "$OUT/mc" "$OUT/pie"
sed "s/heap\[16 << 20\]/heap[64 << 10]/" "$ROOT/gpu_vm/llvm/test/exec/rt/libc.c" > "$OUT/mc/libc_small.c"
$CC -I"$ROOT/gpu_vm/llvm/test/exec/rt" -fno-math-errno -c -o "$OUT/mc/libc.o" "$OUT/mc/libc_small.c"
cp "$RTDIR/rt/librt.a" "$OUT/mc/librt.a"
"$BIN/llvm-ar" r "$OUT/mc/librt.a" "$OUT/mc/libc.o"
cmo=(); for f in core_list_join core_main core_matrix core_state core_util core_portme; do cmo+=("$OUT/$f.o"); done
gzo=(); for f in adler32 crc32 deflate inffast inflate inftrees trees zutil gzloop; do gzo+=("$OUT/$f.o"); done
for k in $(seq 0 63); do
    "$BIN/ld.lld" -e _start --image-base=$(printf 0x%x $((0x02000000 + k * 0x30000))) -o "$OUT/mc/cm$k.elf" "$RTDIR/crt0.o" "${cmo[@]}" "$OUT/mc/librt.a"
done
for k in $(seq 0 15); do
    "$BIN/ld.lld" -e _start --image-base=$(printf 0x%x $((0x03000000 + k * 0x240000))) -o "$OUT/mc/gz$k.elf" "$RTDIR/crt0.o" "${gzo[@]}" "$OUT/mc/librt.a"
done
cp "$OUT/coremark.txt" "$OUT/mc/coremark.txt"
OUT=$OUT/pie bash "$ROOT/gpu_vm/llvm/test/bench/run.sh" -O2 -fPIE > "$OUT/pie/coremark.txt"
cp "$OUT/pie/coremark.elf" "$OUT/coremark_pie.elf"
cat "$OUT/coremark.txt"
