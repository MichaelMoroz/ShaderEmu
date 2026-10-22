#!/bin/bash
# Writes patches 0004 and 0005 again from the llvm-project tree (~/gpu_vm/llvm-project, or GVM_LLVM_SRC) after a
# change to one of their files there. Run in WSL: bash gpu_vm/llvm/patches/regen.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=${GVM_LLVM_SRC:-$HOME/gpu_vm/llvm-project}
cd "$SRC"
L4="llvm/include/llvm/IR/IntrinsicsGPUVM.td llvm/include/llvm/IR/Intrinsics.td llvm/include/llvm/IR/CMakeLists.txt
    llvm/lib/IR/Intrinsics.cpp llvm/lib/TargetParser/TargetDataLayout.cpp llvm/include/llvm/IR/RuntimeLibcalls.td
    llvm/lib/Object/RelocationResolver.cpp"
L5="clang/include/clang/Basic/BuiltinsGPUVM.td clang/include/clang/Basic/CMakeLists.txt
    clang/include/clang/Basic/TargetBuiltins.h clang/lib/Basic/Targets/GPUVM.h clang/lib/Basic/Targets/GPUVM.cpp
    clang/lib/Basic/Targets.cpp clang/lib/Basic/CMakeLists.txt clang/lib/CodeGen/Targets/GPUVM.cpp
    clang/lib/CodeGen/CMakeLists.txt clang/lib/CodeGen/TargetInfo.h clang/lib/CodeGen/CodeGenModule.cpp
    clang/lib/Driver/ToolChains/BareMetal.cpp clang/lib/Driver/ToolChains/BareMetal.h
    clang/lib/Driver/ToolChains/CommonArgs.cpp clang/include/clang/Driver/ToolChain.h
    clang/lib/Driver/ToolChains/Linux.cpp"
git add -N llvm/include/llvm/IR/IntrinsicsGPUVM.td clang/include/clang/Basic/BuiltinsGPUVM.td \
    clang/lib/Basic/Targets/GPUVM.h clang/lib/Basic/Targets/GPUVM.cpp clang/lib/CodeGen/Targets/GPUVM.cpp
git diff -- $L4 > "$HERE/0004-gpuvm-intrinsics-libcalls-datalayout-relocs.patch"
git diff -- $L5 > "$HERE/0005-clang-gpuvm.patch"
wc -l "$HERE"/0004* "$HERE"/0005*
