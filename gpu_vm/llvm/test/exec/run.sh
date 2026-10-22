#!/bin/bash
# Execution tests of the gpu_vm code generator: each *.c here compiled by clang (--target=gpuvm-unknown-elf) at
# -O0, -O1, -O2 and -Os, linked by ld.lld with rt/ (crt0, a small libc over gvm_cpu's semihosting, compiler-rt's
# builtins for i64 division and double), and run on gvm_cpu: a test passes when it exits 0 (and prints NAME.out
# when there is one). In WSL:
#   bash gpu_vm/llvm/test/exec/run.sh [-O "0 2"] [TEST...]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
BIN=${GVM_LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}
SRC=${GVM_LLVM_SRC:-$HOME/gpu_vm/llvm-project}
OUT=${OUT:-$ROOT/build/gvm_exec}
LEVELS="0 1 2 s"
if [ "${1:-}" = "-O" ]; then LEVELS=$2; shift 2; fi
mkdir -p "$OUT/rt"

CC="$BIN/clang --target=gpuvm-unknown-elf -ffreestanding -nostdlibinc -I$HERE/rt/include -I$HERE/rt -Wno-everything"
make -s -C "$ROOT/gpu_vm/cpu" CC=gcc >/dev/null || exit 1
CPU=$ROOT/build/gvm_cpu/gvm_cpu

# The runtime, once (rebuilt when the compiler or a source is newer)
RT="$OUT/rt/librt.a"
if [ ! -f "$RT" ] || [ "$BIN/clang" -nt "$RT" ] || [ -n "$(find "$HERE/rt" -newer "$RT" -type f)" ]; then
    rm -f "$OUT"/rt/*.o "$RT" "$OUT/crt0.o"
    $CC -c -o "$OUT/crt0.o" "$HERE/rt/crt0.s" || exit 1
    $CC -O2 -fno-math-errno -c -o "$OUT/rt/libc.o" "$HERE/rt/libc.c" || exit 1
    # weak: a test may define a function of the same name
    "$BIN/llvm-objcopy" --weaken "$OUT/rt/libc.o" || exit 1
    B=$SRC/compiler-rt/lib/builtins
    for f in absvdi2 absvsi2 adddf3 addvdi3 addvsi3 ashldi3 ashrdi3 bswapdi2 bswapsi2 clzdi2 clzsi2 cmpdi2 \
             comparedf2 comparesf2 ctzdi2 ctzsi2 divdc3 divdf3 divdi3 divmoddi4 divmodsi4 divsc3 divsi3 \
             extendsfdf2 extendhfsf2 ffsdi2 ffssi2 fixdfdi fixdfsi fixsfdi fixsfsi fixunsdfdi fixunsdfsi \
             fixunssfdi fixunssfsi floatdidf floatdisf floatsidf floatsisf floatundidf floatundisf floatunsidf \
             floatunsisf fp_mode int_util lshrdi3 moddi3 modsi3 muldc3 muldf3 muldi3 mulodi4 mulosi4 mulsc3 \
             mulvdi3 mulvsi3 negdf2 negdi2 negvdi2 negvsi2 paritydi2 paritysi2 popcountdi2 popcountsi2 \
             powidf2 powisf2 subdf3 subvdi3 subvsi3 truncdfsf2 truncdfhf2 truncsfhf2 ucmpdi2 udivdi3 \
             udivmoddi4 udivmodsi4 udivsi3 umoddi3 umodsi3; do
        $CC -O2 -fno-builtin -c -o "$OUT/rt/$f.o" "$B/$f.c" || exit 1
    done
    "$BIN/llvm-ar" rcs "$RT" "$OUT"/rt/*.o || exit 1
fi

tests=("$@")
if [ ${#tests[@]} -eq 0 ]; then
    for f in "$HERE"/*.c; do tests+=("$(basename "$f" .c)"); done
fi

pass=0; fail=0; failed=()
for t in "${tests[@]}"; do
    for o in $LEVELS; do
        exe="$OUT/$t-O$o.elf"
        log="$OUT/$t-O$o.log"
        if ! $CC -O$o -c -o "$OUT/$t-O$o.o" "$HERE/$t.c" > "$log" 2>&1; then
            fail=$((fail + 1)); failed+=("$t-O$o(compile)"); continue
        fi
        if ! "$BIN/ld.lld" -e _start -o "$exe" "$OUT/crt0.o" "$OUT/$t-O$o.o" "$RT" >> "$log" 2>&1; then
            fail=$((fail + 1)); failed+=("$t-O$o(link)"); continue
        fi
        timeout 120 "$CPU" --max-bundles 400000000 "$exe" > "$OUT/$t-O$o.stdout" 2>> "$log"
        r=$?
        ok=1
        [ $r = 0 ] || ok=0
        if [ $ok = 1 ] && [ -f "$HERE/$t.out" ] && ! cmp -s "$HERE/$t.out" "$OUT/$t-O$o.stdout"; then ok=0; fi
        if [ $ok = 1 ]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1)); failed+=("$t-O$o(exit $r)")
        fi
    done
done
echo "exec: $pass passed, $fail failed"
[ $fail = 0 ] || { printf '  %s\n' "${failed[@]}"; exit 1; }
