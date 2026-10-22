//===-- GPUVMBundleModel.h - the streamer's packing on machine code -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// What the object streamer will make of machine instructions (GPUVMELFStreamer,
// gvm_asm.h's rules), for the passes that order and move ops to fill bundles:
// each instruction's op word (the fields the packing rules read), and a packer
// that counts the bundles of a run of instructions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUVM_GPUVMBUNDLEMODEL_H
#define LLVM_LIB_TARGET_GPUVM_GPUVMBUNDLEMODEL_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/Register.h"
#include <cstdint>

namespace llvm {
class MachineBasicBlock;
class MachineInstr;
class TargetRegisterInfo;

namespace GPUVM {

// An instruction as the streamer sees it. Known false: not one op (inline
// asm, an unknown pseudo): it is taken to close bundles on both sides.
struct OpDesc {
  bool Known = false;
  bool Control = false; // a control op (it closes its bundle)
  bool Meta = false;    // no op at all (KILL, IMPLICIT_DEF, debug ...)
  bool Long = false;    // takes the bundle's long immediate
  uint32_t Word = 0;
  // A control op's registers compared as the bundle found them (encodings).
  uint8_t NRead = 0;
  uint8_t Read[2] = {0, 0};
};

// The op of MI; Rename (when not 0) names a register by another's encoding:
// every operand of register From is taken as To (for trial renames).
OpDesc describeOp(const MachineInstr &MI, const TargetRegisterInfo *TRI,
                  Register From = Register(), Register To = Register());

// The streamer's open bundle, fed op by op: put() says whether the op starts
// a bundle (it does not join the open one).
class Packer {
  SmallVector<uint32_t, 2> Words;
  bool HasLong = false;

public:
  // Bundles closed so far
  unsigned Closed = 0;
  bool open() const { return !Words.empty(); }
  unsigned openOps() const { return Words.size(); }
  void close() {
    if (!Words.empty())
      ++Closed;
    Words.clear();
    HasLong = false;
  }
  // Whether the op would join the open bundle (it fits as op 1).
  bool joins(const OpDesc &D) const;
  bool put(const OpDesc &D);
};

// A block starts a bundle (it has a label: it is branched to), as the
// AsmPrinter decides.
bool startsBundle(const MachineBasicBlock &MBB);

} // namespace GPUVM
} // namespace llvm

#endif
