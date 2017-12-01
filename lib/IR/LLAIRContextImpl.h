//-*-C++-*-
#ifndef LLAIRCONTEXTIMPL_H
#define LLAIRCONTEXTIMPL_H

#include <llair/IR/LLAIRContext.h>

namespace llvm {
class LLVMContext;
}

namespace llair {

class LLAIRContextImpl {
public:
  
  static const LLAIRContextImpl& Get(const LLAIRContext& context) { return *context.d_impl; }
  static LLAIRContextImpl&       Get(LLAIRContext& context)       { return *context.d_impl; }

  LLAIRContextImpl(llvm::LLVMContext&);
  ~LLAIRContextImpl();

  const llvm::LLVMContext&         getLLContext() const { return d_llcontext; }
  llvm::LLVMContext&               getLLContext()       { return d_llcontext; }

private:

  llvm::LLVMContext& d_llcontext;
};

}

#endif
