add_library(LLAIR STATIC
  Class.cpp
  Dispatcher.cpp
  EntryPoint.cpp
  Interface.cpp
  LLAIRContext.cpp
  Module.cpp
  Named.cpp
  SymbolTable.cpp)

target_compile_definitions(LLAIR PRIVATE ${LLVM_DEFINITIONS_LIST})

# The source headers must precede LLVM's include dir: an earlier install of
# llair puts its own copies of these headers there.
target_include_directories(LLAIR BEFORE
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    ${LLVM_INCLUDE_DIRS})

llvm_map_components_to_libnames(LLVM_LIBRARIES core)

target_link_libraries(LLAIR
  PUBLIC  ${LLVM_LIBRARIES}
  PRIVATE LLAIRDemangleLib)

install(
  TARGETS LLAIR
  EXPORT LLAIRTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
