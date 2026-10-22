//===-- GPUVMDisassembler.cpp - Disassembler for gpu_vm -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Decodes a 16-byte bundle into one MCInst of GPUVM::BUNDLE: an
// immediate (the bundle's path bits when they are not what the assembler
// makes, else -1), its ops and its control op. The opcodes come from the
// instructions' TSFlags (GPUVMInstrFormats.td), not from TableGen tables.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/GPUVMBaseInfo.h"
#include "MCTargetDesc/GPUVMMCTargetDesc.h"
#include "TargetInfo/GPUVMTargetInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDecoderOps.h"
#include "llvm/MC/MCDisassembler/MCDisassembler.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Endian.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-disassembler"

typedef MCDisassembler::DecodeStatus DecodeStatus;

namespace {
class GPUVMDisassembler : public MCDisassembler {
  std::unique_ptr<const MCInstrInfo> MCII;
  unsigned OpOf[256] = {};  // an op's MC opcode by its opcode byte (0: none)
  unsigned BFESOpcode = 0;  // bfe's signed twin (the same opcode byte)
  MCRegister RegOf[64];

  bool decodeOp(MCInst &MI, uint32_t W, bool IsLong, uint32_t Long) const;
  // False for an illegal control word; Ctl's opcode 0 when it only falls
  // through.
  bool decodeControl(MCInst &MI, uint32_t C, bool &None) const;

public:
  GPUVMDisassembler(const MCSubtargetInfo &STI, MCContext &Ctx,
                    const MCInstrInfo *MII)
      : MCDisassembler(STI, Ctx), MCII(MII) {
    for (unsigned I = 0, E = MII->getNumOpcodes(); I < E; ++I) {
      uint64_t TS = MII->get(I).TSFlags;
      GPUVMII::Form F = GPUVMII::getForm(TS);
      if (F == GPUVMII::FrmPseudo || GPUVMII::isControl(TS))
        continue;
      if (GPUVMII::isVariant(TS))
        BFESOpcode = I;
      else
        OpOf[GPUVMII::getOpc(TS)] = I;
    }
    const MCRegisterInfo *MRI = Ctx.getRegisterInfo();
    for (unsigned R = 1, E = MRI->getNumRegs(); R < E; ++R)
      RegOf[MRI->getEncodingValue(R) & 63] = R;
  }

  DecodeStatus getInstruction(MCInst &Instr, uint64_t &Size,
                              ArrayRef<uint8_t> Bytes, uint64_t Address,
                              raw_ostream &CStream) const override;
};
} // namespace

bool GPUVMDisassembler::decodeOp(MCInst &MI, uint32_t W, bool IsLong,
                                 uint32_t Long) const {
  using namespace GPUVM;
  unsigned Opc = opOpc(W);
  unsigned Op = OpOf[Opc];
  if (!Op)
    return false;
  uint64_t TS = MCII->get(Op).TSFlags;
  int64_t Imm = IsLong ? static_cast<int32_t>(Long) : opImm12(W);
  auto Reg = [&](unsigned N) { MI.addOperand(MCOperand::createReg(RegOf[N])); };
  auto ImmOp = [&](int64_t V) { MI.addOperand(MCOperand::createImm(V)); };
  MI.setOpcode(Op);
  switch (GPUVMII::getForm(TS)) {
  case GPUVMII::FrmR:
    Reg(opRD(W)), Reg(opRA(W)), Reg(opRB(W));
    return true;
  case GPUVMII::FrmR4:
    Reg(opRD(W)), Reg(opRA(W)), Reg(opRB(W)), Reg(opRC(W));
    return true;
  case GPUVMII::FrmRSh:
  case GPUVMII::FrmLoadX:
  case GPUVMII::FrmStoreX:
    Reg(opRD(W)), Reg(opRA(W)), Reg(opRB(W)), ImmOp(opRC(W) & 31);
    return true;
  case GPUVMII::FrmR1:
    Reg(opRD(W)), Reg(opRA(W));
    return true;
  case GPUVMII::FrmI:
  case GPUVMII::FrmLoad:
  case GPUVMII::FrmStore:
    Reg(opRD(W)), Reg(opRA(W)), ImmOp(Imm);
    return true;
  case GPUVMII::FrmFI:
    Reg(opRD(W)), Reg(opRA(W));
    ImmOp(IsLong ? Long : (W >> 20) << 20);
    return true;
  case GPUVMII::FrmBF:
    if ((Imm >> 10) & 1)
      MI.setOpcode(BFESOpcode);
    Reg(opRD(W)), Reg(opRA(W)), ImmOp(Imm & 31), ImmOp(((Imm >> 5) & 31) + 1);
    return true;
  case GPUVMII::FrmBFI:
    Reg(opRD(W)), Reg(opRD(W)), Reg(opRA(W)), ImmOp(Imm & 31),
        ImmOp(((Imm >> 5) & 31) + 1);
    return true;
  case GPUVMII::FrmAMO:
    Reg(opRD(W)), Reg(opRB(W)), Reg(opRA(W));
    return true;
  case GPUVMII::FrmCAS:
    Reg(opRD(W)), Reg(opRD(W)), Reg(opRB(W)), Reg(opRA(W));
    return true;
  case GPUVMII::FrmLPC:
    Reg(opRD(W)), ImmOp(Imm);
    return true;
  case GPUVMII::FrmNone:
  case GPUVMII::FrmUnimp:
    return true;
  case GPUVMII::FrmMFSR:
    Reg(opRD(W)), ImmOp(Imm & 0xfff);
    return true;
  case GPUVMII::FrmMTSR:
    ImmOp(Imm & 0xfff), Reg(opRA(W));
    return true;
  case GPUVMII::FrmXSR:
    Reg(opRD(W)), ImmOp(Imm & 0xfff), Reg(opRA(W));
    return true;
  default:
    return false;
  }
}

bool GPUVMDisassembler::decodeControl(MCInst &MI, uint32_t C,
                                      bool &None) const {
  using namespace GPUVM;
  None = false;
  auto Reg = [&](unsigned N) { MI.addOperand(MCOperand::createReg(RegOf[N])); };
  auto ImmOp = [&](int64_t V) { MI.addOperand(MCOperand::createImm(V)); };
  unsigned CC = ctlCC(C);
  if (CC == CCSpecial) {
    switch (ctlSub(C)) {
    case SubNone:
      None = true;
      return true;
    case SubJR:
      MI.setOpcode(GPUVM::JR);
      Reg(ctlRS(C));
      return true;
    case SubCallR:
      MI.setOpcode(GPUVM::CALLR);
      Reg(ctlRS(C));
      return true;
    case SubEret:
      MI.setOpcode(GPUVM::ERET);
      return true;
    case SubJ:
      MI.setOpcode(GPUVM::J);
      ImmOp(static_cast<int64_t>(ctlOff18(C)) * 16);
      return true;
    case SubCall:
      MI.setOpcode(GPUVM::CALL);
      ImmOp(static_cast<int64_t>(ctlOff18(C)) * 16);
      return true;
    case SubAdd:
      MI.setOpcode(GPUVM::BUMP);
      Reg(ctlRS(C)), Reg(ctlRS(C)), ImmOp(ctlImm12(C));
      return true;
    default:
      return false;
    }
  }
  int64_t Off = static_cast<int64_t>(ctlOff9(C)) * 16;
  unsigned RA = ctlRA(C), RB = ctlRB(C);
  if (CC == CCBNe && RA == 0 && RB == 0 && Off == 0) {
    None = true;
    return true;
  }
  if (CC == CCBEq && RA == 0 && RB == 0) {
    MI.setOpcode(GPUVM::J);
    ImmOp(Off);
    return true;
  }
  static const unsigned Br[16] = {
      0,           GPUVM::ADDBNZ, GPUVM::BLTU,  GPUVM::BGEU,
      GPUVM::BEQ,  GPUVM::BNE,    GPUVM::BLT,   GPUVM::BGE,
      GPUVM::BBS,  GPUVM::BBC,    GPUVM::BLTUI, GPUVM::BGEUI,
      GPUVM::BEQI, GPUVM::BNEI,   GPUVM::BLTI,  GPUVM::BGEI};
  MI.setOpcode(Br[CC]);
  if (CC == CCAddBnz) {
    Reg(RA), Reg(RA), ImmOp(ctlImm6(C));
  } else if (CC <= 7) {
    Reg(RA), Reg(RB);
  } else if (CC <= 9) {
    Reg(RA), ImmOp(RB & 31);
  } else {
    Reg(RA), ImmOp(ctlImm6(C));
  }
  ImmOp(Off);
  return true;
}

DecodeStatus GPUVMDisassembler::getInstruction(MCInst &MI, uint64_t &Size,
                                               ArrayRef<uint8_t> Bytes,
                                               uint64_t Address,
                                               raw_ostream &CS) const {
  using namespace GPUVM;
  if (Bytes.size() < BundleSize) {
    Size = Bytes.size();
    return MCDisassembler::Fail;
  }
  Size = BundleSize;
  uint32_t T[4];
  for (unsigned I = 0; I < 4; ++I)
    T[I] = support::endian::read32le(Bytes.data() + 4 * I);
  uint32_t C = T[3];
  unsigned N = ctlN(C);
  if (N == 3 || (ctlLong0(C) && ctlLong1(C)))
    return MCDisassembler::Fail;
  MI.clear();
  MI.setOpcode(GPUVM::BUNDLE);
  MI.addOperand(MCOperand::createImm(-1));
  for (unsigned S = 0; S < N; ++S) {
    MCInst *Op = getContext().createMCInst();
    bool IsLong = S == 0 ? ctlLong0(C) : ctlLong1(C);
    if (!decodeOp(*Op, T[S], IsLong, T[2]))
      return MCDisassembler::Fail;
    MI.addOperand(MCOperand::createInst(Op));
  }
  MCInst *Ctl = getContext().createMCInst();
  bool None;
  if (!decodeControl(*Ctl, C, None))
    return MCDisassembler::Fail;
  if (!None)
    MI.addOperand(MCOperand::createInst(Ctl));
  unsigned Want = ctlCC(C) == CCSpecial ? PathGeneral
                                        : bundle::path(ArrayRef(T, N));
  if (ctlPath(C) != Want)
    MI.getOperand(0).setImm(ctlPath(C));
  return MCDisassembler::Success;
}

static MCDisassembler *createGPUVMDisassembler(const Target &T,
                                               const MCSubtargetInfo &STI,
                                               MCContext &Ctx) {
  return new GPUVMDisassembler(STI, Ctx, T.createMCInstrInfo());
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeGPUVMDisassembler() {
  TargetRegistry::RegisterMCDisassembler(getTheGPUVMTarget(),
                                         createGPUVMDisassembler);
}
