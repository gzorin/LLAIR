//-*-C++-*-
#ifndef LLAIR_PROGRAM_H
#define LLAIR_PROGRAM_H

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>

#include <memory>

namespace llvm {
  class MemoryBuffer;
  class raw_fd_ostream;
}

namespace llair {

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> getMemoryBufferForStream(int FD, const llvm::Twine& BufferName);

  struct Program {
    llvm::sys::ProcessInfo::ProcessId pid;
    std::unique_ptr<llvm::raw_fd_ostream> input;
    int output;
  };

  llvm::ErrorOr<Program> openProgram(llvm::StringRef path, llvm::ArrayRef<llvm::StringRef> args);

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> runProgram(llvm::StringRef path, llvm::ArrayRef<llvm::StringRef> args, llvm::MemoryBufferRef input);

} // End namespace llair

#endif
