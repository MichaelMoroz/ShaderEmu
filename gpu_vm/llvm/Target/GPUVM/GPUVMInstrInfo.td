//===-- GPUVMInstrInfo.td - gpu_vm instructions ------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Every op of gpu_vm/isa.h. No selection patterns yet: add them beside the
// definitions as `def : Pat<(add GPR:$a, GPR:$b), (ADD GPR:$a, GPR:$b)>;`.
// An immediate operand (imm32) takes any 32-bit value or a symbol's
// expression: the encoder makes it short (imm12) or long (word 2).
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Operands
//===----------------------------------------------------------------------===//

class GVMImmAsmOperand<string name> : AsmOperandClass {
  let Name = name;
  let RenderMethod = "addImmOperands";
  let DiagnosticType = "Invalid" # name;
}

def Imm32AsmOperand : GVMImmAsmOperand<"Imm32">;
def FImmAsmOperand : GVMImmAsmOperand<"FImm"> {
  let ParserMethod = "parseFImm";
}
def SImm6AsmOperand : GVMImmAsmOperand<"SImm6">;
def UImm5AsmOperand : GVMImmAsmOperand<"UImm5">;
def Len32AsmOperand : GVMImmAsmOperand<"Len32">;
def SImm12AsmOperand : GVMImmAsmOperand<"SImm12">;
def SRAsmOperand : GVMImmAsmOperand<"SR"> {
  let ParserMethod = "parseSR";
}
def BrTargetAsmOperand : GVMImmAsmOperand<"BrTarget">;

// Any 32-bit value or a relocatable expression (short or long form).
def imm32 : Operand<i32>, ImmLeaf<i32, [{ return true; }]> {
  let ParserMatchClass = Imm32AsmOperand;
  let OperandType = "OPERAND_IMMEDIATE";
}
// A float's bits as an i32 (the I form of the float class).
def fimm32 : Operand<i32> {
  let ParserMatchClass = FImmAsmOperand;
  let PrintMethod = "printFImm";
  let OperandType = "OPERAND_IMMEDIATE";
}
def simm6 : Operand<i32>, ImmLeaf<i32, [{ return isInt<6>(Imm); }]> {
  let ParserMatchClass = SImm6AsmOperand;
  let OperandType = "OPERAND_IMMEDIATE";
}
def uimm5 : Operand<i32>, ImmLeaf<i32, [{ return isUInt<5>(Imm); }]> {
  let ParserMatchClass = UImm5AsmOperand;
  let OperandType = "OPERAND_IMMEDIATE";
}
// A bit field's length, 1-32.
def len32 : Operand<i32>, ImmLeaf<i32, [{ return Imm >= 1 && Imm <= 32; }]> {
  let ParserMatchClass = Len32AsmOperand;
  let OperandType = "OPERAND_IMMEDIATE";
}
def simm12 : Operand<i32>, ImmLeaf<i32, [{ return isInt<12>(Imm); }]> {
  let ParserMatchClass = SImm12AsmOperand;
  let OperandType = "OPERAND_IMMEDIATE";
}
// A special register's number (0-4095) or name.
def srnum : Operand<i32> {
  let ParserMatchClass = SRAsmOperand;
  let PrintMethod = "printSR";
  let OperandType = "OPERAND_IMMEDIATE";
}
// A branch's target: a label, or a byte offset from the bundle.
def brtarget : Operand<OtherVT> {
  let ParserMatchClass = BrTargetAsmOperand;
  let PrintMethod = "printBranchTarget";
  let OperandType = "OPERAND_PCREL";
}
def calltarget : Operand<iPTR> {
  let ParserMatchClass = BrTargetAsmOperand;
  let PrintMethod = "printBranchTarget";
  let OperandType = "OPERAND_PCREL";
}
// lpc's: a label (PC-relative, long), or a byte offset from the bundle.
def pcrel32 : Operand<iPTR> {
  let ParserMatchClass = Imm32AsmOperand;
  let PrintMethod = "printBranchTarget";
  let OperandType = "OPERAND_PCREL";
}

//===----------------------------------------------------------------------===//
// Integer class: 0x40 + f (R), 0x60 + f (I)
//===----------------------------------------------------------------------===//

class GVM_R<bits<5> f, string opcodestr>
    : GVMOp<!add(0x40, f), FrmR, (outs GPR:$rd), (ins GPR:$ra, GPR:$rb),
            opcodestr, "$rd, $ra, $rb">;
class GVM_I<bits<5> f, string opcodestr>
    : GVMOp<!add(0x60, f), FrmI, (outs GPR:$rd), (ins GPR:$ra, imm32:$imm),
            opcodestr, "$rd, $ra, $imm">;

multiclass GVM_RI<bits<5> f, string opcodestr> {
  def NAME : GVM_R<f, opcodestr>;
  def NAME # I : GVM_I<f, opcodestr # "i">;
}

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
let isCommutable = 1 in {
defm ADD   : GVM_RI<0, "add">;
defm AND   : GVM_RI<1, "and">;
defm OR    : GVM_RI<2, "or">;
defm XOR   : GVM_RI<3, "xor">;
defm XNOR  : GVM_RI<7, "xnor">;
defm MIN   : GVM_RI<10, "min">;
defm MINU  : GVM_RI<11, "minu">;
defm MAX   : GVM_RI<14, "max">;
defm MAXU  : GVM_RI<15, "maxu">;
defm MUL   : GVM_RI<19, "mul">;
defm SEQ   : GVM_RI<22, "seq">;
defm SNE   : GVM_RI<23, "sne">;
}
defm SUB   : GVM_RI<4, "sub">;
defm ANDN  : GVM_RI<5, "andn">;
defm ORN   : GVM_RI<6, "orn">;
defm SLT   : GVM_RI<8, "slt">;
defm SLTU  : GVM_RI<9, "sltu">;
defm SGE   : GVM_RI<12, "sge">;
defm SGEU  : GVM_RI<13, "sgeu">;
defm SLL   : GVM_RI<16, "sll">;
defm SRL   : GVM_RI<17, "srl">;
defm SRA   : GVM_RI<18, "sra">;
defm ROR   : GVM_RI<20, "ror">;
defm ROL   : GVM_RI<21, "rol">;

def SHADD : GVMOp<!add(0x40, 24), FrmRSh, (outs GPR:$rd),
                  (ins GPR:$ra, GPR:$rb, uimm5:$sh), "shadd",
                  "$rd, $ra, $rb, $sh">;
class GVM_R4<bits<5> f, string opcodestr>
    : GVMOp<!add(0x40, f), FrmR4, (outs GPR:$rd),
            (ins GPR:$ra, GPR:$rb, GPR:$rc), opcodestr, "$rd, $ra, $rb, $rc">;
def SEL    : GVM_R4<25, "sel">;
def ADDCY  : GVM_R4<26, "addcy">;
def CARRY  : GVM_R4<27, "carry">;
def SUBBY  : GVM_R4<28, "subby">;
def BORROW : GVM_R4<29, "borrow">;

def BFE : GVMOp<!add(0x60, 30), FrmBF, (outs GPR:$rd),
                (ins GPR:$ra, uimm5:$pos, len32:$len), "bfe",
                "$rd, $ra, $pos, $len">;
let Variant = 1 in
def BFES : GVMOp<!add(0x60, 30), FrmBF, (outs GPR:$rd),
                 (ins GPR:$ra, uimm5:$pos, len32:$len), "bfes",
                 "$rd, $ra, $pos, $len">;
} // hasSideEffects = 0, mayLoad = 0, mayStore = 0

//===----------------------------------------------------------------------===//
// Memory class: 0x80 + mode * 8 + w
//===----------------------------------------------------------------------===//

// Widths (isa.h, GVM_W_*)
defvar W_W = 0;
defvar W_D = 1;
defvar W_H = 2;
defvar W_B = 3;
defvar W_Q = 4;
defvar W_HU = 6;
defvar W_BU = 7;

class GVM_Load<int w, string opcodestr>
    : GVMOp<!add(0x80, w), FrmLoad, (outs GPR:$rd), (ins GPR:$ra, imm32:$imm),
            opcodestr, "$rd, ${imm}(${ra})"> {
  let mayLoad = 1;
  let mayStore = 0;
  let hasSideEffects = 0;
}
class GVM_Store<int w, string opcodestr>
    : GVMOp<!add(0x88, w), FrmStore, (outs), (ins GPR:$rs, GPR:$ra, imm32:$imm),
            opcodestr, "$rs, ${imm}(${ra})"> {
  let mayLoad = 0;
  let mayStore = 1;
  let hasSideEffects = 0;
}
class GVM_LoadX<int w, string opcodestr>
    : GVMOp<!add(0x90, w), FrmLoadX, (outs GPR:$rd),
            (ins GPR:$ra, GPR:$rb, uimm5:$sh), opcodestr,
            "$rd, $ra, $rb, $sh"> {
  let mayLoad = 1;
  let mayStore = 0;
  let hasSideEffects = 0;
}
class GVM_StoreX<int w, string opcodestr>
    : GVMOp<!add(0x98, w), FrmStoreX, (outs),
            (ins GPR:$rs, GPR:$ra, GPR:$rb, uimm5:$sh), opcodestr,
            "$rs, $ra, $rb, $sh"> {
  let mayLoad = 0;
  let mayStore = 1;
  let hasSideEffects = 0;
}

// ld, lq (and sd, sq, ldx ...) name the first of 2 or 4 registers. The
// codegen will want register-pair and quad classes for them; for now the MC
// operand is the first register.
def LW  : GVM_Load<W_W, "lw">;
def LD  : GVM_Load<W_D, "ld">;
def LH  : GVM_Load<W_H, "lh">;
def LB  : GVM_Load<W_B, "lb">;
def LQ  : GVM_Load<W_Q, "lq">;
def LHU : GVM_Load<W_HU, "lhu">;
def LBU : GVM_Load<W_BU, "lbu">;
def SW  : GVM_Store<W_W, "sw">;
def SD  : GVM_Store<W_D, "sd">;
def SH  : GVM_Store<W_H, "sh">;
def SB  : GVM_Store<W_B, "sb">;
def SQ  : GVM_Store<W_Q, "sq">;
def LWX  : GVM_LoadX<W_W, "lwx">;
def LDX  : GVM_LoadX<W_D, "ldx">;
def LHX  : GVM_LoadX<W_H, "lhx">;
def LBX  : GVM_LoadX<W_B, "lbx">;
def LQX  : GVM_LoadX<W_Q, "lqx">;
def LHUX : GVM_LoadX<W_HU, "lhux">;
def LBUX : GVM_LoadX<W_BU, "lbux">;
def SWX  : GVM_StoreX<W_W, "swx">;
def SDX  : GVM_StoreX<W_D, "sdx">;
def SHX  : GVM_StoreX<W_H, "shx">;
def SBX  : GVM_StoreX<W_B, "sbx">;
def SQX  : GVM_StoreX<W_Q, "sqx">;

let mayLoad = 1, mayStore = 1, hasSideEffects = 1 in {
class GVM_AMO<bits<8> opc, string opcodestr>
    : GVMOp<opc, FrmAMO, (outs GPR:$rd), (ins GPR:$rb, GPR:$ra), opcodestr,
            "$rd, $rb, (${ra})">;
def AMOSWAP : GVM_AMO<0xa0, "amoswap">;
def AMOADD  : GVM_AMO<0xa1, "amoadd">;
def AMOAND  : GVM_AMO<0xa2, "amoand">;
def AMOOR   : GVM_AMO<0xa3, "amoor">;
def AMOXOR  : GVM_AMO<0xa4, "amoxor">;
def AMOMIN  : GVM_AMO<0xa5, "amomin">;
def AMOMAX  : GVM_AMO<0xa6, "amomax">;
def AMOMINU : GVM_AMO<0xa8, "amominu">;
def AMOMAXU : GVM_AMO<0xa9, "amomaxu">;
let Constraints = "$rd = $exp" in
def AMOCAS : GVMOp<0xa7, FrmCAS, (outs GPR:$rd),
                   (ins GPR:$exp, GPR:$rb, GPR:$ra), "amocas",
                   "$rd, $rb, (${ra})">;
}

//===----------------------------------------------------------------------===//
// Float class: 0xc0 + f (R), 0xe0 + f (I)
//===----------------------------------------------------------------------===//

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
class GVM_FR<bits<5> f, string opcodestr>
    : GVMOp<!add(0xc0, f), FrmR, (outs GPR:$rd), (ins GPR:$ra, GPR:$rb),
            opcodestr, "$rd, $ra, $rb">;
class GVM_FI<bits<5> f, string opcodestr>
    : GVMOp<!add(0xe0, f), FrmFI, (outs GPR:$rd), (ins GPR:$ra, fimm32:$imm),
            opcodestr, "$rd, $ra, $imm">;
class GVM_F1<bits<5> f, string opcodestr>
    : GVMOp<!add(0xc0, f), FrmR1, (outs GPR:$rd), (ins GPR:$ra), opcodestr,
            "$rd, $ra">;
class GVM_F4<bits<5> f, string opcodestr>
    : GVMOp<!add(0xc0, f), FrmR4, (outs GPR:$rd),
            (ins GPR:$ra, GPR:$rb, GPR:$rc), opcodestr, "$rd, $ra, $rb, $rc">;
multiclass GVM_FRI<bits<5> f, string opcodestr> {
  def NAME : GVM_FR<f, opcodestr>;
  def NAME # I : GVM_FI<f, opcodestr # "i">;
}

defm FADD   : GVM_FRI<0, "fadd">;
defm FMUL   : GVM_FRI<1, "fmul">;
def  FMAD   : GVM_F4<2, "fmad">;
def  FMSB   : GVM_F4<3, "fmsb">;
defm FSUB   : GVM_FRI<4, "fsub">;
def  FNMSB  : GVM_F4<5, "fnmsb">;
defm FMIN   : GVM_FRI<6, "fmin">;
defm FMAX   : GVM_FRI<7, "fmax">;
defm FEQ    : GVM_FRI<8, "feq">;
defm FLT    : GVM_FRI<9, "flt">;
defm FLE    : GVM_FRI<10, "fle">;
defm FUNORD : GVM_FRI<11, "funord">;
defm FSGNJ  : GVM_FRI<12, "fsgnj">;
defm FSGNJN : GVM_FRI<13, "fsgnjn">;
defm FSGNJX : GVM_FRI<14, "fsgnjx">;
def  FFRAC  : GVM_F1<15, "ffrac">;
def  ITOF   : GVM_F1<16, "itof">;
def  UTOF   : GVM_F1<17, "utof">;
def  FTOI   : GVM_F1<18, "ftoi">;
def  FTOU   : GVM_F1<19, "ftou">;
def  FFLOOR : GVM_F1<20, "ffloor">;
def  FCEIL  : GVM_F1<21, "fceil">;
def  FTRUNC : GVM_F1<22, "ftrunc">;
def  FROUND : GVM_F1<23, "fround">;
defm FDIV   : GVM_FRI<24, "fdiv">;
def  FSQRT  : GVM_F1<25, "fsqrt">;
def  FRSQRT : GVM_F1<26, "frsqrt">;
def  FRCP   : GVM_F1<27, "frcp">;
def  FSIN   : GVM_F1<28, "fsin">;
def  FCOS   : GVM_F1<29, "fcos">;
def  FEXP2  : GVM_F1<30, "fexp2">;
def  FLOG2  : GVM_F1<31, "flog2">;
} // hasSideEffects = 0, mayLoad = 0, mayStore = 0

//===----------------------------------------------------------------------===//
// Extended class: 0x00-0x3f
//===----------------------------------------------------------------------===//

let hasSideEffects = 0, mayLoad = 0, mayStore = 0 in {
def MULH   : GVMOp<0x01, FrmR, (outs GPR:$rd), (ins GPR:$ra, GPR:$rb), "mulh", "$rd, $ra, $rb">;
def MULHU  : GVMOp<0x02, FrmR, (outs GPR:$rd), (ins GPR:$ra, GPR:$rb), "mulhu", "$rd, $ra, $rb">;
def MULHSU : GVMOp<0x03, FrmR, (outs GPR:$rd), (ins GPR:$ra, GPR:$rb), "mulhsu", "$rd, $ra, $rb">;
def DIV    : GVMOp<0x04, FrmR, (outs GPR:$rd), (ins GPR:$ra, GPR:$rb), "div", "$rd, $ra, $rb">;
def DIVU   : GVMOp<0x05, FrmR, (outs GPR:$rd), (ins GPR:$ra, GPR:$rb), "divu", "$rd, $ra, $rb">;
def REM    : GVMOp<0x06, FrmR, (outs GPR:$rd), (ins GPR:$ra, GPR:$rb), "rem", "$rd, $ra, $rb">;
def REMU   : GVMOp<0x07, FrmR, (outs GPR:$rd), (ins GPR:$ra, GPR:$rb), "remu", "$rd, $ra, $rb">;
def CLZ    : GVMOp<0x08, FrmR1, (outs GPR:$rd), (ins GPR:$ra), "clz", "$rd, $ra">;
def CTZ    : GVMOp<0x09, FrmR1, (outs GPR:$rd), (ins GPR:$ra), "ctz", "$rd, $ra">;
def POPC   : GVMOp<0x0a, FrmR1, (outs GPR:$rd), (ins GPR:$ra), "popc", "$rd, $ra">;
def BSWAP  : GVMOp<0x0b, FrmR1, (outs GPR:$rd), (ins GPR:$ra), "bswap", "$rd, $ra">;
def BREV   : GVMOp<0x0c, FrmR1, (outs GPR:$rd), (ins GPR:$ra), "brev", "$rd, $ra">;
let Constraints = "$rd = $rdin" in
def BFI : GVMOp<0x0d, FrmBFI, (outs GPR:$rd),
                (ins GPR:$rdin, GPR:$ra, uimm5:$pos, len32:$len), "bfi",
                "$rd, $ra, $pos, $len">;
def LPC : GVMOp<0x0e, FrmLPC, (outs GPR:$rd), (ins pcrel32:$target), "lpc",
                "$rd, $target">;
}

let hasSideEffects = 1, mayLoad = 0, mayStore = 0 in {
def SYSCALL : GVMOp<0x10, FrmNone, (outs), (ins), "syscall", "">;
def BREAK   : GVMOp<0x11, FrmNone, (outs), (ins), "break", "">;
def WAIT    : GVMOp<0x12, FrmNone, (outs), (ins), "wait", "">;
def SYNC    : GVMOp<0x13, FrmNone, (outs), (ins), "sync", "">;
def FENCE   : GVMOp<0x14, FrmNone, (outs), (ins), "fence", "">;
def UNIMP   : GVMOp<0x00, FrmUnimp, (outs), (ins), "unimp", "">;
def MFSR  : GVMOp<0x18, FrmMFSR, (outs GPR:$rd), (ins srnum:$sr), "mfsr", "$rd, $sr">;
def MTSR  : GVMOp<0x19, FrmMTSR, (outs), (ins srnum:$sr, GPR:$ra), "mtsr", "$sr, $ra">;
def XSR   : GVMOp<0x1a, FrmXSR, (outs GPR:$rd), (ins srnum:$sr, GPR:$ra), "xsr", "$rd, $sr, $ra">;
def SRSET : GVMOp<0x1b, FrmXSR, (outs GPR:$rd), (ins srnum:$sr, GPR:$ra), "srset", "$rd, $sr, $ra">;
def SRCLR : GVMOp<0x1c, FrmXSR, (outs GPR:$rd), (ins srnum:$sr, GPR:$ra), "srclr", "$rd, $sr, $ra">;
}

//===----------------------------------------------------------------------===//
// Control ops (the control word)
//===----------------------------------------------------------------------===//

let isBranch = 1, isTerminator = 1, hasSideEffects = 0, mayLoad = 0,
    mayStore = 0 in {
class GVM_Br<bits<4> cc, string opcodestr>
    : GVMCtl<cc, 0, CtlBr, (outs), (ins GPR:$ra, GPR:$rb, brtarget:$target),
             opcodestr, "$ra, $rb, $target">;
class GVM_BrI<bits<4> cc, string opcodestr>
    : GVMCtl<cc, 0, CtlBrI, (outs), (ins GPR:$ra, simm6:$imm, brtarget:$target),
             opcodestr, "$ra, $imm, $target">;
def BLTU  : GVM_Br<2, "bltu">;
def BGEU  : GVM_Br<3, "bgeu">;
def BEQ   : GVM_Br<4, "beq">;
def BNE   : GVM_Br<5, "bne">;
def BLT   : GVM_Br<6, "blt">;
def BGE   : GVM_Br<7, "bge">;
def BBS : GVMCtl<8, 0, CtlBit, (outs), (ins GPR:$ra, uimm5:$bit, brtarget:$target),
                 "bbs", "$ra, $bit, $target">;
def BBC : GVMCtl<9, 0, CtlBit, (outs), (ins GPR:$ra, uimm5:$bit, brtarget:$target),
                 "bbc", "$ra, $bit, $target">;
def BLTUI : GVM_BrI<10, "bltui">;
def BGEUI : GVM_BrI<11, "bgeui">;
def BEQI  : GVM_BrI<12, "beqi">;
def BNEI  : GVM_BrI<13, "bnei">;
def BLTI  : GVM_BrI<14, "blti">;
def BGEI  : GVM_BrI<15, "bgei">;
let Constraints = "$rd = $ra" in
def ADDBNZ : GVMCtl<1, 0, CtlAddBnz, (outs GPR:$rd),
                    (ins GPR:$ra, simm6:$imm, brtarget:$target), "addbnz",
                    "$ra, $imm, $target">;
}

let isBranch = 1, isTerminator = 1, isBarrier = 1, hasSideEffects = 0,
    mayLoad = 0, mayStore = 0 in {
// Near (beq r0, r0: +-4 KB) or far (cc 0, sub 5: +-2 MB), chosen when the
// target is known.
def J : GVMCtl<4, 5, CtlJ, (outs), (ins brtarget:$target), "j", "$target">;
let isIndirectBranch = 1 in
def JR : GVMCtl<0, 2, CtlJR, (outs), (ins GPR:$rs), "jr", "$rs">;
}

let isCall = 1, Defs = [R1], hasSideEffects = 0, mayLoad = 0,
    mayStore = 0 in {
def CALL : GVMCtl<0, 6, CtlCall, (outs), (ins calltarget:$target), "call",
                  "$target">;
def CALLR : GVMCtl<0, 3, CtlJR, (outs), (ins GPR:$rs), "callr", "$rs">;
}

let isReturn = 1, isTerminator = 1, isBarrier = 1, hasSideEffects = 1,
    mayLoad = 0, mayStore = 0 in
def ERET : GVMCtl<0, 4, CtlNone, (outs), (ins), "eret", "">;

// rs += imm12 after the bundle's ops, no branch.
let Constraints = "$rd = $rs", hasSideEffects = 0, mayLoad = 0,
    mayStore = 0 in
def BUMP : GVMCtl<0, 7, CtlBump, (outs GPR:$rd), (ins GPR:$rs, simm12:$imm),
                  "bump", "$rs, $imm">;

//===----------------------------------------------------------------------===//
// Aliases (ASM.md, "Pseudo-instructions")
//===----------------------------------------------------------------------===//

def : InstAlias<"nop", (ADD R0, R0, R0)>;
def : InstAlias<"li $rd, $imm", (ADDI GPR:$rd, R0, imm32:$imm)>;
def : InstAlias<"la $rd, $imm", (ADDI GPR:$rd, R0, imm32:$imm), 0>;
def : InstAlias<"lla $rd, $target", (LPC GPR:$rd, pcrel32:$target), 0>;
def : InstAlias<"mv $rd, $ra", (ADDI GPR:$rd, GPR:$ra, 0)>;
def : InstAlias<"fmv $rd, $ra", (ADDI GPR:$rd, GPR:$ra, 0), 0>;
def : InstAlias<"not $rd, $ra", (XORI GPR:$rd, GPR:$ra, -1)>;
def : InstAlias<"neg $rd, $ra", (SUB GPR:$rd, R0, GPR:$ra)>;
def : InstAlias<"seqz $rd, $ra", (SEQI GPR:$rd, GPR:$ra, 0)>;
def : InstAlias<"snez $rd, $ra", (SNEI GPR:$rd, GPR:$ra, 0)>;
def : InstAlias<"sgt $rd, $ra, $rb", (SLT GPR:$rd, GPR:$rb, GPR:$ra), 0>;
def : InstAlias<"sgtu $rd, $ra, $rb", (SLTU GPR:$rd, GPR:$rb, GPR:$ra), 0>;
def : InstAlias<"sle $rd, $ra, $rb", (SGE GPR:$rd, GPR:$rb, GPR:$ra), 0>;
def : InstAlias<"sleu $rd, $ra, $rb", (SGEU GPR:$rd, GPR:$rb, GPR:$ra), 0>;
def : InstAlias<"fneg $rd, $ra", (FSGNJN GPR:$rd, GPR:$ra, GPR:$ra)>;
def : InstAlias<"fabs $rd, $ra", (FSGNJX GPR:$rd, GPR:$ra, GPR:$ra)>;
def : InstAlias<"fgt $rd, $ra, $rb", (FLT GPR:$rd, GPR:$rb, GPR:$ra), 0>;
def : InstAlias<"fge $rd, $ra, $rb", (FLE GPR:$rd, GPR:$rb, GPR:$ra), 0>;

// memory: `lw rd, (ra)` and `lw rd, sym`
multiclass GVM_MemAlias<string opcodestr, Instruction I> {
  def : InstAlias<opcodestr # " $rd, (${ra})", (I GPR:$rd, GPR:$ra, 0), 0>;
  def : InstAlias<opcodestr # " $rd, $imm", (I GPR:$rd, R0, imm32:$imm), 0>;
}
defm : GVM_MemAlias<"lw", LW>;
defm : GVM_MemAlias<"ld", LD>;
defm : GVM_MemAlias<"lh", LH>;
defm : GVM_MemAlias<"lb", LB>;
defm : GVM_MemAlias<"lq", LQ>;
defm : GVM_MemAlias<"lhu", LHU>;
defm : GVM_MemAlias<"lbu", LBU>;
defm : GVM_MemAlias<"sw", SW>;
defm : GVM_MemAlias<"sd", SD>;
defm : GVM_MemAlias<"sh", SH>;
defm : GVM_MemAlias<"sb", SB>;
defm : GVM_MemAlias<"sq", SQ>;

def : InstAlias<"beqz $ra, $target", (BEQ GPR:$ra, R0, brtarget:$target), 0>;
def : InstAlias<"bnez $ra, $target", (BNE GPR:$ra, R0, brtarget:$target), 0>;
def : InstAlias<"bltz $ra, $target", (BLT GPR:$ra, R0, brtarget:$target), 0>;
def : InstAlias<"bgez $ra, $target", (BGE GPR:$ra, R0, brtarget:$target), 0>;
def : InstAlias<"blez $ra, $target", (BGE R0, GPR:$ra, brtarget:$target), 0>;
def : InstAlias<"bgtz $ra, $target", (BLT R0, GPR:$ra, brtarget:$target), 0>;
def : InstAlias<"bgt $ra, $rb, $target", (BLT GPR:$rb, GPR:$ra, brtarget:$target), 0>;
def : InstAlias<"ble $ra, $rb, $target", (BGE GPR:$rb, GPR:$ra, brtarget:$target), 0>;
def : InstAlias<"bgtu $ra, $rb, $target", (BLTU GPR:$rb, GPR:$ra, brtarget:$target), 0>;
def : InstAlias<"bleu $ra, $rb, $target", (BGEU GPR:$rb, GPR:$ra, brtarget:$target), 0>;
def : InstAlias<"ret", (JR R1)>;
def : InstAlias<"tail $target", (J brtarget:$target), 0>;
