add_llvm_component_group(GPUVM)

set(LLVM_TARGET_DEFINITIONS GPUVM.td)

tablegen(LLVM GPUVMGenAsmMatcher.inc -gen-asm-matcher)
tablegen(LLVM GPUVMGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM GPUVMGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM GPUVMGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM GPUVMGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(GPUVMCommonTableGen)

add_llvm_target(GPUVMCodeGen
  GPUVMTargetMachine.cpp

  LINK_COMPONENTS
  Support

  ADD_TO_COMPONENT
  GPUVM
  )

add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
