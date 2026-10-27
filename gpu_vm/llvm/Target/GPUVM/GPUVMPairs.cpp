//===-- GPUVMPairs.cpp - two words of one texel in one op -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// After register allocation and the frame's saves, two word loads (or two word
// stores) from one base register at offsets 4 apart become a pair: ld2 rd, rb
// (sd2 rs, rs2), one op of the tick's paths 3 and 5 (isa.h GVM_IMM_D,
// -gpuvm-pairs). The tick makes a pair whose two words lie in one texel; one
// across two texels redoes its pass, so a pair is made only where the lower
// word is known to be 8-aligned (its memory operand) or lies on the stack at
// an offset that is not 12 mod 16 (sp is 16-aligned).
//
// A load pair takes the first load's place: nothing between writes its base or
// reads or writes the second's register, and no store lies between. A store
// pair takes the second store's place: nothing between writes the first's
// value or base, and no load or store lies between.
//
//===----------------------------------------------------------------------===//

#include "GPUVM.h"
#include "GPUVMInstrInfo.h"
#include "GPUVMSubtarget.h"
#include "MCTargetDesc/GPUVMBaseInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-pairs"

namespace {
class GPUVMPairs : public MachineFunctionPass {
public:
  static char ID;
  GPUVMPairs() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override { return "gpu_vm pairs"; }
  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  const GPUVMInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  bool block(MachineBasicBlock &MBB);
};
} // namespace

char GPUVMPairs::ID = 0;

FunctionPass *llvm::createGPUVMPairsPass() { return new GPUVMPairs(); }

// A plain word load or store: its value register, base and offset.
static bool word(const MachineInstr &I, bool &Store, Register &V, Register &B,
                 int64_t &Off) {
  unsigned Opc = I.getOpcode();
  if (Opc != GPUVM::LW && Opc != GPUVM::SW)
    return false;
  if (!I.getOperand(2).isImm() || !I.getOperand(0).isReg() ||
      !I.getOperand(1).isReg())
    return false;
  for (const MachineMemOperand *M : I.memoperands())
    if (M->isVolatile() || M->isAtomic())
      return false;
  Store = Opc == GPUVM::SW;
  V = I.getOperand(0).getReg();
  B = I.getOperand(1).getReg();
  Off = I.getOperand(2).getImm();
  return true;
}

// The lower word (of the access at Off from B, I's) lies 8-aligned or on the
// stack away from a texel's last word: the pair stays in one texel.
static bool oneTexel(const MachineInstr &I, Register B, int64_t Off) {
  if ((Off & 3) != 0)
    return false;
  if (B == GPUVM::R2)
    return (Off & 15) != 12;
  if (I.memoperands_empty())
    return false;
  for (const MachineMemOperand *M : I.memoperands())
    if (M->getAlign() < Align(8))
      return false;
  return true;
}

bool GPUVMPairs::block(MachineBasicBlock &MBB) {
  bool Changed = false;
  for (auto It = MBB.begin(); It != MBB.end();) {
    MachineInstr &I = *It;
    bool St;
    Register V, B;
    int64_t Off;
    if (!word(I, St, V, B, Off) || (!St && TRI->regsOverlap(V, B))) {
      ++It;
      continue;
    }
    MachineInstr *Mate = nullptr;
    unsigned Seen = 0;
    for (auto J = std::next(It); J != MBB.end() && Seen < 16; ++J, ++Seen) {
      MachineInstr &X = *J;
      if (X.isDebugInstr())
        continue;
      bool St2;
      Register V2, B2;
      int64_t Off2;
      if (word(X, St2, V2, B2, Off2) && St2 == St && B2 == B &&
          (Off2 == Off + 4 || Off2 == Off - 4) &&
          !(St ? false : TRI->regsOverlap(V2, V) || TRI->regsOverlap(V2, B))) {
        Mate = &X;
        break;
      }
      if (X.isCall() || X.isTerminator() || X.hasUnmodeledSideEffects() ||
          X.isInlineAsm() || X.isBundle() || X.mayStore() ||
          (St && X.mayLoad()))
        break;
      if (X.modifiesRegister(B, TRI) || (St && X.modifiesRegister(V, TRI)))
        break;
    }
    if (!Mate) {
      ++It;
      continue;
    }
    bool St2;
    Register V2, B2;
    int64_t Off2;
    word(*Mate, St2, V2, B2, Off2);
    // A load pair takes I's place: nothing between reads or writes V2
    if (!St) {
      bool Ok = true;
      for (auto J = std::next(It); &*J != Mate; ++J)
        if (J->readsRegister(V2, TRI) || J->modifiesRegister(V2, TRI)) {
          Ok = false;
          break;
        }
      if (!Ok) {
        ++It;
        continue;
      }
    }
    bool ILow = Off < Off2;
    const MachineInstr &Lo = ILow ? I : *Mate;
    int64_t LowOff = ILow ? Off : Off2;
    if (!oneTexel(Lo, B, LowOff)) {
      ++It;
      continue;
    }
    MachineOperand VLo = (ILow ? I : *Mate).getOperand(0);
    MachineOperand VHi = (ILow ? *Mate : I).getOperand(0);
    MachineOperand Base = I.getOperand(1);
    MachineInstr &At = St ? *Mate : I;
    MachineInstrBuilder MIB =
        BuildMI(MBB, At, At.getDebugLoc(), TII->get(St ? GPUVM::SD2 : GPUVM::LD2));
    if (St) {
      VLo.setIsKill(false);
      VHi.setIsKill(false);
      Base.setIsKill(Mate->getOperand(1).isKill());
    } else {
      Base.setIsKill(false);
    }
    MIB.add(VLo).add(VHi).add(Base).addImm(LowOff);
    MIB.cloneMergedMemRefs({&I, Mate});
    ++It;
    if (&*It == Mate)
      ++It;
    Mate->eraseFromParent();
    I.eraseFromParent();
    Changed = true;
  }
  return Changed;
}

bool GPUVMPairs::runOnMachineFunction(MachineFunction &MF) {
  if (!GPUVM::pairs() || skipFunction(MF.getFunction()))
    return false;
  TII = MF.getSubtarget<GPUVMSubtarget>().getInstrInfo();
  TRI = MF.getSubtarget().getRegisterInfo();
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF)
    Changed |= block(MBB);
  return Changed;
}
