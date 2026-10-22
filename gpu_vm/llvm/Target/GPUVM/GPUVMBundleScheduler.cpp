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
#include "GPUVMInstrInfo.h"
#include "GPUVMSubtarget.h"
#include "MCTargetDesc/GPUVMBaseInfo.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/ScheduleDAG.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-bundle-sched"

namespace {

// An op's word as the encoder makes it (the fields the packing rules read),
// and whether it takes the long immediate. Unknown for anything that is not
// one op (Known false).
struct OpInfo {
  bool Known = false;
  bool Control = false;
  uint32_t Word = 0;
  bool Long = false;
};

class GPUVMBundleStrategy : public MachineSchedStrategy {
  ScheduleDAGMI *DAG = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  std::vector<SUnit *> Ready;
  DenseMap<const SUnit *, OpInfo> Infos;
  // The open bundle
  SmallVector<uint32_t, 2> Words;
  bool HasLong = false;

  unsigned enc(const MachineOperand &MO) const {
    if (!MO.isReg() || !MO.getReg().isPhysical())
      return 0;
    MCRegister R = MO.getReg().asMCReg();
    if (GPUVM::GPRQRegClass.contains(R))
      R = TRI->getSubReg(R, GPUVM::sub0);
    return TRI->getEncodingValue(R) & 63;
  }

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
  }

  SUnit *pickNode(bool &IsTopNode) override;
  void schedNode(SUnit *SU, bool IsTopNode) override;
  void releaseTopNode(SUnit *SU) override { Ready.push_back(SU); }
  void releaseBottomNode(SUnit *SU) override {}
};

} // namespace

const OpInfo &GPUVMBundleStrategy::info(const SUnit *SU) {
  auto It = Infos.find(SU);
  if (It != Infos.end())
    return It->second;
  OpInfo I;
  const MachineInstr &MI = *SU->getInstr();
  uint64_t TSFlags = MI.getDesc().TSFlags;
  unsigned Opc = GPUVMII::getOpc(TSFlags);
  auto R = [&](unsigned N) { return enc(MI.getOperand(N)); };
  auto Imm = [&](unsigned N, bool Float) {
    const MachineOperand &MO = MI.getOperand(N);
    if (!MO.isImm())
      return true; // a symbol: long
    int64_t V = MO.getImm();
    return Float ? (static_cast<uint32_t>(V) & 0xfffff) != 0 : !isInt<12>(V);
  };
  I.Known = !MI.isPseudo() || MI.getOpcode() == GPUVM::LQ_Q ||
            MI.getOpcode() == GPUVM::SQ_Q || MI.getOpcode() == GPUVM::SQ_OP;
  if (GPUVMII::isControl(TSFlags) || MI.isCall() || MI.isTerminator()) {
    I.Known = true;
    I.Control = true;
  } else if (I.Known) {
    switch (GPUVMII::getForm(TSFlags)) {
    case GPUVMII::FrmR:
      I.Word = GPUVM::wordR(Opc, R(0), R(1), R(2), 0);
      break;
    case GPUVMII::FrmR4:
      I.Word = GPUVM::wordR(Opc, R(0), R(1), R(2), R(3));
      break;
    case GPUVMII::FrmRSh:
    case GPUVMII::FrmLoadX:
    case GPUVMII::FrmStoreX:
      I.Word = GPUVM::wordR(Opc, R(0), R(1), R(2), 0);
      break;
    case GPUVMII::FrmR1:
      I.Word = GPUVM::wordR(Opc, R(0), R(1), 0, 0);
      break;
    case GPUVMII::FrmI:
    case GPUVMII::FrmLoad:
    case GPUVMII::FrmStore:
      I.Word = GPUVM::wordI(Opc, R(0), R(1), 0);
      I.Long = Imm(2, false);
      break;
    case GPUVMII::FrmFI:
      I.Word = GPUVM::wordI(Opc, R(0), R(1), 0);
      I.Long = Imm(2, true);
      break;
    case GPUVMII::FrmBF:
      I.Word = GPUVM::wordI(Opc, R(0), R(1), 0);
      break;
    case GPUVMII::FrmBFI:
      I.Word = GPUVM::wordI(Opc, R(0), R(2), 0);
      break;
    case GPUVMII::FrmAMO:
      I.Word = GPUVM::wordR(Opc, R(0), R(2), R(1), 0);
      break;
    case GPUVMII::FrmCAS:
      I.Word = GPUVM::wordR(Opc, R(0), R(3), R(2), 0);
      break;
    case GPUVMII::FrmLPC:
      I.Word = GPUVM::wordI(Opc, R(0), 0, 0);
      I.Long = true;
      break;
    case GPUVMII::FrmMFSR:
      I.Word = GPUVM::wordI(Opc, R(0), 0, 0);
      break;
    case GPUVMII::FrmMTSR:
      I.Word = GPUVM::wordI(Opc, 0, R(1), 0);
      break;
    case GPUVMII::FrmXSR:
      I.Word = GPUVM::wordI(Opc, R(0), R(2), 0);
      break;
    case GPUVMII::FrmNone:
    case GPUVMII::FrmUnimp:
      I.Word = GPUVM::wordR(Opc, 0, 0, 0, 0);
      break;
    default:
      I.Known = false;
      break;
    }
  }
  return Infos[SU] = I;
}

SUnit *GPUVMBundleStrategy::pickNode(bool &IsTopNode) {
  IsTopNode = true;
  if (Ready.empty())
    return nullptr;
  auto Better = [](const SUnit *A, const SUnit *B) {
    // the longer path to the region's end, then the original order
    if (A->getHeight() != B->getHeight())
      return A->getHeight() > B->getHeight();
    return A->NodeNum < B->NodeNum;
  };
  SUnit *Best = nullptr;
  // One op open: an op that fits as op 1.
  if (Words.size() == 1) {
    for (SUnit *SU : Ready)
      if (fits(info(SU)) && (!Best || Better(SU, Best)))
        Best = SU;
  }
  if (!Best) {
    // A new bundle: an op that another can follow (one ready now, or a
    // successor it alone holds back), a non-lite one first.
    int BestScore = -1;
    for (SUnit *SU : Ready) {
      const OpInfo &I = info(SU);
      int Score = 0;
      if (I.Known && !I.Control) {
        bool Pair = false;
        auto Follows = [&](const SUnit *D) {
          const OpInfo &J = info(D);
          return J.Known && !J.Control && !(I.Long && J.Long) &&
                 GPUVM::bundle::fits1(I.Word, J.Word);
        };
        for (SUnit *D : Ready)
          if (D != SU && Follows(D)) {
            Pair = true;
            break;
          }
        if (!Pair)
          for (const SDep &S : SU->Succs) {
            const SUnit *D = S.getSUnit();
            if (D->isBoundaryNode() || D->NumPredsLeft != 1)
              continue;
            if (Follows(D)) {
              Pair = true;
              break;
            }
          }
        Score = (Pair ? 2 : 0) + (GPUVM::bundle::lite(I.Word) ? 0 : 1);
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
