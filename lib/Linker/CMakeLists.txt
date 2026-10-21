add_library(LLAIRLinker STATIC
  Linker.cpp)

target_compile_definitions(LLAIRLinker PRIVATE ${LLVM_DEFINITIONS_LIST})

target_link_libraries(LLAIRLinker PUBLIC LLAIR)

install(
  TARGETS LLAIRLinker
  EXPORT LLAIRTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
