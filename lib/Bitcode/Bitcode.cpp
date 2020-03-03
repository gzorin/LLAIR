#include <llair/Bitcode/Bitcode.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>

#include <llvm/Bitcode/BitcodeReader.h>

namespace llair {

llvm::Expected<std::unique_ptr<llair::Module>>
getBitcodeModule(llvm::MemoryBufferRef bitcode, LLAIRContext &context) {

    auto llmodule = llvm::getLazyBitcodeModule(bitcode, context.getLLContext());

    if (!llmodule) {
        return llmodule.takeError();
    }

    auto error = (*llmodule)->materializeAll();

    if (error) {
        return std::move(error);
    }

    auto module = std::make_unique<llair::Module>(std::move(*llmodule));

    return module;
}

} // End namespace llair
