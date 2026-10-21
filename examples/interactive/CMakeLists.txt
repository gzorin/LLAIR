llvm_map_components_to_libnames(
    llvm_libs
    support core bitreader bitwriter)

configure_file(config.h.in config.h)

# llair-triangle
set(llair_triangle_SOURCES main.mm llair-triangle.mm)
add_compiled_metal_sources(llair_triangle_SOURCES llair-triangle.metal)
add_executable(llair-triangle ${llair_triangle_SOURCES})

target_include_directories(llair-triangle PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(llair-triangle PRIVATE ${LLVM_DEFINITIONS_LIST})
target_compile_options(llair-triangle PRIVATE -fno-rtti)

target_link_libraries(llair-triangle
    PRIVATE
    LLAIR LLAIRBitcode LLAIRTools
    ${llvm_libs}
    "$<LINK_LIBRARY:FRAMEWORK,Cocoa>"
    "$<LINK_LIBRARY:FRAMEWORK,Metal>"
    "$<LINK_LIBRARY:FRAMEWORK,MetalKit>")
