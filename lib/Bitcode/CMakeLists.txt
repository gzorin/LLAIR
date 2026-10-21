add_library(LLAIRBitcode STATIC
  Bitcode.cpp)

target_compile_definitions(LLAIRBitcode PRIVATE ${LLVM_DEFINITIONS_LIST})

llvm_map_components_to_libnames(LLVM_LIBRARIES core bitreader bitwriter)

target_link_libraries(LLAIRBitcode PUBLIC LLAIR ${LLVM_LIBRARIES})

install(
  TARGETS LLAIRBitcode
  EXPORT LLAIRTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
