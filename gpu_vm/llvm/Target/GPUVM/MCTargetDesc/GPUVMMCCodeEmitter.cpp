//===-- GPUVMMCCodeEmitter.cpp - gpu_vm code emitter ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/GPUVMMCCodeEmitter.h"
#include "MCTargetDesc/GPUVMBaseInfo.h"
#include "MCTargetDesc/GPUVMFixupKinds.h"
#include "MCTargetDesc/GPUVMMCTargetDesc.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;

#define DEBUG_TYPE "mccodeemitter"

unsigned GPUVMMCCodeEmitter::reg(const MCOperand &MO) const {
  return Ctx.getRegisterInfo()->getEncodingValue(MO.getReg()) & 63;
}

uint64_t GPUVMMCCodeEmitter::tsFlags(const MCInst &MI) const {
  return MCII.get(MI.getOpcode()).TSFlags;
}

bool GPUVMMCCodeEmitter::isControl(const MCInst &MI) const {
  return GPUVMII::isControl(MCII.get(MI.getOpcode()).TSFlags);
}

// An operand's value when it is known now (not a symbol's).
static bool constValue(const MCOperand &MO, int64_t &V) {
  if (MO.isImm()) {
    V = MO.getImm();
    return true;
  }
  if (isa<MCSpecifierExpr>(MO.getExpr()))
    return false;
  return MO.getExpr()->evaluateAsAbsolute(V);
}

// An I form's immediate: short (imm12) when it fits, else the long immediate.
static void setImm(GPUVMMCCodeEmitter::OpEncoding &E, const MCOperand &MO,
                   bool Float, unsigned Kind) {
  int64_t V;
  if (constValue(MO, V)) {
    uint32_t U = static_cast<uint32_t>(V);
    if (Float ? (U & 0xfffff) == 0 : isInt<12>(V)) {
      E.Word |= (Float ? U >> 20 : U & 0xfff) << 20;
      return;
    }
    E.Long = true;
    E.LongValue = U;
    return;
  }
  E.Long = true;
  E.LongExpr = MO.getExpr();
  E.LongKind = Kind;
  if (auto *S = dyn_cast<MCSpecifierExpr>(E.LongExpr);
      S && S->getSpecifier() == GPUVM::S_FNREL)
    E.LongKind = GPUVM::fixup_gvm_fnrel32;
}

void GPUVMMCCodeEmitter::encodeOp(const MCInst &MI, OpEncoding &E) const {
  uint64_t TSFlags = MCII.get(MI.getOpcode()).TSFlags;
  unsigned Opc = GPUVMII::getOpc(TSFlags);
  E = OpEncoding();
  auto R = [&](unsigned I) { return reg(MI.getOperand(I)); };
  auto Imm = [&](unsigned I) {
    return static_cast<uint32_t>(MI.getOperand(I).getImm());
  };
  switch (GPUVMII::getForm(TSFlags)) {
  case GPUVMII::FrmR:
    E.Word = GPUVM::wordR(Opc, R(0), R(1), R(2), 0);
    return;
  case GPUVMII::FrmR4:
    E.Word = GPUVM::wordR(Opc, R(0), R(1), R(2), R(3));
    return;
  case GPUVMII::FrmRSh:
  case GPUVMII::FrmLoadX:
  case GPUVMII::FrmStoreX:
    E.Word = GPUVM::wordR(Opc, R(0), R(1), R(2), Imm(3));
    return;
  case GPUVMII::FrmR1:
    E.Word = GPUVM::wordR(Opc, R(0), R(1), 0, 0);
    return;
  case GPUVMII::FrmI:
  case GPUVMII::FrmLoad:
  case GPUVMII::FrmStore:
    E.Word = GPUVM::wordI(Opc, R(0), R(1), 0);
    setImm(E, MI.getOperand(2), false, GPUVM::fixup_gvm_imm32);
    return;
  case GPUVMII::FrmFI:
    E.Word = GPUVM::wordI(Opc, R(0), R(1), 0);
    setImm(E, MI.getOperand(2), true, GPUVM::fixup_gvm_imm32);
    return;
  case GPUVMII::FrmPair: { // rc's words (+-128 bytes), else long
    E.Word = GPUVM::wordR(Opc, R(0), R(2), R(1), 0);
    int64_t V;
    if (constValue(MI.getOperand(3), V) && (V & 3) == 0 && V >= -128 &&
        V <= 124) {
      E.Word |= static_cast<uint32_t>((V >> 2) & 63) << 20;
      return;
    }
    const MCOperand &MO = MI.getOperand(3);
    E.Long = true;
    if (constValue(MO, V)) {
      E.LongValue = static_cast<uint32_t>(V);
      return;
    }
    E.LongExpr = MO.getExpr();
    E.LongKind = GPUVM::fixup_gvm_imm32;
    return;
  }
  case GPUVMII::FrmBF:
    E.Word = GPUVM::wordI(Opc, R(0), R(1),
                          (Imm(2) & 31) | ((Imm(3) - 1) & 31) << 5 |
                              (GPUVMII::isVariant(TSFlags) ? 0x400 : 0));
    return;
  case GPUVMII::FrmBFI:
    E.Word = GPUVM::wordI(Opc, R(0), R(2),
                          (Imm(3) & 31) | ((Imm(4) - 1) & 31) << 5);
    return;
  case GPUVMII::FrmAMO:
    E.Word = GPUVM::wordR(Opc, R(0), R(2), R(1), 0);
    return;
  case GPUVMII::FrmCAS:
    E.Word = GPUVM::wordR(Opc, R(0), R(3), R(2), 0);
    return;
  case GPUVMII::FrmLPC:
    E.Word = GPUVM::wordI(Opc, R(0), 0, 0);
    setImm(E, MI.getOperand(1), false, GPUVM::fixup_gvm_pcimm32);
    return;
  case GPUVMII::FrmNone:
    E.Word = GPUVM::wordR(Opc, 0, 0, 0, 0);
    return;
  case GPUVMII::FrmUnimp:
    E.Word = 0;
    return;
  case GPUVMII::FrmMFSR:
    E.Word = GPUVM::wordI(Opc, R(0), 0, Imm(1));
    return;
  case GPUVMII::FrmMTSR:
    E.Word = GPUVM::wordI(Opc, 0, R(1), Imm(0));
    return;
  case GPUVMII::FrmXSR:
    E.Word = GPUVM::wordI(Opc, R(0), R(2), Imm(1));
    return;
  default:
    llvm_unreachable("not an op");
  }
}

// A branch's target: a fixup, or a byte offset from the bundle (in bundles).
static int64_t target(const MCOperand &MO, unsigned Kind,
                      SmallVectorImpl<MCFixup> &Fixups) {
  if (MO.isImm())
    return MO.getImm() >> 4;
  Fixups.push_back(MCFixup::create(12, MO.getExpr(), Kind, /*PCRel=*/true));
  return 0;
}

uint32_t
GPUVMMCCodeEmitter::encodeControl(const MCInst &MI,
                                  SmallVectorImpl<MCFixup> &Fixups) const {
  uint64_t TSFlags = MCII.get(MI.getOpcode()).TSFlags;
  unsigned CC = GPUVMII::getCC(TSFlags), Sub = GPUVMII::getSub(TSFlags);
  auto R = [&](unsigned I) { return reg(MI.getOperand(I)); };
  auto Imm = [&](unsigned I) {
    return static_cast<uint32_t>(MI.getOperand(I).getImm());
  };
  auto Off9 = [](int64_t Off) {
    return (static_cast<uint32_t>(Off) & 0x1ff) << 17;
  };
  switch (GPUVMII::getForm(TSFlags)) {
  case GPUVMII::CtlBr:
    return CC << 7 | R(0) << 11 | R(1) << 26 |
           Off9(target(MI.getOperand(2), GPUVM::fixup_gvm_br9, Fixups));
  case GPUVMII::CtlBrI:
  case GPUVMII::CtlBit:
    return CC << 7 | R(0) << 11 | (Imm(1) & 63) << 26 |
           Off9(target(MI.getOperand(2), GPUVM::fixup_gvm_br9, Fixups));
  case GPUVMII::CtlAddBnz:
    return CC << 7 | R(1) << 11 | (Imm(2) & 63) << 26 |
           Off9(target(MI.getOperand(3), GPUVM::fixup_gvm_br9, Fixups));
  case GPUVMII::CtlJ: {
    int64_t Off = target(MI.getOperand(0), GPUVM::fixup_gvm_j, Fixups);
    if (isInt<9>(Off))
      return GPUVM::CtrlJ | Off9(Off);
    return GPUVM::ctrlSpecial(GPUVM::SubJ) | static_cast<uint32_t>(Off) << 14;
  }
  case GPUVMII::CtlCall: {
    int64_t Off = target(MI.getOperand(0), GPUVM::fixup_gvm_j18, Fixups);
    return GPUVM::ctrlSpecial(Sub) | static_cast<uint32_t>(Off) << 14;
  }
  case GPUVMII::CtlJR:
    return GPUVM::ctrlSpecial(Sub) | R(0) << 14;
  case GPUVMII::CtlNone:
    return GPUVM::ctrlSpecial(Sub);
  case GPUVMII::CtlBump:
    return GPUVM::ctrlSpecial(Sub) | R(1) << 14 | (Imm(2) & 0xfff) << 20;
  default:
    llvm_unreachable("not a control op");
  }
}

void GPUVMMCCodeEmitter::encodeBundle(ArrayRef<const MCInst *> Ops,
                                      const MCInst *Ctl,
                                      SmallVectorImpl<char> &CB,
                                      SmallVectorImpl<MCFixup> &Fixups) const {
  SmallVector<uint32_t, 2> Words;
  uint32_t Long = 0, LongBits = 0;
  int LongOf = -1;
  for (const MCInst *Op : Ops) {
    OpEncoding E;
    encodeOp(*Op, E);
    if (E.Long) {
      if (LongOf >= 0)
        Ctx.reportError(Op->getLoc(), "two long immediates in one bundle");
      LongOf = Words.size();
      Long = E.LongValue;
      if (E.LongExpr)
        Fixups.push_back(MCFixup::create(8, E.LongExpr, E.LongKind,
                                         E.LongKind ==
                                             GPUVM::fixup_gvm_pcimm32));
    }
    Words.push_back(E.Word);
  }
  if (Words.size() > 2)
    Ctx.reportError(Ops[2]->getLoc(), "three ops in one bundle");
  if (LongOf == 0)
    LongBits = 0x20;
  else if (LongOf == 1)
    LongBits = 0x40;
  uint32_t C = Ctl ? encodeControl(*Ctl, Fixups) : GPUVM::CtrlNone;
  unsigned Path = GPUVM::ctlCC(C) == GPUVM::CCSpecial
                      ? GPUVM::bundle::path12(C, Words)
                      : GPUVM::bundle::path(Words);
  C |= std::min<unsigned>(Words.size(), 2) << 3 | LongBits | Path;
  uint32_t Out[4] = {Words.size() > 0 ? Words[0] : 0,
                     Words.size() > 1 ? Words[1] : 0, Long, C};
  for (uint32_t W : Out)
    for (unsigned I = 0; I < 4; ++I)
      CB.push_back(static_cast<char>(W >> (8 * I)));
}

void GPUVMMCCodeEmitter::encodeInstruction(const MCInst &MI,
                                           SmallVectorImpl<char> &CB,
                                           SmallVectorImpl<MCFixup> &Fixups,
                                           const MCSubtargetInfo &STI) const {
  SmallVector<const MCInst *, 2> Ops;
  const MCInst *Ctl = nullptr;
  if (MI.getOpcode() == GPUVM::BUNDLE) {
    for (const MCOperand &MO : MI) {
      if (!MO.isInst())
        continue;
      const MCInst *Sub = MO.getInst();
      if (isControl(*Sub))
        Ctl = Sub;
      else
        Ops.push_back(Sub);
    }
  } else if (isControl(MI)) {
    Ctl = &MI;
  } else {
    Ops.push_back(&MI);
  }
  encodeBundle(Ops, Ctl, CB, Fixups);
}

MCCodeEmitter *llvm::createGPUVMMCCodeEmitter(const MCInstrInfo &MCII,
                                              MCContext &Ctx) {
  return new GPUVMMCCodeEmitter(Ctx, MCII);
}
