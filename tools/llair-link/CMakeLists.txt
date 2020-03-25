llvm_map_components_to_libnames(LLVM_LIBRARIES core support bitreader bitwriter transformutils)

add_executable(llair-link
  llair-link.cpp Class.cpp InterfaceScope.cpp)

target_compile_features(llair-link PUBLIC cxx_std_17)

target_include_directories(llair-link BEFORE
  PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/extsrc/llvm-demangle/include)

target_include_directories(llair-link
  PUBLIC
  ${LLAIR_DEPS_DIR}/include
  ${LLVM_INCLUDE_DIRS})

target_link_libraries(llair-link
  LLAIR LLAIRBitcode LLAIRLinker LLVMDemangleLib
  ${LLVM_LIBRARIES})