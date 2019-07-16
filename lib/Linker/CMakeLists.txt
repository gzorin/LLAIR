add_definitions(${LLVM_DEFINITIONS})

add_library(LLAIRLinker STATIC
  Linker.cpp)

target_include_directories(LLAIRLinker
  PUBLIC  ${LLVM_INCLUDE_DIRS}
  PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_compile_features(LLAIRLinker PRIVATE cxx_std_17)

install(
  TARGETS LLAIRLinker
  EXPORT LLAIRTargets
  ARCHIVE
  DESTINATION lib)
