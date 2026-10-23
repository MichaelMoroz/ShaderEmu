//===-- GPUVMELFStreamer.cpp - gpu_vm bundling ELF streamer ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Each MCInst is one op or one control op. Ops go into the open bundle in
// program order; the bundle is closed (gvm_asm.h's rules) when it has two ops,
// when the next op cannot be op 1 (bundle::fits1), when the next op wants the
// long immediate and it is taken, at a control op (after it), and at every
// label, section change, alignment and datum. The control word acts after the
// ops on the registers as the bundle found them: a branch whose compared
// register an op of the bundle writes goes into a bundle of its own, so that
// every control op keeps program order. On a machine with late branches
// (-gpuvm-late-br) a branch reads them as the ops left them, and joins.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/GPUVMELFStreamer.h"
#include "MCTargetDesc/GPUVMBaseInfo.h"
#include "MCTargetDesc/GPUVMMCCodeEmitter.h"
#include "MCTargetDesc/GPUVMMCTargetDesc.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FormattedStream.h"

using namespace llvm;

static cl::opt<bool>
    NoPack("gpuvm-no-pack", cl::Hidden, cl::init(false),
           cl::desc("gpu_vm: one op a bundle (for debugging)"));

static cl::opt<bool>
    LateBr("gpuvm-late-br", cl::Hidden, cl::init(false),
           cl::desc("gpu_vm: branches read their registers after the bundle's "
                    "ops (a machine with gvm_machine.late)"));

bool llvm::GPUVM::lateBranches() { return LateBr; }

static GPUVMMCCodeEmitter &emitter(MCAssembler &A) {
  return static_cast<GPUVMMCCodeEmitter &>(A.getEmitter());
}

void GPUVMELFStreamer::emitInstruction(const MCInst &Inst,
                                       const MCSubtargetInfo &STI) {
  LastSTI = &STI;
  if (Inst.getOpcode() == GPUVM::BUNDLE) {
    closeBundle();
    MCELFStreamer::emitInstruction(Inst, STI);
    return;
  }
  if (emitter(getAssembler()).isControl(Inst))
    putControl(Inst, STI);
  else
    putOp(Inst, STI);
}

void GPUVMELFStreamer::putOp(const MCInst &Inst, const MCSubtargetInfo &STI) {
  // A constant expression becomes an immediate now, so that the encoder makes
  // the same choice of short or long as here.
  MCInst I = Inst;
  for (MCOperand &MO : I) {
    int64_t V;
    if (MO.isExpr() && !isa<MCSpecifierExpr>(MO.getExpr()) &&
        MO.getExpr()->evaluateAsAbsolute(V))
      MO = MCOperand::createImm(V);
  }
  GPUVMMCCodeEmitter::OpEncoding E;
  emitter(getAssembler()).encodeOp(I, E);
  using namespace GPUVM::bundle;
  if (NoPack && !Ops.empty())
    closeBundle();
  if (HasBump &&
      touches(E.Word, emitter(getAssembler()).reg(Bump.getOperand(1))))
    closeBundle();
  if (Ops.size() == 2 || (!Ops.empty() && !fits1(Words[0], E.Word)) ||
      (E.Long && HasLong))
    closeBundle();
  Ops.push_back(I);
  Words.push_back(E.Word);
  HasLong |= E.Long;
}

void GPUVMELFStreamer::putControl(const MCInst &Inst,
                                  const MCSubtargetInfo &STI) {
  GPUVMMCCodeEmitter &CE = emitter(getAssembler());
  GPUVMII::Form F = GPUVMII::getForm(CE.tsFlags(Inst));
  using namespace GPUVM::bundle;
  if (F == GPUVMII::CtlBump) {
    // rs += imm12 after the ops: an op before it that writes rs is undefined
    // (the add wins), so it opens a bundle.
    if (HasBump || writes(Words, CE.reg(Inst.getOperand(1))))
      closeBundle();
    HasBump = true;
    Bump = Inst;
    return;
  }
  if (HasBump)
    closeBundle();
  // The registers the control word reads as the bundle found them.
  SmallVector<unsigned, 2> Read;
  switch (F) {
  case GPUVMII::CtlBr:
    Read.push_back(CE.reg(Inst.getOperand(0)));
    Read.push_back(CE.reg(Inst.getOperand(1)));
    break;
  case GPUVMII::CtlBrI:
  case GPUVMII::CtlBit:
    Read.push_back(CE.reg(Inst.getOperand(0)));
    break;
  case GPUVMII::CtlAddBnz:
    Read.push_back(CE.reg(Inst.getOperand(1)));
    break;
  default:
    break; // jr and callr read after the ops
  }
  // (late branches: a branch reads them as the ops left them, program order)
  if (LateBr)
    Read.clear();
  for (unsigned R : Read)
    if (R != 0 && writes(Words, R)) {
      closeBundle();
      break;
    }
  SmallVector<MCInst, 2> BOps(Ops.begin(), Ops.end());
  Ops.clear();
  Words.clear();
  HasLong = false;
  emitBundle(BOps, &Inst);
}

void GPUVMELFStreamer::closeBundle() {
  if (Closing || (Ops.empty() && !HasBump))
    return;
  SmallVector<MCInst, 2> BOps(Ops.begin(), Ops.end());
  MCInst Ctl = Bump;
  bool WithCtl = HasBump;
  Ops.clear();
  Words.clear();
  HasLong = false;
  HasBump = false;
  emitBundle(BOps, WithCtl ? &Ctl : nullptr);
}

void GPUVMELFStreamer::emitBundle(ArrayRef<MCInst> BOps, const MCInst *Ctl) {
  assert(LastSTI && "a bundle with no instruction");
  Closing = true;
  MCSection *Sec = getCurrentSectionOnly();
  if (Unaligned.erase(Sec))
    MCObjectStreamer::emitCodeAlignment(Align(GPUVM::BundleSize), *LastSTI);
  Sec->ensureMinAlignment(Align(GPUVM::BundleSize));
  MCInst B;
  B.setOpcode(GPUVM::BUNDLE);
  B.addOperand(MCOperand::createImm(-1));
  auto Add = [&](const MCInst &I) {
    MCInst *P = getContext().createMCInst();
    *P = I;
    B.addOperand(MCOperand::createInst(P));
    for (const MCOperand &MO : I)
      if (MO.isExpr())
        visitUsedExpr(*MO.getExpr());
  };
  for (const MCInst &I : BOps)
    Add(I);
  if (Ctl)
    Add(*Ctl);
  MCELFStreamer::emitInstruction(B, *LastSTI);
  for (MCSymbol *L : PendingLabels)
    MCELFStreamer::emitLabel(L);
  PendingLabels.clear();
  Closing = false;
}

void GPUVMELFStreamer::emitDwarfLocDirective(unsigned FileNo, unsigned Line,
                                             unsigned Column, unsigned Flags,
                                             unsigned Isa,
                                             unsigned Discriminator,
                                             StringRef FileName,
                                             StringRef Comment) {
  // With a bundle open, the line entry of the .loc before would be a label
  // inside it: only the latest .loc is kept, and the bundle's emission makes
  // its entry.
  if (!Ops.empty() || HasBump) {
    MCStreamer::emitDwarfLocDirective(FileNo, Line, Column, Flags, Isa,
                                      Discriminator, FileName, Comment);
    return;
  }
  MCELFStreamer::emitDwarfLocDirective(FileNo, Line, Column, Flags, Isa,
                                       Discriminator, FileName, Comment);
}

MCSymbol *GPUVMELFStreamer::emitCFILabel() {
  MCSymbol *L = getContext().createTempSymbol("cfi");
  if (Ops.empty() && !HasBump)
    MCELFStreamer::emitLabel(L);
  else
    PendingLabels.push_back(L);
  return L;
}

void GPUVMELFStreamer::markData() {
  if (MCSection *Sec = getCurrentSectionOnly())
    Unaligned.insert(Sec);
}

void GPUVMELFStreamer::emitLabel(MCSymbol *Symbol, SMLoc Loc) {
  if (SoftLabels && !Closing && (!Ops.empty() || HasBump)) {
    PendingLabels.push_back(Symbol);
    return;
  }
  closeBundle();
  MCELFStreamer::emitLabel(Symbol, Loc);
}

void GPUVMELFStreamer::changeSection(MCSection *Section, uint32_t Subsection) {
  closeBundle();
  MCELFStreamer::changeSection(Section, Subsection);
}

void GPUVMELFStreamer::emitBytes(StringRef Data) {
  closeBundle();
  markData();
  MCELFStreamer::emitBytes(Data);
}

void GPUVMELFStreamer::emitValueImpl(const MCExpr *Value, unsigned Size,
                                     SMLoc Loc) {
  closeBundle();
  markData();
  MCELFStreamer::emitValueImpl(Value, Size, Loc);
}

void GPUVMELFStreamer::emitULEB128Value(const MCExpr *Value) {
  closeBundle();
  markData();
  MCELFStreamer::emitULEB128Value(Value);
}

void GPUVMELFStreamer::emitSLEB128Value(const MCExpr *Value) {
  closeBundle();
  markData();
  MCELFStreamer::emitSLEB128Value(Value);
}

void GPUVMELFStreamer::emitFill(const MCExpr &NumBytes, uint64_t FillValue,
                                SMLoc Loc) {
  closeBundle();
  markData();
  MCELFStreamer::emitFill(NumBytes, FillValue, Loc);
}

void GPUVMELFStreamer::emitFill(const MCExpr &NumValues, int64_t Size,
                                int64_t Expr, SMLoc Loc) {
  closeBundle();
  markData();
  MCELFStreamer::emitFill(NumValues, Size, Expr, Loc);
}

void GPUVMELFStreamer::emitValueToAlignment(Align Alignment, int64_t Fill,
                                            uint8_t FillLen,
                                            unsigned MaxBytesToEmit) {
  closeBundle();
  MCELFStreamer::emitValueToAlignment(Alignment, Fill, FillLen,
                                      MaxBytesToEmit);
  if (Alignment >= Align(GPUVM::BundleSize) && !MaxBytesToEmit)
    Unaligned.erase(getCurrentSectionOnly());
}

void GPUVMELFStreamer::emitCodeAlignment(Align Alignment,
                                         const MCSubtargetInfo &STI,
                                         unsigned MaxBytesToEmit) {
  closeBundle();
  MCELFStreamer::emitCodeAlignment(Alignment, STI, MaxBytesToEmit);
  if (Alignment >= Align(GPUVM::BundleSize) && !MaxBytesToEmit)
    Unaligned.erase(getCurrentSectionOnly());
}

void GPUVMELFStreamer::emitValueToOffset(const MCExpr *Offset,
                                         unsigned char Value, SMLoc Loc) {
  closeBundle();
  markData();
  MCELFStreamer::emitValueToOffset(Offset, Value, Loc);
}

void GPUVMTargetELFStreamer::finish() {
  static_cast<GPUVMELFStreamer &>(getStreamer()).closeBundle();
}

void GPUVMTargetELFStreamer::emitBundleEnd() {
  static_cast<GPUVMELFStreamer &>(getStreamer()).closeBundle();
}

void GPUVMTargetELFStreamer::setSoftLabels(bool Soft) {
  static_cast<GPUVMELFStreamer &>(getStreamer()).setSoftLabels(Soft);
}

void GPUVMTargetAsmStreamer::emitBundleEnd() { OS << "\t.endbundle\n"; }

MCStreamer *llvm::createGPUVMELFStreamer(
    const Triple &T, MCContext &Ctx, std::unique_ptr<MCAsmBackend> &&TAB,
    std::unique_ptr<MCObjectWriter> &&OW,
    std::unique_ptr<MCCodeEmitter> &&Emitter) {
  return new GPUVMELFStreamer(Ctx, std::move(TAB), std::move(OW),
                              std::move(Emitter));
}

MCTargetStreamer *
llvm::createGPUVMObjectTargetStreamer(MCStreamer &S,
                                      const MCSubtargetInfo &STI) {
  return new GPUVMTargetELFStreamer(S);
}

MCTargetStreamer *llvm::createGPUVMAsmTargetStreamer(MCStreamer &S,
                                                     formatted_raw_ostream &OS,
                                                     MCInstPrinter *InstPrint) {
  return new GPUVMTargetAsmStreamer(S, OS);
}
