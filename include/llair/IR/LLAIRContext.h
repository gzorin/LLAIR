//-*-C++-*-
#ifndef LLAIR_LLAIRCONTEXT_H
#define LLAIR_LLAIRCONTEXT_H

#include <llvm/ADT/StringRef.h>

#include <memory>

namespace llvm {
class DataLayout;
class LLVMContext;
}

namespace llair {

class LLAIRContextImpl;

class LLAIRContext {
public:
    static LLAIRContext *      Get(llvm::LLVMContext *);
    static const LLAIRContext *Get(const llvm::LLVMContext *);

    LLAIRContext(llvm::LLVMContext &);
    ~LLAIRContext();

    const llvm::LLVMContext &getLLContext() const;
    llvm::LLVMContext &      getLLContext();

    const llvm::DataLayout&  getDataLayout() const;
    llvm::StringRef          getTargetTriple() const;

private:
    std::unique_ptr<LLAIRContextImpl> d_impl;

    friend class LLAIRContextImpl;
};

} // End namespace llair

#endif
