#include <algorithm>
#include <cstdint>
#include <iostream>
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

std::unique_ptr<llvm::Module>
Module::releaseLLModule()
{
  return std::move(d_llmodule);
}

EntryPoint *
Module::getEntryPoint(llvm::StringRef name) const
{
  auto function = d_llmodule->getFunction(name);
  if (!function) {
      return nullptr;
  }

  return EntryPoint::Get(function);
}

namespace {

template<typename Input1, typename Input2, typename BinaryFunction1, typename BinaryFunction2, typename Compare>
void
for_each_symmetric_difference(Input1 first1, Input1 last1,
			      Input2 first2, Input2 last2,
			      BinaryFunction1 fn1,
			      BinaryFunction2 fn2,
			      Compare comp)
{
    while (first1 != last1) {
        if (first2 == last2) {
	    std::for_each(
		first1, last1, fn1);
	    return;
	}
 
        if (comp(*first1, *first2)) {
            fn1(*first1++);
        } else {
            if (comp(*first2, *first1)) {
                fn2(*first2);
            } else {
                ++first1;
            }
            ++first2;
        }
    }
    std::for_each(first2, last2, fn2);
}

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

    std::vector<EntryPoint *> entry_points;
    std::transform(
	d_entry_points.begin(), d_entry_points.end(),
	std::back_inserter(entry_points),
	[](auto& entry_point) -> EntryPoint * {
	    return &entry_point;
	});
    std::sort(
	entry_points.begin(), entry_points.end(),
	[](auto lhs, auto rhs) -> bool {
	    return lhs->metadata() < rhs->metadata();
	});

    // Vertex:
    {
      auto md = d_llmodule->getNamedMetadata("air.vertex");

      if (md) {
	std::vector<llvm::MDNode *> mds;
	std::copy(
	    md->op_begin(), md->op_end(),
	    std::back_inserter(mds));
	std::sort(
	    mds.begin(), mds.end());

	std::vector<EntryPoint *> vertex_entry_points;
	std::copy_if(
	    entry_points.begin(), entry_points.end(),
	    std::back_inserter(vertex_entry_points),
	    [](auto entry_point) -> bool {
		return entry_point->getKind() == EntryPoint::EntryPointKind::Vertex;
	    });

	struct Compare {
	    bool operator()(llvm::MDNode *lhs, EntryPoint *rhs) const {
		return lhs < rhs->metadata();
	    }
	    bool operator()(EntryPoint *lhs, llvm::MDNode *rhs) const {
		return lhs->metadata() < rhs;
	    }
	};

	for_each_symmetric_difference(
	    mds.begin(), mds.end(),
	    vertex_entry_points.begin(), vertex_entry_points.end(),
	    [&](llvm::MDNode *md) -> void {
		auto entry_point = new VertexEntryPoint(md, module);
	    },
	    [](EntryPoint *entry_point) -> void {
	    },
	    Compare());
      }
    }

    // Fragment:
    {
      auto md = d_llmodule->getNamedMetadata("air.fragment");

      if (md) {
	std::vector<llvm::MDNode *> mds;
	std::copy(
	    md->op_begin(), md->op_end(),
	    std::back_inserter(mds));
	std::sort(
	    mds.begin(), mds.end());

	std::vector<EntryPoint *> fragment_entry_points;
	std::copy_if(
	    entry_points.begin(), entry_points.end(),
	    std::back_inserter(fragment_entry_points),
	    [](auto entry_point) -> bool {
		return entry_point->getKind() == EntryPoint::EntryPointKind::Fragment;
	    });

	struct Compare {
	    bool operator()(llvm::MDNode *lhs, EntryPoint *rhs) const {
		return lhs < rhs->metadata();
	    }
	    bool operator()(EntryPoint *lhs, llvm::MDNode *rhs) const {
		return lhs->metadata() < rhs;
	    }
	};

	for_each_symmetric_difference(
	    mds.begin(), mds.end(),
	    fragment_entry_points.begin(), fragment_entry_points.end(),
	    [&](llvm::MDNode *md) -> void {
		auto entry_point = new FragmentEntryPoint(md, module);
	    },
	    [](EntryPoint *entry_point) -> void {
	    },
	    Compare());
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
		    switch (entry_point.getKind()) {
		    case EntryPoint::Vertex: {
		      std::vector<llvm::Metadata *> operands;

		      // The function:
		      operands.push_back(llvm::ValueAsMetadata::getConstant(entry_point.getFunction()));

		      auto entry_point_md = llvm::MDTuple::get(llcontext, operands);
		    } break;
		    case EntryPoint::Fragment: {
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
