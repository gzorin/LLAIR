//-*-C++-*-
#ifndef LLAIR_LLAIRCONTEXT_H
#define LLAIR_LLAIRCONTEXT_H

#include <memory>

namespace llvm {
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

private:
    std::unique_ptr<LLAIRContextImpl> d_impl;

    friend class LLAIRContextImpl;
};

} // End namespace llair

#endif
