//-*-C++-*-
#ifndef LLAIR_MAKELIBRARY_H
#define LLAIR_MAKELIBRARY_H

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>

#include <memory>

namespace llair {
class Module;

void setPathToLibraryTool(llvm::StringRef path);

std::unique_ptr<llvm::Module> finalizeLibrary(const Module&);

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> makeLibrary(const llvm::Module &module);
llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> makeLibrary(const Module &module);

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> makeLibraryWithLLD(const llvm::Module &module);
llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> makeLibraryWithLLD(const Module &module);

} // End namespace llair

#endif
