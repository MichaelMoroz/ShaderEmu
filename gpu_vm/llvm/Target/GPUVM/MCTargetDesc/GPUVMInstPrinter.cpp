//===-- GPUVMInstPrinter.cpp - gpu_vm MCInst to assembly ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/GPUVMInstPrinter.h"
#include "MCTargetDesc/GPUVMMCTargetDesc.h"
#include "llvm/ADT/bit.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

#define PRINT_ALIAS_INSTR
#include "GPUVMGenAsmWriter.inc"

const char *GPUVMInstPrinter::getSRName(unsigned N) {
  static const char *const Names[] = {
      "status", "epc",     "cause",   "tval",   "tvec",   "scratch",
      "ien",    "ipend",   "tcmp_lo", "tcmp_hi", "time_lo", "time_hi",
      "cycle",  "coreid",  "player",  "random", "left",   "ops"};
  return N < std::size(Names) ? Names[N] : nullptr;
}

void GPUVMInstPrinter::printRegName(raw_ostream &O, MCRegister Reg) {
  markup(O, Markup::Register) << getRegisterName(Reg, GPUVM::ABIRegAltName);
}

void GPUVMInstPrinter::printOne(const MCInst *MI, uint64_t Address,
                                const MCSubtargetInfo &STI, raw_ostream &O) {
  std::string S;
  raw_string_ostream SO(S);
  if (!printAliasInstr(MI, Address, STI, SO))
    printInstruction(MI, Address, STI, SO);
  StringRef R = StringRef(S).trim("\t ");
  size_t Tab = R.find('\t');
  if (Tab == StringRef::npos) {
    O << R;
    return;
  }
  O << R.substr(0, Tab) << ' ' << R.substr(Tab + 1);
}

// A bundle prints on one line: `op; op; control ;;`.
void GPUVMInstPrinter::printInst(const MCInst *MI, uint64_t Address,
                                 StringRef Annot, const MCSubtargetInfo &STI,
                                 raw_ostream &O) {
  if (MI->getOpcode() == GPUVM::BUNDLE) {
    int64_t Path = -1;
    bool Any = false;
    O << '\t';
    for (const MCOperand &MO : *MI) {
      if (MO.isImm()) {
        Path = MO.getImm();
        continue;
      }
      if (!MO.isInst())
        continue;
      if (Any)
        O << "; ";
      printOne(MO.getInst(), Address, STI, O);
      Any = true;
    }
    O << (Any ? " ;;" : ";;");
    if (Path >= 0)
      O << " # path " << Path;
  } else if (!printAliasInstr(MI, Address, STI, O)) {
    printInstruction(MI, Address, STI, O);
  }
  printAnnotation(O, Annot);
}

void GPUVMInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                    const MCSubtargetInfo &STI,
                                    raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (MO.isReg()) {
    printRegName(O, MO.getReg());
    return;
  }
  if (MO.isImm()) {
    markup(O, Markup::Immediate) << static_cast<int32_t>(MO.getImm());
    return;
  }
  assert(MO.isExpr() && "unknown operand kind");
  MAI.printExpr(O, *MO.getExpr());
}

void GPUVMInstPrinter::printBranchTarget(const MCInst *MI, uint64_t Address,
                                         unsigned OpNo,
                                         const MCSubtargetInfo &STI,
                                         raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (!MO.isImm()) {
    printOperand(MI, OpNo, STI, O);
    return;
  }
  // A byte offset from the bundle.
  if (PrintBranchImmAsAddress)
    markup(O, Markup::Target)
        << formatHex(static_cast<uint64_t>(
               static_cast<uint32_t>(Address + MO.getImm())));
  else
    markup(O, Markup::Immediate) << MO.getImm();
}

// A float's bits: as a float literal when one reads back exactly, else hex.
void GPUVMInstPrinter::printFImm(const MCInst *MI, unsigned OpNo,
                                 const MCSubtargetInfo &STI, raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (!MO.isImm()) {
    printOperand(MI, OpNo, STI, O);
    return;
  }
  uint32_t Bits = static_cast<uint32_t>(MO.getImm());
  float F = bit_cast<float>(Bits);
  if (std::isfinite(F)) {
    char Buf[40];
    snprintf(Buf, sizeof(Buf), "%.9g", F);
    if (bit_cast<uint32_t>(strtof(Buf, nullptr)) == Bits) {
      StringRef S(Buf);
      O << S;
      if (S.find_first_of(".e") == StringRef::npos)
        O << ".0";
      return;
    }
  }
  O << formatHex(static_cast<uint64_t>(Bits));
}

void GPUVMInstPrinter::printSR(const MCInst *MI, unsigned OpNo,
                               const MCSubtargetInfo &STI, raw_ostream &O) {
  const MCOperand &MO = MI->getOperand(OpNo);
  if (MO.isImm())
    if (const char *Name = getSRName(MO.getImm())) {
      O << Name;
      return;
    }
  printOperand(MI, OpNo, STI, O);
}
