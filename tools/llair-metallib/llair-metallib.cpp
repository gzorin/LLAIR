#include <llair/Bitcode/Bitcode.h>
#include <llair/IR/EntryPoint.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Tools/CompileSession.h>

#include <llvm/ADT/StringSet.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/ToolOutputFile.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

llvm::cl::list<std::string> input_filenames(llvm::cl::Positional, llvm::cl::ZeroOrMore,
                                            llvm::cl::desc("<input .bc files>"));

llvm::cl::opt<std::string> output_filename("o", llvm::cl::Required,
                                           llvm::cl::desc("Override output filename"),
                                           llvm::cl::value_desc("filename"));

llvm::cl::opt<unsigned> opt_level("O", llvm::cl::init(3),
                                  llvm::cl::desc("Optimization level for metallib finalization"));

} // namespace

using namespace llair;

//
int
main(int argc, char **argv) {
    llvm::cl::ParseCommandLineOptions(argc, argv, "llair-metallib\n");

    llvm::ExitOnError exit_on_err("llair-metallib: ");

    auto llvm_context  = std::make_unique<llvm::LLVMContext>();
    auto llair_context = std::make_unique<llair::LLAIRContext>(*llvm_context);

    llair::CompilePool pool;

    auto session = llair::CompileSession::Create(pool, output_filename);

    std::for_each(
        input_filenames.begin(), input_filenames.end(),
        [&exit_on_err, &llair_context, &session](auto input_filename) -> void {
            auto buffer =
                exit_on_err(errorOrToExpected(llvm::MemoryBuffer::getFileOrSTDIN(input_filename)));
            auto module = exit_on_err(
                llair::getBitcodeModule(llvm::MemoryBufferRef(*buffer), *llair_context));
            session->addModule(*module);
        });

    auto result = session->compile(opt_level).get();

    if (!result.metallib) {
        llvm::errs() << "llair-metallib: " << result.error << "\n";
        return 1;
    }

    // Write it out:
    std::error_code                       error_code;
#if LLVM_VERSION_MAJOR >= 7
    std::unique_ptr<llvm::ToolOutputFile> output_file(
        new llvm::ToolOutputFile(output_filename, error_code, llvm::sys::fs::OF_None));
#else
    std::unique_ptr<llvm::ToolOutputFile> output_file(
        new llvm::ToolOutputFile(output_filename, error_code, llvm::sys::fs::F_None));
#endif

    if (error_code) {
        llvm::errs() << error_code.message();
        return 1;
    }

    output_file->os() << result.metallib->getBuffer();
    output_file->keep();

    return 0;
}
