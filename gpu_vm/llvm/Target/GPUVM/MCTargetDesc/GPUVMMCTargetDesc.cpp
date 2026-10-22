//===-- GPUVMMCTargetDesc.cpp - gpu_vm target descriptions ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/GPUVMMCTargetDesc.h"
#include "MCTargetDesc/GPUVMELFStreamer.h"
#include "MCTargetDesc/GPUVMInstPrinter.h"
#include "MCTargetDesc/GPUVMMCAsmInfo.h"
#include "TargetInfo/GPUVMTargetInfo.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Compiler.h"

using namespace llvm;

#define GET_INSTRINFO_MC_DESC
#define ENABLE_INSTR_PREDICATE_VERIFIER
#include "GPUVMGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "GPUVMGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "GPUVMGenRegisterInfo.inc"

static MCInstrInfo *createGPUVMMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitGPUVMMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createGPUVMMCRegisterInfo(const Triple &TT) {
  MCRegisterInfo *X = new MCRegisterInfo();
  InitGPUVMMCRegisterInfo(X, GPUVM::R1);
  return X;
}

static MCSubtargetInfo *
createGPUVMMCSubtargetInfo(const Triple &TT, StringRef CPU, StringRef FS) {
  if (CPU.empty())
    CPU = "generic";
  return createGPUVMMCSubtargetInfoImpl(TT, CPU, /*TuneCPU=*/CPU, FS);
}

static MCAsmInfo *createGPUVMMCAsmInfo(const MCRegisterInfo &MRI,
                                       const Triple &TT,
                                       const MCTargetOptions &Options) {
  MCAsmInfo *MAI = new GPUVMMCAsmInfo(TT, Options);
  // The CFA is sp at entry.
  MAI->addInitialFrameState(
      MCCFIInstruction::cfiDefCfa(nullptr, MRI.getDwarfRegNum(GPUVM::R2, true),
                                  0));
  return MAI;
}

static MCInstPrinter *createGPUVMMCInstPrinter(const Triple &T,
                                               unsigned SyntaxVariant,
                                               const MCAsmInfo &MAI,
                                               const MCInstrInfo &MII,
                                               const MCRegisterInfo &MRI) {
  return new GPUVMInstPrinter(MAI, MII, MRI);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void
LLVMInitializeGPUVMTargetMC() {
  Target &T = getTheGPUVMTarget();
  TargetRegistry::RegisterMCAsmInfo(T, createGPUVMMCAsmInfo);
  TargetRegistry::RegisterMCInstrInfo(T, createGPUVMMCInstrInfo);
  TargetRegistry::RegisterMCRegInfo(T, createGPUVMMCRegisterInfo);
  TargetRegistry::RegisterMCSubtargetInfo(T, createGPUVMMCSubtargetInfo);
  TargetRegistry::RegisterMCCodeEmitter(T, createGPUVMMCCodeEmitter);
  TargetRegistry::RegisterMCAsmBackend(T, createGPUVMAsmBackend);
  TargetRegistry::RegisterMCInstPrinter(T, createGPUVMMCInstPrinter);
  TargetRegistry::RegisterELFStreamer(T, createGPUVMELFStreamer);
  TargetRegistry::RegisterObjectTargetStreamer(
      T, createGPUVMObjectTargetStreamer);
  TargetRegistry::RegisterAsmTargetStreamer(T, createGPUVMAsmTargetStreamer);
}
