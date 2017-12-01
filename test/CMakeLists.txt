include_directories(
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/include
)
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(
  llvm_libs
  support core
)

add_executable(
  llair-test
  test.cpp
)

target_link_libraries(
  llair-test
  LLAIR
  ${llvm_libs}
)


