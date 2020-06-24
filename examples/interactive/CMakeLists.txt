include_directories(
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti")

llvm_map_components_to_libnames(
    llvm_libs
    support core bitreader bitwriter)

configure_file(config.h.in config.h)

# llair-triangle
set(llair_triangle_SOURCES main.mm llair-triangle.mm)
add_compiled_metal_sources(llair_triangle_SOURCES llair-triangle.metal)
add_executable(llair-triangle ${llair_triangle_SOURCES})

target_link_libraries(llair-triangle
    LLAIR LLAIRBitcode LLAIRTools
    ${llvm_libs}
    "-framework Cocoa" "-framework Metal" "-framework MetalKit")