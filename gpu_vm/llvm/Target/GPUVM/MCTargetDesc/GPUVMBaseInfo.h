//===-- GPUVMBaseInfo.h - gpu_vm encoding constants -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The encoding of gpu_vm/isa.h (ShaderEmu), in LLVM's terms, and the rules by
// which ops share a bundle (gpu_vm/ref/gvm_asm.h). isa.h is the truth: keep
// these in step with it.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_GPUVM_MCTARGETDESC_GPUVMBASEINFO_H
#define LLVM_LIB_TARGET_GPUVM_MCTARGETDESC_GPUVMBASEINFO_H

#include "llvm/ADT/ArrayRef.h"
#include <cstdint>

namespace llvm {
namespace GPUVMII {

// TSFlags (GPUVMInstrFormats.td).
enum Form : unsigned {
  FrmPseudo = 0,
  FrmR = 1,
  FrmR4 = 2,
  FrmRSh = 3,
  FrmR1 = 4,
  FrmI = 5,
  FrmFI = 6,
  FrmBF = 7,
  FrmBFI = 8,
  FrmLoad = 9,
  FrmStore = 10,
  FrmLoadX = 11,
  FrmStoreX = 12,
  FrmAMO = 13,
  FrmCAS = 14,
  FrmLPC = 15,
  FrmNone = 16,
  FrmMFSR = 17,
  FrmMTSR = 18,
  FrmXSR = 19,
  FrmUnimp = 20,
  CtlBr = 21,
  CtlBrI = 22,
  CtlBit = 23,
  CtlAddBnz = 24,
  CtlJ = 25,
  CtlCall = 26,
  CtlJR = 27,
  CtlNone = 28,
  CtlBump = 29,
};

inline unsigned getOpc(uint64_t TSFlags) { return TSFlags & 0xff; }
inline Form getForm(uint64_t TSFlags) {
  return static_cast<Form>((TSFlags >> 8) & 31);
}
inline bool isControl(uint64_t TSFlags) { return (TSFlags >> 13) & 1; }
inline bool isVariant(uint64_t TSFlags) { return (TSFlags >> 14) & 1; }
// A control op's cc and sub (Opc = cc | sub << 4).
inline unsigned getCC(uint64_t TSFlags) { return TSFlags & 15; }
inline unsigned getSub(uint64_t TSFlags) { return (TSFlags >> 4) & 7; }

} // namespace GPUVMII

namespace GPUVM {

constexpr unsigned BundleSize = 16;

// Control word
constexpr unsigned PathGeneral = 0, PathALU = 1, PathMemF = 2, PathLoad = 3,
                   PathLoad2 = 4, PathStore = 5, PathMem2 = 6, PathInt = 7;
constexpr unsigned CCSpecial = 0, CCAddBnz = 1, CCBEq = 4, CCBNe = 5;
constexpr unsigned SubNone = 1, SubJR = 2, SubCallR = 3, SubEret = 4, SubJ = 5,
                   SubCall = 6, SubAdd = 7;
constexpr uint32_t CtrlNone = CCBNe << 7;
constexpr uint32_t CtrlJ = CCBEq << 7;
inline uint32_t ctrlSpecial(unsigned Sub) { return Sub << 11; }

inline unsigned ctlPath(uint32_t W) { return W & 7; }
inline unsigned ctlN(uint32_t W) { return (W >> 3) & 3; }
inline unsigned ctlLong0(uint32_t W) { return (W >> 5) & 1; }
inline unsigned ctlLong1(uint32_t W) { return (W >> 6) & 1; }
inline unsigned ctlCC(uint32_t W) { return (W >> 7) & 15; }
inline unsigned ctlRA(uint32_t W) { return (W >> 11) & 63; }
inline int ctlOff9(uint32_t W) { return static_cast<int32_t>(W << 6) >> 23; }
inline unsigned ctlRB(uint32_t W) { return W >> 26; }
inline int ctlImm6(uint32_t W) { return static_cast<int32_t>(W) >> 26; }
inline unsigned ctlSub(uint32_t W) { return (W >> 11) & 7; }
inline unsigned ctlRS(uint32_t W) { return (W >> 14) & 63; }
inline int ctlOff18(uint32_t W) { return static_cast<int32_t>(W) >> 14; }
inline int ctlImm12(uint32_t W) { return static_cast<int32_t>(W) >> 20; }

// Op word
inline unsigned opRA(uint32_t W) { return W & 63; }
inline unsigned opRD(uint32_t W) { return (W >> 6) & 63; }
inline unsigned opOpc(uint32_t W) { return (W >> 12) & 255; }
inline unsigned opRC(uint32_t W) { return (W >> 20) & 63; }
inline unsigned opRB(uint32_t W) { return W >> 26; }
inline int opImm12(uint32_t W) { return static_cast<int32_t>(W) >> 20; }
inline uint32_t wordR(unsigned Opc, unsigned RD, unsigned RA, unsigned RB,
                      unsigned RC) {
  return Opc << 12 | (RD & 63) << 6 | (RA & 63) | (RC & 63) << 20 |
         (RB & 63) << 26;
}
inline uint32_t wordI(unsigned Opc, unsigned RD, unsigned RA, uint32_t Imm12) {
  return Opc << 12 | (RD & 63) << 6 | (RA & 63) | (Imm12 & 0xfff) << 20;
}
inline unsigned opClass(unsigned Opc) { return Opc >> 6; }
inline bool opIForm(unsigned Opc) { return (Opc >> 5) & 1; }

// Functions (isa.h)
constexpr unsigned IXnor = 7, ISll = 16, ISra = 18, IMidLast = 19,
                   ICommonLast = 23;
constexpr unsigned FMad = 2, FMsb = 3, FNmsb = 5, FMax = 7;
constexpr unsigned MStore = 1, MStoreX = 3;
constexpr unsigned WW = 0, WH = 2, WB = 3, WHU = 6, WBU = 7;

// ---- The packing rules, as gvm_asm.h has them, on op words.
namespace bundle {
inline unsigned fn(uint32_t Op) { return opOpc(Op) & 31; }
inline bool intOp(uint32_t Op) {
  return opClass(opOpc(Op)) == 1 && fn(Op) <= ICommonLast;
}
inline bool mid(uint32_t Op) {
  return opClass(opOpc(Op)) == 1 && fn(Op) <= IMidLast;
}
inline bool lite(uint32_t Op) {
  return opClass(opOpc(Op)) == 1 &&
         (fn(Op) <= IXnor || (fn(Op) >= ISll && fn(Op) <= ISra));
}
inline bool memOp(uint32_t Op) {
  unsigned O = opOpc(Op), Wd = O & 7;
  return opClass(O) == 2 && ((O >> 3) & 7) <= 3 &&
         (Wd == WW || Wd == WH || Wd == WB || Wd == WHU || Wd == WBU);
}
inline bool madLike(unsigned F) { return F == FMad || F == FMsb || F == FNmsb; }
inline bool floatOp(uint32_t Op) {
  unsigned O = opOpc(Op);
  return opClass(O) == 3 && !(opIForm(O) && madLike(O & 31));
}
inline bool fLite(uint32_t Op) { return floatOp(Op) && fn(Op) <= FMax; }
inline bool storeOp(uint32_t Op) {
  unsigned O = opOpc(Op);
  return opClass(O) == 2 &&
         (((O >> 3) & 7) == MStore || ((O >> 3) & 7) == MStoreX);
}

// The path of a bundle with these ops (and a control word not of cc 0).
inline unsigned path(ArrayRef<uint32_t> Ops) {
  if (Ops.empty())
    return PathInt;
  uint32_t A = Ops[0];
  bool One = Ops.size() == 1;
  uint32_t B = One ? 0 : Ops[1];
  if (mid(A) && (One || lite(B)))
    return PathInt;
  if ((intOp(A) || floatOp(A)) && (One || lite(B) || fLite(B)))
    return PathALU;
  if (memOp(A) && (One || lite(B)))
    return storeOp(A) ? PathStore : PathLoad;
  if (memOp(A) && memOp(B))
    return storeOp(A) ? PathMem2 : storeOp(B) ? PathGeneral : PathLoad2;
  if (memOp(A))
    return fLite(B) ? PathMemF : PathGeneral;
  return PathGeneral;
}

// Op names op 0's result in a field it reads (a store's rd is its value).
inline bool readsRd0(uint32_t Op0, uint32_t Op) {
  unsigned D = opRD(Op0);
  return !storeOp(Op0) && D != 0 &&
         (opRA(Op) == D || opRB(Op) == D || (storeOp(Op) && opRD(Op) == D));
}

// Op can be op 1 after Op0.
inline bool fits1(uint32_t Op0, uint32_t Op) {
  if (memOp(Op) && memOp(Op0))
    return !readsRd0(Op0, Op) && (storeOp(Op0) || !storeOp(Op));
  return lite(Op) || (fLite(Op) && (floatOp(Op0) || memOp(Op0)));
}

// Op names R in any field (conservatively: rd, ra, rb, rc).
inline bool touches(uint32_t Op, unsigned R) {
  return opRD(Op) == R || opRA(Op) == R || opRB(Op) == R || opRC(Op) == R;
}

// An op of Ops writes R (a store's rd is read); wide memory ops by rd's group.
inline bool writes(ArrayRef<uint32_t> Ops, unsigned R) {
  for (uint32_t Op : Ops)
    if (!storeOp(Op) &&
        (opRD(Op) == R ||
         (opClass(opOpc(Op)) == 2 && (R - opRD(Op)) < 4)))
      return true;
  return false;
}
} // namespace bundle

} // namespace GPUVM
} // namespace llvm

#endif
