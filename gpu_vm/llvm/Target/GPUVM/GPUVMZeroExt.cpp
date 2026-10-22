//===-- GPUVMZeroExt.cpp - drop masks of values already zero-extended -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An i8 or i16 value that crosses blocks (a PHI) is legalized to 32 bits with
// its high bits unknown, so a compare or an extension of it masks it again
// (`andi r, x, 255`) though every value it can be came from lbu, lhu or a
// mask. The DAG sees one block; this pass, before register allocation (SSA),
// knows the leading zero bits of each virtual register through PHIs and
// copies (a fixed point from the optimistic side) and drops the masks that
// clear nothing. In a loop such a mask is an op of every iteration (zlib's
// longest_match, CoreMark's list search).
//
//===----------------------------------------------------------------------===//

#include "GPUVM.h"
#include "GPUVMInstrInfo.h"
#include "GPUVMSubtarget.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-zero-ext"

static cl::opt<bool> ZeroExt("gpuvm-zero-ext", cl::Hidden, cl::init(true),
                             cl::desc("gpu_vm: drop masks of values already "
                                      "zero-extended"));

namespace {
class GPUVMZeroExt : public MachineFunctionPass {
public:
  static char ID;
  GPUVMZeroExt() : MachineFunctionPass(ID) {}
  StringRef getPassName() const override {
    return "gpu_vm masks of zero-extended values";
  }
  bool runOnMachineFunction(MachineFunction &MF) override;

private:
  MachineRegisterInfo *MRI = nullptr;
  DenseMap<Register, unsigned> LZ; // known leading zeros of a vreg
  unsigned lz(const MachineOperand &MO) const;
  unsigned transfer(const MachineInstr &MI) const;
};
} // namespace

char GPUVMZeroExt::ID = 0;

FunctionPass *llvm::createGPUVMZeroExtPass() { return new GPUVMZeroExt(); }

static unsigned lzImm(int64_t V) {
  return V < 0 ? 0 : llvm::countl_zero(static_cast<uint32_t>(V));
}

unsigned GPUVMZeroExt::lz(const MachineOperand &MO) const {
  if (MO.isImm())
    return lzImm(MO.getImm());
  if (!MO.isReg())
    return 0;
  Register R = MO.getReg();
  if (R == GPUVM::R0)
    return 32;
  if (!R.isVirtual() || MO.getSubReg())
    return 0;
  auto It = LZ.find(R);
  return It == LZ.end() ? 0 : It->second;
}

// The leading zeros of MI's result from its operands'.
unsigned GPUVMZeroExt::transfer(const MachineInstr &MI) const {
  auto Op = [&](unsigned N) { return lz(MI.getOperand(N)); };
  switch (MI.getOpcode()) {
  case GPUVM::LBU:
  case GPUVM::LBUX:
    return 24;
  case GPUVM::LHU:
  case GPUVM::LHUX:
    return 16;
  case GPUVM::ANDI:
  case GPUVM::AND:
  case GPUVM::MINU:
  case GPUVM::MINUI:
    return std::max(Op(1), Op(2));
  case GPUVM::OR:
  case GPUVM::ORI:
  case GPUVM::XOR:
  case GPUVM::XORI:
  case GPUVM::MAXU:
  case GPUVM::MAXUI:
    return std::min(Op(1), Op(2));
  case GPUVM::SRLI: {
    const MachineOperand &K = MI.getOperand(2);
    if (!K.isImm())
      return 0;
    return std::min<unsigned>(32, Op(1) + (K.getImm() & 31));
  }
  case GPUVM::ADDI:
    // a copy, or a constant
    if (MI.getOperand(2).isImm() && MI.getOperand(2).getImm() == 0)
      return Op(1);
    if (MI.getOperand(1).isReg() && MI.getOperand(1).getReg() == GPUVM::R0)
      return Op(2);
    return 0;
  case GPUVM::SLT:
  case GPUVM::SLTI:
  case GPUVM::SLTU:
  case GPUVM::SLTUI:
  case GPUVM::SGE:
  case GPUVM::SGEI:
  case GPUVM::SGEU:
  case GPUVM::SGEUI:
  case GPUVM::SEQ:
  case GPUVM::SEQI:
  case GPUVM::SNE:
  case GPUVM::SNEI:
    return 31;
  case TargetOpcode::COPY:
    return Op(1);
  case TargetOpcode::PHI: {
    unsigned M = 32;
    for (unsigned I = 1; I < MI.getNumOperands(); I += 2)
      M = std::min(M, Op(I));
    return M;
  }
  default:
    return 0;
  }
}

bool GPUVMZeroExt::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()) || !ZeroExt)
    return false;
  MRI = &MF.getRegInfo();
  if (!MRI->isSSA())
    return false;
  // Optimistic: every result of an instruction we know starts at 32 leading
  // zeros and only goes down.
  LZ.clear();
  SmallVector<MachineInstr *, 64> Defs;
  for (MachineBasicBlock &MBB : MF)
    for (MachineInstr &MI : MBB) {
      if (MI.getNumOperands() == 0 || !MI.getOperand(0).isReg() ||
          !MI.getOperand(0).isDef() || !MI.getOperand(0).getReg().isVirtual())
        continue;
      LZ[MI.getOperand(0).getReg()] = 32;
      Defs.push_back(&MI);
    }
  for (bool Change = true; Change;) {
    Change = false;
    for (MachineInstr *MI : Defs) {
      Register R = MI->getOperand(0).getReg();
      unsigned V = std::min(LZ[R], transfer(*MI));
      if (V != LZ[R]) {
        LZ[R] = V;
        Change = true;
      }
    }
  }
  bool Changed = false;
  for (MachineInstr *MI : Defs) {
    if (MI->getOpcode() != GPUVM::ANDI || !MI->getOperand(2).isImm() ||
        !MI->getOperand(1).isReg() || !MI->getOperand(1).getReg().isVirtual())
      continue;
    int64_t M = MI->getOperand(2).getImm();
    if (M < 0 || !isMask_64(M))
      continue;
    Register Src = MI->getOperand(1).getReg(), Dst = MI->getOperand(0).getReg();
    if (lz(MI->getOperand(1)) < 32 - llvm::popcount(static_cast<uint64_t>(M)))
      continue;
    if (!MRI->constrainRegClass(Src, MRI->getRegClass(Dst)))
      continue;
    LLVM_DEBUG(dbgs() << "drop " << *MI);
    MRI->replaceRegWith(Dst, Src);
    MRI->clearKillFlags(Src);
    MI->eraseFromParent();
    Changed = true;
  }
  return Changed;
}
