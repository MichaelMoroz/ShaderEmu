#!/bin/bash
# gzip and gunzip (zlib, gzbench.c) of gpu_vm/bench_real's input on gvm_cpu: bundles each, to measure the code
# generator on real code beside CoreMark (run.sh). In WSL:
#   bash gpu_vm/llvm/test/bench/gz.sh [-O2] [-mllvm ...]   (extra flags go to clang)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
BIN=${GVM_LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}
Z=${THIRD:-$HOME/gpu_vm/third}/zlib
OUT=${OUT:-$ROOT/build/gvm_bench/gz}
GZ_BYTES=${GZ_BYTES:-262144}
FLAGS=("$@")
[ ${#FLAGS[@]} -gt 0 ] || FLAGS=(-O2)
mkdir -p "$OUT"
[ -d "$Z" ] || git clone -q --depth 1 https://github.com/madler/zlib.git "$Z" || exit 1
cat "$Z"/*.c | head -c "$GZ_BYTES" > "$OUT/input"
RTDIR=${EXEC_OUT:-$ROOT/build/gvm_exec}
OUT=$RTDIR bash "$ROOT/gpu_vm/llvm/test/exec/run.sh" -O 0 arith > /dev/null || { echo "the exec runtime does not build"; exit 1; }
CC="$BIN/clang --target=gpuvm-unknown-elf -nostdlibinc -I$ROOT/gpu_vm/llvm/test/exec/rt/include -I$Z -w"
objs=()
for f in adler32 crc32 deflate inffast inflate inftrees trees zutil; do
    $CC "${FLAGS[@]}" -c -o "$OUT/$f.o" "$Z/$f.c" || exit 1
    objs+=("$OUT/$f.o")
done
$CC "${FLAGS[@]}" -c -o "$OUT/gzbench.o" "$HERE/gzbench.c" || exit 1
"$BIN/ld.lld" -e _start -o "$OUT/gzbench.elf" "$RTDIR/crt0.o" "${objs[@]}" "$OUT/gzbench.o" "$RTDIR/rt/librt.a" || exit 1
"$ROOT/build/gvm_cpu/gvm_cpu" --stats "$OUT/gzbench.elf" < "$OUT/input" > "$OUT/gzbench.out" 2> "$OUT/gzbench.stats"
r=$?
echo "gz ${FLAGS[*]}: exit $r; $(tr '\n' ';' < "$OUT/gzbench.out")"
