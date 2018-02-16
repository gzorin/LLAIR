include_directories(
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/include
)
add_definitions(${LLVM_DEFINITIONS})

add_library(LLAIRBitcode STATIC
  Bitcode.cpp)

install(
  TARGETS LLAIRBitcode
  ARCHIVE
  DESTINATION lib
)
