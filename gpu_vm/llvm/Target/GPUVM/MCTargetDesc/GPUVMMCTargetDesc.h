//===-- GPUVMMCTargetDesc.h - gpu_vm target descriptions --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUVM_MCTARGETDESC_GPUVMMCTARGETDESC_H
#define LLVM_LIB_TARGET_GPUVM_MCTARGETDESC_GPUVMMCTARGETDESC_H

#include <cstdint>
#include <memory>

namespace llvm {
class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCInstrInfo;
class MCObjectTargetWriter;
class MCRegisterInfo;
class MCSubtargetInfo;
class MCTargetOptions;
class Target;

MCCodeEmitter *createGPUVMMCCodeEmitter(const MCInstrInfo &MCII,
                                        MCContext &Ctx);
MCAsmBackend *createGPUVMAsmBackend(const Target &T,
                                    const MCSubtargetInfo &STI,
                                    const MCRegisterInfo &MRI,
                                    const MCTargetOptions &Options);
std::unique_ptr<MCObjectTargetWriter> createGPUVMELFObjectWriter(uint8_t OSABI);
} // namespace llvm

#define GET_REGINFO_ENUM
#include "GPUVMGenRegisterInfo.inc"

#define GET_INSTRINFO_ENUM
#define GET_INSTRINFO_MC_HELPER_DECLS
#include "GPUVMGenInstrInfo.inc"

#define GET_SUBTARGETINFO_ENUM
#include "GPUVMGenSubtargetInfo.inc"

#endif
