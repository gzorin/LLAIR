//-*-C++-*-
#include <llair/IR/EntryPoint.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Tools/Compile.h>
#include <llair/Tools/MakeLibrary.h>

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/LLVMContext.h>

#include <dispatch/dispatch.h>

#include <Metal/Metal.h>

#include <algorithm>
#include <iostream>

namespace {

#include "example_metal.h"

}

int
main(int argc, char ** argv) {
  std::unique_ptr<llvm::LLVMContext> llcontext(new llvm::LLVMContext());
  std::unique_ptr<llair::LLAIRContext> context(new llair::LLAIRContext(*llcontext));

  // Compile:
  llvm::StringRef source(reinterpret_cast<const char *>(&example_metal[0]),
			 example_metal_len);

  auto module = llair::compileBuffer(llvm::MemoryBufferRef(source, ""), *context);

  if (!module) {
    return -1;
  }

  (*module)->readMetadata();

  const auto& version = (*module)->getVersion();
  std::cerr << "version: " << version.major << "." << version.minor << "." << version.patch << std::endl;

  const auto& language = (*module)->getLanguage();
  std::cerr << "language: " << language.name << " " << language.version.major << "." << language.version.minor << "." << language.version.patch << std::endl;

  const auto& entry_points = (*module)->getEntryPointList();
  std::cerr << "entry points: " << std::endl;
  std::for_each(entry_points.begin(), entry_points.end(),
		[=](const auto& entry_point)->void {
		  std::cerr << "\t" << entry_point.getFunction()->getName().str() << std::endl;
		});

  // Turn into library bitcode:
  auto library = llair::makeLibrary(**module);

  if (!library) {
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

    auto fragment_main = [library newFunctionWithName: @"fragment_main"];

    if (!library) {
      NSLog(@"Failed to get 'fragment_main' function");
      return -1;
    }
  }

  return 0;
}
