#!/bin/bash
# Builds clang and lld with the X86, RISCV and gpu_vm targets (gpu_vm/GPU_VM.md, "Plan" 1), in WSL:
#   wsl -- bash /mnt/e/.../gpu_vm/llvm/build.sh [ninja targets...]
# The source is llvm-project 23.1.4 in ~/gpu_vm/llvm-project (a shallow clone of llvmorg-23.1.4); this
# folder's Target/GPUVM is linked into it as llvm/lib/Target/GPUVM, and patches/*.patch (clang's target, lld's
# ELF, the triple and ELF numbers) are applied once each. The build is ~/gpu_vm/llvm-build: Release with
# assertions, shared libraries (a change to the target relinks one library), installed to ~/gpu_vm/llvm.
# With no Target/GPUVM here yet it builds X86 and RISCV alone.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${GVM_LLVM_SRC:-$HOME/gpu_vm/llvm-project}
OUT=${GVM_LLVM_BUILD:-$HOME/gpu_vm/llvm-build}
PREFIX=${GVM_LLVM_PREFIX:-$HOME/gpu_vm/llvm}
CMAKE=${CMAKE:-$HOME/shaderemu-linux/cmake/bin/cmake}
export PATH=$HOME/.local/bin:$PATH

if [ ! -d "$SRC/llvm" ]; then
    git clone --depth 1 --branch llvmorg-23.1.4 https://github.com/llvm/llvm-project.git "$SRC"
fi

EXPERIMENTAL=
if [ -d "$HERE/Target/GPUVM" ]; then
    if [ ! -e "$SRC/llvm/lib/Target/GPUVM" ]; then ln -s "$HERE/Target/GPUVM" "$SRC/llvm/lib/Target/GPUVM"; fi
    if [ ! -e "$SRC/llvm/test/MC/GPUVM" ]; then ln -s "$HERE/test/MC/GPUVM" "$SRC/llvm/test/MC/GPUVM"; fi
    EXPERIMENTAL=GPUVM
    for p in "$HERE"/patches/*.patch; do
        [ -e "$p" ] || continue
        if git -C "$SRC" apply --check "$p" 2>/dev/null; then
            git -C "$SRC" apply "$p" && echo "applied $(basename "$p")"
        elif ! git -C "$SRC" apply --check -R "$p" 2>/dev/null; then
            echo "$(basename "$p") neither applies nor is applied" >&2; exit 1
        fi
    done
fi

if [ ! -f "$OUT/build.ninja" ] || [ "$(cat "$OUT/.gvm_targets" 2>/dev/null)" != "$EXPERIMENTAL" ]; then
    "$CMAKE" -S "$SRC/llvm" -B "$OUT" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=ON -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DLLVM_ENABLE_PROJECTS="clang;lld" \
        -DLLVM_TARGETS_TO_BUILD="X86;RISCV" -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD="$EXPERIMENTAL" \
        -DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-unknown-linux-gnu \
        -DLLVM_OPTIMIZED_TABLEGEN=ON -DLLVM_PARALLEL_COMPILE_JOBS=${JOBS:-14} -DLLVM_PARALLEL_LINK_JOBS=4 \
        -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_ENABLE_BINDINGS=OFF \
        -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF \
        -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF
    echo "$EXPERIMENTAL" > "$OUT/.gvm_targets"
fi

if [ $# -eq 0 ]; then
    ninja -C "$OUT" clang lld llc llvm-mc llvm-objdump llvm-readelf llvm-ar llvm-ranlib llvm-nm llvm-objcopy FileCheck not count
else
    ninja -C "$OUT" "$@"
fi
