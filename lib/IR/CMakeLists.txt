add_definitions(${LLVM_DEFINITIONS})

add_library(LLAIR STATIC
  Class.cpp
  EntryPoint.cpp
  Interface.cpp
  LLAIRContext.cpp
  Module.cpp)

target_include_directories(LLAIR
  PUBLIC  ${LLVM_INCLUDE_DIRS}
  PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_compile_features(LLAIR PRIVATE cxx_std_17)

llvm_map_components_to_libnames(LLVM_LIBRARIES core)

target_link_libraries(LLAIR ${LLVM_LIBRARIES})

install(
  TARGETS LLAIR
  EXPORT LLAIRTargets
  ARCHIVE
  DESTINATION lib)
