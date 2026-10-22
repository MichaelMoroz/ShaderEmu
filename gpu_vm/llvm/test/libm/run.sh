#!/bin/bash
# gpu_vm's libm (gpu_vm/userland/musl/src/math/gpuvm; gpu_vm/userland/README.md, "libm") measured. In WSL, after
# gpu_vm/userland/build.sh (musl built in ~/gpu_vm/musl):
#   bash gpu_vm/llvm/test/libm/run.sh [STEP...]       steps (default: acc bench kernels):
#     acc       the accuracy over every float on the host (acc.c, the card's float semantics), ~2 minutes on 20 cores
#     probe     the card's special-function ops against the host's libm on gvm_cpu; the card's own run:
#               pwsh tools\gpu_queue.ps1 "C:\Program Files\Git\bin\bash.exe" -c "bin/gvm_gpu.exe build/gvm_libm/probe.elf > build/gvm_libm/probe.gpu"
#               then build/gvm_libm/probe_check build/gvm_libm/probe.gpu
#     libprobe  the libm as compiled, on gvm_cpu (and the card's run, as probe's: libprobe.elf > libprobe.gpu): the same
#               bits as the host's emulation, and ulps
#     bench     bundles a call of each function on gvm_cpu: musl's generic libm, gpu_vm's, the card's ops (-ffast-math)
#     kernels   Quake-like float kernels (qkern.c): bundles a call on gvm_cpu with each libm and flags, and the RISC-V
#               machine's instructions a call (rv32imaf, GCC 13.3 as rv32-cc, its musl; qemu-riscv32's trace)
#     fit       the polynomials' fits again (fit.c)
#     tables    __gvm_log_data.c again (gen_logtab.c)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
BIN=${GVM_LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}
MUSL=${MUSL:-$HOME/gpu_vm/musl}
SYSROOT=${SYSROOT:-$HOME/gpu_vm/sysroot}
RVSYS=${RVSYS:-$HOME/nm/work/sysroot}
RVGCC=${RVGCC:-$HOME/shaderemu-linux/toolchain/bin/riscv32-buildroot-linux-gnu-gcc}
M=$ROOT/gpu_vm/userland/musl/src/math/gpuvm
O=$ROOT/build/gvm_libm
EX=$ROOT/gpu_vm/llvm/test/exec
mkdir -p "$O"
steps=("$@")
[ ${#steps[@]} -gt 0 ] || steps=(acc bench kernels)
CPU=$ROOT/build/gvm_cpu/gvm_cpu
BARE="$BIN/clang --target=gpuvm-unknown-elf -ffreestanding -nostdlibinc -I$EX/rt/include -I$EX/rt -Wno-everything"
MUSLCC="$BIN/clang --target=gpuvm-unknown-linux -std=c99 -nostdinc -ffreestanding -fexcess-precision=standard
    -frounding-math -fno-strict-aliasing -O2 -fPIE -w -D_XOPEN_SOURCE=700 -I$MUSL/arch/gpuvm -I$MUSL/arch/generic
    -I$MUSL/obj/src/internal -I$MUSL/src/include -I$MUSL/src/internal -I$MUSL/obj/include -I$MUSL/include"
MUSLCC=$(echo $MUSLCC)   # one line, for xargs

runtime() {   # the exec tests' runtime (crt0.o, librt.a) and gvm_cpu
    make -s -C "$ROOT/gpu_vm/cpu" CC=gcc > /dev/null
    bash "$EX/run.sh" -O 2 arith > /dev/null
}

libs() {   # libm_generic.a: musl's generic math compiled here; libm_gvm.a: the build's math objects, gpu_vm's among them
    local g=$O/generic
    rm -rf "$g" && mkdir -p "$g"
    for f in "$MUSL"/src/math/*.c; do echo "$f"; done | xargs -P "$(nproc)" -I{} sh -c \
        "$MUSLCC -c {} -o $g/\$(basename {} .c).o"
    rm -f "$O/libm_generic.a" "$O/libm_gvm.a"
    "$BIN/llvm-ar" rcs "$O/libm_generic.a" "$g"/*.o
    "$BIN/llvm-ar" rcs "$O/libm_gvm.a" "$MUSL"/obj/src/math/gpuvm/*.lo "$MUSL"/obj/src/math/*.lo
}

for s in "${steps[@]}"; do
    case "$s" in
    acc)
        gcc -O2 -fopenmp -ffp-contract=off -msse4.1 -Wall -Wno-unused-function -I"$M" -o "$O/acc" "$HERE/acc.c" \
            "$HERE/acc_host.c" -lm
        "$O/acc" ;;
    probe)
        runtime
        $BARE -O2 -c "$HERE/probe.c" -o "$O/probe.o"
        "$BIN/ld.lld" -e _start -o "$O/probe.elf" "$ROOT/build/gvm_exec/crt0.o" "$O/probe.o" "$ROOT/build/gvm_exec/rt/librt.a"
        gcc -O2 -o "$O/probe_check" "$HERE/probe_check.c" -lm
        "$CPU" "$O/probe.elf" > "$O/probe.cpu"
        echo "gvm_cpu (the reference: the host's float functions):"
        "$O/probe_check" "$O/probe.cpu"
        if [ -f "$O/probe.gpu" ]; then echo "the card ($O/probe.gpu):"; "$O/probe_check" "$O/probe.gpu"; fi ;;
    libprobe)
        runtime; libs
        $BARE -O2 -c "$HERE/libprobe.c" -o "$O/libprobe.o"
        "$BIN/ld.lld" -e _start -o "$O/libprobe.elf" "$ROOT/build/gvm_exec/crt0.o" "$O/libprobe.o" "$O/libm_gvm.a" \
            "$ROOT/build/gvm_exec/rt/librt.a"
        gcc -O2 -ffp-contract=off -msse4.1 -w -I"$M" -o "$O/libprobe_check" "$HERE/libprobe_check.c" "$HERE/acc_host.c" -lm
        "$CPU" "$O/libprobe.elf" > "$O/libprobe.cpu"
        echo "gvm_cpu:"
        "$O/libprobe_check" "$O/libprobe.cpu"
        if [ -f "$O/libprobe.gpu" ]; then echo "the card ($O/libprobe.gpu):"; "$O/libprobe_check" "$O/libprobe.gpu"; fi ;;
    bench)
        runtime; libs
        $BARE -O2 -c "$HERE/bench.c" -o "$O/bench.o"
        $BARE -O2 -DFAST -ffast-math -c "$HERE/bench.c" -o "$O/bench_fast.o"
        L="$ROOT/build/gvm_exec/crt0.o"
        "$BIN/ld.lld" -e _start -o "$O/bench_generic.elf" $L "$O/bench.o" "$O/libm_generic.a" "$ROOT/build/gvm_exec/rt/librt.a"
        "$BIN/ld.lld" -e _start -o "$O/bench_gvm.elf" $L "$O/bench.o" "$O/libm_gvm.a" "$ROOT/build/gvm_exec/rt/librt.a"
        "$BIN/ld.lld" -e _start -o "$O/bench_fast.elf" $L "$O/bench_fast.o" "$O/libm_gvm.a" "$ROOT/build/gvm_exec/rt/librt.a"
        mkdir -p "$O/mfloat"
        for f in "$ROOT"/gpu_vm/userland/musl/mfloat/*.c; do $MUSLCC -c "$f" -o "$O/mfloat/$(basename "$f" .c).o"; done
        "$BIN/ld.lld" -e _start -o "$O/bench_mfloat.elf" $L "$O/bench.o" "$O"/mfloat/*.o "$O/libm_gvm.a" \
            "$ROOT/build/gvm_exec/rt/librt.a"
        for v in generic gvm mfloat fast; do "$CPU" "$O/bench_$v.elf" > "$O/bench_$v.txt"; done
        echo "bundles a call (the loop's own taken off): musl's generic, gpu_vm's libm, with libmfloat"
        paste "$O/bench_generic.txt" "$O/bench_gvm.txt" "$O/bench_mfloat.txt" | tail -n +2 |
            awk '{ printf "%-10s %10s %10s %10s\n", $1, $2, $4, $6 }'
        echo "-ffast-math (the card's ops inline):"
        tail -n +2 "$O/bench_fast.txt" ;;
    kernels)
        runtime
        [ -f "$O/libm_gvm.a" ] || libs
        L="$ROOT/build/gvm_exec/crt0.o"
        for v in "generic:-O2" "gvm:-O2" "gvm:-O2 -fno-math-errno -fapprox-func" "gvm:-O2 -ffast-math"; do
            lib=${v%%:*}; fl=${v#*:}
            ${BARE/-ffreestanding /} $fl -c "$HERE/qkern.c" -o "$O/qk.o"   # hosted: sinf and the like are builtins
            "$BIN/ld.lld" -e _start -o "$O/qk.elf" $L "$O/qk.o" "$O/libm_$lib.a" "$ROOT/build/gvm_exec/rt/librt.a"
            echo "== gpu_vm, libm $lib, $fl"
            "$CPU" "$O/qk.elf"
        done
        GI=$(dirname "$RVGCC")/../lib/gcc/riscv32-buildroot-linux-gnu/13.3.0/include
        for fl in "-O2 -fno-math-errno" "-O2 -ffast-math"; do
            "$RVGCC" -march=rv32imaf -mabi=ilp32 $fl -ffp-contract=off -nostdinc -isystem "$RVSYS/include" -isystem "$GI" \
                -static -nostdlib -o "$O/qk_rv" "$RVSYS/lib/crt1.o" "$RVSYS/lib/crti.o" "$HERE/qkern.c" -L"$RVSYS/lib" \
                -Wl,--start-group -lc -lcompiler_rt -Wl,--end-group "$RVSYS/lib/crtn.o"
            echo "== RISC-V (rv32imaf, GCC 13.3 $fl -ffp-contract=off, musl)"
            qemu-riscv32 "$O/qk_rv"
            python3 "$HERE/count_rv.py" "$O/qk_rv" "${RVGCC%gcc}nm"
        done ;;
    fit)
        gcc -O2 -o "$O/fit" "$HERE/fit.c" -lm && "$O/fit" ;;
    tables)
        gcc -O2 -o "$O/gen_logtab" "$HERE/gen_logtab.c" -lm && "$O/gen_logtab" > "$M/__gvm_log_data.c" ;;
    *) echo "run.sh: no step $s" >&2; exit 1 ;;
    esac
done
