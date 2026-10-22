//===-- GPUVMTargetTransformInfo.h - gpu_vm's TTI ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// What the IR passes ask of gpu_vm. A bundle is one iteration of the tick
// whatever it holds: a small loop's body unrolled twice gives the bundle
// filling (GPUVMGlobalFill) two iterations' ops to pair, and a loop whose
// value chains through a register (a list's next pointer) its two copies in
// two registers. So small loops are unrolled by 2, those with a trip count
// known only at run time too, every exit kept.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUVM_GPUVMTARGETTRANSFORMINFO_H
#define LLVM_LIB_TARGET_GPUVM_GPUVMTARGETTRANSFORMINFO_H

#include "GPUVMSubtarget.h"
#include "GPUVMTargetMachine.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"

namespace llvm {

class GPUVMTTIImpl final : public BasicTTIImplBase<GPUVMTTIImpl> {
  using BaseT = BasicTTIImplBase<GPUVMTTIImpl>;
  using TTI = TargetTransformInfo;
  friend BaseT;

  const GPUVMSubtarget *ST;
  const GPUVMTargetLowering *TLI;

  const GPUVMSubtarget *getST() const { return ST; }
  const GPUVMTargetLowering *getTLI() const { return TLI; }

public:
  explicit GPUVMTTIImpl(const GPUVMTargetMachine *TM, const Function &F)
      : BaseT(TM, F.getDataLayout()), ST(TM->getSubtargetImpl(F)),
        TLI(ST->getTargetLowering()) {}

  // A call and its return are cc 0 bundles, off the tick's fast loop (x18:
  // 330-440 ns against 103 for an integer bundle), with the arguments' moves
  // and the callee's setup besides: inline more (the threshold times 3;
  // CoreMark's state machine called in a loop: 124.8k bundles an iteration
  // to 117.5k, the text 6% larger).
  unsigned getInliningThresholdMultiplier() const override;

  void getUnrollingPreferences(Loop *L, ScalarEvolution &SE,
                               TTI::UnrollingPreferences &UP,
                               OptimizationRemarkEmitter *ORE) const override;
};

} // namespace llvm

#endif
