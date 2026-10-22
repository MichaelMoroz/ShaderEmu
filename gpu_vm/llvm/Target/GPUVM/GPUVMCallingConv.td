//===-- GPUVMCallingConv.td - gpu_vm calling convention ----*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The ABI (gpu_vm/GPU_VM.md, "ABI"): arguments in a0-a7 (r5-r12), results in
// a0, a1. The assignment of arguments is CC_GPUVM in GPUVMISelLowering.cpp
// (a value of several parts goes whole in registers or whole on the stack;
// variadic arguments on the stack).
//
//===----------------------------------------------------------------------===//

// ra and s0-s23 are kept across a call.
def CSR_GPUVM : CalleeSavedRegs<(add R1, (sequence "R%u", 32, 55))>;

// For __attribute__((preserve_all)) style runtimes and interrupt handlers:
// nothing is clobbered but ra.
def CSR_GPUVM_All : CalleeSavedRegs<(add R1, (sequence "R%u", 5, 63))>;

def RetCC_GPUVM : CallingConv<[
  CCIfType<[i1, i8, i16], CCPromoteToType<i32>>,
  CCIfType<[i32, f32], CCAssignToReg<[R5, R6]>>
]>;
