//===-- GPUVMRegisterInfo.td - gpu_vm registers ------------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// 64 32-bit registers; a float lives in the same registers (gpu_vm/GPU_VM.md,
// "ABI"). r0 reads 0. The alternative names are the ABI's (ILP32).
//
//===----------------------------------------------------------------------===//

let Namespace = "GPUVM" in {
def ABIRegAltName : RegAltNameIndex;
def sub0 : SubRegIndex<32>;
def sub1 : SubRegIndex<32, 32>;
def sub2 : SubRegIndex<32, 64>;
def sub3 : SubRegIndex<32, 96>;

class GPUVMReg<bits<6> Enc, string n, string alt> : Register<n> {
  let HWEncoding{5-0} = Enc;
  let AltNames = [alt];
  let RegAltNameIndices = [ABIRegAltName];
}
}

let Namespace = "GPUVM" in {
let isConstant = true in // reads 0 always
def R0 : GPUVMReg<0, "r0", "zero">, DwarfRegNum<[0]>;
def R1 : GPUVMReg<1, "r1", "ra">, DwarfRegNum<[1]>;
def R2 : GPUVMReg<2, "r2", "sp">, DwarfRegNum<[2]>;
def R3 : GPUVMReg<3, "r3", "gp">, DwarfRegNum<[3]>;
def R4 : GPUVMReg<4, "r4", "tp">, DwarfRegNum<[4]>;
def R5 : GPUVMReg<5, "r5", "a0">, DwarfRegNum<[5]>;
def R6 : GPUVMReg<6, "r6", "a1">, DwarfRegNum<[6]>;
def R7 : GPUVMReg<7, "r7", "a2">, DwarfRegNum<[7]>;
def R8 : GPUVMReg<8, "r8", "a3">, DwarfRegNum<[8]>;
def R9 : GPUVMReg<9, "r9", "a4">, DwarfRegNum<[9]>;
def R10 : GPUVMReg<10, "r10", "a5">, DwarfRegNum<[10]>;
def R11 : GPUVMReg<11, "r11", "a6">, DwarfRegNum<[11]>;
def R12 : GPUVMReg<12, "r12", "a7">, DwarfRegNum<[12]>;
def R13 : GPUVMReg<13, "r13", "t0">, DwarfRegNum<[13]>;
def R14 : GPUVMReg<14, "r14", "t1">, DwarfRegNum<[14]>;
def R15 : GPUVMReg<15, "r15", "t2">, DwarfRegNum<[15]>;
def R16 : GPUVMReg<16, "r16", "t3">, DwarfRegNum<[16]>;
def R17 : GPUVMReg<17, "r17", "t4">, DwarfRegNum<[17]>;
def R18 : GPUVMReg<18, "r18", "t5">, DwarfRegNum<[18]>;
def R19 : GPUVMReg<19, "r19", "t6">, DwarfRegNum<[19]>;
def R20 : GPUVMReg<20, "r20", "t7">, DwarfRegNum<[20]>;
def R21 : GPUVMReg<21, "r21", "t8">, DwarfRegNum<[21]>;
def R22 : GPUVMReg<22, "r22", "t9">, DwarfRegNum<[22]>;
def R23 : GPUVMReg<23, "r23", "t10">, DwarfRegNum<[23]>;
def R24 : GPUVMReg<24, "r24", "t11">, DwarfRegNum<[24]>;
def R25 : GPUVMReg<25, "r25", "t12">, DwarfRegNum<[25]>;
def R26 : GPUVMReg<26, "r26", "t13">, DwarfRegNum<[26]>;
def R27 : GPUVMReg<27, "r27", "t14">, DwarfRegNum<[27]>;
def R28 : GPUVMReg<28, "r28", "t15">, DwarfRegNum<[28]>;
def R29 : GPUVMReg<29, "r29", "t16">, DwarfRegNum<[29]>;
def R30 : GPUVMReg<30, "r30", "t17">, DwarfRegNum<[30]>;
def R31 : GPUVMReg<31, "r31", "t18">, DwarfRegNum<[31]>;
def R32 : GPUVMReg<32, "r32", "s0">, DwarfRegNum<[32]>;
def R33 : GPUVMReg<33, "r33", "s1">, DwarfRegNum<[33]>;
def R34 : GPUVMReg<34, "r34", "s2">, DwarfRegNum<[34]>;
def R35 : GPUVMReg<35, "r35", "s3">, DwarfRegNum<[35]>;
def R36 : GPUVMReg<36, "r36", "s4">, DwarfRegNum<[36]>;
def R37 : GPUVMReg<37, "r37", "s5">, DwarfRegNum<[37]>;
def R38 : GPUVMReg<38, "r38", "s6">, DwarfRegNum<[38]>;
def R39 : GPUVMReg<39, "r39", "s7">, DwarfRegNum<[39]>;
def R40 : GPUVMReg<40, "r40", "s8">, DwarfRegNum<[40]>;
def R41 : GPUVMReg<41, "r41", "s9">, DwarfRegNum<[41]>;
def R42 : GPUVMReg<42, "r42", "s10">, DwarfRegNum<[42]>;
def R43 : GPUVMReg<43, "r43", "s11">, DwarfRegNum<[43]>;
def R44 : GPUVMReg<44, "r44", "s12">, DwarfRegNum<[44]>;
def R45 : GPUVMReg<45, "r45", "s13">, DwarfRegNum<[45]>;
def R46 : GPUVMReg<46, "r46", "s14">, DwarfRegNum<[46]>;
def R47 : GPUVMReg<47, "r47", "s15">, DwarfRegNum<[47]>;
def R48 : GPUVMReg<48, "r48", "s16">, DwarfRegNum<[48]>;
def R49 : GPUVMReg<49, "r49", "s17">, DwarfRegNum<[49]>;
def R50 : GPUVMReg<50, "r50", "s18">, DwarfRegNum<[50]>;
def R51 : GPUVMReg<51, "r51", "s19">, DwarfRegNum<[51]>;
def R52 : GPUVMReg<52, "r52", "s20">, DwarfRegNum<[52]>;
def R53 : GPUVMReg<53, "r53", "s21">, DwarfRegNum<[53]>;
def R54 : GPUVMReg<54, "r54", "s22">, DwarfRegNum<[54]>;
def R55 : GPUVMReg<55, "r55", "s23">, DwarfRegNum<[55]>;
def R56 : GPUVMReg<56, "r56", "t19">, DwarfRegNum<[56]>;
def R57 : GPUVMReg<57, "r57", "t20">, DwarfRegNum<[57]>;
def R58 : GPUVMReg<58, "r58", "t21">, DwarfRegNum<[58]>;
def R59 : GPUVMReg<59, "r59", "t22">, DwarfRegNum<[59]>;
def R60 : GPUVMReg<60, "r60", "t23">, DwarfRegNum<[60]>;
def R61 : GPUVMReg<61, "r61", "t24">, DwarfRegNum<[61]>;
def R62 : GPUVMReg<62, "r62", "t25">, DwarfRegNum<[62]>;
def R63 : GPUVMReg<63, "r63", "t26">, DwarfRegNum<[63]>;
}

// The allocation order is the codegen's to tune: temporaries and arguments
// first, then the callee-saved, the reserved ones last.
def GPR : RegisterClass<"GPUVM", [i32, f32], 32,
    (add (sequence "R%u", 13, 31), (sequence "R%u", 56, 63),
         (sequence "R%u", 5, 12), (sequence "R%u", 33, 55), R32,
         R0, R1, R2, R3, R4)>;

// Four consecutive registers, any first: what `lq` and `sq` move (a texel).
// The codegen's only (LQ_Q, SQ_Q: inline copies, the commit's operations'
// descriptors); the MC operand is the first register (sub0).
def GPRQTuples : RegisterTuples<[sub0, sub1, sub2, sub3],
                                [(sequence "R%u", 0, 60),
                                 (sequence "R%u", 1, 61),
                                 (sequence "R%u", 2, 62),
                                 (sequence "R%u", 3, 63)]>;
def GPRQ : RegisterClass<"GPUVM", [untyped], 128, (add GPRQTuples)> {
  let Size = 128;
}
