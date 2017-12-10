#include <cstdint>

#include <llair/IR/Module.h>

#include <llvm/IR/Module.h>

#include "LLAIRContextImpl.h"
#include "Metadata.h"

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
  // Version
  {
    auto md = d_llmodule->getNamedMetadata("air.version");
    if (md) {
      auto version_md = md->getOperand(0);

      auto
	major = readMDSInt(*version_md->getOperand(0)),
	minor = readMDSInt(*version_md->getOperand(1)),
	patch = readMDSInt(*version_md->getOperand(2));

      if (major && minor && patch) {
	setVersion({ *major, *minor, *patch });
      }
    }
  }

  // Language
  {
    auto md = d_llmodule->getNamedMetadata("air.language_version");
    if (md) {
      auto language_md = md->getOperand(0);

      auto
	name  = readMDString(*language_md->getOperand(0));
      auto
	major = readMDSInt(*language_md->getOperand(1)),
	minor = readMDSInt(*language_md->getOperand(2)),
	patch = readMDSInt(*language_md->getOperand(3));

      if (name, major && minor && patch) {
	setLanguage({ name->str(), { *major, *minor, *patch } });
      }
    }
  }
}

void
Module::writeMetadata() const
{
  auto& llcontext = d_llmodule->getContext();

  // Version
  {
    auto md = insertNamedMetadata(*d_llmodule, "air.version");
    if (md) {
      md->addOperand(llvm::MDTuple::get(llcontext, {
	  writeMDSInt(llcontext, d_version.major),
	  writeMDSInt(llcontext, d_version.minor),
	  writeMDSInt(llcontext, d_version.patch)
      }));
    }
  }
}

} // End namespace llair
