include_directories(
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/include
)
add_definitions(${LLVM_DEFINITIONS})

add_library(LLAIRLinker STATIC
  Linker.cpp)

install(
  TARGETS LLAIRLinker
  ARCHIVE
  DESTINATION lib
)
