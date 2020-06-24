include_directories(
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

llvm_map_components_to_libnames(
  llvm_libs
  support core bitreader bitwriter
)

configure_file(config.h.in config.h)

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

# make-library
add_metal_sources(MakeLibrary_METAL_SOURCES example.metal)
add_compiled_metal_sources(MakeLibrary_COMPILED_METAL_SOURCES example.metal)

add_executable(
  make-library
  MakeLibrary.mm
  ${MakeLibrary_METAL_SOURCES}
  ${MakeLibrary_COMPILED_METAL_SOURCES}
)

target_link_libraries(
  make-library
  LLAIR LLAIRBitcode LLAIRTools
  ${llvm_libs}
  "-ObjC++"
  "-framework Foundation"
  "-framework Metal"
)
