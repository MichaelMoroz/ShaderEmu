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
  else
    OS << "(";
  printExpr(OS, *Expr.getSubExpr());
  OS << ')';
}
