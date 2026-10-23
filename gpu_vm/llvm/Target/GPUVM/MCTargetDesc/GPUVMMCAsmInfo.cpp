//===-- GPUVMMCAsmInfo.cpp - gpu_vm asm properties ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/GPUVMMCAsmInfo.h"
#include "MCTargetDesc/GPUVMFixupKinds.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

using namespace llvm;

GPUVMMCAsmInfo::GPUVMMCAsmInfo(const Triple &TT,
                               const MCTargetOptions &Options)
    : MCAsmInfoELF(Options) {
  CodePointerSize = 4;
  CalleeSaveStackSlotSize = 4;
  IsLittleEndian = true;
  CommentString = "#";
  SeparatorString = ";";
  AlignmentIsInBytes = false;
  SupportsDebugInformation = true;
  ExceptionsType = ExceptionHandling::DwarfCFI;
  Data16bitsDirective = "\t.half\t";
  Data32bitsDirective = "\t.word\t";
  Data64bitsDirective = "\t.dword\t";
  // Code is bundles of 16 bytes.
  MinInstAlignment = 16;
  MaxInstLength = 16;
}

void GPUVMMCAsmInfo::printSpecifierExpr(raw_ostream &OS,
                                        const MCSpecifierExpr &Expr) const {
  if (Expr.getSpecifier() == GPUVM::S_TPREL)
    OS << "%tprel(";
  else if (Expr.getSpecifier() == GPUVM::S_FNREL)
    OS << "%fnrel(";
  else
    OS << "(";
  printExpr(OS, *Expr.getSubExpr());
  OS << ')';
}

// %fnrel(sym - fn) keeps its difference (the object writer makes it relative
// to the fixup); anything else as MCAsmInfo has it.
bool GPUVMMCAsmInfo::evaluateAsRelocatableImpl(const MCSpecifierExpr &E,
                                               MCValue &Res,
                                               const MCAssembler *Asm) const {
  if (E.getSpecifier() != GPUVM::S_FNREL)
    return MCAsmInfo::evaluateAsRelocatableImpl(E, Res, Asm);
  if (!E.getSubExpr()->evaluateAsRelocatable(Res, Asm))
    return false;
  Res.setSpecifier(E.getSpecifier());
  return true;
}
