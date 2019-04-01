cmake_minimum_required(VERSION 3.0)
project(LLAIR)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")

find_program(XXD xxd)

find_program(XCRUN xcrun)

execute_process(
  COMMAND ${XCRUN} --sdk macosx --find metal
  OUTPUT_VARIABLE METAL
  OUTPUT_STRIP_TRAILING_WHITESPACE)

add_subdirectory(include)
add_subdirectory(lib/Bitcode)
add_subdirectory(lib/IR)
add_subdirectory(lib/Linker)
add_subdirectory(lib/Tools)
add_subdirectory(examples)
