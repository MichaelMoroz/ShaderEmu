#!/bin/bash
# gvm_lone.c for gpu_vm with its clang, the games' flags (gpu_vm/userland/apps/toolchain.sh: -O2, PIE, fmad and the
# card's approximate functions) into build/gamebench/gvm_lone.elf (gvmbench's g_*_cc cases), and gpu_vm's own count
# of its bundles a frame (gvm_cpu --stats). In WSL, after gpu_vm/tick/real/build.sh made the runtime.
set -eu
R=$(cd "$(dirname "$0")/../../.." && pwd)
BIN=${GVM_LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}
O=$R/build/gamebench; mkdir -p $O
RTDIR=$R/build/gvm_exec
"$BIN/clang" --target=gpuvm-unknown-elf -nostdlibinc -I$R/gpu_vm/llvm/test/exec/rt/include -w -O2 -fPIE \
    -ffp-contract=fast -fapprox-func -c -o $O/gvm_lone.o "$R/gpu_vm/tick/gamebench_rv/gvm_lone.c"
"$BIN/ld.lld" -e _start -o $O/gvm_lone.elf "$RTDIR/crt0.o" $O/gvm_lone.o "$RTDIR/rt/librt.a"
echo built $O/gvm_lone.elf
