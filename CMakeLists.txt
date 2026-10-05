cmake_minimum_required(VERSION 3.20)
project(ShaderX86 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "The harness uses Direct3D 11 and FXC; build it on Windows.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Executables land in bin/ (bin/debug/ for Debug) regardless of generator.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
foreach(cfg RELEASE RELWITHDEBINFO MINSIZEREL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfg} ${CMAKE_SOURCE_DIR}/bin)
endforeach()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/bin/debug)

if(MSVC)
  add_compile_options(/W3 /utf-8 /permissive- /MP)
endif()

add_library(harness_core STATIC
  harness/core/common.cpp
  harness/core/gpu.cpp
  harness/core/shaderlab.cpp
  harness/core/shader_compiler.cpp
  harness/core/material.cpp
  harness/core/crt.cpp
  harness/core/readback.cpp
  harness/core/image.cpp
)
target_include_directories(harness_core PUBLIC harness/core)
target_compile_definitions(harness_core PUBLIC
  NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
  SHADERX86_UNITY_INCLUDE_DIR="${CMAKE_SOURCE_DIR}/harness/unity_include")
target_link_libraries(harness_core PUBLIC d3d11 dxgi d3dcompiler dxguid windowscodecs ole32)

add_executable(rvc_harness harness/apps/rvc_harness.cpp)
target_link_libraries(rvc_harness PRIVATE harness_core)
