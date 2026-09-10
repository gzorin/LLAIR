add_definitions(${LLVM_DEFINITIONS})

add_library(LLAIRTools STATIC
  Compile.cpp
  CompileSession.cpp
  MakeLibrary.cpp
  Program.cpp
  Tools.cpp
  popen2.c)

# The source headers must precede LLVM's include dir: an earlier install of
# llair puts its own copies of these headers there.
target_include_directories(LLAIRTools BEFORE
  PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_include_directories(LLAIRTools
  PUBLIC ${LLVM_INCLUDE_DIRS})

target_compile_features(LLAIRTools PRIVATE cxx_std_17)

llvm_map_components_to_libnames(LLVM_LIBRARIES support bitreader bitwriter passes metallib bitwriter50)

target_link_libraries(LLAIRTools LLAIR LLAIRBitcode LLAIRLinker ${LLVM_LIBRARIES})

install(
  TARGETS LLAIRTools
  EXPORT LLAIRTargets
  ARCHIVE
  DESTINATION lib)
