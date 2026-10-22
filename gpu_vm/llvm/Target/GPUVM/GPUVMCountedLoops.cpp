//===-- GPUVMCountedLoops.cpp - fuse a counter's step into its branch -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `addi r, r, k; bnei r, 0, L` becomes `addbnz r, k, L` (k in imm6): the
// control word adds and branches on the sum, so the step takes no op slot and
// the branch no bundle of its own (a branch compares registers as the bundle
// found them: after an op of its bundle wrote r it would need a new one).
// `addi r, r, k; beqi r, 0, A; j B` becomes `addbnz r, k, B; j A`. addbnz has
// no inverse and is left alone by branch relaxation: only a target within
// -gpuvm-addbnz-reach instructions (bundles reach +-256).
//
//===----------------------------------------------------------------------===//

#include "GPUVM.h"
#include "GPUVMInstrInfo.h"
#include "GPUVMSubtarget.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-counted-loops"

static cl::opt<unsigned>
    AddBnzReach("gpuvm-addbnz-reach", cl::Hidden, cl::init(200),
                cl::desc("gpu_vm: instructions an addbnz may branch over"));

namespace {
class GPUVMCountedLoops : public MachineFunctionPass {
public:
  static char ID;
  GPUVMCountedLoops() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override { return "gpu_vm counted loops"; }
  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  const GPUVMInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  bool near(MachineBasicBlock &From, MachineBasicBlock &To) const;
  bool fuse(MachineBasicBlock &MBB);
};
} // namespace

char GPUVMCountedLoops::ID = 0;

FunctionPass *llvm::createGPUVMCountedLoopsPass() {
  return new GPUVMCountedLoops();
}

// The instructions between the two blocks in layout, the two included, are
// few enough.
bool GPUVMCountedLoops::near(MachineBasicBlock &From,
                             MachineBasicBlock &To) const {
  MachineFunction &MF = *From.getParent();
  unsigned A = From.getNumber(), B = To.getNumber();
  // block numbers are in layout order once branch folding renumbered them;
  // walk the layout to be sure
  unsigned Count = 0;
  bool In = false;
  for (MachineBasicBlock &MBB : MF) {
    bool Edge = &MBB == &From || &MBB == &To;
    if (Edge && !In) {
      In = true;
      Count += MBB.size();
      if (&From == &To)
        break;
      continue;
    }
    if (In) {
      Count += MBB.size();
      if (Edge)
        break;
    }
  }
  (void)A;
  (void)B;
  return Count <= AddBnzReach;
}

bool GPUVMCountedLoops::fuse(MachineBasicBlock &MBB) {
  MachineBasicBlock::iterator T = MBB.getFirstTerminator();
  if (T == MBB.end())
    return false;
  MachineInstr &Br = *T;
  bool Inverted;
  if (Br.getOpcode() == GPUVM::BNEI && Br.getOperand(1).getImm() == 0)
    Inverted = false;
  else if (Br.getOpcode() == GPUVM::BEQI && Br.getOperand(1).getImm() == 0)
    Inverted = true;
  else
    return false;
  Register R = Br.getOperand(0).getReg();
  if (R == GPUVM::R0 || R == GPUVM::R2)
    return false;
  MachineBasicBlock *Taken = Br.getOperand(2).getMBB();
  MachineInstr *Jump = nullptr;
  if (Inverted) {
    // beqi r, 0, A; j B: the loop goes on to B
    MachineBasicBlock::iterator N = std::next(T);
    if (N == MBB.end() || N->getOpcode() != GPUVM::J)
      return false;
    Jump = &*N;
  } else if (std::next(T) != MBB.end() &&
             std::next(T)->getOpcode() != GPUVM::J) {
    return false;
  }
  MachineBasicBlock *Target = Inverted ? Jump->getOperand(0).getMBB() : Taken;

  // The step: the last op before the branch that touches r.
  MachineInstr *Step = nullptr;
  for (MachineBasicBlock::iterator I = T; I != MBB.begin();) {
    --I;
    if (I->isDebugInstr())
      continue;
    if (I->isCall() || I->hasUnmodeledSideEffects() || I->isInlineAsm())
      return false;
    if (I->readsRegister(R, TRI) || I->modifiesRegister(R, TRI)) {
      Step = &*I;
      break;
    }
  }
  if (!Step || Step->getOpcode() != GPUVM::ADDI ||
      Step->getOperand(0).getReg() != R || Step->getOperand(1).getReg() != R ||
      !Step->getOperand(2).isImm())
    return false;
  int64_t K = Step->getOperand(2).getImm();
  if (K == 0 || !isInt<6>(K))
    return false;
  if (!near(MBB, *Target))
    return false;

  DebugLoc DL = Br.getDebugLoc();
  BuildMI(MBB, T, DL, TII->get(GPUVM::ADDBNZ), R)
      .addReg(R)
      .addImm(K)
      .addMBB(Target);
  Step->eraseFromParent();
  Br.eraseFromParent();
  if (Inverted)
    Jump->getOperand(0).setMBB(Taken);
  return true;
}

bool GPUVMCountedLoops::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  TII = MF.getSubtarget<GPUVMSubtarget>().getInstrInfo();
  TRI = MF.getSubtarget().getRegisterInfo();
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF)
    Changed |= fuse(MBB);
  return Changed;
}
