llvm_map_components_to_libnames(LLVM_LIBRARIES core support bitreader bitwriter transformutils)

add_executable(llair-link
  llair-link.cpp)

target_compile_definitions(llair-link PRIVATE ${LLVM_DEFINITIONS_LIST})

target_link_libraries(llair-link
  PRIVATE
  LLAIR LLAIRBitcode LLAIRLinker
  ${LLVM_LIBRARIES})

install(
  TARGETS llair-link
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
