//-*-C++-*-
#ifndef LLAIR_BITCODE_H
#define LLAIR_BITCODE_H

#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>

#include <memory>

namespace llair {

class LLAIRContext;
class Module;

llvm::Expected<std::unique_ptr<llair::Module>> getBitcodeModule(llvm::MemoryBufferRef bitcode,
                                                                LLAIRContext &        context);

} // End namespace llair

#endif
