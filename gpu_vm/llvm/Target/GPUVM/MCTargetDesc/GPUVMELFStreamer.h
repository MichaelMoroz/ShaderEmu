//===-- GPUVMELFStreamer.h - gpu_vm bundling ELF streamer -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Gathers ops into bundles (gpu_vm/llvm/ASM.md, "Bundles"; the rules of
// gpu_vm/ref/gvm_asm.h) and emits each as one MCInst of GPUVM::BUNDLE.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUVM_MCTARGETDESC_GPUVMELFSTREAMER_H
#define LLVM_LIB_TARGET_GPUVM_MCTARGETDESC_GPUVMELFSTREAMER_H

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCELFStreamer.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"

namespace llvm {
class formatted_raw_ostream;
class MCInstPrinter;

class GPUVMELFStreamer : public MCELFStreamer {
  // The open bundle.
  SmallVector<MCInst, 2> Ops;
  SmallVector<uint32_t, 2> Words; // their op words, for the rules
  bool HasLong = false;
  bool HasBump = false; // a pending control-word add (bump)
  MCInst Bump;
  const MCSubtargetInfo *LastSTI = nullptr;
  bool Closing = false;
  // Sections where data was emitted since the last bundle: the next bundle
  // is aligned to 16 first.
  DenseSet<const MCSection *> Unaligned;

  void putOp(const MCInst &Inst, const MCSubtargetInfo &STI);
  void putControl(const MCInst &Inst, const MCSubtargetInfo &STI);
  void emitBundle(ArrayRef<MCInst> BOps, const MCInst *Ctl);
  void markData();

public:
  GPUVMELFStreamer(MCContext &C, std::unique_ptr<MCAsmBackend> TAB,
                   std::unique_ptr<MCObjectWriter> OW,
                   std::unique_ptr<MCCodeEmitter> Emitter)
      : MCELFStreamer(C, std::move(TAB), std::move(OW), std::move(Emitter)) {}

  // Closes the open bundle (`;;`, `.endbundle`, and every flush point).
  void closeBundle();

  void emitInstruction(const MCInst &Inst, const MCSubtargetInfo &STI) override;
  void emitLabel(MCSymbol *Symbol, SMLoc Loc = SMLoc()) override;
  void changeSection(MCSection *Section, uint32_t Subsection = 0) override;
  void emitBytes(StringRef Data) override;
  void emitValueImpl(const MCExpr *Value, unsigned Size,
                     SMLoc Loc = SMLoc()) override;
  void emitULEB128Value(const MCExpr *Value) override;
  void emitSLEB128Value(const MCExpr *Value) override;
  void emitFill(const MCExpr &NumBytes, uint64_t FillValue,
                SMLoc Loc = SMLoc()) override;
  void emitFill(const MCExpr &NumValues, int64_t Size, int64_t Expr,
                SMLoc Loc = SMLoc()) override;
  void emitValueToAlignment(Align Alignment, int64_t Fill = 0,
                            uint8_t FillLen = 1,
                            unsigned MaxBytesToEmit = 0) override;
  void emitCodeAlignment(Align ByteAlignment, const MCSubtargetInfo &STI,
                         unsigned MaxBytesToEmit = 0) override;
  void emitValueToOffset(const MCExpr *Offset, unsigned char Value,
                         SMLoc Loc) override;
};

class GPUVMTargetStreamer : public MCTargetStreamer {
public:
  GPUVMTargetStreamer(MCStreamer &S) : MCTargetStreamer(S) {}
  virtual void emitBundleEnd() {}
};

class GPUVMTargetELFStreamer : public GPUVMTargetStreamer {
public:
  GPUVMTargetELFStreamer(MCStreamer &S) : GPUVMTargetStreamer(S) {}
  void emitBundleEnd() override;
  void finish() override; // the last bundle
};

class GPUVMTargetAsmStreamer : public GPUVMTargetStreamer {
  formatted_raw_ostream &OS;

public:
  GPUVMTargetAsmStreamer(MCStreamer &S, formatted_raw_ostream &OS)
      : GPUVMTargetStreamer(S), OS(OS) {}
  void emitBundleEnd() override;
};

MCStreamer *createGPUVMELFStreamer(const Triple &T, MCContext &Ctx,
                                   std::unique_ptr<MCAsmBackend> &&TAB,
                                   std::unique_ptr<MCObjectWriter> &&OW,
                                   std::unique_ptr<MCCodeEmitter> &&Emitter);
MCTargetStreamer *createGPUVMObjectTargetStreamer(MCStreamer &S,
                                                  const MCSubtargetInfo &STI);
MCTargetStreamer *createGPUVMAsmTargetStreamer(MCStreamer &S,
                                               formatted_raw_ostream &OS,
                                               MCInstPrinter *InstPrint);
} // namespace llvm

#endif
