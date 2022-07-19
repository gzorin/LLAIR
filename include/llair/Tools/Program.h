//-*-C++-*-
#ifndef LLAIR_PROGRAM_H
#define LLAIR_PROGRAM_H

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Program.h>

#include <memory>
#include <string>

namespace llvm {
class MemoryBuffer;
class raw_fd_ostream;
} // namespace llvm

namespace llair {

llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
getMemoryBufferForStream(int FD, const llvm::Twine &BufferName);

struct Program {

#if LLVM_VERSION_MAJOR >= 7
    llvm::sys::procid_t                   pid;
#else
    llvm::sys::ProcessInfo::ProcessId     pid;
#endif

    std::unique_ptr<llvm::raw_fd_ostream> input;
    int                                   output;
};

llvm::ErrorOr<Program> openProgram(const std::string& path, llvm::ArrayRef<std::string> args);

llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
runProgram(const std::string& path, llvm::ArrayRef<std::string> args, llvm::MemoryBufferRef input);

} // End namespace llair

#endif
