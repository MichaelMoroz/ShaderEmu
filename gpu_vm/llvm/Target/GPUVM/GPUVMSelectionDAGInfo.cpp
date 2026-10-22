//===-- GPUVMSelectionDAGInfo.cpp - gpu_vm SelectionDAG info --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// memcpy, memmove and memset of a known size: from -gpuvm-op-threshold bytes
// up (4 KB) the commit's operation, one sq of a descriptor to 0xf0000000 (the
// core goes on; an access stops it only where the operation is still at
// work, gpu_vm/GPU_VM.md "Operations"); 16-aligned ones of a few texels by
// lq/sq; the rest by the generic loads and stores, or libc.
//
//===----------------------------------------------------------------------===//

#include "GPUVMSelectionDAGInfo.h"
#include "GPUVMISelLowering.h"
#include "llvm/CodeGen/SelectionDAG.h"

#define GET_SDNODE_DESC
#include "GPUVMGenSDNodeInfo.inc"

using namespace llvm;

GPUVMSelectionDAGInfo::GPUVMSelectionDAGInfo()
    : SelectionDAGGenTargetInfo(GPUVMGenSDNodeInfo) {}

static SDValue addOffset(SelectionDAG &DAG, const SDLoc &DL, SDValue Ptr,
                         uint64_t Off) {
  if (Off == 0)
    return Ptr;
  return DAG.getMemBasePlusOffset(Ptr, TypeSize::getFixed(Off), DL);
}

// Texels from Src to Dst (both 16-aligned), then the tail by words, halves
// and bytes. With Move, every load before the first store.
static SDValue copyTexels(SelectionDAG &DAG, const SDLoc &DL, SDValue Chain,
                          SDValue Dst, SDValue Src, uint64_t Size,
                          bool IsVolatile, MachinePointerInfo DstPtrInfo,
                          MachinePointerInfo SrcPtrInfo, bool Move) {
  MachineFunction &MF = DAG.getMachineFunction();
  auto VolFlag = IsVolatile ? MachineMemOperand::MOVolatile
                            : MachineMemOperand::MONone;
  SmallVector<std::pair<uint64_t, SmallVector<SDValue, 4>>, 16> Loaded;
  SmallVector<SDValue, 16> LoadChains;
  uint64_t Off = 0;
  for (; Off + 16 <= Size; Off += 16) {
    SDVTList VTs =
        DAG.getVTList({MVT::i32, MVT::i32, MVT::i32, MVT::i32, MVT::Other});
    SDValue Ops[] = {Chain, addOffset(DAG, DL, Src, Off)};
    MachineMemOperand *MMO = MF.getMachineMemOperand(
        SrcPtrInfo.getWithOffset(Off), MachineMemOperand::MOLoad | VolFlag,
        LocationSize::precise(16), Align(16));
    SDValue L =
        DAG.getMemIntrinsicNode(GPUVMISD::LQ, DL, VTs, Ops, MVT::v4i32, MMO);
    Loaded.push_back({Off, {L.getValue(0), L.getValue(1), L.getValue(2),
                            L.getValue(3)}});
    LoadChains.push_back(L.getValue(4));
  }
  // The tail
  SmallVector<std::tuple<uint64_t, MVT, SDValue>, 4> Tail;
  for (; Off < Size;) {
    unsigned Bytes = Size - Off >= 4 ? 4 : Size - Off >= 2 ? 2 : 1;
    MVT MemVT = Bytes == 4 ? MVT::i32 : Bytes == 2 ? MVT::i16 : MVT::i8;
    SDValue L = DAG.getExtLoad(
        ISD::EXTLOAD, DL, MVT::i32, Chain, addOffset(DAG, DL, Src, Off),
        SrcPtrInfo.getWithOffset(Off), MemVT, Align(Bytes),
        IsVolatile ? MachineMemOperand::MOVolatile : MachineMemOperand::MONone);
    Tail.push_back({Off, MemVT, L});
    LoadChains.push_back(L.getValue(1));
    Off += Bytes;
  }

  SDValue StoreChain =
      Move ? DAG.getNode(ISD::TokenFactor, DL, MVT::Other, LoadChains) : Chain;
  SmallVector<SDValue, 16> Stores;
  unsigned K = 0;
  for (auto &[O, V] : Loaded) {
    SDValue In = Move ? StoreChain : LoadChains[K];
    ++K;
    SDValue Ops[] = {In, V[0], V[1], V[2], V[3],
                     addOffset(DAG, DL, Dst, O)};
    MachineMemOperand *MMO = MF.getMachineMemOperand(
        DstPtrInfo.getWithOffset(O), MachineMemOperand::MOStore | VolFlag,
        LocationSize::precise(16), Align(16));
    Stores.push_back(DAG.getMemIntrinsicNode(
        GPUVMISD::SQ, DL, DAG.getVTList(MVT::Other), Ops, MVT::v4i32, MMO));
  }
  for (auto &[O, MemVT, V] : Tail) {
    SDValue In = Move ? StoreChain : V.getValue(1);
    Stores.push_back(DAG.getTruncStore(
        In, DL, V, addOffset(DAG, DL, Dst, O), DstPtrInfo.getWithOffset(O),
        MemVT, Align(MemVT.getStoreSize()),
        IsVolatile ? MachineMemOperand::MOVolatile
                   : MachineMemOperand::MONone));
  }
  return DAG.getNode(ISD::TokenFactor, DL, MVT::Other, Stores);
}

SDValue GPUVMSelectionDAGInfo::EmitTargetCodeForMemcpy(
    SelectionDAG &DAG, const SDLoc &DL, SDValue Chain, SDValue Dst,
    SDValue Src, SDValue Size, Align DstAlign, Align SrcAlign, bool IsVolatile,
    bool AlwaysInline, MachinePointerInfo DstPtrInfo,
    MachinePointerInfo SrcPtrInfo) const {
  auto *C = dyn_cast<ConstantSDNode>(Size);
  if (!C)
    return SDValue();
  uint64_t N = C->getZExtValue();
  if (N >= getGPUVMOpThreshold() && !AlwaysInline)
    return GPUVMTargetLowering::emitOperation(DAG, DL, Chain, GPUVM::OpCopy,
                                              Dst, Src, Size);
  if (DstAlign >= Align(16) && SrcAlign >= Align(16) && N >= 16 &&
      (N <= 16 * getGPUVMInlineTexels() || AlwaysInline))
    return copyTexels(DAG, DL, Chain, Dst, Src, N, IsVolatile, DstPtrInfo,
                      SrcPtrInfo, /*Move=*/false);
  return SDValue();
}

SDValue GPUVMSelectionDAGInfo::EmitTargetCodeForMemmove(
    SelectionDAG &DAG, const SDLoc &DL, SDValue Chain, SDValue Dst,
    SDValue Src, SDValue Size, Align DstAlign, Align SrcAlign, bool IsVolatile,
    MachinePointerInfo DstPtrInfo, MachinePointerInfo SrcPtrInfo) const {
  auto *C = dyn_cast<ConstantSDNode>(Size);
  if (!C)
    return SDValue();
  uint64_t N = C->getZExtValue();
  if (N >= getGPUVMOpThreshold())
    return GPUVMTargetLowering::emitOperation(DAG, DL, Chain, GPUVM::OpCopy,
                                              Dst, Src, Size);
  if (DstAlign >= Align(16) && SrcAlign >= Align(16) && N >= 16 &&
      N <= 16 * getGPUVMInlineTexels())
    return copyTexels(DAG, DL, Chain, Dst, Src, N, IsVolatile, DstPtrInfo,
                      SrcPtrInfo, /*Move=*/true);
  return SDValue();
}

// The fill's word: the byte four times.
static SDValue splatByte(SelectionDAG &DAG, const SDLoc &DL, SDValue V) {
  if (auto *C = dyn_cast<ConstantSDNode>(V))
    return DAG.getConstant((C->getZExtValue() & 0xff) * 0x01010101u, DL,
                           MVT::i32);
  V = DAG.getZExtOrTrunc(V, DL, MVT::i32);
  V = DAG.getNode(ISD::AND, DL, MVT::i32, V, DAG.getConstant(0xff, DL, MVT::i32));
  return DAG.getNode(ISD::MUL, DL, MVT::i32, V,
                     DAG.getConstant(0x01010101u, DL, MVT::i32));
}

SDValue GPUVMSelectionDAGInfo::EmitTargetCodeForMemset(
    SelectionDAG &DAG, const SDLoc &DL, SDValue Chain, SDValue Dst,
    SDValue Src, SDValue Size, Align Alignment, bool IsVolatile,
    bool AlwaysInline, MachinePointerInfo DstPtrInfo) const {
  auto *C = dyn_cast<ConstantSDNode>(Size);
  if (!C)
    return SDValue();
  uint64_t N = C->getZExtValue();
  SDValue Word = splatByte(DAG, DL, Src);
  if (N >= getGPUVMOpThreshold() && !AlwaysInline)
    return GPUVMTargetLowering::emitOperation(DAG, DL, Chain, GPUVM::OpFill,
                                              Dst, Word, Size);
  if (Alignment < Align(16) || N < 16 ||
      (N > 16 * getGPUVMInlineTexels() && !AlwaysInline))
    return SDValue();
  MachineFunction &MF = DAG.getMachineFunction();
  auto VolFlag = IsVolatile ? MachineMemOperand::MOVolatile
                            : MachineMemOperand::MONone;
  SmallVector<SDValue, 16> Stores;
  uint64_t Off = 0;
  for (; Off + 16 <= N; Off += 16) {
    SDValue Ops[] = {Chain, Word, Word, Word, Word,
                     addOffset(DAG, DL, Dst, Off)};
    MachineMemOperand *MMO = MF.getMachineMemOperand(
        DstPtrInfo.getWithOffset(Off), MachineMemOperand::MOStore | VolFlag,
        LocationSize::precise(16), Align(16));
    Stores.push_back(DAG.getMemIntrinsicNode(
        GPUVMISD::SQ, DL, DAG.getVTList(MVT::Other), Ops, MVT::v4i32, MMO));
  }
  for (; Off < N;) {
    unsigned Bytes = N - Off >= 4 ? 4 : N - Off >= 2 ? 2 : 1;
    MVT MemVT = Bytes == 4 ? MVT::i32 : Bytes == 2 ? MVT::i16 : MVT::i8;
    Stores.push_back(DAG.getTruncStore(
        Chain, DL, Word, addOffset(DAG, DL, Dst, Off),
        DstPtrInfo.getWithOffset(Off), MemVT, Align(Bytes), VolFlag));
    Off += Bytes;
  }
  return DAG.getNode(ISD::TokenFactor, DL, MVT::Other, Stores);
}
