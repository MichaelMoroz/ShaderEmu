add_llvm_component_library(LLVMGPUVMDisassembler
  GPUVMDisassembler.cpp

  LINK_COMPONENTS
  GPUVMDesc
  GPUVMInfo
  MC
  MCDisassembler
  Support

  ADD_TO_COMPONENT
  GPUVM
  )
