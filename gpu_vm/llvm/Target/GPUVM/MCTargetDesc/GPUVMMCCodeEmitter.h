//===-- GPUVMMCCodeEmitter.h - gpu_vm code emitter --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Encodes a bundle: an MCInst of GPUVM::BUNDLE whose operands are an
// immediate (the disassembler's path note, -1 when made by the streamer) and
// up to two ops and one control op (MCOperand::createInst), in that order. A
// lone op or control op is encoded as a bundle of its own.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUVM_MCTARGETDESC_GPUVMMCCODEEMITTER_H
#define LLVM_LIB_TARGET_GPUVM_MCTARGETDESC_GPUVMMCCODEEMITTER_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/MC/MCCodeEmitter.h"
#include <cstdint>

namespace llvm {
class MCContext;
class MCExpr;
class MCFixup;
class MCInst;
class MCInstrInfo;
class MCOperand;

class GPUVMMCCodeEmitter : public MCCodeEmitter {
  MCContext &Ctx;
  const MCInstrInfo &MCII;

public:
  // One op: its word, and its long immediate if it takes the bundle's.
  struct OpEncoding {
    uint32_t Word = 0;
    bool Long = false;
    uint32_t LongValue = 0;
    const MCExpr *LongExpr = nullptr; // a fixup at word 2
    unsigned LongKind = 0;
  };

  GPUVMMCCodeEmitter(MCContext &Ctx, const MCInstrInfo &MCII)
      : Ctx(Ctx), MCII(MCII) {}

  void encodeInstruction(const MCInst &MI, SmallVectorImpl<char> &CB,
                         SmallVectorImpl<MCFixup> &Fixups,
                         const MCSubtargetInfo &STI) const override;

  uint64_t tsFlags(const MCInst &MI) const;
  bool isControl(const MCInst &MI) const;
  void encodeOp(const MCInst &MI, OpEncoding &E) const;
  // The control word but for n, the long bits and the path; fixups at 12.
  uint32_t encodeControl(const MCInst &MI,
                         SmallVectorImpl<MCFixup> &Fixups) const;
  // The 16 bytes of a bundle of these ops and control op (or none).
  void encodeBundle(ArrayRef<const MCInst *> Ops, const MCInst *Ctl,
                    SmallVectorImpl<char> &CB,
                    SmallVectorImpl<MCFixup> &Fixups) const;

  unsigned reg(const MCOperand &MO) const;
};
} // namespace llvm

#endif
