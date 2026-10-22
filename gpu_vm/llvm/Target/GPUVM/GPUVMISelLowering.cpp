//===-- GPUVMISelLowering.cpp - gpu_vm DAG lowering -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// i32 and f32 in the 64 registers, i64 in pairs of them (the carry ops),
// f64 in software. Every immediate is 32 bits: constants and addresses are an
// op each, an address folds into a load's or a store's immediate.
//
// The calling convention (gpu_vm/GPU_VM.md, "ABI"): arguments in a0-a7
// (r5-r12) and then on the stack, 4 bytes a slot from sp at the call; a value
// of several parts (i64, double) goes whole in registers or whole on the
// stack; variadic arguments are all on the stack. Results in a0 and a1;
// larger ones by a hidden pointer.
//
//===----------------------------------------------------------------------===//

#include "GPUVMISelLowering.h"
#include "GPUVMMachineFunctionInfo.h"
#include "GPUVMRegisterInfo.h"
#include "GPUVMSubtarget.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsGPUVM.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

#define DEBUG_TYPE "gpuvm-lower"

#define GET_CALLING_CONV_IMPL
#include "GPUVMGenCallingConv.inc"

GPUVMTargetLowering::GPUVMTargetLowering(const TargetMachine &TM,
                                         const GPUVMSubtarget &STI)
    : TargetLowering(TM, STI), Subtarget(STI) {
  addRegisterClass(MVT::i32, &GPUVM::GPRRegClass);
  addRegisterClass(MVT::f32, &GPUVM::GPRRegClass);
  computeRegisterProperties(STI.getRegisterInfo());

  setStackPointerRegisterToSaveRestore(GPUVM::R2);
  setBooleanContents(ZeroOrOneBooleanContent);
  setSchedulingPreference(Sched::RegPressure);

  for (MVT VT : MVT::integer_valuetypes()) {
    setLoadExtAction(ISD::EXTLOAD, VT, MVT::i1, Promote);
    setLoadExtAction(ISD::ZEXTLOAD, VT, MVT::i1, Promote);
    setLoadExtAction(ISD::SEXTLOAD, VT, MVT::i1, Promote);
  }
  setLoadExtAction(ISD::EXTLOAD, MVT::f32, MVT::f16, Expand);
  setLoadExtAction(ISD::EXTLOAD, MVT::f32, MVT::bf16, Expand);
  setTruncStoreAction(MVT::f32, MVT::f16, Expand);
  setTruncStoreAction(MVT::f32, MVT::bf16, Expand);

  // Integers
  setOperationAction({ISD::SDIVREM, ISD::UDIVREM, ISD::SMUL_LOHI,
                      ISD::UMUL_LOHI},
                     MVT::i32, Expand);
  setOperationAction({ISD::MULHS, ISD::MULHU, ISD::ROTL, ISD::ROTR, ISD::CTLZ,
                      ISD::CTTZ, ISD::CTPOP, ISD::BSWAP, ISD::BITREVERSE,
                      ISD::ABS, ISD::SMIN, ISD::SMAX, ISD::UMIN, ISD::UMAX},
                     MVT::i32, Legal);
  setOperationAction({ISD::FSHL, ISD::FSHR}, MVT::i32, Expand);
  setOperationAction({ISD::SHL_PARTS, ISD::SRL_PARTS, ISD::SRA_PARTS},
                     MVT::i32, Expand);
  setOperationAction({ISD::UADDO, ISD::USUBO, ISD::UADDO_CARRY,
                      ISD::USUBO_CARRY},
                     MVT::i32, Custom);
  setOperationAction({ISD::SELECT_CC, ISD::BR_CC}, {MVT::i32, MVT::f32},
                     Expand);
  setOperationAction(ISD::BR_JT, MVT::Other, Expand);
  setOperationAction({ISD::GlobalAddress, ISD::GlobalTLSAddress,
                      ISD::ExternalSymbol, ISD::BlockAddress, ISD::JumpTable,
                      ISD::ConstantPool},
                     MVT::i32, Custom);
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i32, Expand);
  setOperationAction({ISD::STACKSAVE, ISD::STACKRESTORE}, MVT::Other, Expand);
  setOperationAction(ISD::VASTART, MVT::Other, Custom);
  setOperationAction({ISD::VAARG, ISD::VACOPY, ISD::VAEND}, MVT::Other,
                     Expand);
  setOperationAction({ISD::FRAMEADDR, ISD::RETURNADDR}, MVT::i32, Custom);
  setOperationAction(ISD::INTRINSIC_VOID, MVT::Other, Custom);
  setOperationAction(ISD::TRAP, MVT::Other, Legal);
  setOperationAction(ISD::DEBUGTRAP, MVT::Other, Legal);

  // Atomics: words; narrower ones by the word's cmpxchg.
  setMaxAtomicSizeInBitsSupported(32);
  setMinCmpXchgSizeInBits(32);
  setOperationAction(ISD::ATOMIC_CMP_SWAP_WITH_SUCCESS, MVT::i32, Expand);
  setOperationAction(ISD::ATOMIC_LOAD_NAND, MVT::i32, Expand);

  // Single precision on the card's units; double in software (no register
  // class for f64: softened to libcalls).
  setOperationAction({ISD::FMA, ISD::FREM, ISD::FPOW, ISD::FSIN, ISD::FCOS,
                      ISD::FTAN, ISD::FSINCOS, ISD::FEXP, ISD::FEXP2,
                      ISD::FEXP10, ISD::FLOG, ISD::FLOG2, ISD::FLOG10,
                      ISD::FROUND, ISD::FMINIMUM, ISD::FMAXIMUM, ISD::LROUND,
                      ISD::LLROUND, ISD::LRINT, ISD::LLRINT, ISD::FLDEXP,
                      ISD::FFREXP, ISD::FP16_TO_FP, ISD::FP_TO_FP16,
                      ISD::FASIN, ISD::FACOS, ISD::FATAN, ISD::FATAN2,
                      ISD::FSINH, ISD::FCOSH, ISD::FTANH, ISD::FCANONICALIZE},
                     MVT::f32, Expand);
  // The card's approximate special functions under afn (-ffast-math), libm
  // otherwise.
  setOperationAction({ISD::FSIN, ISD::FCOS, ISD::FEXP2, ISD::FLOG2, ISD::FEXP,
                      ISD::FEXP10, ISD::FLOG, ISD::FLOG10},
                     MVT::f32, Custom);
  setOperationAction({ISD::FMAD, ISD::FMINNUM, ISD::FMAXNUM,
                      ISD::FMINIMUMNUM, ISD::FMAXIMUMNUM, ISD::FSQRT,
                      ISD::FFLOOR, ISD::FCEIL, ISD::FTRUNC, ISD::FROUNDEVEN,
                      ISD::FRINT, ISD::FNEARBYINT, ISD::FCOPYSIGN},
                     MVT::f32, Legal);
  setOperationAction({ISD::FP_TO_SINT_SAT, ISD::FP_TO_UINT_SAT}, MVT::i32,
                     Expand);
  setCondCodeAction({ISD::SETONE, ISD::SETUEQ, ISD::SETUGT, ISD::SETUGE,
                     ISD::SETULT, ISD::SETULE, ISD::SETUNE, ISD::SETO},
                    MVT::f32, Expand);

  setMinFunctionAlignment(Align(16));
  setPrefFunctionAlignment(Align(16));

  // Inline loads and stores for small copies (words, or texels when both
  // sides are 16-aligned: GPUVMSelectionDAGInfo).
  MaxStoresPerMemset = 8;
  MaxStoresPerMemsetOptSize = 4;
  MaxStoresPerMemcpy = 8;
  MaxStoresPerMemcpyOptSize = 4;
  MaxStoresPerMemmove = 8;
  MaxStoresPerMemmoveOptSize = 4;
}

//===----------------------------------------------------------------------===//
// Properties
//===----------------------------------------------------------------------===//

bool GPUVMTargetLowering::isLegalAddressingMode(const DataLayout &DL,
                                                const AddrMode &AM, Type *Ty,
                                                unsigned AS,
                                                Instruction *I) const {
  // base + imm32 (a symbol too), or base + (index << s).
  if (!isInt<32>(AM.BaseOffs) && !isUInt<32>(AM.BaseOffs))
    return false;
  if (AM.Scale == 0)
    return true;
  if (AM.BaseGV || AM.BaseOffs != 0)
    return false;
  if (AM.Scale < 0 || !isPowerOf2_64(AM.Scale) || AM.Scale > (1ll << 31))
    return false;
  // index << s alone has no base: a base of r0 serves (Scale 1 and a base
  // is reg + reg).
  return true;
}

bool GPUVMTargetLowering::isTruncateFree(Type *Ty1, Type *Ty2) const {
  return Ty1->isIntegerTy() && Ty2->isIntegerTy() &&
         Ty1->getPrimitiveSizeInBits() > Ty2->getPrimitiveSizeInBits();
}

bool GPUVMTargetLowering::isTruncateFree(EVT VT1, EVT VT2) const {
  return VT1.isInteger() && VT2.isInteger() &&
         VT1.getSizeInBits() > VT2.getSizeInBits();
}

bool GPUVMTargetLowering::allowsMisalignedMemoryAccesses(
    EVT VT, unsigned AddrSpace, Align Alignment, MachineMemOperand::Flags Flags,
    unsigned *Fast) const {
  // A byte, half or word access stays in its address's word (isa.h, since
  // x17): one not known to be in one word is split by its alignment. (d and q
  // take any alignment, but the code generator makes them only aligned.)
  if (Fast)
    *Fast = 0;
  return false;
}

static cl::opt<unsigned> OpThreshold(
    "gpuvm-op-threshold", cl::Hidden, cl::init(4096),
    cl::desc("gpu_vm: memcpy, memmove and memset of a known size from this "
             "many bytes up are the commit's operation"));
static cl::opt<unsigned> InlineTexels(
    "gpuvm-inline-texels", cl::Hidden, cl::init(8),
    cl::desc("gpu_vm: texels (16 bytes) a small 16-aligned copy or fill "
             "moves inline at most"));

unsigned llvm::getGPUVMOpThreshold() { return OpThreshold; }
unsigned llvm::getGPUVMInlineTexels() { return InlineTexels; }

static cl::opt<bool> GeneralOps(
    "gpuvm-general-ops", cl::Hidden, cl::init(false),
    cl::desc("gpu_vm: bfe, bfes and shadd where a shift and a mask or add "
             "would do (fewer ops, but the tick's general step)"));
bool llvm::gpuvmGeneralOps() { return GeneralOps; }

bool GPUVMTargetLowering::findOptimalMemOpLowering(
    LLVMContext &Context, std::vector<EVT> &MemOps, unsigned Limit,
    const MemOp &Op, unsigned DstAS, unsigned SrcAS,
    const AttributeList &FuncAttributes, EVT *LargestVT) const {
  // Large ones are the commit's operation; 16-aligned ones lq/sq
  // (GPUVMSelectionDAGInfo).
  if (Op.size() >= OpThreshold)
    return false;
  if (Op.size() >= 16 && Op.isAligned(Align(16)) &&
      Op.size() <= 16 * InlineTexels)
    return false;
  return TargetLowering::findOptimalMemOpLowering(
      Context, MemOps, Limit, Op, DstAS, SrcAS, FuncAttributes, LargestVT);
}

TargetLowering::AtomicExpansionKind
GPUVMTargetLowering::shouldExpandAtomicRMWInIR(const AtomicRMWInst *AI) const {
  if (AI->getType()->isIntegerTy(32) &&
      AI->getOperation() != AtomicRMWInst::Nand && !AI->isFloatingPointOperation())
    return AtomicExpansionKind::None;
  return AtomicExpansionKind::CmpXChg;
}

//===----------------------------------------------------------------------===//
// Inline asm
//===----------------------------------------------------------------------===//

TargetLowering::ConstraintType
GPUVMTargetLowering::getConstraintType(StringRef Constraint) const {
  if (Constraint.size() == 1 && Constraint[0] == 'r')
    return C_RegisterClass;
  return TargetLowering::getConstraintType(Constraint);
}

static unsigned abiRegister(StringRef Name) {
  return StringSwitch<unsigned>(Name)
      .Case("zero", 0)
      .Case("ra", 1)
      .Case("sp", 2)
      .Case("gp", 3)
      .Case("tp", 4)
      .Case("fp", 32)
      .Default(~0u);
}

// "rN" or an ABI name: its number, or ~0u.
static unsigned registerNumber(StringRef Name) {
  unsigned N;
  if (Name.consume_front("r") && !Name.getAsInteger(10, N) && N < 64)
    return N;
  unsigned A = abiRegister(Name);
  if (A != ~0u)
    return A;
  if (Name.consume_front("a") && !Name.getAsInteger(10, N) && N < 8)
    return 5 + N;
  if (Name.consume_front("s") && !Name.getAsInteger(10, N) && N < 24)
    return 32 + N;
  if (Name.consume_front("t") && !Name.getAsInteger(10, N) && N < 27)
    return N < 19 ? 13 + N : 56 + (N - 19);
  return ~0u;
}

std::pair<unsigned, const TargetRegisterClass *>
GPUVMTargetLowering::getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                                                  StringRef Constraint,
                                                  MVT VT) const {
  if (Constraint.size() == 1 && Constraint[0] == 'r')
    return std::make_pair(0U, &GPUVM::GPRRegClass);
  // {r5}, {a0}, {zero}
  if (Constraint.size() > 2 && Constraint.front() == '{' &&
      Constraint.back() == '}') {
    unsigned N = registerNumber(Constraint.slice(1, Constraint.size() - 1));
    if (N != ~0u)
      return std::make_pair(unsigned(GPUVM::R0 + N), &GPUVM::GPRRegClass);
  }
  return TargetLowering::getRegForInlineAsmConstraint(TRI, Constraint, VT);
}

Register
GPUVMTargetLowering::getRegisterByName(const char *RegName, LLT VT,
                                       const MachineFunction &MF) const {
  unsigned N = registerNumber(RegName);
  if (N == ~0u)
    reportFatalUsageError(Twine("invalid register name \"") + RegName + "\"");
  Register R = GPUVM::R0 + N;
  BitVector Reserved = Subtarget.getRegisterInfo()->getReservedRegs(MF);
  if (!Reserved.test(R))
    reportFatalUsageError(Twine("register \"") + RegName +
                          "\" is not reserved: use -ffixed-" + RegName);
  return R;
}

//===----------------------------------------------------------------------===//
// Calling convention
//===----------------------------------------------------------------------===//

static const MCPhysReg ArgRegs[] = {GPUVM::R5, GPUVM::R6,  GPUVM::R7,
                                    GPUVM::R8, GPUVM::R9,  GPUVM::R10,
                                    GPUVM::R11, GPUVM::R12};

static bool CC_GPUVM(unsigned ValNo, MVT ValVT, MVT LocVT,
                     CCValAssign::LocInfo LocInfo, ISD::ArgFlagsTy ArgFlags,
                     Type *OrigTy, CCState &State) {
  if (LocVT == MVT::i1 || LocVT == MVT::i8 || LocVT == MVT::i16) {
    LocVT = MVT::i32;
    if (ArgFlags.isSExt())
      LocInfo = CCValAssign::SExt;
    else if (ArgFlags.isZExt())
      LocInfo = CCValAssign::ZExt;
    else
      LocInfo = CCValAssign::AExt;
  }
  assert((LocVT == MVT::i32 || LocVT == MVT::f32) && "an argument of 32 bits");

  // Variadic arguments: on the stack.
  if (ArgFlags.isVarArg()) {
    unsigned Off = State.AllocateStack(4, Align(4));
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Off, LocVT, LocInfo));
    return false;
  }

  // The parts of a value split in several: all in registers, or all on the
  // stack.
  SmallVectorImpl<CCValAssign> &Pending = State.getPendingLocs();
  if (ArgFlags.isSplit() || !Pending.empty()) {
    Pending.push_back(
        CCValAssign::getPending(ValNo, ValVT, LocVT, LocInfo));
    if (!ArgFlags.isSplitEnd())
      return false;
    unsigned Free = std::size(ArgRegs) - State.getFirstUnallocated(ArgRegs);
    bool InRegs = Pending.size() <= Free;
    for (CCValAssign &VA : Pending) {
      if (InRegs)
        State.addLoc(CCValAssign::getReg(VA.getValNo(), VA.getValVT(),
                                         State.AllocateReg(ArgRegs),
                                         VA.getLocVT(), VA.getLocInfo()));
      else
        State.addLoc(CCValAssign::getMem(VA.getValNo(), VA.getValVT(),
                                         State.AllocateStack(4, Align(4)),
                                         VA.getLocVT(), VA.getLocInfo()));
    }
    Pending.clear();
    return false;
  }

  if (MCRegister Reg = State.AllocateReg(ArgRegs)) {
    State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
    return false;
  }
  unsigned Off = State.AllocateStack(4, Align(4));
  State.addLoc(CCValAssign::getMem(ValNo, ValVT, Off, LocVT, LocInfo));
  return false;
}

static SDValue convertLocToVal(SelectionDAG &DAG, const SDLoc &DL,
                               const CCValAssign &VA, SDValue V) {
  switch (VA.getLocInfo()) {
  case CCValAssign::SExt:
    V = DAG.getNode(ISD::AssertSext, DL, VA.getLocVT(), V,
                    DAG.getValueType(VA.getValVT()));
    break;
  case CCValAssign::ZExt:
    V = DAG.getNode(ISD::AssertZext, DL, VA.getLocVT(), V,
                    DAG.getValueType(VA.getValVT()));
    break;
  default:
    break;
  }
  if (VA.getValVT() != VA.getLocVT())
    V = DAG.getNode(ISD::TRUNCATE, DL, VA.getValVT(), V);
  return V;
}

static SDValue convertValToLoc(SelectionDAG &DAG, const SDLoc &DL,
                               const CCValAssign &VA, SDValue V) {
  switch (VA.getLocInfo()) {
  case CCValAssign::Full:
    return V;
  case CCValAssign::SExt:
    return DAG.getNode(ISD::SIGN_EXTEND, DL, VA.getLocVT(), V);
  case CCValAssign::ZExt:
    return DAG.getNode(ISD::ZERO_EXTEND, DL, VA.getLocVT(), V);
  case CCValAssign::AExt:
    return DAG.getNode(ISD::ANY_EXTEND, DL, VA.getLocVT(), V);
  default:
    llvm_unreachable("unknown loc info");
  }
}

SDValue GPUVMTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &DL,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();

  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeFormalArguments(Ins, CC_GPUVM);

  for (const CCValAssign &VA : ArgLocs) {
    SDValue V;
    if (VA.isRegLoc()) {
      Register VReg = RegInfo.createVirtualRegister(&GPUVM::GPRRegClass);
      RegInfo.addLiveIn(VA.getLocReg(), VReg);
      V = DAG.getCopyFromReg(Chain, DL, VReg, VA.getLocVT());
    } else {
      assert(VA.isMemLoc());
      int FI = MFI.CreateFixedObject(4, VA.getLocMemOffset(),
                                     /*IsImmutable=*/true);
      SDValue FIN = DAG.getFrameIndex(FI, MVT::i32);
      V = DAG.getLoad(VA.getLocVT(), DL, Chain, FIN,
                      MachinePointerInfo::getFixedStack(MF, FI));
      // A part of a vector is not the vector's memory (each part has a slot
      // of its own): not a load the argument's copy may be elided into.
      if (Ins[VA.getValNo()].ArgVT.isVector())
        V = DAG.getFreeze(V);
    }
    InVals.push_back(convertLocToVal(DAG, DL, VA, V));
  }

  if (IsVarArg) {
    auto *FuncInfo = MF.getInfo<GPUVMMachineFunctionInfo>();
    FuncInfo->setVarArgsFrameIndex(
        MFI.CreateFixedObject(4, CCInfo.getStackSize(), true));
  }
  return Chain;
}

bool GPUVMTargetLowering::CanLowerReturn(
    CallingConv::ID CallConv, MachineFunction &MF, bool IsVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs, LLVMContext &Context,
    const Type *RetTy) const {
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, RVLocs, Context);
  return CCInfo.CheckReturn(Outs, RetCC_GPUVM);
}

SDValue
GPUVMTargetLowering::LowerReturn(SDValue Chain, CallingConv::ID CallConv,
                                 bool IsVarArg,
                                 const SmallVectorImpl<ISD::OutputArg> &Outs,
                                 const SmallVectorImpl<SDValue> &OutVals,
                                 const SDLoc &DL, SelectionDAG &DAG) const {
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, IsVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());
  CCInfo.AnalyzeReturn(Outs, RetCC_GPUVM);

  SDValue Glue;
  SmallVector<SDValue, 4> RetOps(1, Chain);
  for (unsigned I = 0; I != RVLocs.size(); ++I) {
    CCValAssign &VA = RVLocs[I];
    assert(VA.isRegLoc() && "results in registers only");
    SDValue V = convertValToLoc(DAG, DL, VA, OutVals[I]);
    Chain = DAG.getCopyToReg(Chain, DL, VA.getLocReg(), V, Glue);
    Glue = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }
  RetOps[0] = Chain;
  if (Glue.getNode())
    RetOps.push_back(Glue);
  return DAG.getNode(GPUVMISD::RET_GLUE, DL, MVT::Other, RetOps);
}

SDValue GPUVMTargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                                       SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG = CLI.DAG;
  SDLoc &DL = CLI.DL;
  SmallVectorImpl<ISD::OutputArg> &Outs = CLI.Outs;
  SmallVectorImpl<SDValue> &OutVals = CLI.OutVals;
  SmallVectorImpl<ISD::InputArg> &Ins = CLI.Ins;
  SDValue Chain = CLI.Chain;
  SDValue Callee = CLI.Callee;
  CallingConv::ID CallConv = CLI.CallConv;
  bool IsVarArg = CLI.IsVarArg;
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  EVT PtrVT = getPointerTy(DAG.getDataLayout());

  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, IsVarArg, MF, ArgLocs, *DAG.getContext());
  CCInfo.AnalyzeCallOperands(Outs, CC_GPUVM);
  unsigned NumBytes = CCInfo.getStackSize();

  // A tail call (j to the callee after the epilogue): its arguments in
  // registers alone, nothing of the caller's frame passed.
  if (CLI.IsTailCall) {
    const Function &Caller = MF.getFunction();
    bool OK = NumBytes == 0 && !IsVarArg && !Caller.isVarArg() &&
              !Caller.hasStructRetAttr() && CallConv == Caller.getCallingConv() &&
              !Caller.getFnAttribute("disable-tail-calls").getValueAsBool() &&
              !(CLI.CB && CLI.CB->isMustTailCall() && NumBytes);
    for (const ISD::OutputArg &O : Outs)
      if (O.Flags.isByVal() || O.Flags.isSRet())
        OK = false;
    // the results must be where the caller's are
    SmallVector<CCValAssign, 4> RVLocs;
    CCState RetCCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
    RetCCInfo.AnalyzeCallResult(Ins, RetCC_GPUVM);
    for (const CCValAssign &VA : RVLocs)
      if (!VA.isRegLoc())
        OK = false;
    CLI.IsTailCall = OK;
  }
  if (CLI.IsTailCall)
    MFI.setHasTailCall();

  // byval arguments: a copy, passed by its address.
  SmallVector<SDValue, 8> ByValArgs;
  for (unsigned I = 0, E = Outs.size(); I != E; ++I) {
    ISD::ArgFlagsTy Flags = Outs[I].Flags;
    if (!Flags.isByVal())
      continue;
    unsigned Size = Flags.getByValSize();
    Align Alignment = Flags.getNonZeroByValAlign();
    int FI = MFI.CreateStackObject(Size, Alignment, false);
    SDValue FIPtr = DAG.getFrameIndex(FI, PtrVT);
    Chain = DAG.getMemcpy(Chain, DL, FIPtr, OutVals[I],
                          DAG.getConstant(Size, DL, MVT::i32), Alignment,
                          Alignment, /*isVol=*/false, /*AlwaysInline=*/false,
                          /*CI=*/nullptr, std::nullopt, MachinePointerInfo(),
                          MachinePointerInfo());
    ByValArgs.push_back(FIPtr);
  }

  if (!CLI.IsTailCall)
    Chain = DAG.getCALLSEQ_START(Chain, NumBytes, 0, DL);

  SmallVector<std::pair<Register, SDValue>, 8> RegsToPass;
  SmallVector<SDValue, 8> MemOpChains;
  SDValue StackPtr;
  for (unsigned I = 0, J = 0, E = ArgLocs.size(); I != E; ++I) {
    CCValAssign &VA = ArgLocs[I];
    SDValue Arg = OutVals[I];
    if (Outs[I].Flags.isByVal())
      Arg = ByValArgs[J++];
    else
      Arg = convertValToLoc(DAG, DL, VA, Arg);

    if (VA.isRegLoc()) {
      RegsToPass.push_back(std::make_pair(VA.getLocReg(), Arg));
      continue;
    }
    if (!StackPtr.getNode())
      StackPtr = DAG.getCopyFromReg(Chain, DL, GPUVM::R2, PtrVT);
    SDValue Addr = DAG.getNode(ISD::ADD, DL, PtrVT, StackPtr,
                               DAG.getIntPtrConstant(VA.getLocMemOffset(), DL));
    MemOpChains.push_back(DAG.getStore(
        Chain, DL, Arg, Addr,
        MachinePointerInfo::getStack(MF, VA.getLocMemOffset())));
  }
  if (!MemOpChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, DL, MVT::Other, MemOpChains);

  SDValue Glue;
  for (auto &[Reg, Val] : RegsToPass) {
    Chain = DAG.getCopyToReg(Chain, DL, Reg, Val, Glue);
    Glue = Chain.getValue(1);
  }

  if (auto *G = dyn_cast<GlobalAddressSDNode>(Callee))
    Callee = DAG.getTargetGlobalAddress(G->getGlobal(), DL, PtrVT,
                                        G->getOffset());
  else if (auto *S = dyn_cast<ExternalSymbolSDNode>(Callee))
    Callee = DAG.getTargetExternalSymbol(S->getSymbol(), PtrVT);

  SmallVector<SDValue, 12> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);
  for (auto &[Reg, Val] : RegsToPass)
    Ops.push_back(DAG.getRegister(Reg, Val.getValueType()));
  const uint32_t *Mask =
      Subtarget.getRegisterInfo()->getCallPreservedMask(MF, CallConv);
  Ops.push_back(DAG.getRegisterMask(Mask));
  if (Glue.getNode())
    Ops.push_back(Glue);

  if (CLI.IsTailCall)
    return DAG.getNode(GPUVMISD::TAIL, DL, MVT::Other, Ops);

  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  Chain = DAG.getNode(GPUVMISD::CALL, DL, NodeTys, Ops);
  Glue = Chain.getValue(1);

  Chain = DAG.getCALLSEQ_END(Chain, NumBytes, 0, Glue, DL);
  Glue = Chain.getValue(1);

  // The results
  SmallVector<CCValAssign, 16> RVLocs;
  CCState RetCCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
  RetCCInfo.AnalyzeCallResult(Ins, RetCC_GPUVM);
  for (const CCValAssign &VA : RVLocs) {
    SDValue V =
        DAG.getCopyFromReg(Chain, DL, VA.getLocReg(), VA.getLocVT(), Glue);
    Chain = V.getValue(1);
    Glue = V.getValue(2);
    InVals.push_back(convertLocToVal(DAG, DL, VA, V));
  }
  return Chain;
}

//===----------------------------------------------------------------------===//
// Custom lowering
//===----------------------------------------------------------------------===//

SDValue GPUVMTargetLowering::LowerOperation(SDValue Op,
                                            SelectionDAG &DAG) const {
  switch (Op.getOpcode()) {
  case ISD::GlobalAddress:
  case ISD::ExternalSymbol:
  case ISD::BlockAddress:
  case ISD::JumpTable:
  case ISD::ConstantPool:
    return lowerAddress(Op, DAG);
  case ISD::GlobalTLSAddress:
    return lowerGlobalTLSAddress(Op, DAG);
  case ISD::VASTART:
    return lowerVASTART(Op, DAG);
  case ISD::UADDO:
  case ISD::USUBO:
  case ISD::UADDO_CARRY:
  case ISD::USUBO_CARRY:
    return lowerCarryOp(Op, DAG);
  case ISD::FRAMEADDR:
    return lowerFRAMEADDR(Op, DAG);
  case ISD::RETURNADDR:
    return lowerRETURNADDR(Op, DAG);
  case ISD::FSIN:
  case ISD::FCOS:
  case ISD::FEXP2:
  case ISD::FLOG2:
  case ISD::FEXP:
  case ISD::FEXP10:
  case ISD::FLOG:
  case ISD::FLOG10:
    return lowerApproxFunc(Op, DAG);
  case ISD::INTRINSIC_VOID:
    return lowerINTRINSIC_VOID(Op, DAG);
  default:
    llvm_unreachable("gpu_vm: no custom lowering for this operation");
  }
}

// A symbol's address is an immediate (absolute: the long immediate's
// relocation).
SDValue GPUVMTargetLowering::lowerAddress(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT VT = Op.getValueType();
  SDValue T;
  switch (Op.getOpcode()) {
  case ISD::GlobalAddress: {
    auto *N = cast<GlobalAddressSDNode>(Op);
    T = DAG.getTargetGlobalAddress(N->getGlobal(), DL, VT, N->getOffset());
    break;
  }
  case ISD::ExternalSymbol:
    T = DAG.getTargetExternalSymbol(cast<ExternalSymbolSDNode>(Op)->getSymbol(),
                                    VT);
    break;
  case ISD::BlockAddress: {
    auto *N = cast<BlockAddressSDNode>(Op);
    T = DAG.getTargetBlockAddress(N->getBlockAddress(), VT, N->getOffset());
    break;
  }
  case ISD::JumpTable:
    T = DAG.getTargetJumpTable(cast<JumpTableSDNode>(Op)->getIndex(), VT);
    break;
  case ISD::ConstantPool: {
    auto *N = cast<ConstantPoolSDNode>(Op);
    if (N->isMachineConstantPoolEntry())
      T = DAG.getTargetConstantPool(N->getMachineCPVal(), VT, N->getAlign(),
                                    N->getOffset());
    else
      T = DAG.getTargetConstantPool(N->getConstVal(), VT, N->getAlign(),
                                    N->getOffset());
    break;
  }
  default:
    llvm_unreachable("not an address");
  }
  // Position-independent code: from the bundle's address (lpc, R_GVM_PCIMM32),
  // so that code has no relocation; data words that hold addresses keep
  // theirs.
  if (isPositionIndependent())
    return DAG.getNode(GPUVMISD::PCADDR, DL, VT, T);
  return DAG.getNode(GPUVMISD::ADDR, DL, VT, T);
}

// Local exec, whatever the model asked for (static links only): tp +
// %tprel(sym).
SDValue GPUVMTargetLowering::lowerGlobalTLSAddress(SDValue Op,
                                                   SelectionDAG &DAG) const {
  SDLoc DL(Op);
  EVT VT = Op.getValueType();
  auto *N = cast<GlobalAddressSDNode>(Op);
  SDValue Off = DAG.getNode(
      GPUVMISD::ADDR, DL, VT,
      DAG.getTargetGlobalAddress(N->getGlobal(), DL, VT, N->getOffset(),
                                 GPUVMII::MO_TPREL));
  SDValue TP = DAG.getCopyFromReg(DAG.getEntryNode(), DL, GPUVM::R4, VT);
  return DAG.getNode(ISD::ADD, DL, VT, TP, Off);
}

SDValue GPUVMTargetLowering::lowerVASTART(SDValue Op, SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  auto *FuncInfo = MF.getInfo<GPUVMMachineFunctionInfo>();
  SDLoc DL(Op);
  SDValue FI = DAG.getFrameIndex(FuncInfo->getVarArgsFrameIndex(),
                                 getPointerTy(MF.getDataLayout()));
  const Value *SV = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();
  return DAG.getStore(Op.getOperand(0), DL, FI, Op.getOperand(1),
                      MachinePointerInfo(SV));
}

// The carries of 64-bit sums: addcy/carry, subby/borrow.
SDValue GPUVMTargetLowering::lowerCarryOp(SDValue Op, SelectionDAG &DAG) const {
  SDLoc DL(Op);
  SDValue A = Op.getOperand(0), B = Op.getOperand(1);
  bool HasIn = Op.getOpcode() == ISD::UADDO_CARRY ||
               Op.getOpcode() == ISD::USUBO_CARRY;
  SDValue In = HasIn ? DAG.getZExtOrTrunc(Op.getOperand(2), DL, MVT::i32)
                     : DAG.getConstant(0, DL, MVT::i32);
  bool Add = Op.getOpcode() == ISD::UADDO || Op.getOpcode() == ISD::UADDO_CARRY;
  SDValue V, C;
  if (Add) {
    V = HasIn ? DAG.getNode(GPUVMISD::ADDCY, DL, MVT::i32, A, B, In)
              : DAG.getNode(ISD::ADD, DL, MVT::i32, A, B);
    C = DAG.getNode(GPUVMISD::CARRY, DL, MVT::i32, A, B, In);
  } else {
    V = HasIn ? DAG.getNode(GPUVMISD::SUBBY, DL, MVT::i32, A, B, In)
              : DAG.getNode(ISD::SUB, DL, MVT::i32, A, B);
    C = DAG.getNode(GPUVMISD::BORROW, DL, MVT::i32, A, B, In);
  }
  C = DAG.getZExtOrTrunc(C, DL, Op->getValueType(1));
  return DAG.getMergeValues({V, C}, DL);
}

SDValue GPUVMTargetLowering::lowerFRAMEADDR(SDValue Op,
                                            SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MF.getFrameInfo().setFrameAddressIsTaken(true);
  SDLoc DL(Op);
  EVT VT = Op.getValueType();
  SDValue FrameAddr = DAG.getCopyFromReg(DAG.getEntryNode(), DL, GPUVM::R32, VT);
  unsigned Depth = Op.getConstantOperandVal(0);
  // No frame record (fp is the caller's sp): deeper frames are not known.
  if (Depth != 0)
    return DAG.getConstant(0, DL, VT);
  return FrameAddr;
}

SDValue GPUVMTargetLowering::lowerRETURNADDR(SDValue Op,
                                             SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  SDLoc DL(Op);
  EVT VT = Op.getValueType();
  if (Op.getConstantOperandVal(0) != 0)
    return DAG.getConstant(0, DL, VT);
  MF.getFrameInfo().setReturnAddressIsTaken(true);
  Register Reg = MF.addLiveIn(GPUVM::R1, &GPUVM::GPRRegClass);
  return DAG.getCopyFromReg(DAG.getEntryNode(), DL, Reg, VT);
}

SDValue GPUVMTargetLowering::emitOperation(SelectionDAG &DAG, const SDLoc &DL,
                                           SDValue Chain, uint32_t Kind,
                                           SDValue Dst, SDValue Src,
                                           SDValue Size) {
  SDValue Ops[] = {Chain, DAG.getConstant(Kind, DL, MVT::i32),
                   DAG.getZExtOrTrunc(Dst, DL, MVT::i32),
                   DAG.getZExtOrTrunc(Src, DL, MVT::i32),
                   DAG.getZExtOrTrunc(Size, DL, MVT::i32)};
  return DAG.getNode(GPUVMISD::OPERATION, DL, MVT::Other, Ops);
}

SDValue GPUVMTargetLowering::lowerINTRINSIC_VOID(SDValue Op,
                                                 SelectionDAG &DAG) const {
  SDLoc DL(Op);
  unsigned IntNo = Op.getConstantOperandVal(1);
  switch (IntNo) {
  case Intrinsic::gpuvm_copy:
    return emitOperation(DAG, DL, Op.getOperand(0), GPUVM::OpCopy,
                         Op.getOperand(2), Op.getOperand(3), Op.getOperand(4));
  case Intrinsic::gpuvm_fill:
    return emitOperation(DAG, DL, Op.getOperand(0), GPUVM::OpFill,
                         Op.getOperand(2), Op.getOperand(3), Op.getOperand(4));
  default:
    return SDValue();
  }
}

// sin, cos, exp2, log2 are the card's (approximate) ops under afn; exp, exp10,
// log, log10 through exp2 and log2. Without afn: libm.
SDValue GPUVMTargetLowering::lowerApproxFunc(SDValue Op,
                                             SelectionDAG &DAG) const {
  SDNodeFlags Flags = Op->getFlags();
  if (!Flags.hasApproximateFuncs())
    return SDValue();
  SDLoc DL(Op);
  EVT VT = Op.getValueType();
  SDValue X = Op.getOperand(0);
  auto Mul = [&](SDValue V, double K) {
    return DAG.getNode(ISD::FMUL, DL, VT, V, DAG.getConstantFP(K, DL, VT),
                       Flags);
  };
  switch (Op.getOpcode()) {
  case ISD::FEXP: // e^x = 2^(x log2 e)
    return DAG.getNode(ISD::FEXP2, DL, VT, Mul(X, 1.4426950408889634), Flags);
  case ISD::FEXP10: // 10^x = 2^(x log2 10)
    return DAG.getNode(ISD::FEXP2, DL, VT, Mul(X, 3.3219280948873622), Flags);
  case ISD::FLOG: // ln x = log2 x ln 2
    return Mul(DAG.getNode(ISD::FLOG2, DL, VT, X, Flags), 0.6931471805599453);
  case ISD::FLOG10: // log10 x = log2 x log10 2
    return Mul(DAG.getNode(ISD::FLOG2, DL, VT, X, Flags), 0.3010299956639812);
  default:
    return Op; // the op itself
  }
}
