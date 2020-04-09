#include <llair/Bitcode/Bitcode.h>
#include <llair/IR/Class.h>
#include <llair/IR/Dispatcher.h>
#include <llair/IR/Interface.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <memory>

namespace {

llvm::cl::opt<std::string> input_filename(llvm::cl::Positional,
                                          llvm::cl::desc("<input .bc file>"),
                                          llvm::cl::init("-"));

} // namespace

using namespace llair;

//
int
main(int argc, char **argv) {
    llvm::cl::ParseCommandLineOptions(argc, argv, "llair-dump\n");

    llvm::ExitOnError exit_on_err("llair-dump: ");

    auto llvm_context  = std::make_unique<llvm::LLVMContext>();
    auto llair_context = std::make_unique<llair::LLAIRContext>(*llvm_context);

    auto buffer =
        exit_on_err(errorOrToExpected(llvm::MemoryBuffer::getFileOrSTDIN(input_filename)));
    auto module = exit_on_err(
        llair::getBitcodeModule(llvm::MemoryBufferRef(*buffer), *llair_context));

    module->print(llvm::outs());

    return 0;
}
