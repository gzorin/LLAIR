add_definitions(${LLVM_DEFINITIONS})

add_library(LLAIR STATIC
  Class.cpp
  Dispatcher.cpp
  EntryPoint.cpp
  Interface.cpp
  LLAIRContext.cpp
  Module.cpp
  Named.cpp
  SymbolTable.cpp)

target_include_directories(LLAIR BEFORE
  PUBLIC  ${LLVM_INCLUDE_DIRS}
  PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/extsrc/llvm-demangle/include)

target_compile_features(LLAIR PRIVATE cxx_std_17)

llvm_map_components_to_libnames(LLVM_LIBRARIES core)

target_link_libraries(LLAIR ${LLVM_LIBRARIES} ${CMAKE_BINARY_DIR}/extsrc/llvm-demangle/lib/Demangle/libLLAIRDemangleLib.a)

install(
  TARGETS LLAIR
  EXPORT LLAIRTargets
  ARCHIVE
  DESTINATION lib)
