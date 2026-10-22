//===-- GPUVMELFObjectWriter.cpp - gpu_vm ELF writer ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/GPUVMFixupKinds.h"
#include "MCTargetDesc/GPUVMMCTargetDesc.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSymbolELF.h"
#include "llvm/MC/MCValue.h"

using namespace llvm;

namespace {
class GPUVMELFObjectWriter : public MCELFObjectTargetWriter {
public:
  explicit GPUVMELFObjectWriter(uint8_t OSABI)
      : MCELFObjectTargetWriter(/*Is64Bit=*/false, OSABI, ELF::EM_GVM,
                                /*HasRelocationAddend=*/true) {}

protected:
  unsigned getRelocType(const MCFixup &Fixup, const MCValue &Target,
                        bool IsPCRel) const override;
};
} // namespace

unsigned GPUVMELFObjectWriter::getRelocType(const MCFixup &Fixup,
                                            const MCValue &Target,
                                            bool IsPCRel) const {
  MCFixupKind Kind = Fixup.getKind();
  if (mc::isRelocation(Kind))
    return Kind;
  if (Target.getSpecifier() == GPUVM::S_TPREL) {
    if (auto *SA = const_cast<MCSymbol *>(Target.getAddSym()))
      static_cast<MCSymbolELF *>(SA)->setType(ELF::STT_TLS);
    if (Kind != GPUVM::fixup_gvm_imm32)
      reportError(Fixup.getLoc(), "%tprel only in a long immediate");
    return ELF::R_GVM_TPREL32;
  }
  switch (Kind) {
  case FK_NONE:
    return ELF::R_GVM_NONE;
  case FK_Data_4:
    return IsPCRel ? ELF::R_GVM_REL32 : ELF::R_GVM_32;
  case GPUVM::fixup_gvm_imm32:
    return ELF::R_GVM_IMM32;
  case GPUVM::fixup_gvm_pcimm32:
    return ELF::R_GVM_PCIMM32;
  case GPUVM::fixup_gvm_br9:
    return ELF::R_GVM_BR9;
  case GPUVM::fixup_gvm_j:
  case GPUVM::fixup_gvm_j18:
    return ELF::R_GVM_J18;
  case GPUVM::fixup_gvm_imm12:
    return ELF::R_GVM_IMM12;
  default:
    reportError(Fixup.getLoc(), "unsupported relocation");
    return ELF::R_GVM_NONE;
  }
}

std::unique_ptr<MCObjectTargetWriter>
llvm::createGPUVMELFObjectWriter(uint8_t OSABI) {
  return std::make_unique<GPUVMELFObjectWriter>(OSABI);
}
