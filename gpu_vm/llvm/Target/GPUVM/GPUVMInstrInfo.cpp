//===-- GPUVMInstrInfo.cpp - gpu_vm instruction information ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUVMInstrInfo.h"
#include "GPUVMSubtarget.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetMachine.h"

#define GET_INSTRINFO_CTOR_DTOR
#include "GPUVMGenInstrInfo.inc"

using namespace llvm;

GPUVMInstrInfo::GPUVMInstrInfo(const GPUVMSubtarget &STI)
    : GPUVMGenInstrInfo(STI, RI, GPUVM::ADJCALLSTACKDOWN,
                        GPUVM::ADJCALLSTACKUP),
      RI() {}

MCInst GPUVMInstrInfo::getNop() const {
  return MCInstBuilder(GPUVM::ADD)
      .addReg(GPUVM::R0)
      .addReg(GPUVM::R0)
      .addReg(GPUVM::R0);
}

void GPUVMInstrInfo::copyPhysReg(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MI,
                                 const DebugLoc &DL, Register DestReg,
                                 Register SrcReg, bool KillSrc,
                                 bool RenamableDest, bool RenamableSrc) const {
  if (GPUVM::GPRRegClass.contains(DestReg, SrcReg)) {
    BuildMI(MBB, MI, DL, get(GPUVM::ADDI), DestReg)
        .addReg(SrcReg, getKillRegState(KillSrc))
        .addImm(0);
    return;
  }
  if (GPUVM::GPRQRegClass.contains(DestReg, SrcReg)) {
    const TargetRegisterInfo &TRI = getRegisterInfo();
    unsigned Subs[4] = {GPUVM::sub0, GPUVM::sub1, GPUVM::sub2, GPUVM::sub3};
    // Overlapping tuples: copy in the direction that reads each register
    // before it is written.
    bool Down = TRI.getEncodingValue(TRI.getSubReg(DestReg, GPUVM::sub0)) >
                TRI.getEncodingValue(TRI.getSubReg(SrcReg, GPUVM::sub0));
    for (unsigned K = 0; K < 4; ++K) {
      unsigned I = Down ? 3 - K : K;
      BuildMI(MBB, MI, DL, get(GPUVM::ADDI), TRI.getSubReg(DestReg, Subs[I]))
          .addReg(TRI.getSubReg(SrcReg, Subs[I]), getKillRegState(KillSrc))
          .addImm(0);
    }
    return;
  }
  report_fatal_error("gpu_vm: impossible register copy");
}

static MachineMemOperand *frameMMO(MachineBasicBlock &MBB, int FI,
                                   MachineMemOperand::Flags F) {
  MachineFunction &MF = *MBB.getParent();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  return MF.getMachineMemOperand(MachinePointerInfo::getFixedStack(MF, FI), F,
                                 MFI.getObjectSize(FI),
                                 MFI.getObjectAlign(FI));
}

void GPUVMInstrInfo::storeRegToStackSlot(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register SrcReg,
    bool IsKill, int FI, const TargetRegisterClass *RC, Register VReg,
    MachineInstr::MIFlag Flags) const {
  DebugLoc DL = MI != MBB.end() ? MI->getDebugLoc() : DebugLoc();
  unsigned Opc;
  if (GPUVM::GPRRegClass.hasSubClassEq(RC))
    Opc = GPUVM::SW;
  else if (GPUVM::GPRQRegClass.hasSubClassEq(RC))
    Opc = GPUVM::SQ_Q;
  else
    llvm_unreachable("gpu_vm: can't store this register to a stack slot");
  BuildMI(MBB, MI, DL, get(Opc))
      .addReg(SrcReg, getKillRegState(IsKill))
      .addFrameIndex(FI)
      .addImm(0)
      .addMemOperand(frameMMO(MBB, FI, MachineMemOperand::MOStore))
      .setMIFlag(Flags);
}

void GPUVMInstrInfo::loadRegFromStackSlot(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator MI, Register DestReg,
    int FI, const TargetRegisterClass *RC, Register VReg, unsigned SubReg,
    MachineInstr::MIFlag Flags) const {
  DebugLoc DL = MI != MBB.end() ? MI->getDebugLoc() : DebugLoc();
  unsigned Opc;
  if (GPUVM::GPRRegClass.hasSubClassEq(RC))
    Opc = GPUVM::LW;
  else if (GPUVM::GPRQRegClass.hasSubClassEq(RC))
    Opc = GPUVM::LQ_Q;
  else
    llvm_unreachable("gpu_vm: can't load this register from a stack slot");
  BuildMI(MBB, MI, DL, get(Opc), DestReg)
      .addFrameIndex(FI)
      .addImm(0)
      .addMemOperand(frameMMO(MBB, FI, MachineMemOperand::MOLoad))
      .setMIFlag(Flags);
}

Register GPUVMInstrInfo::isLoadFromStackSlot(const MachineInstr &MI,
                                             int &FrameIndex) const {
  switch (MI.getOpcode()) {
  case GPUVM::LW:
  case GPUVM::LQ_Q:
    break;
  default:
    return Register();
  }
  if (MI.getOperand(1).isFI() && MI.getOperand(2).isImm() &&
      MI.getOperand(2).getImm() == 0) {
    FrameIndex = MI.getOperand(1).getIndex();
    return MI.getOperand(0).getReg();
  }
  return Register();
}

Register GPUVMInstrInfo::isStoreToStackSlot(const MachineInstr &MI,
                                            int &FrameIndex) const {
  switch (MI.getOpcode()) {
  case GPUVM::SW:
  case GPUVM::SQ_Q:
    break;
  default:
    return Register();
  }
  if (MI.getOperand(1).isFI() && MI.getOperand(2).isImm() &&
      MI.getOperand(2).getImm() == 0) {
    FrameIndex = MI.getOperand(1).getIndex();
    return MI.getOperand(0).getReg();
  }
  return Register();
}

//===----------------------------------------------------------------------===//
// Branches: the control word's compare-and-branch (registers, imm6, a bit),
// j, and jr/ret.
//===----------------------------------------------------------------------===//

bool GPUVMInstrInfo::isCondBranch(unsigned Opc) {
  return invertBranch(Opc) != 0;
}

unsigned GPUVMInstrInfo::invertBranch(unsigned Opc) {
  switch (Opc) {
  case GPUVM::BEQ:
    return GPUVM::BNE;
  case GPUVM::BNE:
    return GPUVM::BEQ;
  case GPUVM::BLT:
    return GPUVM::BGE;
  case GPUVM::BGE:
    return GPUVM::BLT;
  case GPUVM::BLTU:
    return GPUVM::BGEU;
  case GPUVM::BGEU:
    return GPUVM::BLTU;
  case GPUVM::BEQI:
    return GPUVM::BNEI;
  case GPUVM::BNEI:
    return GPUVM::BEQI;
  case GPUVM::BLTI:
    return GPUVM::BGEI;
  case GPUVM::BGEI:
    return GPUVM::BLTI;
  case GPUVM::BLTUI:
    return GPUVM::BGEUI;
  case GPUVM::BGEUI:
    return GPUVM::BLTUI;
  case GPUVM::BBS:
    return GPUVM::BBC;
  case GPUVM::BBC:
    return GPUVM::BBS;
  default:
    return 0;
  }
}

MachineBasicBlock *
GPUVMInstrInfo::getBranchDestBlock(const MachineInstr &MI) const {
  return MI.getOperand(MI.getNumExplicitOperands() - 1).getMBB();
}

static void parseCondBranch(const MachineInstr &MI, MachineBasicBlock *&Target,
                            SmallVectorImpl<MachineOperand> &Cond) {
  Target = MI.getOperand(2).getMBB();
  Cond.push_back(MachineOperand::CreateImm(MI.getOpcode()));
  Cond.push_back(MI.getOperand(0));
  Cond.push_back(MI.getOperand(1));
}

bool GPUVMInstrInfo::analyzeBranch(MachineBasicBlock &MBB,
                                   MachineBasicBlock *&TBB,
                                   MachineBasicBlock *&FBB,
                                   SmallVectorImpl<MachineOperand> &Cond,
                                   bool AllowModify) const {
  TBB = FBB = nullptr;
  Cond.clear();

  MachineBasicBlock::iterator I = MBB.getLastNonDebugInstr();
  if (I == MBB.end() || !isUnpredicatedTerminator(*I))
    return false;

  // The terminators, and the first unconditional or indirect branch.
  MachineBasicBlock::iterator FirstUncondOrIndirectBr = MBB.end();
  int NumTerminators = 0;
  for (auto J = I.getReverse(); J != MBB.rend() && isUnpredicatedTerminator(*J);
       ++J) {
    ++NumTerminators;
    if (J->getDesc().isUnconditionalBranch() ||
        J->getDesc().isIndirectBranch())
      FirstUncondOrIndirectBr = J.getReverse();
  }

  // Whatever follows an unconditional branch is dead.
  if (AllowModify && FirstUncondOrIndirectBr != MBB.end()) {
    while (std::next(FirstUncondOrIndirectBr) != MBB.end()) {
      std::next(FirstUncondOrIndirectBr)->eraseFromParent();
      --NumTerminators;
    }
    I = FirstUncondOrIndirectBr;
  }

  if (I->getDesc().isIndirectBranch() || I->isPreISelOpcode())
    return true;
  if (NumTerminators > 2)
    return true;

  if (NumTerminators == 1 && I->getDesc().isUnconditionalBranch()) {
    TBB = getBranchDestBlock(*I);
    return false;
  }
  if (NumTerminators == 1 && isCondBranch(I->getOpcode())) {
    parseCondBranch(*I, TBB, Cond);
    return false;
  }
  if (NumTerminators == 2 && isCondBranch(std::prev(I)->getOpcode()) &&
      I->getDesc().isUnconditionalBranch()) {
    parseCondBranch(*std::prev(I), TBB, Cond);
    FBB = getBranchDestBlock(*I);
    return false;
  }
  return true;
}

unsigned GPUVMInstrInfo::removeBranch(MachineBasicBlock &MBB,
                                      int *BytesRemoved) const {
  if (BytesRemoved)
    *BytesRemoved = 0;
  MachineBasicBlock::iterator I = MBB.getLastNonDebugInstr();
  if (I == MBB.end())
    return 0;
  if (I->getOpcode() != GPUVM::J && !isCondBranch(I->getOpcode()))
    return 0;
  if (BytesRemoved)
    *BytesRemoved += getInstSizeInBytes(*I);
  I->eraseFromParent();

  I = MBB.end();
  if (I == MBB.begin())
    return 1;
  --I;
  if (!isCondBranch(I->getOpcode()))
    return 1;
  if (BytesRemoved)
    *BytesRemoved += getInstSizeInBytes(*I);
  I->eraseFromParent();
  return 2;
}

unsigned GPUVMInstrInfo::insertBranch(MachineBasicBlock &MBB,
                                      MachineBasicBlock *TBB,
                                      MachineBasicBlock *FBB,
                                      ArrayRef<MachineOperand> Cond,
                                      const DebugLoc &DL,
                                      int *BytesAdded) const {
  if (BytesAdded)
    *BytesAdded = 0;
  assert(TBB && "insertBranch must not be told to insert a fallthrough");
  assert((Cond.size() == 3 || Cond.size() == 0) && "bad branch condition");

  if (Cond.empty()) {
    MachineInstr &MI = *BuildMI(&MBB, DL, get(GPUVM::J)).addMBB(TBB);
    if (BytesAdded)
      *BytesAdded += getInstSizeInBytes(MI);
    return 1;
  }
  MachineInstr &CondMI = *BuildMI(&MBB, DL, get(Cond[0].getImm()))
                              .add(Cond[1])
                              .add(Cond[2])
                              .addMBB(TBB);
  if (BytesAdded)
    *BytesAdded += getInstSizeInBytes(CondMI);
  if (!FBB)
    return 1;
  MachineInstr &MI = *BuildMI(&MBB, DL, get(GPUVM::J)).addMBB(FBB);
  if (BytesAdded)
    *BytesAdded += getInstSizeInBytes(MI);
  return 2;
}

bool GPUVMInstrInfo::reverseBranchCondition(
    SmallVectorImpl<MachineOperand> &Cond) const {
  assert(Cond.size() == 3 && "bad branch condition");
  unsigned Inv = invertBranch(Cond[0].getImm());
  if (!Inv)
    return true;
  Cond[0].setImm(Inv);
  return false;
}

bool GPUVMInstrInfo::isBranchOffsetInRange(unsigned BranchOpc,
                                           int64_t BrOffset) const {
  // In bundles from the branch's bundle: 9 bits (+-256), j 18 (+-2 MB).
  // BrOffset counts 16 bytes an instruction, never less than the bundles.
  if (BranchOpc == GPUVM::J)
    return isInt<22>(BrOffset);
  return BrOffset >= -4096 && BrOffset <= 4080;
}

void GPUVMInstrInfo::insertIndirectBranch(MachineBasicBlock &MBB,
                                          MachineBasicBlock &NewDestBB,
                                          MachineBasicBlock &RestoreBB,
                                          const DebugLoc &DL, int64_t BrOffset,
                                          RegScavenger *RS) const {
  report_fatal_error("gpu_vm: a branch beyond 2 MB in one function");
}

unsigned GPUVMInstrInfo::getInstSizeInBytes(const MachineInstr &MI) const {
  if (MI.isMetaInstruction())
    return 0;
  if (MI.isInlineAsm()) {
    const MachineFunction &MF = *MI.getMF();
    return getInlineAsmLength(MI.getOperand(0).getSymbolName(),
                              MF.getTarget().getMCAsmInfo());
  }
  return 16;
}

bool GPUVMInstrInfo::isReMaterializableImpl(const MachineInstr &MI) const {
  if (MI.getOpcode() != GPUVM::ADDI || !MI.getOperand(1).isReg() ||
      MI.getOperand(1).getReg() != GPUVM::R0)
    return false;
  return TargetInstrInfo::isReMaterializableImpl(MI);
}
