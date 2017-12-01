#include <cstdint>

#include <llair/IR/Module.h>

#include <llvm/IR/Module.h>

#include "LLAIRContextImpl.h"

namespace llair {

const Module *
Module::Get(const llvm::Module* llmodule) {
  auto& llcontext = llmodule->getContext();
  auto context = LLAIRContext::Get(&llcontext);
  if (context) {
    auto it = LLAIRContextImpl::Get(*context).modules().find(const_cast<llvm::Module *>(llmodule));
    if (it != LLAIRContextImpl::Get(*context).modules().end()) {
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

Module *
Module::Get(llvm::Module* llmodule) {
  auto& llcontext = llmodule->getContext();
  auto context = LLAIRContext::Get(&llcontext);
  if (context) {
    auto it = LLAIRContextImpl::Get(*context).modules().find(llmodule);
    if (it != LLAIRContextImpl::Get(*context).modules().end()) {
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

Module::Module(llvm::StringRef id, LLAIRContext& context)
  : d_context(context)
  , d_llmodule(new llvm::Module(id, context.getLLContext()))
{
  LLAIRContextImpl::Get(d_context).modules().insert(std::make_pair(d_llmodule.get(), this));
}

Module::Module(std::unique_ptr<llvm::Module>&& module)
  : d_context(*LLAIRContext::Get(&module->getContext()))
  , d_llmodule(std::move(module))
{
  LLAIRContextImpl::Get(d_context).modules().insert(std::make_pair(d_llmodule.get(), this));
}

Module::~Module()
{
  LLAIRContextImpl::Get(d_context).modules().erase(d_llmodule.get());
}

void
Module::readMetadata()
{
}

void
Module::writeMetadata() const
{
}

} // End namespace llair
