//-*-C++-*-
#ifndef LLAIR_COMPILER_H
#define LLAIR_COMPILER_H

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>

#include <memory>

namespace llair {
  class Module;

  llvm::Expected<std::unique_ptr<Module>> compileBuffer(llvm::MemoryBufferRef buffer);

} // End namespace llair

#endif
