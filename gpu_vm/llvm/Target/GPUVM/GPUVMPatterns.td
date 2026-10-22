//===-- GPUVMPatterns.td - gpu_vm selection patterns -------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The code generator's part of the instruction set: target nodes, pseudos and
// the selection patterns over the ops of GPUVMInstrInfo.td. An I form's
// immediate (imm32) takes any 32-bit value or a symbol: the encoder makes it
// short or long, so every constant and every address is one op.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Target nodes
//===----------------------------------------------------------------------===//

def SDT_GPUVMCallSeqStart : SDCallSeqStart<[SDTCisVT<0, i32>,
                                            SDTCisVT<1, i32>]>;
def SDT_GPUVMCallSeqEnd : SDCallSeqEnd<[SDTCisVT<0, i32>, SDTCisVT<1, i32>]>;
def SDT_GPUVMCall : SDTypeProfile<0, -1, [SDTCisVT<0, i32>]>;
def SDT_GPUVMCarry : SDTypeProfile<1, 3, [SDTCisVT<0, i32>, SDTCisVT<1, i32>,
                                          SDTCisVT<2, i32>, SDTCisVT<3, i32>]>;
def SDT_GPUVMLQ : SDTypeProfile<4, 1, [SDTCisVT<0, i32>, SDTCisVT<1, i32>,
                                       SDTCisVT<2, i32>, SDTCisVT<3, i32>,
                                       SDTCisVT<4, i32>]>;
def SDT_GPUVMSQ : SDTypeProfile<0, 5, [SDTCisVT<0, i32>, SDTCisVT<1, i32>,
                                       SDTCisVT<2, i32>, SDTCisVT<3, i32>,
                                       SDTCisVT<4, i32>]>;

def callseq_start : SDNode<"ISD::CALLSEQ_START", SDT_GPUVMCallSeqStart,
                           [SDNPHasChain, SDNPOutGlue]>;
def callseq_end : SDNode<"ISD::CALLSEQ_END", SDT_GPUVMCallSeqEnd,
                         [SDNPHasChain, SDNPOptInGlue, SDNPOutGlue]>;

// A call: the callee, the registers it reads, a register mask.
def gpuvm_call : SDNode<"GPUVMISD::CALL", SDT_GPUVMCall,
                        [SDNPHasChain, SDNPOptInGlue, SDNPOutGlue,
                         SDNPVariadic]>;
// The return: jr ra.
def gpuvm_ret : SDNode<"GPUVMISD::RET_GLUE", SDTNone,
                       [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;
// A symbol's address (a global, an external symbol, a block address, a jump
// table, a constant pool entry, a TLS offset): an immediate of an op.
def gpuvm_addr : SDNode<"GPUVMISD::ADDR", SDTIntUnaryOp>;
// The carry ops (ra, rb, the carry in).
def gpuvm_addcy : SDNode<"GPUVMISD::ADDCY", SDT_GPUVMCarry>;
def gpuvm_carry : SDNode<"GPUVMISD::CARRY", SDT_GPUVMCarry>;
def gpuvm_subby : SDNode<"GPUVMISD::SUBBY", SDT_GPUVMCarry>;
def gpuvm_borrow : SDNode<"GPUVMISD::BORROW", SDT_GPUVMCarry>;
// A texel: lq's four words from an address, sq's four words to one (the
// inline copies and the commit's operations). Selected by hand
// (GPUVMISelDAGToDAG.cpp): the four registers are a GPRQ tuple.
def gpuvm_lq : SDNode<"GPUVMISD::LQ", SDT_GPUVMLQ,
                      [SDNPHasChain, SDNPMayLoad, SDNPMemOperand]>;
def gpuvm_sq : SDNode<"GPUVMISD::SQ", SDT_GPUVMSQ,
                      [SDNPHasChain, SDNPMayStore, SDNPMemOperand]>;
// The commit's operation {kind, dst, src or word, bytes}: sq of the
// descriptor to 0xf0000000 (SQ_OP, which keeps its place among every other
// access). Selected by hand.
def SDT_GPUVMOp : SDTypeProfile<0, 4, [SDTCisVT<0, i32>, SDTCisVT<1, i32>,
                                       SDTCisVT<2, i32>, SDTCisVT<3, i32>]>;
def gpuvm_op : SDNode<"GPUVMISD::OPERATION", SDT_GPUVMOp,
                      [SDNPHasChain, SDNPSideEffect, SDNPMayLoad,
                       SDNPMayStore]>;

//===----------------------------------------------------------------------===//
// Operands and leaves
//===----------------------------------------------------------------------===//

// Addresses: base + imm32 (a constant or a symbol), base + (index << sh).
def AddrRI : ComplexPattern<iPTR, 2, "selectAddrRI", [frameindex], []>;
def AddrRR : ComplexPattern<iPTR, 3, "selectAddrRR", [], [], 20>;

def i32imm_t : Operand<i32>;

// imm + 1, for a > imm as a >= imm + 1.
def ImmPlus1 : SDNodeXForm<imm, [{
  return CurDAG->getTargetConstant(static_cast<uint32_t>(N->getZExtValue() + 1),
                                   SDLoc(N), MVT::i32);
}]>;
def notSMax : ImmLeaf<i32, [{ return Imm != INT32_MAX; }]>;
def notUMax : ImmLeaf<i32, [{ return (uint32_t)Imm != UINT32_MAX; }]>;
// imm6 for the control word's compares (the unsigned forms sign-extend it)
def simm6Plus1 : ImmLeaf<i32, [{ return isInt<6>(Imm + 1); }]>;
def usimm6 : ImmLeaf<i32, [{ return isInt<6>(Imm); }]>;
def usimm6Plus1 : ImmLeaf<i32, [{
  return (uint32_t)Imm != UINT32_MAX && isInt<6>((int32_t)((uint32_t)Imm + 1));
}]>;
def toTImm : SDNodeXForm<imm, [{
  return CurDAG->getSignedTargetConstant(N->getSExtValue(), SDLoc(N), MVT::i32);
}]>;
// A single bit set: its number.
def pow2imm : ImmLeaf<i32, [{ return isPowerOf2_32((uint32_t)Imm); }]>;
def Log2 : SDNodeXForm<imm, [{
  return CurDAG->getTargetConstant(Log2_32((uint32_t)N->getZExtValue()),
                                   SDLoc(N), MVT::i32);
}]>;
// A low mask of 12 to 31 bits: bfe from 0 (andi would take a long immediate).
def lowmask12 : ImmLeaf<i32, [{
  return isMask_32((uint32_t)Imm) && !isInt<12>(Imm);
}]>;
def MaskLen : SDNodeXForm<imm, [{
  return CurDAG->getTargetConstant(llvm::popcount((uint32_t)N->getZExtValue()),
                                   SDLoc(N), MVT::i32);
}]>;
// A float constant's bits.
def FPBits : SDNodeXForm<fpimm, [{
  return CurDAG->getTargetConstant(
      N->getValueAPF().bitcastToAPInt().getZExtValue(), SDLoc(N), MVT::i32);
}]>;
def fpimm32 : FPImmLeaf<f32, [{ return true; }]>;

//===----------------------------------------------------------------------===//
// Pseudos
//===----------------------------------------------------------------------===//

let Defs = [R2], Uses = [R2] in {
def ADJCALLSTACKDOWN : GPUVMPseudo<(outs), (ins i32imm_t:$amt1, i32imm_t:$amt2),
                                   [(callseq_start timm:$amt1, timm:$amt2)]>;
def ADJCALLSTACKUP : GPUVMPseudo<(outs), (ins i32imm_t:$amt1, i32imm_t:$amt2),
                                 [(callseq_end timm:$amt1, timm:$amt2)]>;
}

// jr ra
// ra is callee-saved: live out of every return without a use here.
let isReturn = 1, isTerminator = 1, isBarrier = 1,
    hasSideEffects = 0, mayLoad = 0, mayStore = 0 in
def PseudoRET : GPUVMPseudo<(outs), (ins), [(gpuvm_ret)]>;

// lq and sq on a GPRQ tuple: the MC instruction names its first register.
let isCodeGenOnly = 1, hasSideEffects = 0 in {
let mayLoad = 1, mayStore = 0 in
def LQ_Q : GVMOp<0x84, FrmLoad, (outs GPRQ:$rd), (ins GPR:$ra, imm32:$imm),
                 "lq", "$rd, ${imm}(${ra})">;
let mayLoad = 0, mayStore = 1 in
def SQ_Q : GVMOp<0x8c, FrmStore, (outs), (ins GPRQ:$rs, GPR:$ra, imm32:$imm),
                 "sq", "$rs, ${imm}(${ra})">;
// A descriptor to the commit's operations: ordered against every access.
let mayLoad = 1, mayStore = 1, hasSideEffects = 1 in
def SQ_OP : GVMOp<0x8c, FrmStore, (outs), (ins GPRQ:$rs, GPR:$ra, imm32:$imm),
                  "sq", "$rs, ${imm}(${ra})">;
}

def : Pat<(trap), (BREAK)>;
def : Pat<(debugtrap), (BREAK)>;

//===----------------------------------------------------------------------===//
// Integer
//===----------------------------------------------------------------------===//

def : Pat<(i32 imm32:$imm), (ADDI (i32 R0), imm32:$imm)>;

multiclass PatRI<SDPatternOperator Op, Instruction R, Instruction I> {
  def : Pat<(Op (i32 GPR:$a), (i32 GPR:$b)), (R GPR:$a, GPR:$b)>;
  def : Pat<(Op (i32 GPR:$a), imm32:$imm), (I GPR:$a, imm32:$imm)>;
}
defm : PatRI<add, ADD, ADDI>;
defm : PatRI<and, AND, ANDI>;
defm : PatRI<or, OR, ORI>;
defm : PatRI<xor, XOR, XORI>;
defm : PatRI<mul, MUL, MULI>;
defm : PatRI<shl, SLL, SLLI>;
defm : PatRI<srl, SRL, SRLI>;
defm : PatRI<sra, SRA, SRAI>;
defm : PatRI<rotl, ROL, ROLI>;
defm : PatRI<rotr, ROR, RORI>;
defm : PatRI<smin, MIN, MINI>;
defm : PatRI<smax, MAX, MAXI>;
defm : PatRI<umin, MINU, MINUI>;
defm : PatRI<umax, MAXU, MAXUI>;
def : Pat<(sub (i32 GPR:$a), (i32 GPR:$b)), (SUB GPR:$a, GPR:$b)>;
def : Pat<(sub (i32 GPR:$a), imm32:$imm), (SUBI GPR:$a, imm32:$imm)>;
def : Pat<(sub 0, (i32 GPR:$a)), (SUB (i32 R0), GPR:$a)>;
def : Pat<(sub imm32:$imm, (i32 GPR:$a)),
          (SUB (ADDI (i32 R0), imm32:$imm), GPR:$a)>;

def : Pat<(and (i32 GPR:$a), (not (i32 GPR:$b))), (ANDN GPR:$a, GPR:$b)>;
def : Pat<(or (i32 GPR:$a), (not (i32 GPR:$b))), (ORN GPR:$a, GPR:$b)>;
def : Pat<(not (xor (i32 GPR:$a), (i32 GPR:$b))), (XNOR GPR:$a, GPR:$b)>;
// The tick's fast loop runs bundles whose op 0 is a mid integer op (functions
// 0-19) or a load or store of a word or less, and op 1 lite (0-7, the three
// shifts) or such a memory op: two of those ops in a bundle cost less than one
// op of the general step (shadd, bfe, sel, the carries, the extended class).
// By default a shift and a mask, a shift and an add, two shifts are made;
// -gpuvm-general-ops takes the one op instead.
def GeneralOps : Predicate<"llvm::gpuvmGeneralOps()">;
def lowmask : ImmLeaf<i32, [{ return isMask_32((uint32_t)Imm); }]>;
let Predicates = [GeneralOps], AddedComplexity = 1 in {
def : Pat<(and (i32 GPR:$a), lowmask12:$m), (BFE GPR:$a, 0, (MaskLen $m))>;
// shadd: a + (b << sh)
def : Pat<(add (i32 GPR:$a), (shl (i32 GPR:$b), uimm5:$sh)),
          (SHADD GPR:$a, GPR:$b, uimm5:$sh)>;
// bfe: a field, one op for the shift and the mask
def : Pat<(and (srl (i32 GPR:$a), uimm5:$pos), lowmask:$m),
          (BFE GPR:$a, uimm5:$pos, (MaskLen $m))>;
def : Pat<(sext_inreg (i32 GPR:$a), i1), (BFES GPR:$a, 0, 1)>;
def : Pat<(sext_inreg (i32 GPR:$a), i8), (BFES GPR:$a, 0, 8)>;
def : Pat<(sext_inreg (i32 GPR:$a), i16), (BFES GPR:$a, 0, 16)>;
}
def : Pat<(sext_inreg (i32 GPR:$a), i1), (SRAI (SLLI GPR:$a, 31), 31)>;
def : Pat<(sext_inreg (i32 GPR:$a), i8), (SRAI (SLLI GPR:$a, 24), 24)>;
def : Pat<(sext_inreg (i32 GPR:$a), i16), (SRAI (SLLI GPR:$a, 16), 16)>;

def : Pat<(abs (i32 GPR:$a)), (MAX GPR:$a, (SUB (i32 R0), GPR:$a))>;

def : Pat<(mulhs (i32 GPR:$a), (i32 GPR:$b)), (MULH GPR:$a, GPR:$b)>;
def : Pat<(mulhu (i32 GPR:$a), (i32 GPR:$b)), (MULHU GPR:$a, GPR:$b)>;
def : Pat<(sdiv (i32 GPR:$a), (i32 GPR:$b)), (DIV GPR:$a, GPR:$b)>;
def : Pat<(udiv (i32 GPR:$a), (i32 GPR:$b)), (DIVU GPR:$a, GPR:$b)>;
def : Pat<(srem (i32 GPR:$a), (i32 GPR:$b)), (REM GPR:$a, GPR:$b)>;
def : Pat<(urem (i32 GPR:$a), (i32 GPR:$b)), (REMU GPR:$a, GPR:$b)>;
def : Pat<(ctlz (i32 GPR:$a)), (CLZ GPR:$a)>;
def : Pat<(cttz (i32 GPR:$a)), (CTZ GPR:$a)>;
def : Pat<(ctpop (i32 GPR:$a)), (POPC GPR:$a)>;
def : Pat<(bswap (i32 GPR:$a)), (BSWAP GPR:$a)>;
def : Pat<(bitreverse (i32 GPR:$a)), (BREV GPR:$a)>;

// The carry ops
def : Pat<(gpuvm_addcy (i32 GPR:$a), (i32 GPR:$b), (i32 GPR:$c)),
          (ADDCY GPR:$a, GPR:$b, GPR:$c)>;
def : Pat<(gpuvm_carry (i32 GPR:$a), (i32 GPR:$b), (i32 GPR:$c)),
          (CARRY GPR:$a, GPR:$b, GPR:$c)>;
def : Pat<(gpuvm_subby (i32 GPR:$a), (i32 GPR:$b), (i32 GPR:$c)),
          (SUBBY GPR:$a, GPR:$b, GPR:$c)>;
def : Pat<(gpuvm_borrow (i32 GPR:$a), (i32 GPR:$b), (i32 GPR:$c)),
          (BORROW GPR:$a, GPR:$b, GPR:$c)>;

// Addresses
def : Pat<(gpuvm_addr tglobaladdr:$s), (ADDI (i32 R0), tglobaladdr:$s)>;
def : Pat<(gpuvm_addr texternalsym:$s), (ADDI (i32 R0), texternalsym:$s)>;
def : Pat<(gpuvm_addr tblockaddress:$s), (ADDI (i32 R0), tblockaddress:$s)>;
def : Pat<(gpuvm_addr tjumptable:$s), (ADDI (i32 R0), tjumptable:$s)>;
def : Pat<(gpuvm_addr tconstpool:$s), (ADDI (i32 R0), tconstpool:$s)>;
def : Pat<(gpuvm_addr tglobaltlsaddr:$s), (ADDI (i32 R0), tglobaltlsaddr:$s)>;
def : Pat<(add (i32 GPR:$a), (gpuvm_addr tglobaladdr:$s)),
          (ADDI GPR:$a, tglobaladdr:$s)>;
def : Pat<(add (i32 GPR:$a), (gpuvm_addr tglobaltlsaddr:$s)),
          (ADDI GPR:$a, tglobaltlsaddr:$s)>;
def : Pat<(add (i32 GPR:$a), (gpuvm_addr tconstpool:$s)),
          (ADDI GPR:$a, tconstpool:$s)>;

//===----------------------------------------------------------------------===//
// Comparisons and selects
//===----------------------------------------------------------------------===//

// x == 0, x != 0: sltu (a mid op) rather than seq/sne (20-23: the full step)
let AddedComplexity = 1 in {
def : Pat<(seteq (i32 GPR:$a), 0), (SLTUI GPR:$a, 1)>;
def : Pat<(setne (i32 GPR:$a), 0), (SLTU (i32 R0), GPR:$a)>;
}
def : Pat<(seteq (i32 GPR:$a), (i32 GPR:$b)), (SEQ GPR:$a, GPR:$b)>;
def : Pat<(seteq (i32 GPR:$a), imm32:$i), (SEQI GPR:$a, imm32:$i)>;
def : Pat<(setne (i32 GPR:$a), (i32 GPR:$b)), (SNE GPR:$a, GPR:$b)>;
def : Pat<(setne (i32 GPR:$a), imm32:$i), (SNEI GPR:$a, imm32:$i)>;
def : Pat<(setlt (i32 GPR:$a), (i32 GPR:$b)), (SLT GPR:$a, GPR:$b)>;
def : Pat<(setlt (i32 GPR:$a), imm32:$i), (SLTI GPR:$a, imm32:$i)>;
def : Pat<(setult (i32 GPR:$a), (i32 GPR:$b)), (SLTU GPR:$a, GPR:$b)>;
def : Pat<(setult (i32 GPR:$a), imm32:$i), (SLTUI GPR:$a, imm32:$i)>;
def : Pat<(setge (i32 GPR:$a), (i32 GPR:$b)), (SGE GPR:$a, GPR:$b)>;
def : Pat<(setge (i32 GPR:$a), imm32:$i), (SGEI GPR:$a, imm32:$i)>;
def : Pat<(setuge (i32 GPR:$a), (i32 GPR:$b)), (SGEU GPR:$a, GPR:$b)>;
def : Pat<(setuge (i32 GPR:$a), imm32:$i), (SGEUI GPR:$a, imm32:$i)>;
def : Pat<(setgt (i32 GPR:$a), (i32 GPR:$b)), (SLT GPR:$b, GPR:$a)>;
def : Pat<(setgt (i32 GPR:$a), notSMax:$i), (SGEI GPR:$a, (ImmPlus1 $i))>;
def : Pat<(setle (i32 GPR:$a), (i32 GPR:$b)), (SGE GPR:$b, GPR:$a)>;
def : Pat<(setle (i32 GPR:$a), notSMax:$i), (SLTI GPR:$a, (ImmPlus1 $i))>;
def : Pat<(setugt (i32 GPR:$a), (i32 GPR:$b)), (SLTU GPR:$b, GPR:$a)>;
def : Pat<(setugt (i32 GPR:$a), notUMax:$i), (SGEUI GPR:$a, (ImmPlus1 $i))>;
def : Pat<(setule (i32 GPR:$a), (i32 GPR:$b)), (SGEU GPR:$b, GPR:$a)>;
def : Pat<(setule (i32 GPR:$a), notUMax:$i), (SLTUI GPR:$a, (ImmPlus1 $i))>;

def : Pat<(select (i32 GPR:$c), (i32 GPR:$a), (i32 GPR:$b)),
          (SEL GPR:$a, GPR:$b, GPR:$c)>;
def : Pat<(select (i32 GPR:$c), (f32 GPR:$a), (f32 GPR:$b)),
          (SEL GPR:$a, GPR:$b, GPR:$c)>;
def : Pat<(select (i32 (seteq (i32 GPR:$c), 0)), (i32 GPR:$a), (i32 GPR:$b)),
          (SEL GPR:$b, GPR:$a, GPR:$c)>;
def : Pat<(select (i32 (setne (i32 GPR:$c), 0)), (i32 GPR:$a), (i32 GPR:$b)),
          (SEL GPR:$a, GPR:$b, GPR:$c)>;
def : Pat<(select (i32 (setne (i32 GPR:$c), 0)), (f32 GPR:$a), (f32 GPR:$b)),
          (SEL GPR:$a, GPR:$b, GPR:$c)>;
def : Pat<(select (i32 (seteq (i32 GPR:$c), 0)), (f32 GPR:$a), (f32 GPR:$b)),
          (SEL GPR:$b, GPR:$a, GPR:$c)>;

//===----------------------------------------------------------------------===//
// Loads and stores
//===----------------------------------------------------------------------===//

multiclass LdPat<PatFrag LoadOp, Instruction I, Instruction IX, ValueType vt> {
  def : Pat<(vt (LoadOp (AddrRR (i32 GPR:$a), (i32 GPR:$b), uimm5:$sh))),
            (IX GPR:$a, GPR:$b, uimm5:$sh)>;
  def : Pat<(vt (LoadOp (AddrRI (i32 GPR:$a), imm32:$imm))),
            (I GPR:$a, imm32:$imm)>;
}
multiclass StPat<PatFrag StoreOp, Instruction I, Instruction IX, ValueType vt> {
  def : Pat<(StoreOp (vt GPR:$v), (AddrRR (i32 GPR:$a), (i32 GPR:$b),
                                          uimm5:$sh)),
            (IX GPR:$v, GPR:$a, GPR:$b, uimm5:$sh)>;
  def : Pat<(StoreOp (vt GPR:$v), (AddrRI (i32 GPR:$a), imm32:$imm)),
            (I GPR:$v, GPR:$a, imm32:$imm)>;
}

defm : LdPat<load, LW, LWX, i32>;
defm : LdPat<load, LW, LWX, f32>;
defm : LdPat<sextloadi8, LB, LBX, i32>;
defm : LdPat<zextloadi8, LBU, LBUX, i32>;
defm : LdPat<extloadi8, LBU, LBUX, i32>;
defm : LdPat<sextloadi16, LH, LHX, i32>;
defm : LdPat<zextloadi16, LHU, LHUX, i32>;
defm : LdPat<extloadi16, LHU, LHUX, i32>;
defm : StPat<store, SW, SWX, i32>;
defm : StPat<store, SW, SWX, f32>;
defm : StPat<truncstorei8, SB, SBX, i32>;
defm : StPat<truncstorei16, SH, SHX, i32>;

// Atomics: a load or store of a word is atomic; the amo ops.
defm : LdPat<atomic_load_nonext_32, LW, LWX, i32>;
defm : LdPat<atomic_load_sext_8, LB, LBX, i32>;
defm : LdPat<atomic_load_zext_8, LBU, LBUX, i32>;
defm : LdPat<atomic_load_aext_8, LBU, LBUX, i32>;
defm : LdPat<atomic_load_nonext_8, LBU, LBUX, i32>;
defm : LdPat<atomic_load_sext_16, LH, LHX, i32>;
defm : LdPat<atomic_load_zext_16, LHU, LHUX, i32>;
defm : LdPat<atomic_load_aext_16, LHU, LHUX, i32>;
defm : LdPat<atomic_load_nonext_16, LHU, LHUX, i32>;
defm : StPat<atomic_store_32, SW, SWX, i32>;
defm : StPat<atomic_store_16, SH, SHX, i32>;
defm : StPat<atomic_store_8, SB, SBX, i32>;

class AMOPat<PatFrag Op, Instruction I>
    : Pat<(Op (i32 GPR:$addr), (i32 GPR:$v)), (I GPR:$v, GPR:$addr)>;
def : AMOPat<atomic_swap_i32, AMOSWAP>;
def : AMOPat<atomic_load_add_i32, AMOADD>;
def : AMOPat<atomic_load_and_i32, AMOAND>;
def : AMOPat<atomic_load_or_i32, AMOOR>;
def : AMOPat<atomic_load_xor_i32, AMOXOR>;
def : AMOPat<atomic_load_min_i32, AMOMIN>;
def : AMOPat<atomic_load_max_i32, AMOMAX>;
def : AMOPat<atomic_load_umin_i32, AMOMINU>;
def : AMOPat<atomic_load_umax_i32, AMOMAXU>;
def : Pat<(atomic_load_sub_i32 (i32 GPR:$addr), (i32 GPR:$v)),
          (AMOADD (SUB (i32 R0), GPR:$v), GPR:$addr)>;
def : Pat<(atomic_cmp_swap_i32 (i32 GPR:$addr), (i32 GPR:$cmp),
                               (i32 GPR:$new)),
          (AMOCAS GPR:$cmp, GPR:$new, GPR:$addr)>;
def : Pat<(atomic_fence timm, timm), (FENCE)>;

//===----------------------------------------------------------------------===//
// Branches and calls
//===----------------------------------------------------------------------===//

def : Pat<(br bb:$t), (J bb:$t)>;
def : Pat<(brind (i32 GPR:$r)), (JR GPR:$r)>;
def : Pat<(brcond (i32 GPR:$c), bb:$t), (BNEI GPR:$c, 0, bb:$t)>;

multiclass BrPat<PatFrag Cond, Instruction I, Instruction II> {
  def : Pat<(brcond (i32 (Cond (i32 GPR:$a), (i32 GPR:$b))), bb:$t),
            (I GPR:$a, GPR:$b, bb:$t)>;
  def : Pat<(brcond (i32 (Cond (i32 GPR:$a), usimm6:$i)), bb:$t),
            (II GPR:$a, (toTImm $i), bb:$t)>;
}
defm : BrPat<seteq, BEQ, BEQI>;
defm : BrPat<setne, BNE, BNEI>;
defm : BrPat<setlt, BLT, BLTI>;
defm : BrPat<setge, BGE, BGEI>;
defm : BrPat<setult, BLTU, BLTUI>;
defm : BrPat<setuge, BGEU, BGEUI>;
def : Pat<(brcond (i32 (setgt (i32 GPR:$a), (i32 GPR:$b))), bb:$t),
          (BLT GPR:$b, GPR:$a, bb:$t)>;
def : Pat<(brcond (i32 (setle (i32 GPR:$a), (i32 GPR:$b))), bb:$t),
          (BGE GPR:$b, GPR:$a, bb:$t)>;
def : Pat<(brcond (i32 (setugt (i32 GPR:$a), (i32 GPR:$b))), bb:$t),
          (BLTU GPR:$b, GPR:$a, bb:$t)>;
def : Pat<(brcond (i32 (setule (i32 GPR:$a), (i32 GPR:$b))), bb:$t),
          (BGEU GPR:$b, GPR:$a, bb:$t)>;
def : Pat<(brcond (i32 (setgt (i32 GPR:$a), simm6Plus1:$i)), bb:$t),
          (BGEI GPR:$a, (ImmPlus1 $i), bb:$t)>;
def : Pat<(brcond (i32 (setle (i32 GPR:$a), simm6Plus1:$i)), bb:$t),
          (BLTI GPR:$a, (ImmPlus1 $i), bb:$t)>;
def : Pat<(brcond (i32 (setugt (i32 GPR:$a), usimm6Plus1:$i)), bb:$t),
          (BGEUI GPR:$a, (ImmPlus1 $i), bb:$t)>;
def : Pat<(brcond (i32 (setule (i32 GPR:$a), usimm6Plus1:$i)), bb:$t),
          (BLTUI GPR:$a, (ImmPlus1 $i), bb:$t)>;
// a bit's test
def : Pat<(brcond (i32 (setne (and (i32 GPR:$a), pow2imm:$m), 0)), bb:$t),
          (BBS GPR:$a, (Log2 $m), bb:$t)>;
def : Pat<(brcond (i32 (seteq (and (i32 GPR:$a), pow2imm:$m), 0)), bb:$t),
          (BBC GPR:$a, (Log2 $m), bb:$t)>;
def : Pat<(brcond (i32 (and (i32 GPR:$a), pow2imm:$m)), bb:$t),
          (BBS GPR:$a, (Log2 $m), bb:$t)>;

def : Pat<(gpuvm_call tglobaladdr:$f), (CALL tglobaladdr:$f)>;
def : Pat<(gpuvm_call texternalsym:$f), (CALL texternalsym:$f)>;
def : Pat<(gpuvm_call (i32 GPR:$r)), (CALLR GPR:$r)>;

//===----------------------------------------------------------------------===//
// Float (single precision, in the same registers)
//===----------------------------------------------------------------------===//

def : Pat<(f32 (bitconvert (i32 GPR:$a))), (f32 GPR:$a)>;
def : Pat<(i32 (bitconvert (f32 GPR:$a))), (i32 GPR:$a)>;
def : Pat<(f32 fpimm32:$f), (ADDI (i32 R0), (FPBits $f))>;

multiclass FPatRI<SDPatternOperator Op, Instruction R, Instruction I> {
  def : Pat<(Op (f32 GPR:$a), (f32 GPR:$b)), (R GPR:$a, GPR:$b)>;
  def : Pat<(Op (f32 GPR:$a), fpimm32:$f), (I GPR:$a, (FPBits $f))>;
}
defm : FPatRI<fadd, FADD, FADDI>;
defm : FPatRI<fsub, FSUB, FSUBI>;
defm : FPatRI<fmul, FMUL, FMULI>;
defm : FPatRI<fdiv, FDIV, FDIVI>;
defm : FPatRI<fminnum, FMIN, FMINI>;
defm : FPatRI<fmaxnum, FMAX, FMAXI>;
defm : FPatRI<fminimumnum, FMIN, FMINI>;
defm : FPatRI<fmaximumnum, FMAX, FMAXI>;
def : Pat<(fcopysign (f32 GPR:$a), (f32 GPR:$b)), (FSGNJ GPR:$a, GPR:$b)>;
def : Pat<(fneg (f32 GPR:$a)), (FSGNJN GPR:$a, GPR:$a)>;
def : Pat<(fabs (f32 GPR:$a)), (FSGNJX GPR:$a, GPR:$a)>;
def : Pat<(fsqrt (f32 GPR:$a)), (FSQRT GPR:$a)>;
def : Pat<(ffloor (f32 GPR:$a)), (FFLOOR GPR:$a)>;
def : Pat<(fceil (f32 GPR:$a)), (FCEIL GPR:$a)>;
def : Pat<(ftrunc (f32 GPR:$a)), (FTRUNC GPR:$a)>;
def : Pat<(froundeven (f32 GPR:$a)), (FROUND GPR:$a)>;
def : Pat<(frint (f32 GPR:$a)), (FROUND GPR:$a)>;
def : Pat<(fnearbyint (f32 GPR:$a)), (FROUND GPR:$a)>;
def : Pat<(i32 (fp_to_sint (f32 GPR:$a))), (FTOI GPR:$a)>;
def : Pat<(i32 (fp_to_uint (f32 GPR:$a))), (FTOU GPR:$a)>;
def : Pat<(f32 (sint_to_fp (i32 GPR:$a))), (ITOF GPR:$a)>;
def : Pat<(f32 (uint_to_fp (i32 GPR:$a))), (UTOF GPR:$a)>;
// fmad is fmul and fadd, each rounded: what fmad does.
def : Pat<(fmad (f32 GPR:$a), (f32 GPR:$b), (f32 GPR:$c)),
          (FMAD GPR:$a, GPR:$b, GPR:$c)>;
def : Pat<(fmad (f32 GPR:$a), (f32 GPR:$b), (fneg (f32 GPR:$c))),
          (FMSB GPR:$a, GPR:$b, GPR:$c)>;
def : Pat<(fmad (fneg (f32 GPR:$a)), (f32 GPR:$b), (f32 GPR:$c)),
          (FNMSB GPR:$a, GPR:$b, GPR:$c)>;

def : Pat<(seteq (f32 GPR:$a), (f32 GPR:$b)), (FEQ GPR:$a, GPR:$b)>;
def : Pat<(setoeq (f32 GPR:$a), (f32 GPR:$b)), (FEQ GPR:$a, GPR:$b)>;
def : Pat<(setlt (f32 GPR:$a), (f32 GPR:$b)), (FLT GPR:$a, GPR:$b)>;
def : Pat<(setolt (f32 GPR:$a), (f32 GPR:$b)), (FLT GPR:$a, GPR:$b)>;
def : Pat<(setle (f32 GPR:$a), (f32 GPR:$b)), (FLE GPR:$a, GPR:$b)>;
def : Pat<(setole (f32 GPR:$a), (f32 GPR:$b)), (FLE GPR:$a, GPR:$b)>;
def : Pat<(setgt (f32 GPR:$a), (f32 GPR:$b)), (FLT GPR:$b, GPR:$a)>;
def : Pat<(setogt (f32 GPR:$a), (f32 GPR:$b)), (FLT GPR:$b, GPR:$a)>;
def : Pat<(setge (f32 GPR:$a), (f32 GPR:$b)), (FLE GPR:$b, GPR:$a)>;
def : Pat<(setoge (f32 GPR:$a), (f32 GPR:$b)), (FLE GPR:$b, GPR:$a)>;
def : Pat<(setuo (f32 GPR:$a), (f32 GPR:$b)), (FUNORD GPR:$a, GPR:$b)>;
def : Pat<(setne (f32 GPR:$a), (f32 GPR:$b)),
          (XORI (FEQ GPR:$a, GPR:$b), 1)>;

// The card's special functions: what reaches selection is afn's (the others
// became libm calls, GPUVMTargetLowering::lowerApproxFunc).
def : Pat<(fsin (f32 GPR:$a)), (FSIN GPR:$a)>;
def : Pat<(fcos (f32 GPR:$a)), (FCOS GPR:$a)>;
def : Pat<(fexp2 (f32 GPR:$a)), (FEXP2 GPR:$a)>;
def : Pat<(flog2 (f32 GPR:$a)), (FLOG2 GPR:$a)>;
// 1 / x and 1 / sqrt(x) approximately (afn or arcp)
def fpimm_one : FPImmLeaf<f32, [{ return Imm.isExactlyValue(1.0); }]>;
def fdiv_approx : PatFrag<(ops node:$a, node:$b), (fdiv node:$a, node:$b), [{
  return N->getFlags().hasApproximateFuncs() ||
         N->getFlags().hasAllowReciprocal();
}]>;
let AddedComplexity = 2 in {
def : Pat<(fdiv_approx fpimm_one, (fsqrt (f32 GPR:$a))), (FRSQRT GPR:$a)>;
def : Pat<(fdiv_approx fpimm_one, (f32 GPR:$a)), (FRCP GPR:$a)>;
}

// Tail calls: j to the callee (after the epilogue), jr for one in a register
// the epilogue does not restore.
def gpuvm_tail : SDNode<"GPUVMISD::TAIL", SDT_GPUVMCall,
                        [SDNPHasChain, SDNPOptInGlue, SDNPVariadic]>;
let isCall = 1, isTerminator = 1, isReturn = 1, isBarrier = 1, Uses = [R2],
    hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
def PseudoTAIL : GPUVMPseudo<(outs), (ins calltarget:$dst)>;
def PseudoTAILR : GPUVMPseudo<(outs), (ins GPRTC:$rs)>;
}
def : Pat<(gpuvm_tail tglobaladdr:$f), (PseudoTAIL tglobaladdr:$f)>;
def : Pat<(gpuvm_tail texternalsym:$f), (PseudoTAIL texternalsym:$f)>;
def : Pat<(gpuvm_tail (i32 GPRTC:$r)), (PseudoTAILR GPRTC:$r)>;
