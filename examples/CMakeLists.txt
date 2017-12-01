include_directories(
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/include
)
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

llvm_map_components_to_libnames(
  llvm_libs
  support core bitreader bitwriter
)

# llair-test
add_executable(
  llair-test
  test.cpp
)

target_link_libraries(
  llair-test
  LLAIR
  ${llvm_libs}
)

# metalc
add_executable(
  metalc
  MetalC.cpp
)

target_link_libraries(
  metalc
  LLAIR LLAIRTools
  ${llvm_libs}
)
