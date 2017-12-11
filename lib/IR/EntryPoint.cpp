#include <llair/IR/EntryPoint.h>
#include <llair/IR/Module.h>

namespace llvm {

template<>
void SymbolTableListTraits<llair::EntryPoint>::addNodeToList(llair::EntryPoint *entry_point)
{
  auto owner = getListOwner();
  entry_point->setModule(owner);
}

template<>
void SymbolTableListTraits<llair::EntryPoint>::removeNodeFromList(llair::EntryPoint *entry_point)
{
  entry_point->setModule(nullptr);
}

}

namespace llair {

EntryPoint * EntryPoint::Create(EntryPoint::Type type, llvm::Function * function, Module *module) {
  return new EntryPoint(type, function, module);
}

EntryPoint::EntryPoint(EntryPoint::Type type, llvm::Function *function, Module *module)
  : d_module(module)
  , d_function(function)
  , d_type(type) {
  if (module) {
    module->getEntryPointList().push_back(this);
  }
}

void
EntryPoint::setModule(Module *module) {
  d_module = module;
}

llvm::StringRef
EntryPoint::getName() const {
  return d_function? d_function->getName() : llvm::StringRef();
}

}
