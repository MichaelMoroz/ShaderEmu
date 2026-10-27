//===-- GPUVMInstrFormats.td - gpu_vm instruction formats --*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// An MCInst is one op (an op word, perhaps with the bundle's long immediate)
// or one control op (the control word). The object streamer packs them into
// bundles. The encoding is not TableGen's: an instruction carries its opcode
// byte (Opc; for a control op cc | sub << 4) and its operand layout (Form) in
// TSFlags, and GPUVMMCCodeEmitter / GPUVMDisassembler do the bits. Keep Form
// in step with GPUVMII::Form in MCTargetDesc/GPUVMBaseInfo.h.
//
//===----------------------------------------------------------------------===//

class Form<bits<5> val> {
  bits<5> Value = val;
}
def FrmPseudo : Form<0>;
def FrmR      : Form<1>;  // rd, ra, rb
def FrmR4     : Form<2>;  // rd, ra, rb, rc (a register)
def FrmRSh    : Form<3>;  // rd, ra, rb, sh (rc a shift)
def FrmR1     : Form<4>;  // rd, ra
def FrmI      : Form<5>;  // rd, ra, imm
def FrmFI     : Form<6>;  // rd, ra, fimm (a float's bits)
def FrmBF     : Form<7>;  // rd, ra, pos, len (Variant: signed)
def FrmBFI    : Form<8>;  // rd, rd (tied), ra, pos, len
def FrmLoad   : Form<9>;  // rd, ra, imm
def FrmStore  : Form<10>; // rs, ra, imm
def FrmLoadX  : Form<11>; // rd, ra, rb, sh
def FrmStoreX : Form<12>; // rs, ra, rb, sh
def FrmAMO    : Form<13>; // rd, rb, ra
def FrmCAS    : Form<14>; // rd, rd (tied), rb, ra
def FrmLPC    : Form<15>; // rd, target
def FrmNone   : Form<16>; // no operands
def FrmMFSR   : Form<17>; // rd, sr
def FrmMTSR   : Form<18>; // sr, ra
def FrmXSR    : Form<19>; // rd, sr, ra
def FrmUnimp  : Form<20>; // the op word 0
def CtlBr     : Form<21>; // ra, rb, target
def CtlBrI    : Form<22>; // ra, imm6, target
def CtlBit    : Form<23>; // ra, bit, target
def CtlAddBnz : Form<24>; // rd, ra (tied), imm6, target
def CtlJ      : Form<25>; // target (near or far)
def CtlCall   : Form<26>; // target
def CtlJR     : Form<27>; // rs
def CtlNone   : Form<28>; // eret
def CtlBump   : Form<29>; // rd, rs (tied), imm12
def FrmPair   : Form<30>; // rd, rb, ra, imm (ld2) or rs, rs2, ra, imm (sd2): imm in rc's words or long

class GPUVMInst<dag outs, dag ins, string asmstr, list<dag> pattern,
                Form form>
    : Instruction {
  let Namespace = "GPUVM";
  let OutOperandList = outs;
  let InOperandList = ins;
  let AsmString = asmstr;
  let Pattern = pattern;
  let Size = 4;

  bits<8> Opc = 0;
  bit IsControl = 0;
  bit Variant = 0;
  let TSFlags{7-0} = Opc;
  let TSFlags{12-8} = form.Value;
  let TSFlags{13} = IsControl;
  let TSFlags{14} = Variant;
}

// An op of the op slots.
class GVMOp<bits<8> opc, Form form, dag outs, dag ins, string opcodestr,
            string argstr, list<dag> pattern = []>
    : GPUVMInst<outs, ins, opcodestr # "\t" # argstr, pattern, form> {
  let Opc = opc;
}

// A control op: the bundle's control word. cc, and for cc 0 sub.
class GVMCtl<bits<4> cc, bits<3> sub, Form form, dag outs, dag ins,
             string opcodestr, string argstr, list<dag> pattern = []>
    : GPUVMInst<outs, ins, opcodestr # "\t" # argstr, pattern, form> {
  let Opc{3-0} = cc;
  let Opc{6-4} = sub;
  let IsControl = 1;
}

class GPUVMPseudo<dag outs, dag ins, list<dag> pattern = []>
    : GPUVMInst<outs, ins, "", pattern, FrmPseudo> {
  let isPseudo = 1;
  let isCodeGenOnly = 1;
}
