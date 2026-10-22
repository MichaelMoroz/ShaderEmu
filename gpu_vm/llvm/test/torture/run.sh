#!/bin/bash
# GCC's c-torture execute tests on gpu_vm: each test compiled by clang (--target=gpuvm-unknown-elf), linked with the
# exec tests' runtime (../exec/rt: crt0, a small libc with its headers, compiler-rt's builtins) and run on gvm_cpu; it
# passes when it exits 0. The same test is also compiled by the same clang for the host (x86_64, glibc) and run there:
# a test that fails on the host too is clang's (GCC's extensions, GCC's own behaviour where C leaves it open), and is
# counted apart. The tests are untrusted data in their own folder (TORTURE, default ~/gpu_vm/torture/gcc/gcc/testsuite/
# gcc.c-torture/execute: a sparse clone of gcc's testsuite); nothing is run from there but the compiled programs (on
# gvm_cpu, and the host's builds with a timeout). In WSL:
#   bash gpu_vm/llvm/test/torture/run.sh [-O "0 2"] [-j N] [TEST...]
# Prints the pass rates at each level, and the failures into $OUT/fail-O<level>.txt.
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
RTINC=$ROOT/gpu_vm/llvm/test/exec/rt/include
CPU=$ROOT/build/gvm_cpu/gvm_cpu

tests=("$@")
if [ ${#tests[@]} -eq 0 ]; then
    for f in "$TORTURE"/*.c; do tests+=("$(basename "$f" .c)"); done
fi

one() { # test level -> prints "test level result host-result"
    local t=$1 o=$2 src="$TORTURE/$1.c" d="$OUT/O$2"
    local exe="$d/$t.elf" log="$d/$t.log"
    # what the target cannot have: skip
    if grep -qE 'dg-require-effective-target (int128|trampolines|lp64|size64|signal|mmap)|__int128|setjmp|signal\.h|<complex\.h>|dg-require-stack-size|__builtin_apply|dg-additional-sources|_Float128|__float128|_Decimal|sys/mman' "$src" 2>/dev/null; then
        echo "$t $o skip -"; return
    fi
    # the test's own options that matter
    local opts=""
    for a in $(grep -hoE 'dg-(additional-)?options (\{ *)?"[^"]*"' "$src" | sed -E 's/.*"(.*)"/\1/'); do
        case "$a" in
        -fwrapv|-funsigned-char|-fsigned-char|-fno-inline|-fno-builtin*|-fgnu89-inline|-std=*|-fno-strict-aliasing|\
        -ffast-math|-fno-math-errno|-fno-trapping-math|-fno-common|-ftrapv) opts="$opts $a" ;;
        esac
    done
    local common="-O$o -w -fno-strict-aliasing $opts -Wno-error=implicit-function-declaration -Wno-error=implicit-int
        -Wno-error=int-conversion -Wno-error=incompatible-pointer-types -Wno-error=return-type
        -Wno-error=incompatible-function-pointer-types"
    # the host's run
    local host=pass
    if ! timeout 60 "$BIN/clang" $common -o "$d/$t.host" "$src" -lm > "$d/$t.hostlog" 2>&1; then
        host=compile
    else
        timeout 10 "$d/$t.host" > /dev/null 2>&1 || host=run
    fi
    rm -f "$d/$t.host"
    if ! timeout 60 "$BIN/clang" --target=gpuvm-unknown-elf $common -nostdlibinc -I"$RTINC" \
            -c -o "$d/$t.o" "$src" > "$log" 2>&1; then
        echo "$t $o compile $host"; return
    fi
    if ! "$BIN/ld.lld" -e _start -o "$exe" "$RTDIR/crt0.o" "$d/$t.o" "$RTDIR/rt/librt.a" >> "$log" 2>&1; then
        echo "$t $o link $host"; return
    fi
    timeout 60 "$CPU" --max-bundles 300000000 "$exe" > "$d/$t.stdout" 2>> "$log"
    local r=$?
    if [ $r = 0 ]; then echo "$t $o pass $host"; else echo "$t $o run$r $host"; fi
}
export -f one
export TORTURE OUT BIN RTDIR RTINC CPU

for o in $LEVELS; do
    mkdir -p "$OUT/O$o"
    printf '%s\n' "${tests[@]}" | xargs -P "$JOBS" -I{} bash -c "one {} $o" | sort > "$OUT/results-O$o.txt"
    awk -v o="$o" '
        $3 == "skip" { skip++; next }
        { total++; if ($3 == "pass") pass++; if ($4 == "pass") { htotal++; if ($3 == "pass") hpass++ } }
        END {
            printf "-O%s: %d of %d pass (%.1f%%); of the %d the host passes too, %d (%.1f%%); %d skipped\n",
                o, pass, total, 100 * pass / total, htotal, hpass, 100 * hpass / htotal, skip
        }' "$OUT/results-O$o.txt"
    awk '$3 != "pass" && $3 != "skip"' "$OUT/results-O$o.txt" > "$OUT/fail-O$o.txt"
    echo "  failing here but passing on the host: $(awk '$4 == "pass"' "$OUT/fail-O$o.txt" | wc -l)" \
         "($(awk '$4 == "pass" {print $3}' "$OUT/fail-O$o.txt" | sort | uniq -c | tr -s ' \n' ' '))"
done
