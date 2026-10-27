//===-- GPUVMRegisterInfo.cpp - gpu_vm register information ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUVMRegisterInfo.h"
#include "GPUVMFrameLowering.h"
#include "GPUVMSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetFrameLowering.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"

#define GET_REGINFO_TARGET_DESC
#include "GPUVMGenRegisterInfo.inc"

using namespace llvm;

// -gpuvm-regs=N (the ISA search, GPU-VM's RESULTS.md): a machine of N registers, as the allocator sees it. Kept: r0-r12
// (zero, ra, sp, gp, tp, the eight arguments), then half of the rest as temporaries from r13 and half as saved
// registers from r32 (the frame pointer's), as a smaller ISA's own ABI would split them; the others are reserved.
static cl::opt<unsigned> GvmRegs("gpuvm-regs", cl::Hidden, cl::init(64),
                                 cl::desc("Registers the allocator may use (16-64)"));

GPUVMRegisterInfo::GPUVMRegisterInfo() : GPUVMGenRegisterInfo(GPUVM::R1) {}

const MCPhysReg *
GPUVMRegisterInfo::getCalleeSavedRegs(const MachineFunction *MF) const {
  return CSR_GPUVM_SaveList;
}

const uint32_t *
GPUVMRegisterInfo::getCallPreservedMask(const MachineFunction &MF,
                                        CallingConv::ID CC) const {
  return CSR_GPUVM_RegMask;
}

bool GPUVMRegisterInfo::hasBasePointer(const MachineFunction &MF) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  return MFI.hasVarSizedObjects() && hasStackRealignment(MF);
}

BitVector GPUVMRegisterInfo::getReservedRegs(const MachineFunction &MF) const {
  BitVector Reserved(getNumRegs());
  const TargetFrameLowering *TFL = MF.getSubtarget().getFrameLowering();
  // zero, sp, gp, tp
  for (MCPhysReg R : {GPUVM::R0, GPUVM::R2, GPUVM::R3, GPUVM::R4})
    markSuperRegs(Reserved, R);
  if (TFL->hasFP(MF))
    markSuperRegs(Reserved, GPUVM::R32);
  if (GvmRegs < 64) {
    unsigned Rest = GvmRegs > 13 ? GvmRegs - 13 : 0, Saved = Rest / 2, Temps = Rest - Saved;
    for (MCPhysReg R : GPUVM::GPRRegClass) {
      unsigned E = getEncodingValue(R);
      bool Kept = E <= 12 || (E >= 13 && E < 13 + Temps) || (E >= 32 && E < 32 + Saved);
      if (!Kept)
        markSuperRegs(Reserved, R);
    }
  }
  if (hasBasePointer(MF))
    markSuperRegs(Reserved, GPUVM::R33);
  assert(checkAllSuperRegsMarked(Reserved));
  return Reserved;
}

Register GPUVMRegisterInfo::getFrameRegister(const MachineFunction &MF) const {
  const TargetFrameLowering *TFL = MF.getSubtarget().getFrameLowering();
  return TFL->hasFP(MF) ? GPUVM::R32 : GPUVM::R2;
}

// Every op that takes a frame index has its immediate next to it (an I
// form's base and imm32): the offset goes there, whatever its size.
bool GPUVMRegisterInfo::eliminateFrameIndex(MachineBasicBlock::iterator II,
                                            int SPAdj, unsigned FIOperandNum,
                                            RegScavenger *RS) const {
  MachineInstr &MI = *II;
  MachineFunction &MF = *MI.getMF();
  const TargetFrameLowering *TFL = MF.getSubtarget().getFrameLowering();
  int FI = MI.getOperand(FIOperandNum).getIndex();
  Register FrameReg;
  int64_t Offset = TFL->getFrameIndexReference(MF, FI, FrameReg).getFixed();
  if (FrameReg == GPUVM::R2)
    Offset += SPAdj;
  MachineOperand &ImmOp = MI.getOperand(FIOperandNum + 1);
  if (!ImmOp.isImm())
    report_fatal_error("gpu_vm: a frame index without an immediate after it");
  Offset += ImmOp.getImm();
  if (!isInt<32>(Offset))
    report_fatal_error("gpu_vm: frame offset out of range");
  MI.getOperand(FIOperandNum).ChangeToRegister(FrameReg, false);
  ImmOp.setImm(Offset);
  return false;
}
