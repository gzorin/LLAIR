#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Tools/Compile.h>

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/ToolOutputFile.h>

#include <iostream>

namespace {

llvm::cl::opt<std::string> input_filename(llvm::cl::Positional, llvm::cl::desc("<input bitcode>"),
                                          llvm::cl::init("-"));

llvm::cl::opt<std::string> output_filename("o", llvm::cl::desc("Override output filename"),
                                           llvm::cl::value_desc("filename"));

} // namespace

int
main(int argc, char **argv) {
    std::unique_ptr<llvm::LLVMContext> context(new llvm::LLVMContext());

    llvm::cl::ParseCommandLineOptions(argc, argv, "Metal compiler\n");

    llvm::ExitOnError exit_on_err("metalc: ");

    auto buffer =
        exit_on_err(llvm::errorOrToExpected(llvm::MemoryBuffer::getFileOrSTDIN(input_filename)));

    if (output_filename.empty()) {
        if (input_filename == "-") {
            output_filename = "-";
        }
        else {
            llvm::StringRef IFN = input_filename;
            output_filename     = (IFN.endswith(".metal") ? IFN.drop_back(6) : IFN).str();
            output_filename += ".bc";
        }
    }

    std::unique_ptr<llair::LLAIRContext> llair_context(new llair::LLAIRContext(*context));

    auto module = exit_on_err(llair::compileBuffer(*buffer, {}, *llair_context));

    std::error_code                       error_code;
    std::unique_ptr<llvm::ToolOutputFile> output_file(
        new llvm::ToolOutputFile(output_filename, error_code, llvm::sys::fs::F_None));

    if (error_code) {
        std::cerr << error_code.message() << std::endl;
        return 1;
    }

#if LLVM_VERSION_MAJOR > 7
    llvm::WriteBitcodeToFile(*module->getLLModule(), output_file->os());
#else
    llvm::WriteBitcodeToFile(module->getLLModule(), output_file->os());
#endif
    output_file->keep();

    return 0;
}
