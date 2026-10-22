//===-- GPUVMMCAsmInfo.h - gpu_vm asm properties ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUVM_MCTARGETDESC_GPUVMMCASMINFO_H
#define LLVM_LIB_TARGET_GPUVM_MCTARGETDESC_GPUVMMCASMINFO_H

#include "llvm/MC/MCAsmInfoELF.h"

namespace llvm {
class Triple;

class GPUVMMCAsmInfo : public MCAsmInfoELF {
public:
  explicit GPUVMMCAsmInfo(const Triple &TT, const MCTargetOptions &Options);
  void printSpecifierExpr(raw_ostream &OS,
                          const MCSpecifierExpr &Expr) const override;
};
} // namespace llvm

#endif
