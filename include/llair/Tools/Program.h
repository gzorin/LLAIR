//-*-C++-*-
#ifndef LLAIR_PROGRAM_H
#define LLAIR_PROGRAM_H

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/MemoryBuffer.h>

#include <memory>

namespace llvm {
  class MemoryBuffer;
}

namespace llair {

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> runAndWait(llvm::StringRef path, llvm::ArrayRef<llvm::StringRef> args, llvm::MemoryBufferRef input);

} // End namespace llair

#endif
