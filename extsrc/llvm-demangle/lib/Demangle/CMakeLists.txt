add_library(LLAIRDemangleLib STATIC
  ItaniumDemangle.cpp
  MicrosoftDemangle.cpp
  MicrosoftDemangleNodes.cpp)

target_include_directories(LLAIRDemangleLib
  PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/extsrc/llvm-demangle/include>)

# Exported only because LLAIR links it; its headers stay private to the build.
install(
  TARGETS LLAIRDemangleLib
  EXPORT LLAIRTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
