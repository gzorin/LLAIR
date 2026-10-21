llvm_map_components_to_libnames(LLVM_LIBRARIES core support bitreader bitwriter transformutils passes metallib bitwriter140)

add_executable(llair-metallib
  llair-metallib.cpp)

target_compile_definitions(llair-metallib PRIVATE ${LLVM_DEFINITIONS_LIST})

target_link_libraries(llair-metallib
  PRIVATE
  LLAIR LLAIRBitcode LLAIRLinker LLAIRTools
  ${LLVM_LIBRARIES})

install(
  TARGETS llair-metallib
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
