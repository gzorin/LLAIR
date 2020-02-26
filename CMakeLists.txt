cmake_minimum_required(VERSION 3.14)
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

install(
  EXPORT LLAIRTargets
  DESTINATION lib/cmake/llair)

add_subdirectory(extsrc/llvm-demangle)
add_subdirectory(include)
add_subdirectory(lib/Bitcode)
add_subdirectory(lib/IR)
add_subdirectory(lib/Linker)
add_subdirectory(lib/Tools)
add_subdirectory(tools/llair-link)
add_subdirectory(examples)

include(CMakePackageConfigHelpers)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/LLAIRConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/LLAIRConfig.cmake
  INSTALL_DESTINATION lib/cmake/llair)

install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/LLAIRConfig.cmake
  DESTINATION lib/cmake/llair)
