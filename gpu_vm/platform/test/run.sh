#!/bin/bash
# gpu_vm/platform's bare-metal tests (gpu_vm/platform.md, "Tests"): built with the exec tests' runtime
# (gpu_vm/llvm/test/exec/rt, made by its run.sh) into build/gvm_platform, run on gvm_cpu. In WSL:
#   bash gpu_vm/platform/test/run.sh            workers.c with 3 and 15 workers, and once with GVM_RACES
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
BIN=${GVM_LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}
EXEC=$ROOT/build/gvm_exec
OUT=$ROOT/build/gvm_platform
mkdir -p "$OUT"
[ -f "$EXEC/rt/librt.a" ] || bash "$ROOT/gpu_vm/llvm/test/exec/run.sh" -O 2 arith >/dev/null
make -s -C "$ROOT/gpu_vm/cpu" CC=gcc
CPU=$ROOT/build/gvm_cpu/gvm_cpu
RT=$ROOT/gpu_vm/llvm/test/exec/rt
"$BIN/clang" --target=gpuvm-unknown-elf -ffreestanding -nostdlibinc -I"$RT/include" -I"$RT" -O2 -c -o "$OUT/workers.o" "$HERE/workers.c"
"$BIN/ld.lld" -e _start -o "$OUT/workers.elf" "$EXEC/crt0.o" "$OUT/workers.o" "$EXEC/rt/librt.a"
fail=0
for n in 3 15; do
    if "$CPU" --cores $((n + 1)) --devices "$OUT/workers.elf" $n > "$OUT/workers-$n.out" 2>&1; then
        echo "workers $n: PASS"
    else
        echo "workers $n: FAIL"; cat "$OUT/workers-$n.out"; fail=1
    fi
done
GVM_RACES="$OUT/races.txt" "$CPU" --cores 4 --devices "$OUT/workers.elf" 3 > "$OUT/workers-races.out" 2>&1 || fail=1
grep "races:" "$OUT/workers-races.out"
# the two halves of one word: 40 passes, each one undone; nothing else
grep -q "races: 40 undone" "$OUT/workers-races.out" || { echo "workers races: expected 40 undone"; cat "$OUT/workers-races.out"; fail=1; }
exit $fail
