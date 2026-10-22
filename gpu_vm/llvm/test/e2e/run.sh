#!/bin/bash
# End-to-end: hello.s and pack.s assembled by llvm-mc, linked by ld.lld at 0x02000000, compared bundle for bundle
# with gvm_asm.h's (check.cpp), then run on gvm_cpu (gpu_vm/cpu). In WSL: bash gpu_vm/llvm/test/e2e/run.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
BIN=${GVM_LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}
OUT=${OUT:-$ROOT/build/gvm_llvm_e2e}
mkdir -p "$OUT"
g++ -O1 -std=c++17 -o "$OUT/check" "$HERE/check.cpp"
make -s -C "$ROOT/gpu_vm/cpu" CC=gcc >/dev/null
CPU=$ROOT/build/gvm_cpu/gvm_cpu
fail=0
for t in hello pack; do
    "$BIN/llvm-mc" -triple=gpuvm -filetype=obj -o "$OUT/$t.o" "$HERE/$t.s"
    "$BIN/ld.lld" -Ttext=0x02000000 -e _start -o "$OUT/$t.elf" "$OUT/$t.o"
    "$OUT/check" $t "$OUT/$t.elf" || fail=1
done
out=$("$CPU" "$OUT/hello.elf") || { echo "hello: gvm_cpu exit $?"; fail=1; }
[ "$out" = "Hello, world!" ] && echo "hello: gvm_cpu prints Hello, world!" || { echo "hello: gvm_cpu printed '$out'"; fail=1; }
set +e
"$CPU" "$OUT/pack.elf"; r=$?
set -e
[ $r = 68 ] && echo "pack: gvm_cpu exits 68" || { echo "pack: gvm_cpu exit $r, want 68"; fail=1; }
[ $fail = 0 ] && echo PASS || { echo FAIL; exit 1; }
