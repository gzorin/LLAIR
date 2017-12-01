cmake_minimum_required(VERSION 3.0)
project(LLAIR)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
