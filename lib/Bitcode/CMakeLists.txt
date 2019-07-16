add_definitions(${LLVM_DEFINITIONS})

add_library(LLAIRBitcode STATIC
  Bitcode.cpp)

target_include_directories(LLAIRBitcode
  PUBLIC  ${LLVM_INCLUDE_DIRS}
  PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_compile_features(LLAIRBitcode PRIVATE cxx_std_17)

llvm_map_components_to_libnames(LLVM_LIBRARIES core bitreader bitwriter)

target_link_libraries(LLAIRBitcode LLAIR ${LLVM_LIBRARIES})

install(
  TARGETS LLAIRBitcode
  EXPORT LLAIRTargets
  ARCHIVE
  DESTINATION lib)
