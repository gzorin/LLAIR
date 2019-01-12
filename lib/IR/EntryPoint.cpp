#include <llair/IR/EntryPoint.h>
#include <llair/IR/Module.h>

#include "LLAIRContextImpl.h"

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

const EntryPoint *
EntryPoint::Get(const llvm::Function* llfunction) {
  auto& llcontext = llfunction->getContext();
  auto context = LLAIRContext::Get(&llcontext);
  if (context) {
    auto it = LLAIRContextImpl::Get(*context).entry_points().find(const_cast<llvm::Function *>(llfunction));
    if (it != LLAIRContextImpl::Get(*context).entry_points().end()) {
      return it->second;
    }
    else {
      return nullptr;
    }
  }
  else {
    return nullptr;
  }
}

EntryPoint *
EntryPoint::Get(llvm::Function* llfunction) {
  auto& llcontext = llfunction->getContext();
  auto context = LLAIRContext::Get(&llcontext);
  if (context) {
    auto it = LLAIRContextImpl::Get(*context).entry_points().find(llfunction);
    if (it != LLAIRContextImpl::Get(*context).entry_points().end()) {
      return it->second;
    }
    else {
      return nullptr;
    }
  }
  else {
    return nullptr;
  }
}

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
  if (d_module && d_function) {
    LLAIRContextImpl::Get(d_module->getContext()).entry_points().erase(d_function);
  }

  d_module = module;

  if (d_module) {
    LLAIRContextImpl::Get(d_module->getContext()).entry_points().insert(std::make_pair(d_function, this));
  }
}

llvm::StringRef
EntryPoint::getName() const {
  return d_function? d_function->getName() : llvm::StringRef();
}

}
