llvm_map_components_to_libnames(LLVM_LIBRARIES core support bitreader bitwriter transformutils passes metallib bitwriter50)

add_executable(llair-metallib
  llair-metallib.cpp)

target_compile_features(llair-metallib PUBLIC cxx_std_17)

target_include_directories(llair-metallib BEFORE
  PRIVATE
  ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/extsrc/llvm-demangle/include)

target_include_directories(llair-metallib
  PUBLIC
  ${LLAIR_DEPS_DIR}/include
  ${LLVM_INCLUDE_DIRS})

target_link_libraries(llair-metallib
  LLAIR LLAIRBitcode LLAIRLinker LLAIRTools LLAIRDemangleLib
  ${LLVM_LIBRARIES})

install(
  TARGETS llair-metallib
  DESTINATION bin)