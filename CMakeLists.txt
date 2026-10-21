cmake_minimum_required(VERSION 3.25)
project(LLAIR LANGUAGES C CXX OBJCXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/modules")

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(metal)

get_filename_component(
  LLAIR_TOOLS_PATH "${METAL}" DIRECTORY)

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

set(LLAIR_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/llair")

install(
  EXPORT LLAIRTargets
  DESTINATION ${LLAIR_INSTALL_CMAKEDIR})

include(CMakePackageConfigHelpers)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/LLAIRConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/LLAIRConfig.cmake
  INSTALL_DESTINATION ${LLAIR_INSTALL_CMAKEDIR})

install(
  FILES ${CMAKE_CURRENT_BINARY_DIR}/LLAIRConfig.cmake
  DESTINATION ${LLAIR_INSTALL_CMAKEDIR})
