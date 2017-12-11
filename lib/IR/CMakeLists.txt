include_directories(
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/include
)
add_definitions(${LLVM_DEFINITIONS})

add_library(LLAIR STATIC
  EntryPoint.cpp
  LLAIRContext.cpp
  Module.cpp)

install(
  TARGETS LLAIR
  ARCHIVE
  DESTINATION lib
)
