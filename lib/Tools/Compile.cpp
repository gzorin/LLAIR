#include <llair/IR/Module.h>
#include <llair/Tools/Compile.h>

#include <llvm/IR/Module.h>

namespace llair {

llvm::Expected<std::unique_ptr<Module>>
compileBuffer(llvm::MemoryBufferRef buffer, LLAIRContext& context) {
  std::unique_ptr<Module> result;

  return std::move(result);
}

} // End llair namespace
