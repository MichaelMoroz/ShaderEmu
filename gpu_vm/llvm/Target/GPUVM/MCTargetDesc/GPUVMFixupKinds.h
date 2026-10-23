//===-- GPUVMFixupKinds.h - gpu_vm fixups -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUVM_MCTARGETDESC_GPUVMFIXUPKINDS_H
#define LLVM_LIB_TARGET_GPUVM_MCTARGETDESC_GPUVMFIXUPKINDS_H

#include "llvm/MC/MCFixup.h"

namespace llvm {
namespace GPUVM {
// A fixup's offset is its word in the bundle: the long immediate is word 2
// (+8), the control word word 3 (+12). PC-relative values are from the
// bundle's start.
enum Fixups {
  fixup_gvm_imm32 = FirstTargetFixupKind, // word 2: S + A (R_GVM_IMM32,
                                          // or R_GVM_TPREL32 by specifier)
  fixup_gvm_pcimm32,                      // word 2: S + A - bundle
  fixup_gvm_br9,                          // control word [25:17]
  fixup_gvm_j,    // j: near (br9, beq r0, r0) or far (j18, cc 0), as it lands
  fixup_gvm_j18,  // call: control word [31:14]
  fixup_gvm_imm12, // op word [31:20]: S + A
  fixup_gvm_fnrel32, // word 2: S + A - the function (R_GVM_PCIMM32)

  fixup_gvm_invalid,
  NumTargetFixupKinds = fixup_gvm_invalid - FirstTargetFixupKind
};

enum Specifier : uint16_t {
  S_None = 0,
  S_TPREL = 1, // %tprel(sym)
  S_FNREL = 2, // %fnrel(sym - fn): fn a symbol of the fixup's section
};
} // namespace GPUVM
} // namespace llvm

#endif
