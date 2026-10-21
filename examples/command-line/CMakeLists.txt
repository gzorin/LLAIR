llvm_map_components_to_libnames(
  llvm_libs
  support core bitreader bitwriter transformutils metallib bitwriter140
)

configure_file(config.h.in config.h)

# llair-test
add_executable(
  llair-test
  test.cpp
)

target_link_libraries(
  llair-test
  PRIVATE
  LLAIR LLAIRLinker LLAIRTools LLAIRBitcode
  ${llvm_libs}
)

# metalc
add_executable(
  metalc
  MetalC.cpp
)

target_link_libraries(
  metalc
  PRIVATE
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
  PRIVATE
  LLAIR LLAIRBitcode LLAIRTools
  ${llvm_libs}
  "$<LINK_LIBRARY:FRAMEWORK,Foundation>"
  "$<LINK_LIBRARY:FRAMEWORK,Metal>"
)

foreach(target llair-test metalc make-library)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  target_compile_definitions(${target} PRIVATE ${LLVM_DEFINITIONS_LIST})
  target_compile_options(${target} PRIVATE -fno-rtti)
endforeach()
