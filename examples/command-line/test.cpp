#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>

#include <llvm/IR/LLVMContext.h>

#include <iostream>

int
main(int argc, const char **argv) {
    std::cerr << "Hello, llair" << std::endl;

    std::unique_ptr<llvm::LLVMContext>   llcontext(new llvm::LLVMContext());
    std::unique_ptr<llair::LLAIRContext> context(new llair::LLAIRContext(*llcontext));

    std::unique_ptr<llair::Module> module(new llair::Module("test", *context));

#if defined(LLVM_ENABLE_DUMP)
    module->getLLModule()->dump();
#endif
}
