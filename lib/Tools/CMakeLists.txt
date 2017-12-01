include_directories(
  ${LLVM_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/include
)
add_definitions(${LLVM_DEFINITIONS})

add_library(LLAIRTools STATIC
  Compile.cpp
  Program.cpp
  Tools.cpp
  popen2.c)

install(
  TARGETS LLAIRTools
  ARCHIVE
  DESTINATION lib
)
