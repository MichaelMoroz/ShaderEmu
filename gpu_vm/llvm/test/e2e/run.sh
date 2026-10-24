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
# relocations across objects: R_GVM_BR9, R_GVM_PCIMM32, R_GVM_J18, R_GVM_TPREL32
"$BIN/llvm-mc" -triple=gpuvm -filetype=obj -o "$OUT/xref1.o" "$HERE/xref1.s"
"$BIN/llvm-mc" -triple=gpuvm -filetype=obj -o "$OUT/xref2.o" "$HERE/xref2.s"
"$BIN/ld.lld" -Ttext=0x02000000 -o "$OUT/xref.elf" "$OUT/xref1.o" "$OUT/xref2.o"
"$BIN/llvm-objdump" -d -z "$OUT/xref.elf" > "$OUT/xref.dis"
for want in "beq a0, a1, 0x2000040 ;;" "lpc a2, 0x2000040; call 0x2000040 ;;" "lw a3, 4(tp); li a7, 93 ;;"             "00 00 00 00 00 00 00 00 00 00 00 00 07 2a 08 18" "c0 e1 00 00 00 00 00 00 30 00 00 00 29 f0 00 00"; do
    grep -qF "$want" "$OUT/xref.dis" || { echo "xref: no '$want' in the disassembly"; fail=1; }
done
[ $fail = 0 ] && echo "xref: ld.lld's relocations as expected"
out=$("$CPU" "$OUT/hello.elf") || { echo "hello: gvm_cpu exit $?"; fail=1; }
[ "$out" = "Hello, world!" ] && echo "hello: gvm_cpu prints Hello, world!" || { echo "hello: gvm_cpu printed '$out'"; fail=1; }
set +e
"$CPU" "$OUT/pack.elf"; r=$?
set -e
[ $r = 68 ] && echo "pack: gvm_cpu exits 68" || { echo "pack: gvm_cpu exit $r, want 68"; fail=1; }
# a call and a j beyond +-2 MB, through ld.lld's thunks
"$BIN/llvm-mc" -triple=gpuvm -filetype=obj -o "$OUT/thunk.o" "$HERE/thunk.s"
"$BIN/ld.lld" -Ttext=0x02000000 --section-start=.far=0x02400000 -o "$OUT/thunk.elf" "$OUT/thunk.o"
set +e
"$CPU" --ram 8 --max-bundles 1000 "$OUT/thunk.elf"; r=$?
set -e
n=$("$BIN/llvm-nm" "$OUT/thunk.elf" | grep -c __gpuvm_thunk_)
[ $r = 7 ] && [ "$n" = 2 ] && echo "thunk: 2 thunks, gvm_cpu exits 7" || { echo "thunk: exit $r, $n thunks; want 7, 2"; fail=1; }
# a call of an undefined weak function: linked (to its own bundle), never made
"$BIN/llvm-mc" -triple=gpuvm -filetype=obj -o "$OUT/weak.o" "$HERE/weak.s"
if "$BIN/ld.lld" -Ttext=0x02000000 -o "$OUT/weak.elf" "$OUT/weak.o"; then
    set +e
    "$CPU" --ram 8 --max-bundles 1000 "$OUT/weak.elf"; r=$?
    set -e
    [ $r = 5 ] && echo "weak: linked, gvm_cpu exits 5" || { echo "weak: exit $r, want 5"; fail=1; }
else
    echo "weak: ld.lld refused the call"; fail=1
fi
[ $fail = 0 ] && echo PASS || { echo FAIL; exit 1; }
