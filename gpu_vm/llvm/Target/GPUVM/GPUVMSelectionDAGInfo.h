//===-- GPUVMSelectionDAGInfo.h - gpu_vm SelectionDAG info ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUVM_GPUVMSELECTIONDAGINFO_H
#define LLVM_LIB_TARGET_GPUVM_GPUVMSELECTIONDAGINFO_H

#include "llvm/CodeGen/SelectionDAGTargetInfo.h"

#define GET_SDNODE_ENUM
#include "GPUVMGenSDNodeInfo.inc"

namespace llvm {

class GPUVMSelectionDAGInfo : public SelectionDAGGenTargetInfo {
public:
  GPUVMSelectionDAGInfo();

  // memcpy, memmove, memset: a texel at a time (lq/sq) when small, the
  // commit's operation (one sq of a descriptor) from a threshold up, libc
  // between and for unknown sizes.
  SDValue EmitTargetCodeForMemcpy(SelectionDAG &DAG, const SDLoc &DL,
                                  SDValue Chain, SDValue Dst, SDValue Src,
                                  SDValue Size, Align DstAlign, Align SrcAlign,
                                  bool IsVolatile, bool AlwaysInline,
                                  MachinePointerInfo DstPtrInfo,
                                  MachinePointerInfo SrcPtrInfo) const override;
  SDValue EmitTargetCodeForMemmove(SelectionDAG &DAG, const SDLoc &DL,
                                   SDValue Chain, SDValue Dst, SDValue Src,
                                   SDValue Size, Align DstAlign,
                                   Align SrcAlign, bool IsVolatile,
                                   MachinePointerInfo DstPtrInfo,
                                   MachinePointerInfo SrcPtrInfo) const override;
  SDValue EmitTargetCodeForMemset(SelectionDAG &DAG, const SDLoc &DL,
                                  SDValue Chain, SDValue Dst, SDValue Src,
                                  SDValue Size, Align Alignment,
                                  bool IsVolatile, bool AlwaysInline,
                                  MachinePointerInfo DstPtrInfo) const override;
};

} // namespace llvm

#endif
