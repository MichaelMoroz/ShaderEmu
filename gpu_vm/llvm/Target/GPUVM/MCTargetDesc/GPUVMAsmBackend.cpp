//===-- GPUVMAsmBackend.cpp - gpu_vm assembler backend --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/GPUVMBaseInfo.h"
#include "MCTargetDesc/GPUVMFixupKinds.h"
#include "MCTargetDesc/GPUVMMCTargetDesc.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCELFObjectWriter.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/ELF.h"

using namespace llvm;
using namespace llvm::support::endian;

namespace {
class GPUVMAsmBackend : public MCAsmBackend {
  uint8_t OSABI;

public:
  GPUVMAsmBackend(uint8_t OSABI)
      : MCAsmBackend(llvm::endianness::little), OSABI(OSABI) {}

  void applyFixup(const MCFragment &F, const MCFixup &Fixup,
                  const MCValue &Target, uint8_t *Data, uint64_t Value,
                  bool IsResolved) override;

  std::unique_ptr<MCObjectTargetWriter>
  createObjectTargetWriter() const override {
    return createGPUVMELFObjectWriter(OSABI);
  }

  MCFixupKindInfo getFixupKindInfo(MCFixupKind Kind) const override;

  // .reloc's names: the ELF relocations, and the BFD ones the code generator
  // asks for (a profile-guided build's call graph section: BFD_RELOC_NONE)
  std::optional<MCFixupKind> getFixupKind(StringRef Name) const override {
    unsigned Type = StringSwitch<unsigned>(Name)
#define ELF_RELOC(NAME, ID) .Case(#NAME, ID)
#include "llvm/BinaryFormat/ELFRelocs/GPUVM.def"
#undef ELF_RELOC
                        .Case("BFD_RELOC_NONE", ELF::R_GVM_NONE)
                        .Case("BFD_RELOC_32", ELF::R_GVM_32)
                        .Default(-1u);
    if (Type == -1u)
      return std::nullopt;
    return static_cast<MCFixupKind>(FirstLiteralRelocationKind + Type);
  }

  bool writeNopData(raw_ostream &OS, uint64_t Count,
                    const MCSubtargetInfo *STI) const override;
};
} // namespace

MCFixupKindInfo GPUVMAsmBackend::getFixupKindInfo(MCFixupKind Kind) const {
  // The offset and size are of the field in its word (the fixup's offset is
  // the word's); they are for show.
  static const MCFixupKindInfo Infos[GPUVM::NumTargetFixupKinds] = {
      {"fixup_gvm_imm32", 0, 32, 0},  {"fixup_gvm_pcimm32", 0, 32, 0},
      {"fixup_gvm_br9", 17, 9, 0},    {"fixup_gvm_j", 14, 18, 0},
      {"fixup_gvm_j18", 14, 18, 0},   {"fixup_gvm_imm12", 20, 12, 0},
      {"fixup_gvm_fnrel32", 0, 32, 0},
  };
  if (Kind < FirstTargetFixupKind)
    return MCAsmBackend::getFixupKindInfo(Kind);
  assert(unsigned(Kind - FirstTargetFixupKind) < GPUVM::NumTargetFixupKinds &&
         "Invalid kind!");
  return Infos[Kind - FirstTargetFixupKind];
}

// An empty bundle that falls through.
bool GPUVMAsmBackend::writeNopData(raw_ostream &OS, uint64_t Count,
                                   const MCSubtargetInfo *STI) const {
  OS.write_zeros(Count % GPUVM::BundleSize);
  for (uint64_t I = 0; I < Count / GPUVM::BundleSize; ++I) {
    char B[16] = {};
    write32le(B + 12, GPUVM::CtrlNone | GPUVM::PathInt);
    OS.write(B, 16);
  }
  return true;
}

void GPUVMAsmBackend::applyFixup(const MCFragment &F, const MCFixup &Fixup,
                                 const MCValue &Target, uint8_t *Data,
                                 uint64_t Value, bool IsResolved) {
  MCFixupKind Kind = Fixup.getKind();
  // %fnrel's relocation is R_GVM_PCIMM32 from word 2, which lld counts from
  // the bundle: 8 less in the addend gives S + A - fn.
  if (Kind == GPUVM::fixup_gvm_fnrel32)
    maybeAddReloc(F, Fixup,
                  MCValue::get(Target.getAddSym(), Target.getSubSym(),
                               Target.getConstant() - 8,
                               Target.getSpecifier()),
                  Value, IsResolved);
  else
    maybeAddReloc(F, Fixup, Target, Value, IsResolved);
  if (mc::isRelocation(Kind))
    return;
  MCContext &Ctx = getContext();
  int64_t SV = static_cast<int64_t>(Value);
  // A branch's offset in bundles, from the bundle (the control word is its
  // last word).
  auto Bundles = [&](int Bits) -> int64_t {
    int64_t Off = SV + 12;
    if (Off & 15)
      Ctx.reportError(Fixup.getLoc(), "branch target not a bundle's start");
    Off >>= 4;
    if (!isIntN(Bits, Off))
      Ctx.reportError(Fixup.getLoc(), "branch target out of range");
    return Off;
  };
  switch (Kind) {
  case FK_Data_1:
    if (IsResolved)
      *Data = Value;
    return;
  case FK_Data_2:
    if (IsResolved)
      write16le(Data, Value);
    return;
  case FK_Data_4:
    if (IsResolved)
      write32le(Data, Value);
    return;
  case FK_Data_8:
    if (IsResolved)
      write64le(Data, Value);
    return;
  case GPUVM::fixup_gvm_imm32:
  case GPUVM::fixup_gvm_fnrel32:
    if (IsResolved)
      write32le(Data, Value);
    return;
  case GPUVM::fixup_gvm_pcimm32:
    // Value is from word 2: the bundle is 8 bytes before.
    if (IsResolved)
      write32le(Data, Value + 8);
    return;
  case GPUVM::fixup_gvm_imm12: {
    if (!IsResolved)
      return;
    if (!isInt<12>(SV))
      Ctx.reportError(Fixup.getLoc(), "immediate out of range");
    write32le(Data, (read32le(Data) & 0x000fffff) | (Value & 0xfff) << 20);
    return;
  }
  case GPUVM::fixup_gvm_br9: {
    if (!IsResolved)
      return;
    int64_t Off = Bundles(9);
    write32le(Data, (read32le(Data) & ~(0x1ffu << 17)) |
                        (static_cast<uint32_t>(Off) & 0x1ff) << 17);
    return;
  }
  case GPUVM::fixup_gvm_j: {
    // The encoder made a near j (beq r0, r0, the ops' path); a far one is cc
    // 0's (path 1 or 2 by its ops, x18, else the general path), as a
    // relocation always is (R_GVM_J18). The bundle's ops are the 12 bytes
    // before its control word.
    uint32_t C = read32le(Data);
    int64_t Off = 0;
    if (IsResolved) {
      Off = SV + 12;
      if (Off & 15)
        Ctx.reportError(Fixup.getLoc(), "branch target not a bundle's start");
      Off >>= 4;
      if (isInt<9>(Off)) {
        write32le(Data, (C & ~(0x1ffu << 17)) |
                            (static_cast<uint32_t>(Off) & 0x1ff) << 17);
        return;
      }
      if (!isInt<18>(Off))
        Ctx.reportError(Fixup.getLoc(), "jump target out of range");
    }
    uint32_t NC = (C & 0x78u) | GPUVM::ctrlSpecial(GPUVM::SubJ) |
                  static_cast<uint32_t>(Off) << 14;
    SmallVector<uint32_t, 2> Ops;
    for (unsigned I = 0; I < GPUVM::ctlN(C) && I < 2; ++I)
      Ops.push_back(read32le(Data - 12 + 4 * I));
    write32le(Data, NC | GPUVM::bundle::path12(NC, Ops));
    return;
  }
  case GPUVM::fixup_gvm_j18: {
    if (!IsResolved)
      return;
    int64_t Off = Bundles(18);
    write32le(Data, (read32le(Data) & 0x3fffu) | static_cast<uint32_t>(Off)
                                                     << 14);
    return;
  }
  default:
    llvm_unreachable("unknown fixup kind");
  }
}

MCAsmBackend *llvm::createGPUVMAsmBackend(const Target &T,
                                          const MCSubtargetInfo &STI,
                                          const MCRegisterInfo &MRI,
                                          const MCTargetOptions &Options) {
  return new GPUVMAsmBackend(
      MCELFObjectTargetWriter::getOSABI(STI.getTargetTriple().getOS()));
}
