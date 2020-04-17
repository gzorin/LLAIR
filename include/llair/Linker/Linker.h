//-*-C++-*-
#ifndef LLAIR_LINKER
#define LLAIR_LINKER

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/Hashing.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>

namespace llvm {
class Function;
class Module;
class SwitchInst;
class Type;
} // End namespace llvm

namespace llair {

class Class;
class Interface;
class LLAIRContext;
class Module;

void linkModules(Module *, const Module *);
void finalizeInterfaces(Module *, llvm::ArrayRef<Interface *>, std::function<uint32_t(const Class*)>);

} // End namespace llair

#endif
