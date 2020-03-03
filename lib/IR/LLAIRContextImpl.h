//-*-C++-*-
#ifndef LLAIRCONTEXTIMPL_H
#define LLAIRCONTEXTIMPL_H

#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>

#include <map>

namespace llvm {
class Function;
class LLVMContext;
class Module;
} // namespace llvm

namespace llair {

class LLAIRContextImpl {
public:
    static const LLAIRContextImpl &Get(const LLAIRContext &context) { return *context.d_impl; }
    static LLAIRContextImpl &      Get(LLAIRContext &context) { return *context.d_impl; }

    LLAIRContextImpl(llvm::LLVMContext &);
    ~LLAIRContextImpl();

    const llvm::LLVMContext &getLLContext() const { return d_llcontext; }
    llvm::LLVMContext &      getLLContext() { return d_llcontext; }

    std::map<llvm::Module *, Module *> &      modules() { return d_modules; }
    const std::map<llvm::Module *, Module *> &modules() const { return d_modules; }

    std::map<llvm::Function *, EntryPoint *> &      entry_points() { return d_entry_points; }
    const std::map<llvm::Function *, EntryPoint *> &entry_points() const { return d_entry_points; }

private:
    llvm::LLVMContext &d_llcontext;

    std::map<llvm::Module *, Module *>       d_modules;
    std::map<llvm::Function *, EntryPoint *> d_entry_points;
};

} // namespace llair

#endif
