//===-- GPUVMLoadHoist.cpp - loads of a loop's invariant addresses out ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A load from a loop-invariant address that no store of the loop may write
// (alias analysis with the type-based tags) is made once before the loop.
// LLVM's LICM leaves such a load where it is when it sits in a block of the
// loop that does not always run (zlib's INSERT_STRING in deflate_slow: six
// fields of the state read again on every iteration), as it may not be
// dereferenceable before the loop. On gpu_vm a load faults only beyond RAM's
// end (and the ROM's): one near an address the code accesses through the same
// register on every way into the loop (before it, or in its header before
// any exit), or between two such accesses, reads the same object.
//
//===----------------------------------------------------------------------===//

#include "GPUVM.h"
#include "GPUVMInstrInfo.h"
#include "GPUVMSubtarget.h"
#include "MCTargetDesc/GPUVMBaseInfo.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-load-hoist"

static cl::opt<bool> LoadHoist("gpuvm-load-hoist", cl::Hidden, cl::init(true),
                               cl::desc("gpu_vm: loads of a loop's invariant "
                                        "addresses made before the loop"));
static cl::opt<unsigned>
    MaxHoist("gpuvm-load-hoist-max", cl::Hidden, cl::init(8),
             cl::desc("gpu_vm: loads hoisted out of one loop at most"));
static cl::opt<unsigned>
    Near("gpuvm-load-hoist-near", cl::Hidden, cl::init(16),
         cl::desc("gpu_vm: a load is safe this many bytes from a known "
                  "access through its base"));

namespace {
class GPUVMLoadHoist : public MachineFunctionPass {
public:
  static char ID;
  GPUVMLoadHoist() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override {
    return "gpu_vm loads of invariant addresses out of loops";
  }
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<AAResultsWrapperPass>();
    AU.addRequired<MachineLoopInfoWrapperPass>();
    AU.addPreserved<MachineLoopInfoWrapperPass>();
    AU.addRequired<MachineDominatorTreeWrapperPass>();
    AU.addPreserved<MachineDominatorTreeWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  AAResults *AA = nullptr;
  MachineRegisterInfo *MRI = nullptr;
  MachineDominatorTree *MDT = nullptr;
  bool hoist(MachineLoop &L);
  void knownAccesses(MachineLoop &L, Register Base,
                     SmallVectorImpl<int64_t> &Offs) const;
  Register root(Register R) const;
};
} // namespace

char GPUVMLoadHoist::ID = 0;

FunctionPass *llvm::createGPUVMLoadHoistPass() { return new GPUVMLoadHoist(); }

static bool baseImmAccess(const MachineInstr &MI) {
  unsigned F = GPUVMII::getForm(MI.getDesc().TSFlags);
  return (F == GPUVMII::FrmLoad || F == GPUVMII::FrmStore) &&
         MI.getNumOperands() >= 3 && MI.getOperand(1).isReg() &&
         MI.getOperand(2).isImm() && MI.mayLoadOrStore() &&
         !MI.hasOrderedMemoryRef();
}

// Offsets of the accesses through Base that run whenever the loop is
// entered: in the blocks that dominate the preheader, and in the header before
// any branch out of it.
// The register a value was copied from, through copies and moves.
Register GPUVMLoadHoist::root(Register R) const {
  for (unsigned K = 0; K < 8 && R.isVirtual(); ++K) {
    MachineInstr *D = MRI->getUniqueVRegDef(R);
    if (!D)
      break;
    if (D->isCopy() && D->getOperand(1).getReg().isVirtual() &&
        !D->getOperand(1).getSubReg())
      R = D->getOperand(1).getReg();
    else if (D->getOpcode() == GPUVM::ADDI && D->getOperand(1).isReg() &&
             D->getOperand(1).getReg().isVirtual() &&
             D->getOperand(2).isImm() && D->getOperand(2).getImm() == 0)
      R = D->getOperand(1).getReg();
    else
      break;
  }
  return R;
}

// Offsets of the accesses through Base (or a copy of the same value) that run
// whenever the loop is entered: in the blocks that dominate the preheader,
// and in the header (before its terminators).
void GPUVMLoadHoist::knownAccesses(MachineLoop &L, Register Base,
                                   SmallVectorImpl<int64_t> &Offs) const {
  Register Root = root(Base);
  auto Scan = [&](MachineBasicBlock &B) {
    for (MachineInstr &U : B)
      if (baseImmAccess(U) && U.getOperand(1).getReg().isVirtual() &&
          root(U.getOperand(1).getReg()) == Root)
        Offs.push_back(U.getOperand(2).getImm());
  };
  Scan(*L.getHeader());
  unsigned Blocks = 0;
  for (MachineDomTreeNode *N = MDT->getNode(L.getLoopPreheader());
       N && Blocks < 16; N = N->getIDom(), ++Blocks)
    Scan(*N->getBlock());
}

bool GPUVMLoadHoist::hoist(MachineLoop &L) {
  MachineBasicBlock *P = L.getLoopPreheader();
  if (!P)
    return false;
  SmallVector<MachineInstr *, 16> Stores;
  SmallVector<MachineInstr *, 16> Loads;
  for (MachineBasicBlock *B : L.blocks())
    for (MachineInstr &MI : *B) {
      if (MI.isCall() || MI.hasUnmodeledSideEffects() || MI.isInlineAsm() ||
          (MI.mayStore() && MI.memoperands_empty()))
        return false;
      if (MI.mayStore())
        Stores.push_back(&MI);
      else if (MI.mayLoad() && baseImmAccess(MI) && !MI.isPseudo())
        Loads.push_back(&MI);
    }
  unsigned N = 0;
  bool Changed = false;
  for (MachineInstr *MI : Loads) {
    if (N >= MaxHoist)
      break;
    Register Base = MI->getOperand(1).getReg();
    Register Dst = MI->getOperand(0).getReg();
    if (!Base.isVirtual() || !Dst.isVirtual() || MI->memoperands_empty())
      continue;
    // the base the same through the loop
    MachineInstr *Def = MRI->getUniqueVRegDef(Base);
    if (!Def || L.contains(Def->getParent()))
      continue;
    bool Alias = false;
    for (MachineInstr *S : Stores)
      if (MI->mayAlias(AA, *S, /*UseTBAA=*/true)) {
        LLVM_DEBUG(dbgs() << "  " << *MI << "    may alias " << *S);
        Alias = true;
        break;
      }
    if (Alias)
      continue;
    // safe before the loop
    int64_t Off = MI->getOperand(2).getImm();
    SmallVector<int64_t, 8> Offs;
    knownAccesses(L, Base, Offs);
    bool Lo = false, Hi = false, Close = false;
    for (int64_t A : Offs) {
      Lo |= A <= Off;
      Hi |= A >= Off;
      Close |= std::abs(A - Off) <= (int64_t)Near;
      // base <= base + Off <= base + A, which is below RAM's end: readable
      // (every address below it is)
      Close |= Off >= 0 && Off <= A;
    }
    if (!Close && !(Lo && Hi)) {
      LLVM_DEBUG(dbgs() << "  " << *MI << "    not known safe\n");
      continue;
    }
    LLVM_DEBUG(dbgs() << "hoist " << *MI);
    MI->removeFromParent();
    P->insert(P->getFirstTerminator(), MI);
    ++N;
    Changed = true;
  }
  return Changed;
}

bool GPUVMLoadHoist::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()) || !LoadHoist)
    return false;
  MRI = &MF.getRegInfo();
  if (!MRI->isSSA())
    return false;
  AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
  MDT = &getAnalysis<MachineDominatorTreeWrapperPass>().getDomTree();
  MachineLoopInfo &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  bool Changed = false;
  SmallVector<MachineLoop *, 8> Work(MLI.begin(), MLI.end());
  // inner loops first: a load hoisted out of one may leave its parent too
  SmallVector<MachineLoop *, 16> Order;
  while (!Work.empty()) {
    MachineLoop *L = Work.pop_back_val();
    Order.push_back(L);
    for (MachineLoop *Sub : *L)
      Work.push_back(Sub);
  }
  for (MachineLoop *L : reverse(Order))
    Changed |= hoist(*L);
  return Changed;
}
