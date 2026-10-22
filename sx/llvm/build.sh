#!/bin/bash
# Builds clang and lld with the X86, RISCV and SX targets (sx/SX.md, "Plan" 1), in WSL:
#   wsl -- bash /mnt/e/.../sx/llvm/build.sh [ninja targets...]
# The source is llvm-project 23.1.4 in ~/sx/llvm-project (a shallow clone of llvmorg-23.1.4); this
# folder's Target/SX is linked into it as llvm/lib/Target/SX, and patches/*.patch (clang's target, lld's
# ELF, the triple and ELF numbers) are applied once each. The build is ~/sx/llvm-build: Release with
# assertions, shared libraries (a change to the target relinks one library), installed to ~/sx/llvm.
# With no Target/SX here yet it builds X86 and RISCV alone.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${SX_LLVM_SRC:-$HOME/sx/llvm-project}
OUT=${SX_LLVM_BUILD:-$HOME/sx/llvm-build}
PREFIX=${SX_LLVM_PREFIX:-$HOME/sx/llvm}
CMAKE=${CMAKE:-$HOME/shaderemu-linux/cmake/bin/cmake}
export PATH=$HOME/.local/bin:$PATH

if [ ! -d "$SRC/llvm" ]; then
    git clone --depth 1 --branch llvmorg-23.1.4 https://github.com/llvm/llvm-project.git "$SRC"
fi

EXPERIMENTAL=
if [ -d "$HERE/Target/SX" ]; then
    if [ ! -e "$SRC/llvm/lib/Target/SX" ]; then ln -s "$HERE/Target/SX" "$SRC/llvm/lib/Target/SX"; fi
    EXPERIMENTAL=SX
    for p in "$HERE"/patches/*.patch; do
        [ -e "$p" ] || continue
        if git -C "$SRC" apply --check "$p" 2>/dev/null; then
            git -C "$SRC" apply "$p" && echo "applied $(basename "$p")"
        elif ! git -C "$SRC" apply --check -R "$p" 2>/dev/null; then
            echo "$(basename "$p") neither applies nor is applied" >&2; exit 1
        fi
    done
fi

if [ ! -f "$OUT/build.ninja" ] || [ "$(cat "$OUT/.sx_targets" 2>/dev/null)" != "$EXPERIMENTAL" ]; then
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
    echo "$EXPERIMENTAL" > "$OUT/.sx_targets"
fi

if [ $# -eq 0 ]; then
    ninja -C "$OUT" clang lld llc llvm-mc llvm-objdump llvm-readelf llvm-ar llvm-ranlib llvm-nm llvm-objcopy FileCheck not count
else
    ninja -C "$OUT" "$@"
fi
