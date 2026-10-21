add_library(LLAIRTools STATIC
  Compile.cpp
  CompileSession.cpp
  MakeLibrary.cpp
  Program.cpp
  Tools.cpp
  popen2.c)

target_compile_definitions(LLAIRTools PRIVATE ${LLVM_DEFINITIONS_LIST})

llvm_map_components_to_libnames(LLVM_LIBRARIES support bitreader bitwriter passes metallib bitwriter140 codegen)

target_link_libraries(LLAIRTools PUBLIC LLAIR LLAIRBitcode LLAIRLinker ${LLVM_LIBRARIES})

install(
  TARGETS LLAIRTools
  EXPORT LLAIRTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
