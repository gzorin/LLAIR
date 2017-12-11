#include <algorithm>
#include <cstdint>
#include <vector>

#include <llair/IR/EntryPoint.h>
#include <llair/IR/Module.h>

#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

#include "LLAIRContextImpl.h"
#include "Metadata.h"

template class llvm::SymbolTableListTraits<llair::EntryPoint>;

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
  d_entry_points.clear();
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

      if (name && major && minor && patch) {
	setLanguage({ name->str(), { *major, *minor, *patch } });
      }
    }
  }

  // Entry points:
  {
    auto module = this;

    // Vertex:
    {
      auto md = d_llmodule->getNamedMetadata("air.vertex");

      if (md) {
	std::for_each(md->op_begin(), md->op_end(),
		      [=](auto entry_point_md)->void {
			auto it = entry_point_md->op_begin();
			
			// The function:
			if (it == entry_point_md->op_end()) {
			  return;
			}
			
			auto function_md = llvm::dyn_cast<llvm::ValueAsMetadata>(it++->get());
			if (!function_md) {
			  return;
			}
			
			auto function = llvm::dyn_cast<llvm::Function>(function_md->getValue());
			if (!function) {
			  return;
			}
			
			// Create the entry point:
			auto entry_point = EntryPoint::Create(EntryPoint::kVertex, function, module);
		      });
      }
    }

    // Fragment:
    {
      auto md = d_llmodule->getNamedMetadata("air.fragment");

      if (md) {
	std::for_each(md->op_begin(), md->op_end(),
		      [=](auto entry_point_md)->void {
			auto it = entry_point_md->op_begin();
			
			// The function:
			if (it == entry_point_md->op_end()) {
			  return;
			}
			
			auto function_md = llvm::dyn_cast<llvm::ValueAsMetadata>(it++->get());
			if (!function_md) {
			  return;
			}
			
			auto function = llvm::dyn_cast<llvm::Function>(function_md->getValue());
			if (!function) {
			  return;
			}
			
			// Create the entry point:
			auto entry_point = EntryPoint::Create(EntryPoint::kFragment, function, module);
		      });
      }
    }
  }
}

void
Module::writeMetadata()
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

  // Language
  {
    auto md = insertNamedMetadata(*d_llmodule, "air.language_version");
    if (md) {
      md->addOperand(llvm::MDTuple::get(llcontext, {
	    writeMDString(llcontext, d_language.name),
	    writeMDInt(llcontext, d_language.version.major),
	    writeMDInt(llcontext, d_language.version.minor),
	    writeMDInt(llcontext, d_language.version.patch)
      }));
    }
  }

  // Entry points
  {
    llvm::NamedMDNode *vertex = nullptr, *fragment = nullptr;

    std::for_each(getEntryPointList().begin(), getEntryPointList().end(),
		  [&](auto& entry_point) -> void {
		    switch (entry_point.getType()) {
		    case EntryPoint::kVertex: {
		      std::vector<llvm::Metadata *> operands;

		      // The function:
		      operands.push_back(llvm::ValueAsMetadata::getConstant(entry_point.getFunction()));

		      auto entry_point_md = llvm::MDTuple::get(llcontext, operands);
		    } break;
		    case EntryPoint::kFragment: {
		      std::vector<llvm::Metadata *> operands;

		      // The function:
		      operands.push_back(llvm::ValueAsMetadata::getConstant(entry_point.getFunction()));

		      auto entry_point_md = llvm::MDTuple::get(llcontext, operands);
		    } break;
		    default:
		      break;
		    }
		  });
  }
}

} // End namespace llair
