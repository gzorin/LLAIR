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
}

void
llair_example_resize(CGSize size) {
    s_context->d_size = { (unsigned int)size.width, (unsigned int)size.height };
}

void
llair_example_draw(MTLRenderPassDescriptor *descriptor, id<MTLDrawable> drawable) {
    auto command_buffer = [s_context->d_command_queue commandBuffer];
    auto command_encoder = [command_buffer renderCommandEncoderWithDescriptor: descriptor];

    [command_encoder setViewport: { 0, 0, (double)s_context->d_size[0], (double)s_context->d_size[1], 0, 1 }];

    struct Vertex {
        simd::float2 position;
        simd::float4 color;
    };

    static const Vertex vertices[] = {
        { {  250,  -250 }, { 1, 0, 0, 1 } },
        { { -250,  -250 }, { 0, 1, 0, 1 } },
        { {    0,   250 }, { 0, 0, 1, 1 } }
    };

    [command_encoder setRenderPipelineState: s_context->d_pipeline_state];

    [command_encoder setVertexBytes: &vertices[0]
                     length: sizeof(vertices)
                     atIndex: 0];

    [command_encoder setVertexBytes: &s_context->d_size
                     length: sizeof(simd::uint2)
                     atIndex: 1];

    [command_encoder drawPrimitives: MTLPrimitiveTypeTriangle
                     vertexStart: 0 vertexCount: 3];

    [command_encoder endEncoding];

    [command_buffer presentDrawable: drawable];
    [command_buffer commit];
}