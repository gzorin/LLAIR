#include <llair/IR/LLAIRContext.h>

#include <llvm/IR/LLVMContext.h>

#include "LLAIRContextImpl.h"

#include <map>

namespace llair {

// Implementation:
LLAIRContextImpl::LLAIRContextImpl(llvm::LLVMContext &llcontext)
    : d_llcontext(llcontext) {}

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

} // End namespace llair
