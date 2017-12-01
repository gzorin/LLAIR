//-*-C++-*-
#ifndef LLAIR_TOOLSIMPL_H
#define LLAIR_TOOLSIMPL_H

#include <llvm/ADT/SmallString.h>

namespace llair {
  llvm::SmallString<256> getPathToTools();
  llvm::SmallString<256> getPathToCompileTool();
} // End namespace llair

#endif
