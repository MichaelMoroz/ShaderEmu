//===-- GPUVMFrameLowering.cpp - gpu_vm frame lowering --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The frame: sp moves once by the frame's size (any size: an immediate is 32
// bits), the callee-saved registers are stored just under the caller's sp,
// and fp (s0) is the caller's sp when there is one. A realigned frame keeps
// the caller's sp in fp and its objects from the realigned sp, or from bp
// (s1) when it also has objects of variable size.
//
//   prologue: addi sp, sp, -N; sw ra/s*, ...(sp); addi fp, sp, N;
//             andi sp, sp, -align; mv bp, sp
//   epilogue: addi sp, fp, -N (sp moved); lw ra/s*, ...(sp); addi sp, sp, N
//
//===----------------------------------------------------------------------===//

#include "GPUVMFrameLowering.h"
#include "GPUVMInstrInfo.h"
#include "GPUVMMachineFunctionInfo.h"
#include "GPUVMRegisterInfo.h"
#include "GPUVMSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/RegisterScavenging.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

bool GPUVMFrameLowering::hasFPImpl(const MachineFunction &MF) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  const TargetRegisterInfo *TRI = STI.getRegisterInfo();
  return MF.getTarget().Options.DisableFramePointerElim(MF) ||
         MFI.hasVarSizedObjects() || MFI.isFrameAddressTaken() ||
         TRI->hasStackRealignment(MF);
}

bool GPUVMFrameLowering::hasReservedCallFrame(const MachineFunction &MF) const {
  return !MF.getFrameInfo().hasVarSizedObjects();
}

static void adjustReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                      const DebugLoc &DL, const TargetInstrInfo &TII,
                      Register Dst, Register Src, int64_t Val,
                      MachineInstr::MIFlag Flag) {
  if (Dst == Src && Val == 0)
    return;
  BuildMI(MBB, I, DL, TII.get(GPUVM::ADDI), Dst)
      .addReg(Src)
      .addImm(Val)
      .setMIFlag(Flag);
}

MachineBasicBlock::iterator GPUVMFrameLowering::eliminateCallFramePseudoInstr(
    MachineFunction &MF, MachineBasicBlock &MBB,
    MachineBasicBlock::iterator I) const {
  if (!hasReservedCallFrame(MF)) {
    int64_t Amount = alignTo(I->getOperand(0).getImm(), getStackAlign());
    if (Amount != 0) {
      if (I->getOpcode() == GPUVM::ADJCALLSTACKDOWN)
        Amount = -Amount;
      adjustReg(MBB, I, I->getDebugLoc(), *STI.getInstrInfo(), GPUVM::R2,
                GPUVM::R2, Amount, MachineInstr::NoFlags);
    }
  }
  return MBB.erase(I);
}

void GPUVMFrameLowering::determineCalleeSaves(MachineFunction &MF,
                                              BitVector &SavedRegs,
                                              RegScavenger *RS) const {
  TargetFrameLowering::determineCalleeSaves(MF, SavedRegs, RS);
  if (hasFP(MF))
    SavedRegs.set(GPUVM::R32);
  if (STI.getRegisterInfo()->hasBasePointer(MF))
    SavedRegs.set(GPUVM::R33);
}

bool GPUVMFrameLowering::spillCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    ArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  const TargetInstrInfo &TII = *STI.getInstrInfo();
  DebugLoc DL = MI != MBB.end() ? MI->getDebugLoc() : DebugLoc();
  for (const CalleeSavedInfo &CS : CSI) {
    Register Reg = CS.getReg();
    // ra and the callee-saved registers live into the function.
    if (!MBB.isLiveIn(Reg))
      MBB.addLiveIn(Reg);
    BuildMI(MBB, MI, DL, TII.get(GPUVM::SW))
        .addReg(Reg, RegState::Kill)
        .addFrameIndex(CS.getFrameIdx())
        .addImm(0)
        .setMIFlag(MachineInstr::FrameSetup);
  }
  return true;
}

bool GPUVMFrameLowering::restoreCalleeSavedRegisters(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
    MutableArrayRef<CalleeSavedInfo> CSI, const TargetRegisterInfo *TRI) const {
  const TargetInstrInfo &TII = *STI.getInstrInfo();
  DebugLoc DL = MI != MBB.end() ? MI->getDebugLoc() : DebugLoc();
  for (const CalleeSavedInfo &CS : CSI)
    BuildMI(MBB, MI, DL, TII.get(GPUVM::LW), CS.getReg())
        .addFrameIndex(CS.getFrameIdx())
        .addImm(0)
        .setMIFlag(MachineInstr::FrameDestroy);
  return true;
}

void GPUVMFrameLowering::emitPrologue(MachineFunction &MF,
                                      MachineBasicBlock &MBB) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  const GPUVMRegisterInfo *RI = STI.getRegisterInfo();
  const TargetInstrInfo &TII = *STI.getInstrInfo();
  MachineBasicBlock::iterator MBBI = MBB.begin();
  DebugLoc DL;

  uint64_t StackSize = alignTo(MFI.getStackSize(), getStackAlign());
  MFI.setStackSize(StackSize);
  if (StackSize == 0 && !hasFP(MF))
    return;

  adjustReg(MBB, MBBI, DL, TII, GPUVM::R2, GPUVM::R2,
            -static_cast<int64_t>(StackSize), MachineInstr::FrameSetup);

  // Past the callee-saved registers' stores.
  while (MBBI != MBB.end() && MBBI->getFlag(MachineInstr::FrameSetup))
    ++MBBI;

  if (hasFP(MF))
    adjustReg(MBB, MBBI, DL, TII, GPUVM::R32, GPUVM::R2, StackSize,
              MachineInstr::FrameSetup);

  if (RI->hasStackRealignment(MF)) {
    Align MaxAlign = MFI.getMaxAlign();
    BuildMI(MBB, MBBI, DL, TII.get(GPUVM::ANDI), GPUVM::R2)
        .addReg(GPUVM::R2)
        .addImm(-static_cast<int64_t>(MaxAlign.value()))
        .setMIFlag(MachineInstr::FrameSetup);
    if (RI->hasBasePointer(MF))
      adjustReg(MBB, MBBI, DL, TII, GPUVM::R33, GPUVM::R2, 0,
                MachineInstr::FrameSetup);
  }
}

void GPUVMFrameLowering::emitEpilogue(MachineFunction &MF,
                                      MachineBasicBlock &MBB) const {
  MachineFrameInfo &MFI = MF.getFrameInfo();
  const GPUVMRegisterInfo *RI = STI.getRegisterInfo();
  const TargetInstrInfo &TII = *STI.getInstrInfo();
  MachineBasicBlock::iterator MBBI = MBB.getFirstTerminator();
  DebugLoc DL = MBBI != MBB.end() ? MBBI->getDebugLoc() : DebugLoc();
  uint64_t StackSize = MFI.getStackSize();
  if (StackSize == 0 && !hasFP(MF))
    return;

  // Before the callee-saved registers' loads.
  MachineBasicBlock::iterator FirstRestore = MBBI;
  while (FirstRestore != MBB.begin() &&
         std::prev(FirstRestore)->getFlag(MachineInstr::FrameDestroy))
    --FirstRestore;

  // sp as the prologue left it, before realignment or allocas
  if (hasFP(MF) && (MFI.hasVarSizedObjects() || RI->hasStackRealignment(MF)))
    adjustReg(MBB, FirstRestore, DL, TII, GPUVM::R2, GPUVM::R32,
              -static_cast<int64_t>(StackSize), MachineInstr::FrameDestroy);

  adjustReg(MBB, MBBI, DL, TII, GPUVM::R2, GPUVM::R2, StackSize,
            MachineInstr::FrameDestroy);
}

StackOffset
GPUVMFrameLowering::getFrameIndexReference(const MachineFunction &MF, int FI,
                                           Register &FrameReg) const {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  const GPUVMRegisterInfo *RI = STI.getRegisterInfo();
  int64_t Offset = MFI.getObjectOffset(FI) - getOffsetOfLocalArea() +
                   MFI.getOffsetAdjustment();
  uint64_t StackSize = MFI.getStackSize();

  // The callee-saved registers' slots are only used by the prologue and the
  // epilogue, where sp is the prologue's (before realignment).
  const std::vector<CalleeSavedInfo> &CSI = MFI.getCalleeSavedInfo();
  bool IsCSR = false;
  for (const CalleeSavedInfo &CS : CSI)
    if (CS.getFrameIdx() == FI)
      IsCSR = true;

  if (IsCSR) {
    FrameReg = GPUVM::R2;
    return StackOffset::getFixed(Offset + StackSize);
  }
  if (MFI.isFixedObjectIndex(FI)) {
    if (hasFP(MF)) {
      FrameReg = GPUVM::R32;
      return StackOffset::getFixed(Offset);
    }
    FrameReg = GPUVM::R2;
    return StackOffset::getFixed(Offset + StackSize);
  }
  if (RI->hasStackRealignment(MF)) {
    FrameReg = RI->hasBasePointer(MF) ? GPUVM::R33 : GPUVM::R2;
    return StackOffset::getFixed(Offset + StackSize);
  }
  if (MFI.hasVarSizedObjects()) {
    FrameReg = GPUVM::R32;
    return StackOffset::getFixed(Offset);
  }
  FrameReg = GPUVM::R2;
  return StackOffset::getFixed(Offset + StackSize);
}
