//-*-C++-*-

#include "config.h"

#include <llair/Bitcode/Bitcode.h>
#include <llair/IR/EntryPoint.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Tools/MakeLibrary.h>
#include <llair/Tools/Tools.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <dispatch/dispatch.h>

#include <Metal/Metal.h>

#include <algorithm>
#include <iostream>

namespace {

llvm::cl::opt<std::string> input_filename(llvm::cl::Positional,
                                          llvm::cl::desc("<input .bc file>"),
                                          llvm::cl::init("-"));

llvm::cl::opt<std::string> vertex_function_name("vertex-function", llvm::cl::desc("Vertex function name"));
llvm::cl::opt<std::string> fragment_function_name("fragment-function", llvm::cl::desc("Fragment function name"));

} // namespace

int
main(int argc, char ** argv) {
    llvm::cl::ParseCommandLineOptions(argc, argv, "make-library\n");

    llvm::ExitOnError exit_on_err("make-library: ");

    llair::setPathToTools(LLAIR_TOOLS_PATH);

    auto llcontext  = std::make_unique<llvm::LLVMContext>();
    auto context = std::make_unique<llair::LLAIRContext>(*llcontext);

    auto buffer =
        exit_on_err(errorOrToExpected(llvm::MemoryBuffer::getFileOrSTDIN(input_filename)));
    auto module = exit_on_err(
        llair::getBitcodeModule(llvm::MemoryBufferRef(*buffer), *context));

    const auto& version = module->getVersion();
    std::cerr << "version: " << version.major << "." << version.minor << "." << version.patch << std::endl;

    const auto& language = module->getLanguage();
    std::cerr << "language: " << language.name << " " << language.version.major << "." << language.version.minor << "." << language.version.patch << std::endl;

    const auto& entry_points = module->getEntryPointList();
    std::cerr << "entry points: " << std::endl;
    std::for_each(entry_points.begin(), entry_points.end(),
    [=](const auto& entry_point)->void {
        std::cerr << "\t" << entry_point.getFunction()->getName().str() << std::endl;
    });

    // Turn into library bitcode:
    auto library = llair::makeLibrary(*module);

    if (!library) {
        std::cerr << "makeLibrary failed" << std::endl;
        return -1;
    }

    auto library_buffer = library->release();

    @autoreleasepool {
    NSError *err = nil;

    auto device = MTLCreateSystemDefaultDevice();

    auto library_data = dispatch_data_create(library_buffer->getBufferStart(), library_buffer->getBufferSize(),
              dispatch_get_main_queue(),
              ^{ delete library_buffer; });

    auto library = [device newLibraryWithData: library_data error: &err];

    if (!library) {
        NSLog(@"Error occurred creating MTLLibrary: %@", err);
        return -1;
    }

    auto pipeline_descriptor = [MTLRenderPipelineDescriptor new];

    auto vertex_function = [library newFunctionWithName: [NSString stringWithUTF8String: vertex_function_name.data()]];

    if (!vertex_function) {
        NSLog(@"Failed to get vertex function");
        return -1;
    }

    pipeline_descriptor.vertexFunction = vertex_function;

    auto fragment_function = [library newFunctionWithName: [NSString stringWithUTF8String: fragment_function_name.data()]];

    if (!fragment_function) {
        NSLog(@"Failed to get fragment function");
        return -1;
    }

    pipeline_descriptor.fragmentFunction = fragment_function;

    pipeline_descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

    //
    MTLAutoreleasedRenderPipelineReflection reflection;

    auto pipeline_state = [device
                           newRenderPipelineStateWithDescriptor: pipeline_descriptor
                           options: MTLPipelineOptionArgumentInfo
                           reflection: &reflection
                           error: &err];

    if (!pipeline_state) {
        NSLog(@"Error occurred when creating pipeline state: %@", err);
        return -1;
    }
  }

  return 0;
}
