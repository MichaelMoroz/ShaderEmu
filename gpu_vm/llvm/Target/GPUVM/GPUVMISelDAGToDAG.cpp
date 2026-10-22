//===-- GPUVMISelDAGToDAG.cpp - gpu_vm instruction selection --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUVM.h"
#include "GPUVMISelLowering.h"
#include "GPUVMTargetMachine.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-isel"
#define PASS_NAME "gpu_vm DAG->DAG Pattern Instruction Selection"

namespace {

class GPUVMDAGToDAGISel : public SelectionDAGISel {
public:
  GPUVMDAGToDAGISel() = delete;
  explicit GPUVMDAGToDAGISel(GPUVMTargetMachine &TM) : SelectionDAGISel(TM) {}

  bool SelectInlineAsmMemoryOperand(const SDValue &Op,
                                    InlineAsm::ConstraintCode ConstraintID,
                                    std::vector<SDValue> &OutOps) override;

private:
#include "GPUVMGenDAGISel.inc"

  void Select(SDNode *N) override;

  bool selectAddrRI(SDValue Addr, SDValue &Base, SDValue &Offset);
  bool selectAddrRR(SDValue Addr, SDValue &Base, SDValue &Index,
                    SDValue &Shift);
  SDValue makeQuad(const SDLoc &DL, ArrayRef<SDValue> Vals);
};

class GPUVMDAGToDAGISelLegacy : public SelectionDAGISelLegacy {
public:
  static char ID;
  explicit GPUVMDAGToDAGISelLegacy(GPUVMTargetMachine &TM)
      : SelectionDAGISelLegacy(ID, std::make_unique<GPUVMDAGToDAGISel>(TM)) {}
};
} // namespace

char GPUVMDAGToDAGISelLegacy::ID = 0;

INITIALIZE_PASS(GPUVMDAGToDAGISelLegacy, DEBUG_TYPE, PASS_NAME, false, false)

FunctionPass *llvm::createGPUVMISelDag(GPUVMTargetMachine &TM) {
  return new GPUVMDAGToDAGISelLegacy(TM);
}

// An immediate operand: a constant or a symbol (the long immediate).
static bool isSymbol(SDValue V) {
  return V.getOpcode() == GPUVMISD::ADDR;
}

// base + imm32: the immediate a constant or a symbol's address (+ offset).
bool GPUVMDAGToDAGISel::selectAddrRI(SDValue Addr, SDValue &Base,
                                     SDValue &Offset) {
  SDLoc DL(Addr);
  EVT VT = Addr.getValueType();
  if (auto *FIN = dyn_cast<FrameIndexSDNode>(Addr)) {
    Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), VT);
    Offset = CurDAG->getTargetConstant(0, DL, VT);
    return true;
  }
  if (auto *C = dyn_cast<ConstantSDNode>(Addr)) {
    Base = CurDAG->getRegister(GPUVM::R0, VT);
    Offset = CurDAG->getSignedTargetConstant(C->getSExtValue(), DL, VT);
    return true;
  }
  if (isSymbol(Addr)) {
    Base = CurDAG->getRegister(GPUVM::R0, VT);
    Offset = Addr.getOperand(0);
    return true;
  }
  if (CurDAG->isBaseWithConstantOffset(Addr)) {
    int64_t C = cast<ConstantSDNode>(Addr.getOperand(1))->getSExtValue();
    SDValue B = Addr.getOperand(0);
    // A symbol and an offset: one immediate.
    if (isSymbol(B)) {
      SDValue Sym = B.getOperand(0);
      if (auto *G = dyn_cast<GlobalAddressSDNode>(Sym)) {
        Base = CurDAG->getRegister(GPUVM::R0, VT);
        Offset = CurDAG->getTargetGlobalAddress(
            G->getGlobal(), DL, VT, G->getOffset() + C, G->getTargetFlags());
        return true;
      }
    }
    if (auto *FIN = dyn_cast<FrameIndexSDNode>(B))
      Base = CurDAG->getTargetFrameIndex(FIN->getIndex(), VT);
    else
      Base = B;
    Offset = CurDAG->getSignedTargetConstant(C, DL, VT);
    return true;
  }
  if (Addr.getOpcode() == ISD::ADD) {
    for (unsigned I = 0; I < 2; ++I) {
      SDValue S = Addr.getOperand(I);
      if (isSymbol(S) && !isa<FrameIndexSDNode>(Addr.getOperand(1 - I))) {
        Base = Addr.getOperand(1 - I);
        Offset = S.getOperand(0);
        return true;
      }
    }
  }
  Base = Addr;
  Offset = CurDAG->getTargetConstant(0, DL, VT);
  return true;
}

// base + (index << sh): neither a constant, a symbol nor a frame index.
bool GPUVMDAGToDAGISel::selectAddrRR(SDValue Addr, SDValue &Base,
                                     SDValue &Index, SDValue &Shift) {
  if (Addr.getOpcode() != ISD::ADD)
    return false;
  SDValue A = Addr.getOperand(0), B = Addr.getOperand(1);
  auto Simple = [](SDValue V) {
    return !isa<ConstantSDNode>(V) && !isSymbol(V) &&
           !isa<FrameIndexSDNode>(V);
  };
  if (!Simple(A) || !Simple(B))
    return false;
  SDLoc DL(Addr);
  unsigned Sh = 0;
  auto ShiftOf = [&](SDValue V, unsigned &S) {
    if (V.getOpcode() != ISD::SHL)
      return false;
    auto *C = dyn_cast<ConstantSDNode>(V.getOperand(1));
    if (!C || C->getZExtValue() > 31)
      return false;
    S = C->getZExtValue();
    return true;
  };
  if (ShiftOf(B, Sh)) {
    Base = A;
    Index = B.getOperand(0);
  } else if (ShiftOf(A, Sh)) {
    Base = B;
    Index = A.getOperand(0);
  } else {
    Base = A;
    Index = B;
  }
  Shift = CurDAG->getTargetConstant(Sh, DL, MVT::i32);
  return true;
}

bool GPUVMDAGToDAGISel::SelectInlineAsmMemoryOperand(
    const SDValue &Op, InlineAsm::ConstraintCode ConstraintID,
    std::vector<SDValue> &OutOps) {
  switch (ConstraintID) {
  case InlineAsm::ConstraintCode::m:
  case InlineAsm::ConstraintCode::o: {
    SDValue Base, Offset;
    selectAddrRI(Op, Base, Offset);
    OutOps.push_back(Base);
    OutOps.push_back(Offset);
    return false;
  }
  default:
    return true;
  }
}

// Four i32 values in a GPRQ tuple.
SDValue GPUVMDAGToDAGISel::makeQuad(const SDLoc &DL, ArrayRef<SDValue> Vals) {
  SDValue Ops[] = {
      CurDAG->getTargetConstant(GPUVM::GPRQRegClassID, DL, MVT::i32),
      Vals[0], CurDAG->getTargetConstant(GPUVM::sub0, DL, MVT::i32),
      Vals[1], CurDAG->getTargetConstant(GPUVM::sub1, DL, MVT::i32),
      Vals[2], CurDAG->getTargetConstant(GPUVM::sub2, DL, MVT::i32),
      Vals[3], CurDAG->getTargetConstant(GPUVM::sub3, DL, MVT::i32)};
  return SDValue(
      CurDAG->getMachineNode(TargetOpcode::REG_SEQUENCE, DL, MVT::Untyped, Ops),
      0);
}

void GPUVMDAGToDAGISel::Select(SDNode *N) {
  if (N->isMachineOpcode()) {
    N->setNodeId(-1);
    return;
  }
  SDLoc DL(N);
  switch (N->getOpcode()) {
  case ISD::Constant: {
    // 0 is r0: the coalescer makes it an operand.
    if (N->getValueType(0) == MVT::i32 && cast<ConstantSDNode>(N)->isZero()) {
      SDValue New = CurDAG->getCopyFromReg(CurDAG->getEntryNode(), DL,
                                           GPUVM::R0, MVT::i32);
      ReplaceNode(N, New.getNode());
      return;
    }
    break;
  }
  case ISD::FrameIndex: {
    int FI = cast<FrameIndexSDNode>(N)->getIndex();
    SDValue TFI = CurDAG->getTargetFrameIndex(FI, MVT::i32);
    SDValue Imm = CurDAG->getTargetConstant(0, DL, MVT::i32);
    if (N->hasOneUse()) {
      CurDAG->SelectNodeTo(N, GPUVM::ADDI, MVT::i32, TFI, Imm);
      return;
    }
    ReplaceNode(N, CurDAG->getMachineNode(GPUVM::ADDI, DL, MVT::i32, TFI, Imm));
    return;
  }
  case GPUVMISD::LQ: {
    auto *MN = cast<MemIntrinsicSDNode>(N);
    SDValue Base, Offset;
    selectAddrRI(N->getOperand(1), Base, Offset);
    SDValue Ops[] = {Base, Offset, N->getOperand(0)};
    MachineSDNode *L = CurDAG->getMachineNode(GPUVM::LQ_Q, DL, MVT::Untyped,
                                              MVT::Other, Ops);
    CurDAG->setNodeMemRefs(L, {MN->getMemOperand()});
    SDValue Q(L, 0);
    unsigned Subs[4] = {GPUVM::sub0, GPUVM::sub1, GPUVM::sub2, GPUVM::sub3};
    for (unsigned I = 0; I < 4; ++I)
      ReplaceUses(SDValue(N, I),
                  CurDAG->getTargetExtractSubreg(Subs[I], DL, MVT::i32, Q));
    ReplaceUses(SDValue(N, 4), SDValue(L, 1));
    CurDAG->RemoveDeadNode(N);
    return;
  }
  case GPUVMISD::SQ: {
    auto *MN = cast<MemIntrinsicSDNode>(N);
    SDValue Base, Offset;
    selectAddrRI(N->getOperand(5), Base, Offset);
    SDValue Q = makeQuad(DL, {N->getOperand(1), N->getOperand(2),
                              N->getOperand(3), N->getOperand(4)});
    SDValue Ops[] = {Q, Base, Offset, N->getOperand(0)};
    MachineSDNode *S = CurDAG->getMachineNode(GPUVM::SQ_Q, DL, MVT::Other, Ops);
    CurDAG->setNodeMemRefs(S, {MN->getMemOperand()});
    ReplaceNode(N, S);
    return;
  }
  case GPUVMISD::OPERATION: {
    SDValue Q = makeQuad(DL, {N->getOperand(1), N->getOperand(2),
                              N->getOperand(3), N->getOperand(4)});
    SDValue Ops[] = {
        Q, CurDAG->getRegister(GPUVM::R0, MVT::i32),
        CurDAG->getTargetConstant(GPUVM::OpsAddress, DL, MVT::i32),
        N->getOperand(0)};
    ReplaceNode(N,
                CurDAG->getMachineNode(GPUVM::SQ_OP, DL, MVT::Other, Ops));
    return;
  }
  default:
    break;
  }
  SelectCode(N);
}
