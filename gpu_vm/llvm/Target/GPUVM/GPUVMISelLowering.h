//===-- GPUVMISelLowering.h - gpu_vm DAG lowering ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUVM_GPUVMISELLOWERING_H
#define LLVM_LIB_TARGET_GPUVM_GPUVMISELLOWERING_H

#include "GPUVM.h"
#include "GPUVMSelectionDAGInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetLowering.h"

namespace llvm {
class GPUVMSubtarget;

namespace GPUVM {
// The commit's operations (gpu_vm/isa.h, GVM_OPS)
constexpr uint32_t OpsAddress = 0xf0000000u;
constexpr uint32_t OpCopy = 1, OpFill = 2;
} // namespace GPUVM

// -gpuvm-op-threshold and -gpuvm-inline-texels
unsigned getGPUVMOpThreshold();
unsigned getGPUVMInlineTexels();
// -gpuvm-general-ops: one op of the general step (bfe, shadd) where two of the
// fast loop's would do (GPUVMPatterns.td)
bool gpuvmGeneralOps();
bool gpuvmXorEq();
// -gpuvm-mask-select: integer selects as masks (GPUVMPatterns.td)
bool gpuvmMaskSelect();

class GPUVMTargetLowering : public TargetLowering {
  const GPUVMSubtarget &Subtarget;

public:
  GPUVMTargetLowering(const TargetMachine &TM, const GPUVMSubtarget &STI);

  SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;
  void ReplaceNodeResults(SDNode *N, SmallVectorImpl<SDValue> &Results,
                          SelectionDAG &DAG) const override;
  SDValue PerformDAGCombine(SDNode *N, DAGCombinerInfo &DCI) const override;

  // Under arcp (-freciprocal-math), three divisions by one value or more are
  // one frcp and products: frcp and fmul share a bundle, fdiv is no op 1.
  unsigned combineRepeatedFPDivisors() const override { return 3; }

  // A symbol + offset is one immediate, absolute or from the bundle (no GOT:
  // static links only).
  bool isOffsetFoldingLegal(const GlobalAddressSDNode *GA) const override {
    return true;
  }

  EVT getSetCCResultType(const DataLayout &DL, LLVMContext &Context,
                         EVT VT) const override {
    if (VT.isVector())
      return VT.changeVectorElementTypeToInteger();
    return MVT::i32;
  }

  bool isFPImmLegal(const APFloat &Imm, EVT VT,
                    bool ForCodeSize) const override {
    return VT == MVT::f32;
  }
  bool isLegalICmpImmediate(int64_t Imm) const override {
    return isInt<32>(Imm) || isUInt<32>(Imm);
  }
  bool isLegalAddImmediate(int64_t Imm) const override {
    return isInt<32>(Imm) || isUInt<32>(Imm);
  }
  bool isLegalAddressingMode(const DataLayout &DL, const AddrMode &AM, Type *Ty,
                             unsigned AS,
                             Instruction *I = nullptr) const override;
  bool isCheapToSpeculateCttz(Type *Ty) const override { return true; }
  bool isCheapToSpeculateCtlz(Type *Ty) const override { return true; }
  bool isTruncateFree(Type *Ty1, Type *Ty2) const override;
  bool isTruncateFree(EVT VT1, EVT VT2) const override;
  bool allowsMisalignedMemoryAccesses(
      EVT VT, unsigned AddrSpace, Align Alignment,
      MachineMemOperand::Flags Flags,
      unsigned *Fast = nullptr) const override;
  bool findOptimalMemOpLowering(LLVMContext &Context, std::vector<EVT> &MemOps,
                                unsigned Limit, const MemOp &Op,
                                unsigned DstAS, unsigned SrcAS,
                                const AttributeList &FuncAttributes,
                                EVT *LargestVT = nullptr) const override;

  AtomicExpansionKind
  shouldExpandAtomicRMWInIR(const AtomicRMWInst *AI) const override;

  ConstraintType getConstraintType(StringRef Constraint) const override;
  std::pair<unsigned, const TargetRegisterClass *>
  getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                               StringRef Constraint, MVT VT) const override;
  Register getRegisterByName(const char *RegName, LLT VT,
                             const MachineFunction &MF) const override;

  // A landing pad finds the exception in a0 and its selector in a1 (what the
  // unwinder's personality sets: libunwind's Registers_gpuvm).
  Register
  getExceptionPointerRegister(const Constant *PersonalityFn) const override {
    return GPUVM::R5;
  }
  Register
  getExceptionSelectorRegister(const Constant *PersonalityFn) const override {
    return GPUVM::R6;
  }

  SDValue LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv,
                               bool IsVarArg,
                               const SmallVectorImpl<ISD::InputArg> &Ins,
                               const SDLoc &DL, SelectionDAG &DAG,
                               SmallVectorImpl<SDValue> &InVals) const override;
  SDValue LowerCall(TargetLowering::CallLoweringInfo &CLI,
                    SmallVectorImpl<SDValue> &InVals) const override;
  bool CanLowerReturn(CallingConv::ID CallConv, MachineFunction &MF,
                      bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      LLVMContext &Context, const Type *RetTy) const override;
  SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
                      SelectionDAG &DAG) const override;

  bool mayBeEmittedAsTailCall(const CallInst *CI) const override {
    return CI->isTailCall();
  }

  // The commit's operation: a descriptor {kind, dst, src or word, bytes}
  // stored by one sq to 0xf0000000.
  static SDValue emitOperation(SelectionDAG &DAG, const SDLoc &DL,
                               SDValue Chain, uint32_t Kind, SDValue Dst,
                               SDValue Src, SDValue Size);

private:
  SDValue lowerAddress(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerGlobalTLSAddress(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerVASTART(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerCarryOp(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerShiftParts(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerFRAMEADDR(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerRETURNADDR(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerINTRINSIC_VOID(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerApproxFunc(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerRound(SDValue Op, SelectionDAG &DAG) const;
};

} // namespace llvm

#endif
