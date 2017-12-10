include_directories(
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

llvm_map_components_to_libnames(
  llvm_libs
  support core bitreader bitwriter
)

macro(add_metal_source srclist src)
  get_filename_component(we ${src} NAME_WE)
  set(dot_h "${we}_metal.h")

  set(${srclist} ${${srclist}} ${CMAKE_CURRENT_BINARY_DIR}/${dot_h})
  set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/${dot_h} PROPERTIES GENERATED 1)

  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${dot_h}
    COMMAND ${XXD} -i ${src} > ${CMAKE_CURRENT_BINARY_DIR}/${dot_h}
    MAIN_DEPENDENCY ${src}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

  message("Generated ${CMAKE_CURRENT_BINARY_DIR}/${dot_h}")
endmacro()

macro(add_metal_sources srclist)
  foreach(arg IN ITEMS ${ARGN})
    add_metal_source(${srclist} ${arg})
  endforeach()
endmacro()

# llair-test
add_executable(
  llair-test
  test.cpp
)

target_link_libraries(
  llair-test
  LLAIR
  ${llvm_libs}
)

# metalc
add_executable(
  metalc
  MetalC.cpp
)

target_link_libraries(
  metalc
  LLAIR LLAIRTools
  ${llvm_libs}
)

# make-library
add_metal_sources(MakeLibrary_METAL_SOURCES example.metal)

add_executable(
  make-library
  MakeLibrary.mm
  ${MakeLibrary_METAL_SOURCES}
)

target_link_libraries(
  make-library
  LLAIR LLAIRTools
  ${llvm_libs}
  "-ObjC++"
  "-framework Foundation"
  "-framework Metal"
)
