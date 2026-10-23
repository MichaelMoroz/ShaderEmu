//===-- GPUVMRebase.cpp - long offsets of one base made short -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A bundle has one long immediate, so loads and stores whose offsets are
// beyond imm12 take a bundle each: Quake III's gpuModelviewBlock copies 16
// words from 196480(t2) to 196608(t2), 32 bundles. Before register
// allocation, the accesses of a block through one base register with long
// offsets that lie within 4 KB of each other are made through a new base,
// `addi b, base, mid` (one long immediate), with short offsets: two go in a
// bundle then. Done for -gpuvm-rebase-min accesses (3) or more.
//
//===----------------------------------------------------------------------===//

#include "GPUVM.h"
#include "GPUVMInstrInfo.h"
#include "GPUVMSubtarget.h"
#include "MCTargetDesc/GPUVMBaseInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-rebase"

static cl::opt<unsigned>
    RebaseMin("gpuvm-rebase-min", cl::Hidden, cl::init(3),
              cl::desc("gpu_vm: accesses through one base with long offsets "
                       "made short through a new base from this many (0: "
                       "never)"));

static cl::opt<bool>
    FnRelRebase("gpuvm-rebase-fnrel", cl::Hidden, cl::init(true),
                cl::desc("gpu_vm: PIC's %fnrel accesses of one symbol and "
                         "addresses in it through a new base too"));

namespace {
class GPUVMRebase : public MachineFunctionPass {
public:
  static char ID;
  GPUVMRebase() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override {
    return "gpu_vm long offsets through a new base";
  }
  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};
} // namespace

char GPUVMRebase::ID = 0;

FunctionPass *llvm::createGPUVMRebasePass() { return new GPUVMRebase(); }

// A load or store `op r, imm(base)` with a virtual base and a long immediate,
// or PIC's `op r, %fnrel(sym+k)(base)`; and an `addi d, base, ...` of either
// (a global's address and an index after it: Quake III's arrays of structs).
static bool candidate(const MachineInstr &MI) {
  unsigned F = GPUVMII::getForm(MI.getDesc().TSFlags);
  bool Addi = MI.getOpcode() == GPUVM::ADDI;
  if (F != GPUVMII::FrmLoad && F != GPUVMII::FrmStore && !Addi)
    return false;
  if (MI.isPseudo() || MI.getNumExplicitOperands() != 3)
    return false;
  const MachineOperand &B = MI.getOperand(1), &O = MI.getOperand(2);
  if (!B.isReg() || !B.getReg().isVirtual())
    return false;
  if (O.isGlobal())
    return O.getTargetFlags() == GPUVMII::MO_FNREL && FnRelRebase;
  // (an addi of a long constant is not an address: only symbols for it)
  return !Addi && O.isImm() && !isInt<12>(O.getImm()) && isInt<32>(O.getImm());
}

static int64_t offsetOf(const MachineInstr &MI) {
  const MachineOperand &O = MI.getOperand(2);
  return O.isImm() ? O.getImm() : O.getOffset();
}

bool GPUVMRebase::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()) || !RebaseMin)
    return false;
  MachineRegisterInfo &MRI = MF.getRegInfo();
  if (!MRI.isSSA())
    return false;
  const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    // the candidates by base (and symbol), in program order
    using Key = std::pair<Register, const GlobalValue *>;
    DenseMap<Key, SmallVector<MachineInstr *, 8>> ByBase;
    SmallVector<Key, 8> Bases;
    for (MachineInstr &MI : MBB)
      if (candidate(MI)) {
        const MachineOperand &O = MI.getOperand(2);
        Key K(MI.getOperand(1).getReg(), O.isGlobal() ? O.getGlobal() : nullptr);
        auto &L = ByBase[K];
        if (L.empty())
          Bases.push_back(K);
        L.push_back(&MI);
      }
    for (Key KB : Bases) {
      Register B = KB.first;
      auto &L = ByBase[KB];
      if (L.size() < RebaseMin)
        continue;
      // groups of offsets within 4094 of the lowest (sorted, greedily)
      SmallVector<MachineInstr *, 8> S(L.begin(), L.end());
      llvm::stable_sort(S, [](const MachineInstr *A, const MachineInstr *C) {
        return offsetOf(*A) < offsetOf(*C);
      });
      for (unsigned I = 0; I < S.size();) {
        int64_t Lo = offsetOf(*S[I]);
        unsigned J = I;
        while (J < S.size() && offsetOf(*S[J]) - Lo <= 4094)
          ++J;
        if (J - I >= RebaseMin) {
          int64_t Hi = offsetOf(*S[J - 1]);
          int64_t Mid = Lo + (Hi - Lo) / 2;
          // before the first of the group in the block's order
          MachineInstr *First = nullptr;
          for (MachineInstr &MI : MBB) {
            if (llvm::is_contained(ArrayRef(S).slice(I, J - I), &MI)) {
              First = &MI;
              break;
            }
          }
          Register NB = MRI.createVirtualRegister(MRI.getRegClass(B));
          auto NewBase =
              BuildMI(MBB, First, First->getDebugLoc(), TII->get(GPUVM::ADDI), NB)
                  .addReg(B);
          if (KB.second)
            NewBase.addGlobalAddress(KB.second, Mid, GPUVMII::MO_FNREL);
          else
            NewBase.addImm(Mid);
          for (unsigned K = I; K < J; ++K) {
            MachineInstr *MI = S[K];
            int64_t Off = offsetOf(*MI) - Mid;
            MI->getOperand(1).setReg(NB);
            MI->getOperand(1).setIsKill(false);
            MI->getOperand(2).ChangeToImmediate(Off);
          }
          MRI.clearKillFlags(B);
          Changed = true;
        }
        I = J;
      }
    }
  }
  return Changed;
}
