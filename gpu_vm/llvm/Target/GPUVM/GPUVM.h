//===-- GPUVM.h - Top-level interface for gpu_vm ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUVM_GPUVM_H
#define LLVM_LIB_TARGET_GPUVM_GPUVM_H

#include "MCTargetDesc/GPUVMMCTargetDesc.h"

namespace llvm {
class AsmPrinter;
class FunctionPass;
class GPUVMTargetMachine;
class MachineInstr;
class MCInst;
class MCOperand;
class MachineOperand;
class PassRegistry;

FunctionPass *createGPUVMISelDag(GPUVMTargetMachine &TM);
FunctionPass *createGPUVMCountedLoopsPass();
FunctionPass *createGPUVMHoistConstantsPass();
FunctionPass *createGPUVMBranchFillPass();
FunctionPass *createGPUVMGlobalFillPass();
FunctionPass *createGPUVMZeroExtPass();
FunctionPass *createGPUVMLoadHoistPass();
FunctionPass *createGPUVMRebasePass();
FunctionPass *createGPUVMPairsPass();
// Whether a load reads inside a global the program defines (its memory
// operand says which and where): it cannot fault wherever it runs
// (GPUVMRebase's new bases hide the symbol from the fills otherwise).
bool gpuvmLoadInsideGlobal(const MachineInstr &MI);

void lowerGPUVMMachineInstrToMCInst(const MachineInstr *MI, MCInst &OutMI,
                                    AsmPrinter &AP);
bool lowerGPUVMMachineOperandToMCOperand(const MachineOperand &MO,
                                         MCOperand &MCOp, AsmPrinter &AP);

void initializeGPUVMAsmPrinterPass(PassRegistry &);
void initializeGPUVMDAGToDAGISelLegacyPass(PassRegistry &);

namespace GPUVMII {
// Target flags of a machine operand (a symbol's).
enum TOF : unsigned {
  MO_None = 0,
  MO_TPREL = 1, // %tprel(sym): from the thread pointer
  MO_FNREL = 2, // %fnrel(sym - fn): from the function's start (PIC)
  MO_FNBASE = 3, // the function's start as a label of its own (lpc's base
                 // of %fnrel: not the symbol, which --wrap may move)
};
} // namespace GPUVMII
} // namespace llvm

#endif
