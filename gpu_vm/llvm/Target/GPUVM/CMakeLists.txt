add_llvm_component_group(GPUVM)

set(LLVM_TARGET_DEFINITIONS GPUVM.td)

tablegen(LLVM GPUVMGenAsmMatcher.inc -gen-asm-matcher)
tablegen(LLVM GPUVMGenAsmWriter.inc -gen-asm-writer)
tablegen(LLVM GPUVMGenCallingConv.inc -gen-callingconv)
tablegen(LLVM GPUVMGenDAGISel.inc -gen-dag-isel)
tablegen(LLVM GPUVMGenInstrInfo.inc -gen-instr-info)
tablegen(LLVM GPUVMGenRegisterInfo.inc -gen-register-info)
tablegen(LLVM GPUVMGenSDNodeInfo.inc -gen-sd-node-info)
tablegen(LLVM GPUVMGenSubtargetInfo.inc -gen-subtarget)

add_public_tablegen_target(GPUVMCommonTableGen)

add_llvm_target(GPUVMCodeGen
  GPUVMAsmPrinter.cpp
  GPUVMBranchFill.cpp
  GPUVMBundleModel.cpp
  GPUVMBundleScheduler.cpp
  GPUVMCountedLoops.cpp
  GPUVMPairs.cpp
  GPUVMFrameLowering.cpp
  GPUVMGlobalFill.cpp
  GPUVMHoistConstants.cpp
  GPUVMInstrInfo.cpp
  GPUVMISelDAGToDAG.cpp
  GPUVMLoadHoist.cpp
  GPUVMRebase.cpp
  GPUVMISelLowering.cpp
  GPUVMRegisterInfo.cpp
  GPUVMSelectionDAGInfo.cpp
  GPUVMSubtarget.cpp
  GPUVMTargetMachine.cpp
  GPUVMTargetTransformInfo.cpp
  GPUVMUnrollHint.cpp
  GPUVMZeroExt.cpp

  LINK_COMPONENTS
  Analysis
  AsmPrinter
  CodeGen
  CodeGenTypes
  Core
  GPUVMDesc
  GPUVMInfo
  MC
  Passes
  ScalarOpts
  SelectionDAG
  Support
  Target
  TargetParser
  TransformUtils

  ADD_TO_COMPONENT
  GPUVM
  )

add_subdirectory(AsmParser)
add_subdirectory(Disassembler)
add_subdirectory(MCTargetDesc)
add_subdirectory(TargetInfo)
