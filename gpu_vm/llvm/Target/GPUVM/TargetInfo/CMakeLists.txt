add_llvm_component_library(LLVMGPUVMInfo
  GPUVMTargetInfo.cpp

  LINK_COMPONENTS
  MC
  Support

  ADD_TO_COMPONENT
  GPUVM
  )
