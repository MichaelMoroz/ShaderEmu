//===-- GPUVMBundleScheduler.cpp - order ops to fill bundles --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The post-RA machine scheduler's strategy for gpu_vm. A bundle is one
// iteration of the tick whatever it holds, and the streamer packs ops in
// program order (GPUVMELFStreamer, gvm_asm.h's rules): op 1 must be lite (or
// float lite after a float or memory op 0, or a memory op after a memory op 0
// that reads nothing it writes), and a bundle has one long immediate. So the
// cost of a block is its number of bundles, and dependent ops may share one
// (each sees the one before). Top down, this strategy simulates the open
// bundle: with one op in it, it takes a ready op that fits as op 1; starting a
// bundle, it takes an op that some other op can follow, a non-lite one first
// (lite ops can be op 1 of something else).
//
//===----------------------------------------------------------------------===//

#include "GPUVM.h"
#include "GPUVMBundleModel.h"
#include "GPUVMInstrInfo.h"
#include "GPUVMSubtarget.h"
#include "MCTargetDesc/GPUVMBaseInfo.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-bundle-sched"

namespace {

using OpInfo = GPUVM::OpDesc;

// What a bundle of the op costs the tick: a memory body (paths 3-6), the
// integer path (7), or step12 and the general step (floats, sel, lpc, the
// extended class). A pair of one kind spends one bundle where two mixed spend
// two of the dearer kind.
static cl::opt<bool> SchedKind("gpuvm-sched-kind", cl::Hidden, cl::init(false),
                               cl::desc("gpu_vm: pair ops of one kind first"));

static unsigned kind(uint32_t W) {
  if (!SchedKind)
    return 0;
  if (GPUVM::bundle::memOp(W))
    return 1;
  return GPUVM::bundle::mid(W) ? 0 : 2;
}

class GPUVMBundleStrategy : public MachineSchedStrategy {
  ScheduleDAGMI *DAG = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  std::vector<SUnit *> Ready;
  DenseMap<const SUnit *, OpInfo> Infos;
  // The open bundle
  SmallVector<uint32_t, 2> Words;
  bool HasLong = false;
  // The registers the branch after the region compares: their writers go
  // early (a branch whose bundle writes one needs a bundle of its own).
  SmallVector<Register, 2> BranchRegs;

  bool writesBranchReg(const SUnit *SU) const {
    for (Register R : BranchRegs)
      if (SU->getInstr()->modifiesRegister(R, TRI))
        return true;
    return false;
  }
  void findBranchRegs();

  const OpInfo &info(const SUnit *SU);
  bool fits(const OpInfo &I) const {
    if (!I.Known || I.Control || Words.size() != 1)
      return false;
    if (I.Long && HasLong)
      return false;
    return GPUVM::bundle::fits1(Words[0], I.Word);
  }

public:
  bool shouldTrackPressure() const override { return false; }

  void initialize(ScheduleDAGMI *Dag) override {
    DAG = Dag;
    TRI = DAG->TRI;
    TII = DAG->TII;
    Ready.clear();
    Infos.clear();
    Words.clear();
    HasLong = false;
    findBranchRegs();
  }

  SUnit *pickNode(bool &IsTopNode) override;
  void schedNode(SUnit *SU, bool IsTopNode) override;
  void releaseTopNode(SUnit *SU) override { Ready.push_back(SU); }
  void releaseBottomNode(SUnit *SU) override {}
};

} // namespace

void GPUVMBundleStrategy::findBranchRegs() {
  BranchRegs.clear();
  MachineBasicBlock::iterator E = DAG->end();
  if (DAG->SUnits.empty() ||
      E == DAG->SUnits[0].getInstr()->getParent()->end() ||
      !GPUVMInstrInfo::isCondBranch(E->getOpcode()))
    return;
  for (unsigned I = 0; I < 2; ++I) {
    const MachineOperand &MO = E->getOperand(I);
    if (MO.isReg() && MO.getReg() != GPUVM::R0)
      BranchRegs.push_back(MO.getReg());
  }
  // `addi r, r, k; bnei r, 0` becomes addbnz (GPUVMCountedLoops): the step
  // is no op then, and stays the last to touch r.
  if (!BranchRegs.empty() &&
      (E->getOpcode() == GPUVM::BNEI || E->getOpcode() == GPUVM::BEQI) &&
      E->getOperand(1).getImm() == 0)
    for (const SUnit &SU : DAG->SUnits) {
      const MachineInstr &MI = *SU.getInstr();
      if (MI.getOpcode() == GPUVM::ADDI &&
          MI.getOperand(0).getReg() == BranchRegs[0] &&
          MI.getOperand(1).getReg() == BranchRegs[0] &&
          MI.getOperand(2).isImm() && isInt<6>(MI.getOperand(2).getImm())) {
        BranchRegs.clear();
        break;
      }
    }
}

const OpInfo &GPUVMBundleStrategy::info(const SUnit *SU) {
  auto It = Infos.find(SU);
  if (It != Infos.end())
    return It->second;
  return Infos[SU] = GPUVM::describeOp(*SU->getInstr(), TRI);
}

SUnit *GPUVMBundleStrategy::pickNode(bool &IsTopNode) {
  IsTopNode = true;
  if (Ready.empty())
    return nullptr;
  auto Better = [this](const SUnit *A, const SUnit *B) {
    // a writer of the branch's registers, the longer path to the region's
    // end, then the original order
    bool WA = writesBranchReg(A), WB = writesBranchReg(B);
    if (WA != WB)
      return WA;
    if (A->getHeight() != B->getHeight())
      return A->getHeight() > B->getHeight();
    return A->NodeNum < B->NodeNum;
  };
  SUnit *Best = nullptr;
  // What makes no op goes first: it takes no place.
  for (SUnit *SU : Ready)
    if (info(SU).Meta) {
      Best = SU;
      break;
    }
  // One op open: an op that fits as op 1, one of op 0's kind first (two
  // memory ops, two integer ops), a float after a memory op last (path 2).
  if (!Best && Words.size() == 1) {
    auto Rank = [&](const SUnit *SU) {
      uint32_t W = info(SU).Word;
      if (kind(W) == kind(Words[0]))
        return 2;
      return GPUVM::bundle::memOp(Words[0]) && GPUVM::bundle::fLite(W) ? 0
                                                                      : 1;
    };
    int BestRank = -1;
    for (SUnit *SU : Ready) {
      if (!fits(info(SU)))
        continue;
      int R = Rank(SU);
      if (R > BestRank || (R == BestRank && Better(SU, Best))) {
        Best = SU;
        BestRank = R;
      }
    }
  }
  if (!Best) {
    // A new bundle: an op that another can follow (one ready now, or a
    // successor it alone holds back), a non-lite one first.
    int BestScore = -1;
    for (SUnit *SU : Ready) {
      const OpInfo &I = info(SU);
      int Score = 0;
      if (I.Known && !I.Control) {
        bool Pair = false, Same = false;
        auto Follows = [&](const SUnit *D) {
          const OpInfo &J = info(D);
          if (!J.Known || J.Control || (I.Long && J.Long) ||
              !GPUVM::bundle::fits1(I.Word, J.Word))
            return false;
          Pair = true;
          Same |= kind(J.Word) == kind(I.Word);
          return Same;
        };
        for (SUnit *D : Ready)
          if (D != SU && Follows(D))
            break;
        if (!Same)
          for (const SDep &S : SU->Succs) {
            const SUnit *D = S.getSUnit();
            if (D->isBoundaryNode() || D->NumPredsLeft != 1)
              continue;
            if (Follows(D))
              break;
          }
        Score = (writesBranchReg(SU) ? 8 : 0) + (Same ? 4 : 0) +
                (Pair ? 2 : 0) + (GPUVM::bundle::lite(I.Word) ? 0 : 1);
      }
      if (Score > BestScore || (Score == BestScore && Better(SU, Best))) {
        Best = SU;
        BestScore = Score;
      }
    }
  }
  Ready.erase(llvm::find(Ready, Best));
  return Best;
}

void GPUVMBundleStrategy::schedNode(SUnit *SU, bool IsTopNode) {
  const OpInfo &I = info(SU);
  if (I.Meta)
    return;
  if (!I.Known || I.Control) {
    Words.clear();
    HasLong = false;
    return;
  }
  if (!fits(I)) {
    Words.clear();
    HasLong = false;
  }
  Words.push_back(I.Word);
  HasLong |= I.Long;
  if (Words.size() == 2) {
    Words.clear();
    HasLong = false;
  }
}

namespace llvm {
ScheduleDAGInstrs *createGPUVMPostMachineScheduler(MachineSchedContext *C) {
  return new ScheduleDAGMI(C, std::make_unique<GPUVMBundleStrategy>(),
                           /*RemoveKillFlags=*/true);
}
} // namespace llvm
