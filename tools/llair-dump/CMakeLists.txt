llvm_map_components_to_libnames(LLVM_LIBRARIES core support bitreader)

add_executable(llair-dump
  llair-dump.cpp)

target_compile_features(llair-dump PUBLIC cxx_std_17)

target_include_directories(llair-dump BEFORE
  PRIVATE
  ${CMAKE_SOURCE_DIR}/include)

target_include_directories(llair-dump
  PUBLIC
  ${LLAIR_DEPS_DIR}/include
  ${LLVM_INCLUDE_DIRS})

target_link_libraries(llair-dump
  LLAIR LLAIRBitcode LLAIRLinker LLAIRDemangleLib
  ${LLVM_LIBRARIES})