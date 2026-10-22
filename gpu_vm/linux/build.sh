#!/bin/bash
# Linux 5.17.11 for gpu_vm (gpu_vm/linux/README.md). Run in WSL.
#
#   build.sh [STEP ...]     steps, in order: sync config image dtbs headers (default: sync config image dtbs)
#   build.sh check          sync, config and every C file of the kernel compiled by the stand-in (below)
#
# KDIR the tree (~/gpu_vm/linux), OUT the build (~/gpu_vm/linux-build), LLVM_BIN clang's folder
# (~/gpu_vm/llvm-build/bin), GPUVM_TRIPLE the target (gpuvm-unknown-linux), JOBS. `check` takes clang from
# STANDIN_LLVM (~/gpu_vm/standin-llvm: a copy of llvm-build's bin and lib) when it is there.
#
# The stand-in (`check`): until clang compiles C for gpuvm, the same build with the triple riscv32-unknown-linux and
# a compiler driver (standin-cc.sh) that compiles each C file to assembly (the C, the headers and the inline asm's
# operands are checked; the asm text is gpu_vm's, so it is not assembled) and preprocesses each .S. No vmlinux.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KDIR="${KDIR:-$HOME/gpu_vm/linux}"
OUT="${OUT:-$HOME/gpu_vm/linux-build}"
LLVM_BIN="${LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}"
GPUVM_TRIPLE="${GPUVM_TRIPLE:-gpuvm-unknown-linux}"
JOBS="${JOBS:-$(nproc)}"
export PATH="$LLVM_BIN:$PATH"

steps=("$@")
[ ${#steps[@]} -gt 0 ] || steps=(sync config image dtbs)

mk() { make -C "$KDIR" O="$OUT" ARCH=gpuvm LLVM=1 CLANG_TARGET_FLAGS_gpuvm="$GPUVM_TRIPLE" -j"$JOBS" "$@"; }

for s in "${steps[@]}"; do
    case "$s" in
    sync) bash "$HERE/sync.sh" ;;
    config) mk gpuvm_defconfig ;;
    image) mk Image ;;
    dtbs) mk dtbs ;;
    headers) mk INSTALL_HDR_PATH="$OUT/usr" headers_install ;;
    check)
        bash "$HERE/sync.sh"
        OUT="$OUT-check"
        # a copy of clang that another build of LLVM does not change under us, when there is one
        if [ -x "${STANDIN_LLVM:-$HOME/gpu_vm/standin-llvm}/bin/clang" ]; then
            LLVM_BIN="${STANDIN_LLVM:-$HOME/gpu_vm/standin-llvm}/bin"
            export PATH="$LLVM_BIN:$PATH"
        fi
        GPUVM_TRIPLE=riscv32-unknown-linux
        mkcheck() { mk CC="$HERE/standin-cc.sh" "$@"; }
        export STANDIN_CLANG="$LLVM_BIN/clang"
        mkcheck gpuvm_defconfig
        mkcheck -k init/ kernel/ mm/ fs/ ipc/ security/ crypto/ block/ lib/ drivers/ arch/gpuvm/ \
            2>&1 | tee "$OUT.log" | grep -E "error|warning:" || true
        echo "build.sh check: $(grep -c 'error' "$OUT.log" || true) lines with 'error' (all in $OUT.log)"
        ;;
    *) echo "build.sh: no step $s" >&2; exit 1 ;;
    esac
done
