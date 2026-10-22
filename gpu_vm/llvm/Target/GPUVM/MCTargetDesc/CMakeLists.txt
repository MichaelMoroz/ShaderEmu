add_llvm_component_library(LLVMGPUVMDesc
  GPUVMAsmBackend.cpp
  GPUVMELFObjectWriter.cpp
  GPUVMELFStreamer.cpp
  GPUVMInstPrinter.cpp
  GPUVMMCAsmInfo.cpp
  GPUVMMCCodeEmitter.cpp
  GPUVMMCTargetDesc.cpp

  LINK_COMPONENTS
  GPUVMInfo
  MC
  Support
  TargetParser

  ADD_TO_COMPONENT
  GPUVM
  )
