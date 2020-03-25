#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

#include <llair/IR/Class.h>
#include <llair/IR/EntryPoint.h>
#include <llair/IR/Module.h>

#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>

#include "LLAIRContextImpl.h"
#include "Metadata.h"

template class llvm::SymbolTableListTraits<llair::EntryPoint>;

namespace llair {

namespace {

llvm::StringRef s_data_layout =
    "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-f80:128:128-v16:"
    "16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:"
    "256-v512:512:512-v1024:1024:1024-f80:128:128-n8:16:32";
llvm::StringRef s_target_triple = "air64-apple-macosx10.14.0";

} // namespace

const Module *
Module::Get(const llvm::Module *llmodule) {
    auto &llcontext = llmodule->getContext();
    auto  context   = LLAIRContext::Get(&llcontext);
    if (context) {
        auto it =
            LLAIRContextImpl::Get(*context).modules().find(const_cast<llvm::Module *>(llmodule));
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
Module::Get(llvm::Module *llmodule) {
    auto &llcontext = llmodule->getContext();
    auto  context   = LLAIRContext::Get(&llcontext);
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

Module::Module(llvm::StringRef id, LLAIRContext &context)
    : d_context(context)
    , d_llmodule(new llvm::Module(id, context.getLLContext())) {
    LLAIRContextImpl::Get(d_context).modules().insert(std::make_pair(d_llmodule.get(), this));

    d_llmodule->setDataLayout(s_data_layout);
    d_llmodule->setTargetTriple(s_target_triple);

    setVersion({2, 1, 0});
    setLanguage({"Metal", {2, 1, 0}});
}

Module::Module(std::unique_ptr<llvm::Module> &&module)
    : d_context(*LLAIRContext::Get(&module->getContext()))
    , d_llmodule(std::move(module)) {
    LLAIRContextImpl::Get(d_context).modules().insert(std::make_pair(d_llmodule.get(), this));

    auto version_named_md = d_llmodule->getOrInsertNamedMetadata("air.version");
    if (version_named_md->getNumOperands() > 0) {
        d_version_md.reset(llvm::cast<llvm::MDTuple>(version_named_md->getOperand(0)));
    }

    auto language_named_md = d_llmodule->getOrInsertNamedMetadata("air.language_version");
    if (language_named_md->getNumOperands() > 0) {
        d_language_md.reset(llvm::cast<llvm::MDTuple>(language_named_md->getOperand(0)));
    }

    syncMetadata();
}

Module::~Module() {
    d_classes.clear();
    d_entry_points.clear();
    LLAIRContextImpl::Get(d_context).modules().erase(d_llmodule.get());
}

std::unique_ptr<llvm::Module>
Module::releaseLLModule() {
    return std::move(d_llmodule);
}

void
Module::setVersion(const Module::Version &version) {
    auto &ll_context = getLLContext();

    d_version_md.reset(llvm::MDTuple::get(
        ll_context, {llvm::ConstantAsMetadata::get(
                         llvm::ConstantInt::get(ll_context, llvm::APInt(32, version.major, true))),
                     llvm::ConstantAsMetadata::get(
                         llvm::ConstantInt::get(ll_context, llvm::APInt(32, version.minor, true))),
                     llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                         ll_context, llvm::APInt(32, version.patch, true)))}));

    auto version_named_md = d_llmodule->getOrInsertNamedMetadata("air.version");
    if (version_named_md->getNumOperands() > 0) {
        version_named_md->setOperand(0, d_version_md.get());
    }
    else {
        version_named_md->addOperand(d_version_md.get());
    }
}

Module::Version
Module::getVersion() const {
    if (d_version_md) {
        auto major = readMDSInt(*d_version_md->getOperand(0)),
             minor = readMDSInt(*d_version_md->getOperand(1)),
             patch = readMDSInt(*d_version_md->getOperand(2));

        if (major && minor && patch) {
            return {*major, *minor, *patch};
        }
    }

    return {0, 0, 0};
}

void
Module::setLanguage(const Module::Language &language) {
    auto &ll_context = getLLContext();

    d_language_md.reset(
        llvm::MDTuple::get(ll_context, {writeMDString(ll_context, language.name),
                                        writeMDInt(ll_context, language.version.major),
                                        writeMDInt(ll_context, language.version.minor),
                                        writeMDInt(ll_context, language.version.patch)}));

    auto language_named_md = d_llmodule->getOrInsertNamedMetadata("air.language_version");
    if (language_named_md->getNumOperands() > 0) {
        language_named_md->setOperand(0, d_language_md.get());
    }
    else {
        language_named_md->addOperand(d_language_md.get());
    }
}

Module::Language
Module::getLanguage() const {
    if (d_language_md) {
        auto name  = readMDString(*d_language_md->getOperand(0));
        auto major = readMDSInt(*d_language_md->getOperand(1)),
             minor = readMDSInt(*d_language_md->getOperand(2)),
             patch = readMDSInt(*d_language_md->getOperand(3));

        if (name && major && minor && patch) {
            return {name->str(), {*major, *minor, *patch}};
        }
    }

    return {"", {0, 0, 0}};
}

EntryPoint *
Module::getEntryPoint(llvm::StringRef name) const {
    auto function = d_llmodule->getFunction(name);
    if (!function) {
        return nullptr;
    }

    return EntryPoint::Get(function);
}

namespace {

template <typename Input1, typename Input2, typename BinaryFunction1, typename BinaryFunction2,
          typename Compare>
void
for_each_symmetric_difference(Input1 first1, Input1 last1, Input2 first2, Input2 last2,
                              BinaryFunction1 fn1, BinaryFunction2 fn2, Compare comp) {
    while (first1 != last1) {
        if (first2 == last2) {
            std::for_each(first1, last1, fn1);
            return;
        }

        if (comp(*first1, *first2)) {
            fn1(*first1++);
        }
        else {
            if (comp(*first2, *first1)) {
                fn2(*first2);
            }
            else {
                ++first1;
            }
            ++first2;
        }
    }
    std::for_each(first2, last2, fn2);
}

} // namespace

void
Module::syncMetadata() {
    // Entry points:
    {
        auto module = this;

        std::vector<EntryPoint *> entry_points;
        std::transform(d_entry_points.begin(), d_entry_points.end(),
                       std::back_inserter(entry_points),
                       [](auto &entry_point) -> EntryPoint * { return &entry_point; });
        std::sort(entry_points.begin(), entry_points.end(),
                  [](auto lhs, auto rhs) -> bool { return lhs->metadata() < rhs->metadata(); });

        // Vertex:
        {
            auto md = d_llmodule->getNamedMetadata("air.vertex");

            if (md) {
                std::vector<llvm::MDNode *> mds;
                std::copy(md->op_begin(), md->op_end(), std::back_inserter(mds));
                std::sort(mds.begin(), mds.end());

                std::vector<EntryPoint *> vertex_entry_points;
                std::copy_if(entry_points.begin(), entry_points.end(),
                             std::back_inserter(vertex_entry_points), [](auto entry_point) -> bool {
                                 return entry_point->getKind() ==
                                        EntryPoint::EntryPointKind::Vertex;
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
                    mds.begin(), mds.end(), vertex_entry_points.begin(), vertex_entry_points.end(),
                    [&](llvm::MDNode *md) -> void {
                        auto entry_point = new VertexEntryPoint(md, module);
                    },
                    [](EntryPoint *entry_point) -> void {}, Compare());
            }
        }

        // Fragment:
        {
            auto md = d_llmodule->getNamedMetadata("air.fragment");

            if (md) {
                std::vector<llvm::MDNode *> mds;
                std::copy(md->op_begin(), md->op_end(), std::back_inserter(mds));
                std::sort(mds.begin(), mds.end());

                std::vector<EntryPoint *> fragment_entry_points;
                std::copy_if(
                    entry_points.begin(), entry_points.end(),
                    std::back_inserter(fragment_entry_points), [](auto entry_point) -> bool {
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

                for_each_symmetric_difference(mds.begin(), mds.end(), fragment_entry_points.begin(),
                                              fragment_entry_points.end(),
                                              [&](llvm::MDNode *md) -> void {
                                                  auto entry_point =
                                                      new FragmentEntryPoint(md, module);
                                              },
                                              [](EntryPoint *entry_point) -> void {}, Compare());
            }
        }
    }
}

} // End namespace llair
