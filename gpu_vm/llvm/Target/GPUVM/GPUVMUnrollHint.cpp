//===-- GPUVMUnrollHint.cpp - ask small while loops to be unrolled by 2 ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A small loop whose trip count the compiler cannot compute (a list walked to
// its end, a string to its 0, a state machine) is unrolled by 2, every exit
// kept: the two copies give the bundle filling (GPUVMGlobalFill) the next
// iteration's first ops for this one's branch bundles, and a value that
// chains through a register (a list's next pointer) gets a register a copy,
// so no mv a step. LLVM's runtime unrolling makes no remainder for such a
// loop; an unroll count in the loop's metadata (as `#pragma unroll 2` would)
// unrolls it all the same. Loops with a trip count are left to LLVM's own
// heuristics (unrolling them by 2 made zlib's slower: crc32, deflate).
//
//===----------------------------------------------------------------------===//

#include "GPUVMUnrollHint.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

using namespace llvm;

static cl::opt<unsigned>
    HintSize("gpuvm-unroll-while", cl::Hidden, cl::init(16),
             cl::desc("gpu_vm: a loop of no computable trip count and at most "
                      "this many instructions is unrolled by 2 (0: none)"));
static cl::opt<unsigned>
    CountedSize("gpuvm-unroll-counted", cl::Hidden, cl::init(0),
                cl::desc("gpu_vm: a loop of a trip count known at run time and "
                         "at most this many instructions is unrolled by 2"));

PreservedAnalyses GPUVMUnrollHintPass::run(Loop &L, LoopAnalysisManager &,
                                           LoopStandardAnalysisResults &AR,
                                           LPMUpdater &) {
  if (HintSize == 0 || !L.isInnermost() ||
      findOptionMDForLoop(&L, "llvm.loop.unroll.disable") ||
      findOptionMDForLoop(&L, "llvm.loop.unroll.count") ||
      findOptionMDForLoop(&L, "llvm.loop.unroll.full"))
    return PreservedAnalyses::all();
  unsigned Size = 0;
  for (BasicBlock *BB : L.blocks())
    for (Instruction &I : *BB) {
      if (I.isDebugOrPseudoInst())
        continue;
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (!isa<IntrinsicInst>(CB) || isa<MemIntrinsic>(CB))
          return PreservedAnalyses::all();
      if (++Size > HintSize)
        return PreservedAnalyses::all();
    }
  if (!isa<SCEVCouldNotCompute>(AR.SE.getBackedgeTakenCount(&L)) &&
      (Size > CountedSize || AR.SE.getSmallConstantTripCount(&L) != 0))
    return PreservedAnalyses::all();
  addStringMetadataToLoop(&L, "llvm.loop.unroll.count", 2);
  return PreservedAnalyses::all();
}
