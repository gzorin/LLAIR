add_definitions(${LLVM_DEFINITIONS})

add_library(LLAIRTools STATIC
  Compile.cpp
  MakeLibrary.cpp
  Program.cpp
  Tools.cpp
  popen2.c)

target_include_directories(LLAIRTools
  PUBLIC  ${LLVM_INCLUDE_DIRS}
  PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_compile_features(LLAIRTools PRIVATE cxx_std_17)

llvm_map_components_to_libnames(LLVM_LIBRARIES support bitreader bitwriter passes)

target_link_libraries(LLAIRTools LLAIR ${LLVM_LIBRARIES})

install(
  TARGETS LLAIRTools
  EXPORT LLAIRTargets
  ARCHIVE
  DESTINATION lib)
