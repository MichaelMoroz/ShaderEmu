//===-- GPUVMBundleModel.cpp - the streamer's packing on machine code -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUVMBundleModel.h"
#include "GPUVMInstrInfo.h"
#include "MCTargetDesc/GPUVMBaseInfo.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

GPUVM::OpDesc GPUVM::describeOp(const MachineInstr &MI,
                                const TargetRegisterInfo *TRI, Register From,
                                Register To) {
  OpDesc D;
  auto Enc = [&](const MachineOperand &MO) -> unsigned {
    if (!MO.isReg() || !MO.getReg().isPhysical())
      return 0;
    MCRegister R = MO.getReg().asMCReg();
    if (From && R == From.asMCReg())
      R = To.asMCReg();
    if (GPUVM::GPRQRegClass.contains(R))
      R = TRI->getSubReg(R, GPUVM::sub0);
    return TRI->getEncodingValue(R) & 63;
  };
  auto R = [&](unsigned N) { return Enc(MI.getOperand(N)); };
  auto IsLong = [&](unsigned N, bool Float) {
    const MachineOperand &MO = MI.getOperand(N);
    if (!MO.isImm())
      return true; // a symbol: long
    int64_t V = MO.getImm();
    return Float ? (static_cast<uint32_t>(V) & 0xfffff) != 0 : !isInt<12>(V);
  };
  uint64_t TSFlags = MI.getDesc().TSFlags;
  unsigned Opc = GPUVMII::getOpc(TSFlags);
  D.Meta = MI.isMetaInstruction() || MI.isDebugInstr();
  if (D.Meta)
    return D;
  D.Known = !MI.isPseudo() || MI.getOpcode() == GPUVM::LQ_Q ||
            MI.getOpcode() == GPUVM::SQ_Q || MI.getOpcode() == GPUVM::SQ_OP;
  if (GPUVMII::isControl(TSFlags) || MI.isCall() || MI.isTerminator()) {
    D.Known = true;
    D.Control = true;
    switch (GPUVMII::getForm(TSFlags)) {
    case GPUVMII::CtlBr:
      D.Read[D.NRead++] = R(0);
      D.Read[D.NRead++] = R(1);
      break;
    case GPUVMII::CtlBrI:
    case GPUVMII::CtlBit:
      D.Read[D.NRead++] = R(0);
      break;
    case GPUVMII::CtlAddBnz:
      D.Read[D.NRead++] = R(1);
      break;
    default:
      break;
    }
    return D;
  }
  if (!D.Known)
    return D;
  switch (GPUVMII::getForm(TSFlags)) {
  case GPUVMII::FrmR:
    D.Word = wordR(Opc, R(0), R(1), R(2), 0);
    break;
  case GPUVMII::FrmR4:
    D.Word = wordR(Opc, R(0), R(1), R(2), R(3));
    break;
  case GPUVMII::FrmRSh:
  case GPUVMII::FrmLoadX:
  case GPUVMII::FrmStoreX:
    D.Word = wordR(Opc, R(0), R(1), R(2), 0);
    break;
  case GPUVMII::FrmR1:
    D.Word = wordR(Opc, R(0), R(1), 0, 0);
    break;
  case GPUVMII::FrmI:
  case GPUVMII::FrmLoad:
  case GPUVMII::FrmStore:
    D.Word = wordI(Opc, R(0), R(1), 0);
    D.Long = IsLong(2, false);
    break;
  case GPUVMII::FrmFI:
    D.Word = wordI(Opc, R(0), R(1), 0);
    D.Long = IsLong(2, true);
    break;
  case GPUVMII::FrmBF:
    D.Word = wordI(Opc, R(0), R(1), 0);
    break;
  case GPUVMII::FrmBFI:
    D.Word = wordI(Opc, R(0), R(2), 0);
    break;
  case GPUVMII::FrmAMO:
    D.Word = wordR(Opc, R(0), R(2), R(1), 0);
    break;
  case GPUVMII::FrmCAS:
    D.Word = wordR(Opc, R(0), R(3), R(2), 0);
    break;
  case GPUVMII::FrmLPC:
    D.Word = wordI(Opc, R(0), 0, 0);
    D.Long = true;
    break;
  case GPUVMII::FrmMFSR:
    D.Word = wordI(Opc, R(0), 0, 0);
    break;
  case GPUVMII::FrmMTSR:
    D.Word = wordI(Opc, 0, R(1), 0);
    break;
  case GPUVMII::FrmXSR:
    D.Word = wordI(Opc, R(0), R(2), 0);
    break;
  case GPUVMII::FrmNone:
  case GPUVMII::FrmUnimp:
    D.Word = wordR(Opc, 0, 0, 0, 0);
    break;
  default:
    D.Known = false;
    break;
  }
  return D;
}

bool GPUVM::Packer::joins(const OpDesc &D) const {
  if (!D.Known || D.Control || Words.size() != 1)
    return false;
  if (D.Long && HasLong)
    return false;
  return bundle::fits1(Words[0], D.Word);
}

bool GPUVM::Packer::put(const OpDesc &D) {
  if (D.Meta)
    return false;
  if (!D.Known) {
    close();
    ++Closed; // whatever it is, at least a bundle of its own
    return true;
  }
  if (D.Control) {
    for (unsigned I = 0; I < (GPUVM::lateBranches() ? 0u : D.NRead); ++I)
      if (D.Read[I] != 0 && bundle::writes(Words, D.Read[I])) {
        close();
        break;
      }
    bool Started = Words.empty();
    Words.clear();
    HasLong = false;
    ++Closed;
    return Started;
  }
  if (!Words.empty() && !joins(D))
    close();
  bool Started = Words.empty();
  Words.push_back(D.Word);
  HasLong |= D.Long;
  return Started;
}

bool GPUVM::startsBundle(const MachineBasicBlock &MBB) {
  if (MBB.isEHPad() || MBB.hasAddressTaken() || MBB.hasLabelMustBeEmitted() ||
      MBB.pred_empty() || MBB.pred_size() > 1)
    return true;
  const MachineBasicBlock *Pred = *MBB.pred_begin();
  if (!Pred->isLayoutSuccessor(&MBB))
    return true;
  for (const MachineInstr &MI : Pred->terminators()) {
    if (!MI.isBranch() || MI.isIndirectBranch())
      return true;
    for (const MachineOperand &MO : MI.operands())
      if (MO.isJTI() || (MO.isMBB() && MO.getMBB() == &MBB))
        return true;
  }
  return false;
}
