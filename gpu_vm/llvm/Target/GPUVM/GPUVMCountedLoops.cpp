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
// A loop that steps an index i by 1 (or -1) once an iteration and goes on
// while i != n (n the same through the loop) counts down a register of its
// own instead, c = n - i made before the loop: `addbnz c, -1, header`. The
// index stays (it addresses), but the branch no longer compares what an op of
// its bundle just wrote. Made where the latch takes fewer bundles.
//
//===----------------------------------------------------------------------===//

#include "GPUVM.h"
#include "GPUVMBundleModel.h"
#include "GPUVMInstrInfo.h"
#include "GPUVMSubtarget.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-counted-loops"

static cl::opt<bool>
    CountDown("gpuvm-count-down", cl::Hidden, cl::init(true),
              cl::desc("gpu_vm: a loop to i != n counts down a register of "
                       "its own (addbnz)"));
static cl::opt<unsigned>
    AddBnzReach("gpuvm-addbnz-reach", cl::Hidden, cl::init(200),
                cl::desc("gpu_vm: instructions an addbnz may branch over"));

namespace {
class GPUVMCountedLoops : public MachineFunctionPass {
public:
  static char ID;
  GPUVMCountedLoops() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override { return "gpu_vm counted loops"; }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<MachineLoopInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  const GPUVMInstrInfo *TII = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  bool near(MachineBasicBlock &From, MachineBasicBlock &To) const;
  bool fuse(MachineBasicBlock &MBB);
  bool countDown(MachineLoop &L, BitVector &Used);
  unsigned bundles(const MachineBasicBlock &MBB) const;
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

unsigned GPUVMCountedLoops::bundles(const MachineBasicBlock &MBB) const {
  GPUVM::Packer P;
  unsigned N = 0;
  for (const MachineInstr &MI : MBB)
    N += P.put(GPUVM::describeOp(MI, TRI));
  return N;
}

bool GPUVMCountedLoops::countDown(MachineLoop &L, BitVector &Used) {
  MachineBasicBlock *H = L.getHeader();
  MachineBasicBlock *Latch = L.getLoopLatch();
  MachineBasicBlock *P = L.getLoopPreheader();
  if (!Latch || !P)
    return false;
  MachineBasicBlock::iterator T = Latch->getFirstTerminator();
  if (T == Latch->end() || T->getOpcode() != GPUVM::BNE ||
      T->getOperand(2).getMBB() != H)
    return false;
  if (std::next(T) != Latch->end() && std::next(T)->getOpcode() != GPUVM::J)
    return false;
  // no call in the loop (c is caller-saved)
  for (MachineBasicBlock *B : L.blocks())
    for (MachineInstr &MI : *B)
      if (MI.isCall() || MI.isInlineAsm())
        return false;
  // the index: one def in the loop, a step of 1 or -1 in the header or the
  // latch (once an iteration); n: no def in the loop
  Register I, N;
  int64_t K = 0;
  for (unsigned Op = 0; Op < 2 && !I; ++Op) {
    Register X = T->getOperand(Op).getReg(), Y = T->getOperand(1 - Op).getReg();
    if (X == GPUVM::R0)
      continue;
    MachineInstr *Step = nullptr;
    unsigned Defs = 0, YDefs = 0;
    for (MachineBasicBlock *B : L.blocks())
      for (MachineInstr &MI : *B) {
        if (MI.modifiesRegister(X, TRI)) {
          ++Defs;
          Step = &MI;
        }
        if (Y != GPUVM::R0 && MI.modifiesRegister(Y, TRI))
          ++YDefs;
      }
    if (Defs != 1 || YDefs != 0 || Step->getOpcode() != GPUVM::ADDI ||
        Step->getOperand(0).getReg() != X || !Step->getOperand(1).isReg() ||
        Step->getOperand(1).getReg() != X || !Step->getOperand(2).isImm() ||
        (Step->getParent() != H && Step->getParent() != Latch))
      continue;
    int64_t S = Step->getOperand(2).getImm();
    if (S != 1 && S != -1)
      continue;
    I = X;
    N = Y;
    K = S;
  }
  if (!I || !near(*Latch, *H))
    return false;
  // a register nothing in the function names
  MachineFunction &MF = *Latch->getParent();
  const MCPhysReg *CSR = TRI->getCalleeSavedRegs(&MF);
  Register C;
  for (MCPhysReg R : GPUVM::GPRRegClass) {
    unsigned E = TRI->getEncodingValue(R);
    if (E < 5 || E == 63 || Used.test(R) || MF.getRegInfo().isReserved(R))
      continue;
    bool Saved = false;
    for (const MCPhysReg *Q = CSR; *Q; ++Q)
      Saved |= *Q == R;
    if (!Saved) {
      C = R;
      break;
    }
  }
  if (!C)
    return false;
  // made where the latch takes fewer bundles
  unsigned Before = bundles(*Latch);
  DebugLoc DL = T->getDebugLoc();
  MachineInstr *Br = &*T;
  MachineInstr *New = BuildMI(*Latch, T, DL, TII->get(GPUVM::ADDBNZ), C)
                          .addReg(C)
                          .addImm(-1)
                          .addMBB(H);
  Br->removeFromParent();
  if (bundles(*Latch) >= Before) {
    Latch->insert(New->getIterator(), Br);
    New->eraseFromParent();
    return false;
  }
  MF.deleteMachineInstr(Br);
  // c = n - i (or i - n) before the loop
  MachineBasicBlock::iterator PT = P->getFirstTerminator();
  BuildMI(*P, PT, DL, TII->get(GPUVM::SUB), C)
      .addReg(K > 0 ? N : I)
      .addReg(K > 0 ? I : N);
  for (MCRegAliasIterator A(C, TRI, true); A.isValid(); ++A)
    Used.set(*A);
  SmallVector<MachineBasicBlock *, 8> Bs(L.blocks().begin(), L.blocks().end());
  Bs.push_back(P);
  fullyRecomputeLiveIns(Bs);
  return true;
}

bool GPUVMCountedLoops::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;
  TII = MF.getSubtarget<GPUVMSubtarget>().getInstrInfo();
  TRI = MF.getSubtarget().getRegisterInfo();
  bool Changed = false;
  if (CountDown && MF.getRegInfo().tracksLiveness()) {
    BitVector Used(TRI->getNumRegs());
    for (MachineBasicBlock &MBB : MF) {
      for (const auto &LI : MBB.liveins())
        for (MCRegAliasIterator A(LI.PhysReg, TRI, true); A.isValid(); ++A)
          Used.set(*A);
      for (MachineInstr &MI : MBB)
        for (const MachineOperand &MO : MI.operands())
          if (MO.isReg() && MO.getReg() && MO.getReg().isPhysical())
            for (MCRegAliasIterator A(MO.getReg(), TRI, true); A.isValid();
                 ++A)
              Used.set(*A);
    }
    MachineLoopInfo &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();
    SmallVector<MachineLoop *, 8> Loops;
    for (MachineLoop *L : MLI)
      for (MachineLoop *Sub : depth_first(L))
        Loops.push_back(Sub);
    for (MachineLoop *L : Loops)
      Changed |= countDown(*L, Used);
  }
  for (MachineBasicBlock &MBB : MF)
    Changed |= fuse(MBB);
  return Changed;
}
