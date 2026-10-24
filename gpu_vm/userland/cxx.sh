#!/bin/bash
# The C++ runtime for gpu_vm's programs (gpu_vm/userland/README.md, "C++"): LLVM's libunwind, libc++abi and libc++
# from ~/gpu_vm/llvm-project, static, for musl, into the sysroot (lib/libunwind.a, libc++abi.a, libc++.a and
# include/c++/v1). `gpuvm-linux-c++` compiles and links with them: exceptions, run-time types, the whole library.
#   bash gpu_vm/userland/cxx.sh            (WSL; after build.sh: musl and the builtins; a few minutes)
# libunwind is built bare-metal: a flat program has no program headers to find its unwind tables by, so it finds them
# by flat.ld's __eh_frame_start/__eh_frame_end and the index ld.lld makes (--eh-frame-hdr).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYSROOT="${SYSROOT:-$HOME/gpu_vm/sysroot}"
LLVM_BIN="${LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}"
SRC="${LLVM_SRC:-$HOME/gpu_vm/llvm-project}"
B="${CXX_BUILD:-$HOME/gpu_vm/cxx-build}"
JOBS="${JOBS:-$(nproc)}"
CMAKE=${CMAKE:-$HOME/shaderemu-linux/cmake/bin/cmake}   # (as gpu_vm/llvm/build.sh)
export PATH=$HOME/.local/bin:$PATH
RES="$("$LLVM_BIN/clang" -print-resource-dir)"

# gpu_vm in libunwind (Registers_gpuvm, its context's save and restore): applied to the tree once
python3 "$HERE/../llvm/patches/libunwind_gpuvm.py" "$SRC/libunwind"

# a new compiler builds everything again (ninja does not know the compiler as an input)
STAMP="$(stat -L -c %Y "$LLVM_BIN/clang" "$LLVM_BIN/llc")"
[ "$(cat "$B/.compiler" 2>/dev/null)" = "$STAMP" ] || rm -rf "$B"
mkdir -p "$B" && echo "$STAMP" > "$B/.compiler"

FLAGS="-fPIE -nostdlibinc -isystem $SYSROOT/include -isystem $RES/include -O2 -ffunction-sections -fdata-sections"
"$CMAKE" -S "$SRC/runtimes" -B "$B" -G Ninja \
    -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$B/install" \
    -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=gpuvm \
    -DCMAKE_C_COMPILER="$LLVM_BIN/clang" -DCMAKE_CXX_COMPILER="$LLVM_BIN/clang++" -DCMAKE_ASM_COMPILER="$LLVM_BIN/clang" \
    -DCMAKE_C_COMPILER_TARGET=gpuvm-unknown-linux -DCMAKE_CXX_COMPILER_TARGET=gpuvm-unknown-linux \
    -DCMAKE_ASM_COMPILER_TARGET=gpuvm-unknown-linux \
    -DCMAKE_AR="$LLVM_BIN/llvm-ar" -DCMAKE_RANLIB="$LLVM_BIN/llvm-ranlib" -DCMAKE_NM="$LLVM_BIN/llvm-nm" \
    -DCMAKE_C_FLAGS="$FLAGS" -DCMAKE_CXX_FLAGS="$FLAGS" -DCMAKE_ASM_FLAGS="$FLAGS" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DLIBUNWIND_ENABLE_SHARED=OFF -DLIBUNWIND_IS_BAREMETAL=ON -DLIBUNWIND_USE_COMPILER_RT=ON \
    -DLIBCXXABI_ENABLE_SHARED=OFF -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_USE_COMPILER_RT=ON \
    -DLIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF \
    -DLIBCXXABI_ENABLE_STATIC_UNWINDER=ON -DLIBCXXABI_STATICALLY_LINK_UNWINDER_IN_STATIC_LIBRARY=OFF \
    -DLIBCXX_ENABLE_SHARED=OFF -DLIBCXX_HAS_MUSL_LIBC=ON -DLIBCXX_USE_COMPILER_RT=ON \
    -DLIBCXX_CXX_ABI=libcxxabi -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=OFF \
    -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF -DLIBUNWIND_INCLUDE_TESTS=OFF \
    > "$B.cmake.log" 2>&1 || { tail -40 "$B.cmake.log"; exit 1; }
ninja -C "$B" -j"$JOBS" install > "$B.ninja.log" 2>&1 || { grep -E "error|FAILED" "$B.ninja.log" | head -40; exit 1; }

I="$B/install"
cp "$I/lib/libunwind.a" "$I/lib/libc++abi.a" "$I/lib/libc++.a" "$SYSROOT/lib/"
[ -f "$I/lib/libc++experimental.a" ] && cp "$I/lib/libc++experimental.a" "$SYSROOT/lib/"
rm -rf "$SYSROOT/include/c++" && mkdir -p "$SYSROOT/include/c++"
cp -r "$I/include/c++/v1" "$SYSROOT/include/c++/"
# (the configuration header libc++ writes for this target is under include/<triple>/c++/v1 in a per-target layout)
for d in "$I"/include/*/c++/v1; do [ -d "$d" ] && cp -r "$d/." "$SYSROOT/include/c++/v1/"; done
ls -l "$SYSROOT"/lib/libunwind.a "$SYSROOT"/lib/libc++abi.a "$SYSROOT"/lib/libc++.a
