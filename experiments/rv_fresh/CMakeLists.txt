cmake_minimum_required(VERSION 3.20)
project(fresh CXX)
add_executable(fresh bench.cpp)
set_target_properties(fresh PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_CURRENT_SOURCE_DIR}/../../bin)
