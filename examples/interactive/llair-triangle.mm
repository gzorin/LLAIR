// This example is equivalent to Apple's sample code, 'Using a Render Pipeline to Render
// Primitives' (https://developer.apple.com/documentation/metal/using_a_render_pipeline_to_render_primitives),
// except that it uses the `llair` library to load previously compiled shader bitcode.

#include "config.h"

#include <llair/Bitcode/Bitcode.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Tools/MakeLibrary.h>
#include <llair/Tools/Tools.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/MemoryBuffer.h>

#import <Metal/Metal.h>
#import <MetalKit/MTKView.h>
#import <simd/simd.h>

#include <iostream>
#include <memory>

namespace {
#include "llair-triangle_metal_bc.h"
}

struct Context {
    Context()
    : llvm_context(new llvm::LLVMContext())
    , llair_context(new llair::LLAIRContext(*llvm_context)) {
        llair::setPathToTools(LLAIR_TOOLS_PATH);
    }

    std::unique_ptr<llvm::LLVMContext>   llvm_context;
    std::unique_ptr<llair::LLAIRContext> llair_context;

    id<MTLDevice>              d_device;
    id<MTLCommandQueue>        d_command_queue;
    id<MTLRenderPipelineState> d_pipeline_state;

    simd::uint2 d_size;

    id<MTLBuffer> d_viewport_buffer;
};

std::unique_ptr<Context> s_context;

std::unique_ptr<llair::Module>
CreateModule(unsigned char *bitcode, unsigned int bitcode_length,
             llvm::StringRef name, llair::LLAIRContext& llair_context) {
    auto buffer = llvm::MemoryBuffer::getMemBuffer(
        llvm::StringRef(reinterpret_cast<const char *>(bitcode), bitcode_length),
        name,
        false);

    auto module = llair::getBitcodeModule(llvm::MemoryBufferRef(*buffer), llair_context);
    if (!module) {
        std::cerr << "Failed to create llair::Module" << std::endl;
        std::exit(-1);
    }

    return std::move(*module);
}

struct Vertex {
    simd::float4 position;
    simd::float4 color;
};

struct Viewport {
    simd::uint2 extent;
};

void
llair_example_init(int argc, const char * argv[], id<MTLDevice> device, MTKView *view) {
    s_context = std::make_unique<Context>();

    s_context->d_device = device;
    s_context->d_command_queue = [s_context->d_device newCommandQueue];

    auto module = CreateModule(&llair_triangle_metal_bc[0], llair_triangle_metal_bc_len, "llair-triangle", *s_context->llair_context);

    auto library = llair::makeLibrary(*module);
    if (!library) {
        std::cerr << "llair::makeLibrary failed: " << llvm::toString(library.takeError()) << std::endl;
        std::exit(-1);
    }

    auto library_buffer = library->release();
    auto library_data = dispatch_data_create(library_buffer->getBufferStart(), library_buffer->getBufferSize(),
                                             dispatch_get_main_queue(),
                                             ^{ delete library_buffer; });

    NSError *err = nil;

    auto metal_library = [s_context->d_device newLibraryWithData: library_data
                          error: &err];

    if (!metal_library) {
        std::cerr << "Error occurred when creating shader library: " << [err code] << std::endl;
        std::exit(-1);
    }

    auto pipeline_descriptor = [MTLRenderPipelineDescriptor new];

    auto vertex_descriptor = [[MTLVertexDescriptor alloc] init];

    vertex_descriptor.attributes[0].format = MTLVertexFormatFloat4;
    vertex_descriptor.attributes[0].bufferIndex = 0;
    vertex_descriptor.attributes[0].offset = 0;

    vertex_descriptor.attributes[1].format = MTLVertexFormatFloat4;
    vertex_descriptor.attributes[1].bufferIndex = 0;
    vertex_descriptor.attributes[1].offset = sizeof(simd::float4);

    vertex_descriptor.layouts[0].stride = sizeof(Vertex);

    pipeline_descriptor.vertexDescriptor = vertex_descriptor;

    auto vertex_function = [metal_library newFunctionWithName: @"VertexMain"];

    if (!vertex_function) {
        std::cerr << "Failed to get vertex function" << std::endl;
        std::exit(-1);
    }

    pipeline_descriptor.vertexFunction = vertex_function;

    auto fragment_function = [metal_library newFunctionWithName: @"FragmentMain"];

    if (!fragment_function) {
        std::cerr << "Failed to get fragment function" << std::endl;
        std::exit(-1);
    }

    pipeline_descriptor.fragmentFunction = fragment_function;

    pipeline_descriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat;

    //
    MTLAutoreleasedRenderPipelineReflection reflection;

    s_context->d_pipeline_state = [device newRenderPipelineStateWithDescriptor: pipeline_descriptor
                                   options: MTLPipelineOptionArgumentInfo
                                   reflection: &reflection
                                   error: &err];

    if (!s_context->d_pipeline_state) {
        std::cerr << "Error occurred when creating pipeline: " << [err code] << std::endl;
        std::exit(-1);
    }
}

void
llair_example_exit() {
    s_context.reset();
}

void
llair_example_resize(CGSize size) {
    s_context->d_size = { (unsigned int)size.width, (unsigned int)size.height };

    Viewport viewport = {
        s_context->d_size
    };

    s_context->d_viewport_buffer = [s_context->d_device newBufferWithBytes: &viewport length: sizeof(Viewport) options: MTLResourceStorageModeManaged];
}

void
llair_example_draw(MTLRenderPassDescriptor *descriptor, id<MTLDrawable> drawable) {
    auto command_buffer = [s_context->d_command_queue commandBuffer];
    auto command_encoder = [command_buffer renderCommandEncoderWithDescriptor: descriptor];

    [command_encoder setViewport: { 0, 0, (double)s_context->d_size[0], (double)s_context->d_size[1], 0, 1 }];



    Vertex vertices[] = {
        { {  250,  -250, 0, 1 }, { 1, 0, 0, 1 } },
        { { -250,  -250, 0, 1 }, { 0, 1, 0, 1 } },
        { {    0,   250, 0, 1 }, { 0, 0, 1, 1 } }
    };

    auto vertex_buffer = [s_context->d_device newBufferWithBytes: &vertices[0] length: sizeof(Vertex) * 3 options: MTLResourceStorageModeManaged];

    uint32_t indices[] = {
        0, 1, 2
    };

    auto index_buffer = [s_context->d_device newBufferWithBytes: &indices[0] length: sizeof(uint32_t) * 3 options: MTLResourceStorageModeManaged];

    [command_encoder setRenderPipelineState: s_context->d_pipeline_state];

    [command_encoder setVertexBuffer: vertex_buffer
                     offset: 0
                     atIndex: 0];

    [command_encoder setVertexBuffer: s_context->d_viewport_buffer
                     offset: 0
                     atIndex: 1];

    [command_encoder drawIndexedPrimitives: MTLPrimitiveTypeTriangle
                     indexCount: 3
                     indexType: MTLIndexTypeUInt32
                     indexBuffer: index_buffer
                     indexBufferOffset: 0];

    [command_encoder endEncoding];

    [command_buffer presentDrawable: drawable];
    [command_buffer commit];
}