#!/bin/bash
# GCC's c-torture execute tests on gpu_vm: each test compiled by clang (--target=gpuvm-unknown-elf), linked with the
# exec tests' runtime (../exec/rt: crt0, a small libc, compiler-rt's builtins) and run on gvm_cpu; it passes when it
# exits 0. The tests are untrusted data in their own folder (TORTURE, default ~/gpu_vm/torture/gcc/gcc/testsuite/
# gcc.c-torture/execute: a sparse clone of gcc's testsuite); nothing is run from there but the compiled programs, on
# gvm_cpu. In WSL:
#   bash gpu_vm/llvm/test/torture/run.sh [-O "0 2"] [-j N] [TEST...]
# Prints the pass rate at each level, and the failures by kind into $OUT/fail-O<level>.txt.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
BIN=${GVM_LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}
TORTURE=${TORTURE:-$HOME/gpu_vm/torture/gcc/gcc/testsuite/gcc.c-torture/execute}
OUT=${OUT:-$HOME/gpu_vm/torture-out}
LEVELS="0 2"
JOBS=${JOBS:-12}
while [ $# -gt 0 ]; do
    case "$1" in
    -O) LEVELS=$2; shift 2 ;;
    -j) JOBS=$2; shift 2 ;;
    *) break ;;
    esac
done
mkdir -p "$OUT"

# The runtime: the exec tests' (built by their run.sh)
bash "$ROOT/gpu_vm/llvm/test/exec/run.sh" -O 0 arith > /dev/null || { echo "the exec runtime does not build"; exit 1; }
RTDIR=${EXEC_OUT:-$ROOT/build/gvm_exec}
CPU=$ROOT/build/gvm_cpu/gvm_cpu

tests=("$@")
if [ ${#tests[@]} -eq 0 ]; then
    for f in "$TORTURE"/*.c; do tests+=("$(basename "$f" .c)"); done
fi

one() { # test level -> prints "test level result"
    local t=$1 o=$2 src="$TORTURE/$1.c" d="$OUT/O$2"
    local exe="$d/$t.elf" log="$d/$t.log"
    # what the target cannot do: skip
    if grep -qE 'dg-require-effective-target (int128|trampolines|lp64|size64|label_values|indirect_jumps|signal)|dg-skip-if|__int128|setjmp|signal\.h|<complex\.h>|dg-require-stack-size|__builtin_apply|dg-additional-sources|dg-xfail-run-if|_Float128|__float128|_Float16|_Decimal' "$src" 2>/dev/null; then
        echo "$t $o skip"; return
    fi
    if ! timeout 60 "$BIN/clang" --target=gpuvm-unknown-elf -O$o -w -fno-strict-aliasing \
            -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=int-conversion \
            -Wno-error=incompatible-pointer-types -Wno-error=return-type -Wno-error=incompatible-function-pointer-types \
            -c -o "$d/$t.o" "$src" > "$log" 2>&1; then
        echo "$t $o compile"; return
    fi
    if ! "$BIN/ld.lld" -e _start -o "$exe" "$RTDIR/crt0.o" "$d/$t.o" "$RTDIR/rt/librt.a" >> "$log" 2>&1; then
        echo "$t $o link"; return
    fi
    timeout 60 "$CPU" --max-bundles 300000000 "$exe" > "$d/$t.stdout" 2>> "$log"
    local r=$?
    if [ $r = 0 ]; then echo "$t $o pass"; else echo "$t $o run$r"; fi
}
export -f one
export TORTURE OUT BIN RTDIR CPU

for o in $LEVELS; do
    mkdir -p "$OUT/O$o"
    printf '%s\n' "${tests[@]}" | xargs -P "$JOBS" -I{} bash -c "one {} $o" > "$OUT/results-O$o.txt"
    total=$(grep -vc ' skip$' "$OUT/results-O$o.txt")
    pass=$(grep -c ' pass$' "$OUT/results-O$o.txt")
    skip=$(grep -c ' skip$' "$OUT/results-O$o.txt")
    grep -v ' pass$\| skip$' "$OUT/results-O$o.txt" | sort > "$OUT/fail-O$o.txt"
    echo "-O$o: $pass of $total pass ($(awk "BEGIN{printf \"%.1f\", 100*$pass/($total?$total:1)}")%), $skip skipped;" \
         "failures: $(awk '{print $3}' "$OUT/fail-O$o.txt" | sort | uniq -c | tr '\n' ' ')"
done
