cmake_minimum_required(VERSION 3.0)
project(LLAIR)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${LLVM_CXX_FLAGS} -std=c++14")

add_subdirectory(lib/IR)
add_subdirectory(examples)
