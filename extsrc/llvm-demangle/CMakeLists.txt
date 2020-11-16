cmake_minimum_required(VERSION 3.4.3)

project(LLAIRDemangle)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++14")

include_directories("${CMAKE_SOURCE_DIR}/extsrc/llvm-demangle/include")

add_subdirectory(include)
add_subdirectory(lib/Demangle)
