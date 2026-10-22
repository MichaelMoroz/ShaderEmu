//===-- GPUVMTargetInfo.cpp - gpu_vm target implementation ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TargetInfo/GPUVMTargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

Target &llvm::getTheGPUVMTarget() {
  static Target TheGPUVMTarget;
  return TheGPUVMTarget;
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeGPUVMTargetInfo() {
  RegisterTarget<Triple::gpuvm> X(getTheGPUVMTarget(), "gpuvm",
                                  "gpu_vm (the shader machine's own ISA)",
                                  "GPUVM");
}
