#include <llair/IR/LLAIRContext.h>

#include <llvm/IR/LLVMContext.h>

#include "LLAIRContextImpl.h"

#include <map>

namespace llair {

// Implementation:
LLAIRContextImpl::LLAIRContextImpl(llvm::LLVMContext &llcontext)
    : d_llcontext(llcontext)
    , d_data_layout("e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32")
    , d_target_triple("air64-apple-macosx14.0.0") {
}

LLAIRContextImpl::~LLAIRContextImpl() {}

// Interface:
namespace {
namespace contexts {

std::map<llvm::LLVMContext *, LLAIRContext *> &
llvm_to_llair() {
    static std::map<llvm::LLVMContext *, LLAIRContext *> s_llvm_to_llair;
    return s_llvm_to_llair;
}

} // End namespace contexts
} // End anonymous namespace

const LLAIRContext *
LLAIRContext::Get(const llvm::LLVMContext *llcontext) {
    auto it = contexts::llvm_to_llair().find(const_cast<llvm::LLVMContext *>(llcontext));
    return it != contexts::llvm_to_llair().end() ? it->second : nullptr;
}

LLAIRContext *
LLAIRContext::Get(llvm::LLVMContext *llcontext) {
    auto it = contexts::llvm_to_llair().find(llcontext);
    return it != contexts::llvm_to_llair().end() ? it->second : nullptr;
}

LLAIRContext::LLAIRContext(llvm::LLVMContext &llcontext)
    : d_impl(new LLAIRContextImpl(llcontext)) {
    contexts::llvm_to_llair().insert(std::make_pair(&llcontext, this));
}

LLAIRContext::~LLAIRContext() {
    contexts::llvm_to_llair().erase(&d_impl->getLLContext());
}

const llvm::LLVMContext &
LLAIRContext::getLLContext() const {
    return d_impl->getLLContext();
}

llvm::LLVMContext &
LLAIRContext::getLLContext() {
    return d_impl->getLLContext();
}

const llvm::DataLayout&
LLAIRContext::getDataLayout() const {
    return d_impl->getDataLayout();
}

llvm::StringRef
LLAIRContext::getTargetTriple() const {
    return d_impl->getTargetTriple();
}

} // End namespace llair
