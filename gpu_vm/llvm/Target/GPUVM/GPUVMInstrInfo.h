//===-- GPUVMInstrInfo.h - gpu_vm instruction information -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUVM_GPUVMINSTRINFO_H
#define LLVM_LIB_TARGET_GPUVM_GPUVMINSTRINFO_H

#include "GPUVMRegisterInfo.h"
#include "MCTargetDesc/GPUVMMCTargetDesc.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

#define GET_INSTRINFO_HEADER
#include "GPUVMGenInstrInfo.inc"

namespace llvm {
class GPUVMSubtarget;

class GPUVMInstrInfo : public GPUVMGenInstrInfo {
  const GPUVMRegisterInfo RI;

public:
  explicit GPUVMInstrInfo(const GPUVMSubtarget &STI);

  const GPUVMRegisterInfo &getRegisterInfo() const { return RI; }

  void copyPhysReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator MI,
                   const DebugLoc &DL, Register DestReg, Register SrcReg,
                   bool KillSrc, bool RenamableDest = false,
                   bool RenamableSrc = false) const override;

  void storeRegToStackSlot(
      MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register SrcReg,
      bool IsKill, int FrameIndex, const TargetRegisterClass *RC,
      Register VReg,
      MachineInstr::MIFlag Flags = MachineInstr::NoFlags) const override;

  void loadRegFromStackSlot(
      MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register DestReg,
      int FrameIndex, const TargetRegisterClass *RC, Register VReg,
      unsigned SubReg = 0,
      MachineInstr::MIFlag Flags = MachineInstr::NoFlags) const override;

  Register isLoadFromStackSlot(const MachineInstr &MI,
                               int &FrameIndex) const override;
  Register isStoreToStackSlot(const MachineInstr &MI,
                              int &FrameIndex) const override;

  bool analyzeBranch(MachineBasicBlock &MBB, MachineBasicBlock *&TBB,
                     MachineBasicBlock *&FBB,
                     SmallVectorImpl<MachineOperand> &Cond,
                     bool AllowModify) const override;
  unsigned removeBranch(MachineBasicBlock &MBB,
                        int *BytesRemoved = nullptr) const override;
  unsigned insertBranch(MachineBasicBlock &MBB, MachineBasicBlock *TBB,
                        MachineBasicBlock *FBB, ArrayRef<MachineOperand> Cond,
                        const DebugLoc &DL,
                        int *BytesAdded = nullptr) const override;
  bool
  reverseBranchCondition(SmallVectorImpl<MachineOperand> &Cond) const override;

  MachineBasicBlock *getBranchDestBlock(const MachineInstr &MI) const override;
  bool isBranchOffsetInRange(unsigned BranchOpc,
                             int64_t BrOffset) const override;
  void insertIndirectBranch(MachineBasicBlock &MBB,
                            MachineBasicBlock &NewDestBB,
                            MachineBasicBlock &RestoreBB, const DebugLoc &DL,
                            int64_t BrOffset, RegScavenger *RS) const override;

  // Every op is at most a bundle (the streamer packs them; it never splits
  // one): 16 bytes, an upper bound for branch relaxation.
  unsigned getInstSizeInBytes(const MachineInstr &MI) const override;

  MCInst getNop() const override;

  // li (addi rd, zero, imm) alone: made again rather than spilled.
  bool isReMaterializableImpl(const MachineInstr &MI) const override;

  static bool isCondBranch(unsigned Opc);
  static unsigned invertBranch(unsigned Opc);
};

} // namespace llvm

#endif
