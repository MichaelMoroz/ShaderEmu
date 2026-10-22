//===-- GPUVMHoistConstants.cpp - constants of branches to the entry ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A branch compares registers as its bundle found them: `li t, 1000;
// beq a, t, L` is two bundles, the second for the branch alone. A constant a
// compare-and-branch needs (beyond imm6) is made once in the entry block
// instead, where it shares a bundle with something (li is lite: any op 0
// takes it as op 1), and the same constant once for every branch. With 64
// registers this rarely costs a spill; li is rematerializable, so the
// register allocator makes it again where one is short rather than spill it.
// A constant in a loop stays there (-gpuvm-hoist-loops=0): the bundle filling
// (GPUVMGlobalFill) finds it an empty slot of the loop's branch bundles, where
// the entry would make it on every call (CoreMark's state machine: 126.1k to
// 124.7k bundles an iteration).
//
//===----------------------------------------------------------------------===//

#include "GPUVM.h"
#include "GPUVMInstrInfo.h"
#include "GPUVMSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/MachineBlockFrequencyInfo.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-hoist-constants"

static cl::opt<unsigned>
    MaxHoisted("gpuvm-hoist-constants", cl::Hidden, cl::init(16),
               cl::desc("gpu_vm: constants of branches made in the entry "
                        "block at most (0: none)"));
static cl::opt<bool>
    HoistLoops("gpuvm-hoist-loops", cl::Hidden, cl::init(false),
               cl::desc("gpu_vm: constants of branches in loops move to the "
                        "entry too"));
static cl::opt<double>
    HoistCold("gpuvm-hoist-cold", cl::Hidden, cl::init(0.3),
              cl::desc("gpu_vm: a constant of a block that runs less often "
                       "than this (the entry 1) stays"));

namespace {
class GPUVMHoistConstants : public MachineFunctionPass {
public:
  static char ID;
  GPUVMHoistConstants() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override {
    return "gpu_vm branch constants to the entry";
  }
  bool runOnMachineFunction(MachineFunction &MF) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<MachineLoopInfoWrapperPass>();
    AU.addPreserved<MachineLoopInfoWrapperPass>();
    AU.addRequired<MachineBlockFrequencyInfoWrapperPass>();
    AU.addPreserved<MachineBlockFrequencyInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};
} // namespace

char GPUVMHoistConstants::ID = 0;

FunctionPass *llvm::createGPUVMHoistConstantsPass() {
  return new GPUVMHoistConstants();
}

// li rd, imm: addi rd, zero, imm
static bool isLi(const MachineInstr &MI) {
  return MI.getOpcode() == GPUVM::ADDI && MI.getOperand(1).isReg() &&
         MI.getOperand(1).getReg() == GPUVM::R0 && MI.getOperand(2).isImm() &&
         MI.getOperand(0).getReg().isVirtual();
}

static bool isRegBranch(unsigned Opc) {
  switch (Opc) {
  case GPUVM::BEQ:
  case GPUVM::BNE:
  case GPUVM::BLT:
  case GPUVM::BGE:
  case GPUVM::BLTU:
  case GPUVM::BGEU:
    return true;
  default:
    return false;
  }
}

bool GPUVMHoistConstants::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()) || MaxHoisted == 0 || MF.size() < 2)
    return false;
  MachineRegisterInfo &MRI = MF.getRegInfo();
  MachineLoopInfo &MLI = getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  MachineBlockFrequencyInfo &MBFI =
      getAnalysis<MachineBlockFrequencyInfoWrapperPass>().getMBFI();
  MachineBasicBlock &Entry = MF.front();
  DenseMap<int64_t, Register> InEntry;
  for (MachineInstr &MI : Entry)
    if (isLi(MI))
      InEntry.try_emplace(MI.getOperand(2).getImm(),
                          MI.getOperand(0).getReg());
  unsigned Hoisted = 0;
  bool Changed = false;
  for (MachineBasicBlock &MBB : MF) {
    if (&MBB == &Entry)
      continue;
    for (MachineInstr &MI : make_early_inc_range(MBB)) {
      if (!isLi(MI))
        continue;
      Register R = MI.getOperand(0).getReg();
      if (MRI.use_nodbg_empty(R))
        continue;
      bool OnlyBranches = true;
      for (const MachineInstr &U : MRI.use_nodbg_instructions(R))
        if (!isRegBranch(U.getOpcode()))
          OnlyBranches = false;
      if (!OnlyBranches)
        continue;
      int64_t Imm = MI.getOperand(2).getImm();
      auto It = InEntry.find(Imm);
      if (It != InEntry.end()) {
        Register E = It->second;
        if (MRI.getRegClass(E) != MRI.getRegClass(R))
          continue;
        MRI.replaceRegWith(R, E);
        MRI.clearKillFlags(E);
        MI.eraseFromParent();
        Changed = true;
        continue;
      }
      if (Hoisted >= MaxHoisted ||
          (!HoistLoops && MLI.getLoopDepth(&MBB) > 0) ||
          MBFI.getBlockFreqRelativeToEntryBlock(&MBB) < HoistCold)
        continue;
      MI.removeFromParent();
      Entry.insert(Entry.getFirstTerminator(), &MI);
      MRI.clearKillFlags(R);
      InEntry[Imm] = R;
      ++Hoisted;
      Changed = true;
    }
  }
  return Changed;
}
