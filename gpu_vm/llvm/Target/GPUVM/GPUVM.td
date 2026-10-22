//===-- GPUVM.td - Describe the gpu_vm target machine ------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// gpu_vm, the shader machine's own instruction set (gpu_vm/isa.h in ShaderEmu):
// bundles of 16 bytes, two op slots, a long immediate and a control word.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/Target.td"

include "GPUVMRegisterInfo.td"
include "GPUVMInstrFormats.td"
include "GPUVMInstrInfo.td"
include "GPUVMPatterns.td"
include "GPUVMCallingConv.td"

defm : RemapAllTargetPseudoPointerOperands<GPR>;

def : ProcessorModel<"generic", NoSchedModel, []>;

def GPUVMInstrInfo : InstrInfo;

def GPUVMAsmParser : AsmParser {
  let ShouldEmitMatchRegisterAltName = true;
  // The GPRQ tuples carry their first register's ABI name: the GPR wins.
  let AllowDuplicateRegisterNames = true;
}

def GPUVMAsmWriter : AsmWriter {
  int PassSubtarget = 1;
}

def GPUVM : Target {
  let InstructionSet = GPUVMInstrInfo;
  let AssemblyParsers = [GPUVMAsmParser];
  let AssemblyWriters = [GPUVMAsmWriter];
}
