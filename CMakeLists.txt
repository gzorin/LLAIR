cmake_minimum_required(VERSION 3.14)
project(LLAIR)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")

include(metal)

get_filename_component(
  LLAIR_TOOLS_PATH "${METAL}" DIRECTORY)

install(
  EXPORT LLAIRTargets
  DESTINATION lib/cmake/llair)

add_subdirectory(extsrc/llvm-demangle)
add_subdirectory(include)
add_subdirectory(lib/Bitcode)
add_subdirectory(lib/IR)
add_subdirectory(lib/Linker)
add_subdirectory(lib/Tools)
add_subdirectory(tools/llair-dump)
add_subdirectory(tools/llair-link)
add_subdirectory(tools/llair-metallib)
add_subdirectory(examples/command-line)
add_subdirectory(examples/interactive)

include(CMakePackageConfigHelpers)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/LLAIRConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/LLAIRConfig.cmake
  INSTALL_DESTINATION lib/cmake/llair)

install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/LLAIRConfig.cmake
  DESTINATION lib/cmake/llair)
