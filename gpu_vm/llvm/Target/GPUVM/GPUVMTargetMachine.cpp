//===-- GPUVMTargetMachine.cpp - gpu_vm target machine --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// No code generator yet: the MC layer (assembler, disassembler, object
// files) is all there is. The codegen registers its TargetMachine here.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Compiler.h"

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeGPUVMTarget() {}
