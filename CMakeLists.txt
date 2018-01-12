cmake_minimum_required(VERSION 3.0)
project(LLAIR)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14")

find_program(XXD xxd)

find_program(XCODE_SELECT xcode-select)

execute_process(
  COMMAND ${XCODE_SELECT} -p
  OUTPUT_VARIABLE XCODE_DEVELOPER_PATH
  OUTPUT_STRIP_TRAILING_WHITESPACE)

find_program(
  METAL metal
  PATHS ${XCODE_DEVELOPER_PATH}/Platforms/MacOSX.platform/usr/bin)

add_subdirectory(include)
add_subdirectory(lib/IR)
add_subdirectory(lib/Tools)
add_subdirectory(examples)
