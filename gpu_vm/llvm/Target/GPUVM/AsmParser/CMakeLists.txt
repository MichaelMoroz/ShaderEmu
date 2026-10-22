add_llvm_component_library(LLVMGPUVMAsmParser
  GPUVMAsmParser.cpp

  LINK_COMPONENTS
  GPUVMDesc
  GPUVMInfo
  MC
  MCParser
  Support

  ADD_TO_COMPONENT
  GPUVM
  )
