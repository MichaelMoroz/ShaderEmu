//===-- GPUVMBranchFill.cpp - fill a branch's bundle from the path on -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A branch compares registers as its bundle found them: after an op that
// writes one (a value just loaded or computed) the branch takes a bundle of
// its own, with empty op slots. The first ops of the block it falls through
// to, when that block has no other way in, are moved before the branch into
// those slots: they run on the taken path too, for nothing but at no cost.
// An op moves when it stores nothing, has no side effects, loads only from
// the stack or a global (an address that cannot fault), writes no register
// the branch compares, and writes nothing the taken block reads.
//
//===----------------------------------------------------------------------===//

#include "GPUVM.h"
#include "GPUVMInstrInfo.h"
#include "GPUVMSubtarget.h"
#include "MCTargetDesc/GPUVMBaseInfo.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-branch-fill"

static cl::opt<bool> BranchFill("gpuvm-branch-fill", cl::Hidden,
                                cl::init(true),
                                cl::desc("gpu_vm: fill a branch's bundle with "
                                         "the ops of the path on"));

namespace {
class GPUVMBranchFill : public MachineFunctionPass {
public:
  static char ID;
  GPUVMBranchFill() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override {
    return "gpu_vm branch bundle fill";
  }
  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  const GPUVMInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  const MachineRegisterInfo *MRI = nullptr;
  bool fill(MachineBasicBlock &B);
  bool movable(const MachineInstr &I, ArrayRef<Register> Compared,
               const MachineBasicBlock &Taken) const;
};
} // namespace

char GPUVMBranchFill::ID = 0;

FunctionPass *llvm::createGPUVMBranchFillPass() {
  return new GPUVMBranchFill();
}

static bool liveIn(const MachineBasicBlock &MBB, Register R,
                   const TargetRegisterInfo *TRI) {
  for (const auto &LI : MBB.liveins())
    if (TRI->regsOverlap(LI.PhysReg, R))
      return true;
  return false;
}

bool GPUVMBranchFill::movable(const MachineInstr &I,
                              ArrayRef<Register> Compared,
                              const MachineBasicBlock &Taken) const {
  if (I.isTerminator() || I.isCall() || I.hasUnmodeledSideEffects() ||
      I.mayStore() || I.isInlineAsm() || I.isMetaInstruction() ||
      I.isDebugInstr() || I.hasOrderedMemoryRef() ||
      I.getFlag(MachineInstr::FrameSetup) ||
      I.getFlag(MachineInstr::FrameDestroy))
    return false;
  if (I.isPseudo() && I.getOpcode() != GPUVM::LQ_Q)
    return false;
  if (I.mayLoad()) {
    // [r0 + symbol], [sp + k], [fp + k], PIC's [the function + %fnrel(symbol)]:
    // cannot fault
    if (GPUVMII::getForm(I.getDesc().TSFlags) != GPUVMII::FrmLoad)
      return false;
    const MachineOperand &Base = I.getOperand(1), &Imm = I.getOperand(2);
    bool FnRel = !Imm.isImm() && Imm.getTargetFlags() == GPUVMII::MO_FNREL;
    if (!Base.isReg() || (Base.getReg() != GPUVM::R0 &&
                          Base.getReg() != GPUVM::R2 &&
                          Base.getReg() != GPUVM::R32 && !FnRel))
      return false;
    if (Base.getReg() == GPUVM::R32 &&
        !I.getMF()->getSubtarget().getFrameLowering()->hasFP(*I.getMF()))
      return false;
  }
  for (const MachineOperand &MO : I.operands()) {
    if (!MO.isReg() || !MO.isDef() || !MO.getReg())
      continue;
    Register D = MO.getReg();
    if (!D.isPhysical() || MRI->isReserved(D))
      return false;
    for (Register C : Compared)
      if (TRI->regsOverlap(C, D))
        return false;
    if (liveIn(Taken, D, TRI))
      return false;
  }
  return true;
}

bool GPUVMBranchFill::fill(MachineBasicBlock &B) {
  MachineBasicBlock::iterator T = B.getFirstTerminator();
  if (T == B.end())
    return false;
  MachineInstr &Br = *T;
  if (!GPUVMInstrInfo::isCondBranch(Br.getOpcode()) &&
      Br.getOpcode() != GPUVM::ADDBNZ)
    return false;
  MachineBasicBlock *Taken = TII->getBranchDestBlock(Br);
  // The block the branch falls through to (or jumps to after it)
  MachineBasicBlock *F = nullptr;
  MachineBasicBlock::iterator N = std::next(T);
  if (N != B.end()) {
    if (N->getOpcode() != GPUVM::J || std::next(N) != B.end())
      return false;
    F = N->getOperand(0).getMBB();
  } else {
    MachineFunction::iterator Next = std::next(B.getIterator());
    if (Next == B.getParent()->end() || !B.isSuccessor(&*Next))
      return false;
    F = &*Next;
  }
  if (!F || F == Taken || F == &B || F->pred_size() != 1 || F->isEHPad() ||
      F->hasAddressTaken())
    return false;

  // The registers the branch compares (addbnz: its counted one)
  SmallVector<Register, 2> Compared;
  for (const MachineOperand &MO : Br.operands())
    if (MO.isReg() && MO.getReg() && MO.getReg() != GPUVM::R0)
      Compared.push_back(MO.getReg());

  // Alone in its bundle: the last op before it writes a compared register.
  MachineInstr *Last = nullptr;
  for (MachineBasicBlock::iterator I = T; I != B.begin();) {
    --I;
    if (I->isMetaInstruction() || I->isDebugInstr())
      continue;
    Last = &*I;
    break;
  }
  if (Last) {
    bool Writes = false;
    for (Register C : Compared)
      if (Last->modifiesRegister(C, TRI))
        Writes = true;
    if (!Writes)
      return false;
  }

  // A prefix of F, two ops at most.
  SmallVector<MachineInstr *, 2> Move;
  for (MachineInstr &I : *F) {
    if (Move.size() == 2 || !movable(I, Compared, *Taken))
      break;
    Move.push_back(&I);
  }
  if (Move.empty())
    return false;
  for (MachineInstr *MI : Move) {
    MI->removeFromParent();
    B.insert(T, MI);
  }
  recomputeLiveIns(*F);
  recomputeLivenessFlags(B);
  return true;
}

bool GPUVMBranchFill::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()) || !BranchFill ||
      !MF.getRegInfo().tracksLiveness())
    return false;
  TII = MF.getSubtarget<GPUVMSubtarget>().getInstrInfo();
  TRI = MF.getSubtarget().getRegisterInfo();
  MRI = &MF.getRegInfo();
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF)
    Changed |= fill(MBB);
  return Changed;
}
