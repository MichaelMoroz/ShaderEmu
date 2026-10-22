#!/bin/bash
# Bigger programs on gpu_vm: third-party sources (untrusted data in ~/gpu_vm/third, cloned there; nothing is run
# from there) compiled by clang with the exec tests' runtime, run on gvm_cpu, checked against a run of the same
# sources built for the host. In WSL:
#   bash gpu_vm/llvm/test/apps/run.sh [-O "0 2"]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
BIN=${GVM_LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}
THIRD=${THIRD:-$HOME/gpu_vm/third}
OUT=${OUT:-$ROOT/build/gvm_apps}
LEVELS="0 2"
if [ "${1:-}" = "-O" ]; then LEVELS=$2; shift 2; fi
mkdir -p "$OUT"
bash "$ROOT/gpu_vm/llvm/test/exec/run.sh" -O 0 arith > /dev/null || { echo "the exec runtime does not build"; exit 1; }
RTDIR=$ROOT/build/gvm_exec
CPU=$ROOT/build/gvm_cpu/gvm_cpu
CC="$BIN/clang --target=gpuvm-unknown-elf -nostdlibinc -I$ROOT/gpu_vm/llvm/test/exec/rt/include -w"
fail=0

# app NAME "sources" "flags": builds both ways, runs both, compares stdout
app() {
    local name=$1 srcs=$2 flags=$3 o
    for o in $LEVELS; do
        local d="$OUT/$name-O$o" objs=() ok=1
        mkdir -p "$d"
        for s in $srcs; do
            local obj="$d/$(basename "$s" .c).o"
            $CC -O$o $flags -c -o "$obj" "$s" || { ok=0; break; }
            objs+=("$obj")
        done
        if [ $ok = 1 ] && "$BIN/ld.lld" -e _start -o "$d/$name.elf" "$RTDIR/crt0.o" "${objs[@]}" "$RTDIR/rt/librt.a" &&
            timeout 300 "$CPU" "$d/$name.elf" > "$d/gvm.out" 2> "$d/gvm.err" &&
            "$BIN/clang" -O$o $flags -w -o "$d/host" $srcs -lm && "$d/host" > "$d/host.out" &&
            cmp -s "$d/gvm.out" "$d/host.out"; then
            echo "$name -O$o: ok ($(tail -1 "$d/gvm.out"))"
        else
            echo "$name -O$o: FAIL (see $d)"; fail=1
        fi
    done
}

Z=$THIRD/zlib
app zlib "$Z/adler32.c $Z/compress.c $Z/crc32.c $Z/deflate.c $Z/inffast.c $Z/inflate.c $Z/inftrees.c $Z/trees.c
    $Z/uncompr.c $Z/zutil.c $HERE/zlib_test.c" "-I$Z"

L=$THIRD/lua
app lua "$L/lapi.c $L/lcode.c $L/lctype.c $L/ldebug.c $L/ldo.c $L/ldump.c $L/lfunc.c $L/lgc.c $L/llex.c $L/lmem.c
    $L/lobject.c $L/lopcodes.c $L/lparser.c $L/lstate.c $L/lstring.c $L/ltable.c $L/ltm.c $L/lundump.c $L/lvm.c
    $L/lzio.c $L/lauxlib.c $L/lbaselib.c $L/lcorolib.c $L/lstrlib.c $L/ltablib.c $L/lutf8lib.c $HERE/lua_test.c" \
    "-I$L"
exit $fail
