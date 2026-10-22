//===-- GPUVMTargetMachine.cpp - gpu_vm target machine --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The code generator: each machine instruction is one op or control op, and
// the object streamer packs them into bundles (MCTargetDesc/GPUVMELFStreamer).
// After register allocation the ops are ordered to fill bundles
// (GPUVMBundleScheduler.cpp); conditional branches beyond +-256 bundles are
// relaxed.
//
//===----------------------------------------------------------------------===//

#include "GPUVMTargetMachine.h"
#include "GPUVM.h"
#include "GPUVMMachineFunctionInfo.h"
#include "TargetInfo/GPUVMTargetInfo.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include <optional>

using namespace llvm;

static cl::opt<bool>
    BundleSched("gpuvm-bundle-sched", cl::Hidden, cl::init(true),
                cl::desc("gpu_vm: order ops after register allocation to "
                         "fill bundles"));

namespace llvm {
ScheduleDAGInstrs *createGPUVMPostMachineScheduler(MachineSchedContext *C);
}

extern "C" LLVM_ABI LLVM_EXTERNAL_VISIBILITY void LLVMInitializeGPUVMTarget() {
  RegisterTargetMachine<GPUVMTargetMachine> X(getTheGPUVMTarget());
  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeGPUVMAsmPrinterPass(PR);
  initializeGPUVMDAGToDAGISelLegacyPass(PR);
}

GPUVMTargetMachine::GPUVMTargetMachine(const Target &T, const Triple &TT,
                                       StringRef CPU, StringRef FS,
                                       const TargetOptions &Options,
                                       std::optional<Reloc::Model> RM,
                                       std::optional<CodeModel::Model> CM,
                                       CodeGenOptLevel OL, bool JIT)
    : CodeGenTargetMachineImpl(T, TT.computeDataLayout(), TT, CPU, FS, Options,
                               RM.value_or(Reloc::Static),
                               getEffectiveCodeModel(CM, CodeModel::Small),
                               OL),
      TLOF(std::make_unique<TargetLoweringObjectFileELF>()),
      Subtarget(TT, CPU, FS, *this) {
  initAsmInfo();
}

MachineFunctionInfo *GPUVMTargetMachine::createMachineFunctionInfo(
    BumpPtrAllocator &Allocator, const Function &F,
    const TargetSubtargetInfo *STI) const {
  return GPUVMMachineFunctionInfo::create<GPUVMMachineFunctionInfo>(Allocator,
                                                                    F, STI);
}

ScheduleDAGInstrs *
GPUVMTargetMachine::createPostMachineScheduler(MachineSchedContext *C) const {
  return createGPUVMPostMachineScheduler(C);
}

namespace {
class GPUVMPassConfig : public TargetPassConfig {
public:
  GPUVMPassConfig(GPUVMTargetMachine &TM, PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {}

  GPUVMTargetMachine &getGPUVMTargetMachine() const {
    return getTM<GPUVMTargetMachine>();
  }

  void addIRPasses() override {
    addPass(createAtomicExpandLegacyPass());
    TargetPassConfig::addIRPasses();
  }
  bool addInstSelector() override {
    addPass(createGPUVMISelDag(getGPUVMTargetMachine()));
    return false;
  }
  void addPreSched2() override {
    if (getOptLevel() != CodeGenOptLevel::None && BundleSched)
      addPass(&PostMachineSchedulerID);
  }
  // A conditional branch reaches +-256 bundles: farther ones branch over a
  // j (+-2 MB).
  void addPreEmitPass() override {
    if (getOptLevel() != CodeGenOptLevel::None)
      addPass(createGPUVMCountedLoopsPass());
    addPass(&BranchRelaxationPassID);
  }
};
} // namespace

TargetPassConfig *GPUVMTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new GPUVMPassConfig(*this, PM);
}
