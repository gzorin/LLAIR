llvm_map_components_to_libnames(LLVM_LIBRARIES core support bitreader)

add_executable(llair-dump
  llair-dump.cpp)

target_compile_definitions(llair-dump PRIVATE ${LLVM_DEFINITIONS_LIST})

target_link_libraries(llair-dump
  PRIVATE
  LLAIR LLAIRBitcode LLAIRLinker
  ${LLVM_LIBRARIES})

install(
  TARGETS llair-dump
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
