#!/bin/bash
# CoreMark on gvm_cpu, to measure the code generator: bundles and ops an iteration (gvm_cpu --stats). The CoreMark
# sources are EEMBC's (COREMARK, default ~/gpu_vm/coremark-src/coremark, a clone of github.com/eembc/coremark); the
# port is coremark/core_portme.*. In WSL:
#   bash gpu_vm/llvm/test/bench/run.sh [-O2] [-mllvm ...]   (extra flags go to clang)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
BIN=${GVM_LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}
COREMARK=${COREMARK:-$HOME/gpu_vm/coremark-src/coremark}
OUT=${OUT:-$ROOT/build/gvm_bench}
ITER=${ITER:-20}
FLAGS=("$@")
[ ${#FLAGS[@]} -gt 0 ] || FLAGS=(-O2)
mkdir -p "$OUT"
bash "$ROOT/gpu_vm/llvm/test/exec/run.sh" -O 0 arith > /dev/null || { echo "the exec runtime does not build"; exit 1; }
RTDIR=$ROOT/build/gvm_exec
CC="$BIN/clang --target=gpuvm-unknown-elf -nostdlibinc -I$ROOT/gpu_vm/llvm/test/exec/rt/include -I$HERE/coremark
    -I$COREMARK -DITERATIONS=$ITER -DPERFORMANCE_RUN=1 -w"
objs=()
for f in core_list_join core_main core_matrix core_state core_util; do
    $CC "${FLAGS[@]}" -DFLAGS_STR="\"${FLAGS[*]}\"" -c -o "$OUT/$f.o" "$COREMARK/$f.c" || exit 1
    objs+=("$OUT/$f.o")
done
$CC "${FLAGS[@]}" -c -o "$OUT/core_portme.o" "$HERE/coremark/core_portme.c" || exit 1
"$BIN/ld.lld" -e _start -o "$OUT/coremark.elf" "$RTDIR/crt0.o" "${objs[@]}" "$OUT/core_portme.o" \
    "$RTDIR/rt/librt.a" || exit 1
"$ROOT/build/gvm_cpu/gvm_cpu" --stats "$OUT/coremark.elf" > "$OUT/coremark.out" 2> "$OUT/coremark.stats"
r=$?
grep -E "Total ticks|Iterations/Sec|Correct|Errors|ERROR" "$OUT/coremark.out" | grep -v "Must execute"
stats=$(cat "$OUT/coremark.stats")
bundles=$(echo "$stats" | sed -nE 's/.* ([0-9]+) bundles.*/\1/p')
ops=$(echo "$stats" | sed -nE 's/.*bundles, ([0-9]+) ops.*/\1/p')
ticks=$(grep "Total ticks" "$OUT/coremark.out" | sed -nE 's/.*: *([0-9]+).*/\1/p')
echo "coremark ${FLAGS[*]}: exit $r; $ITER iterations: $ticks bundles timed, $((ticks / ITER)) an iteration;" \
     "whole run $bundles bundles, $ops ops ($(awk "BEGIN{printf \"%.2f\", $ops/$bundles}") a bundle)"
