//===-- GPUVMSubtarget.h - gpu_vm subtarget ---------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUVM_GPUVMSUBTARGET_H
#define LLVM_LIB_TARGET_GPUVM_GPUVMSUBTARGET_H

#include "GPUVMFrameLowering.h"
#include "GPUVMISelLowering.h"
#include "GPUVMInstrInfo.h"
#include "GPUVMSelectionDAGInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/DataLayout.h"

#define GET_SUBTARGETINFO_HEADER
#include "GPUVMGenSubtargetInfo.inc"

namespace llvm {
class TargetMachine;

class GPUVMSubtarget : public GPUVMGenSubtargetInfo {
  GPUVMInstrInfo InstrInfo;
  GPUVMFrameLowering FrameLowering;
  GPUVMTargetLowering TLInfo;
  GPUVMSelectionDAGInfo TSInfo;

  GPUVMSubtarget &initializeSubtargetDependencies(StringRef CPU, StringRef FS);

public:
  GPUVMSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                 const TargetMachine &TM);

  void ParseSubtargetFeatures(StringRef CPU, StringRef TuneCPU, StringRef FS);

  bool enablePostRAMachineScheduler() const override { return true; }

  const GPUVMInstrInfo *getInstrInfo() const override { return &InstrInfo; }
  const GPUVMFrameLowering *getFrameLowering() const override {
    return &FrameLowering;
  }
  const GPUVMRegisterInfo *getRegisterInfo() const override {
    return &InstrInfo.getRegisterInfo();
  }
  const GPUVMTargetLowering *getTargetLowering() const override {
    return &TLInfo;
  }
  const GPUVMSelectionDAGInfo *getSelectionDAGInfo() const override {
    return &TSInfo;
  }
};

} // namespace llvm

#endif
