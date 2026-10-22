//===-- GPUVMTargetTransformInfo.cpp - gpu_vm's TTI -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUVMTargetTransformInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvmtti"

static cl::opt<unsigned>
    UnrollCount("gpuvm-unroll", cl::Hidden, cl::init(1),
                cl::desc("gpu_vm: small loops unrolled this many times (1: "
                         "not)"));
static cl::opt<unsigned>
    UnrollSize("gpuvm-unroll-size", cl::Hidden, cl::init(40),
               cl::desc("gpu_vm: the largest unrolled loop body (TTI's "
                        "instruction cost)"));

static cl::opt<unsigned>
    InlineMultiplier("gpuvm-inline-multiplier", cl::Hidden, cl::init(3),
                     cl::desc("gpu_vm: the inlining threshold's multiplier"));

unsigned GPUVMTTIImpl::getInliningThresholdMultiplier() const {
  return InlineMultiplier;
}

void GPUVMTTIImpl::getUnrollingPreferences(
    Loop *L, ScalarEvolution &SE, TTI::UnrollingPreferences &UP,
    OptimizationRemarkEmitter *ORE) const {
  BaseT::getUnrollingPreferences(L, SE, UP, ORE);
  LLVM_DEBUG(dbgs() << "gpuvm unroll prefs for " << L->getName() << "\n");
  if (UnrollCount < 2 || !L->isInnermost())
    return;
  // A call in the loop: its bundles are not the loop's.
  for (BasicBlock *BB : L->blocks())
    for (Instruction &I : *BB)
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (!isa<IntrinsicInst>(CB) || isa<MemIntrinsic>(CB)) {
          LLVM_DEBUG(dbgs() << "  a call: " << *CB << "\n");
          return;
        }
  LLVM_DEBUG(dbgs() << "  unrolled by " << UnrollCount << "\n");
  UP.Partial = true;
  UP.Runtime = true;
  UP.RuntimeUnrollMultiExit = true;
  UP.UnrollRemainder = false;
  UP.DefaultUnrollRuntimeCount = UnrollCount;
  UP.MaxCount = UnrollCount;
  UP.PartialThreshold = UnrollSize;
  UP.UpperBound = true;
}
