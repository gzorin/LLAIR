//-*-C++-*-
#ifndef LLAIR_MAKELIBRARY_H
#define LLAIR_MAKELIBRARY_H

#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>

#include <memory>

namespace llair {
class Module;

// Optimizes in place. The overload taking an llair::Module clones first, so a
// caller that owns its module outright should prefer this one.
void finalizeLibrary(llvm::Module&, unsigned opt_level = 3);

std::unique_ptr<llvm::Module> finalizeLibrary(const Module&, unsigned opt_level = 3);

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> makeLibrary(const llvm::Module &module);
llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>> makeLibrary(const Module &module, unsigned opt_level = 3);

} // End namespace llair

#endif
