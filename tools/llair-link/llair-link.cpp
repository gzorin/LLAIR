#include <llair/Bitcode/Bitcode.h>
#include <llair/IR/Class.h>
#include <llair/IR/Dispatcher.h>
#include <llair/IR/Interface.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Linker/Linker.h>

#include "InterfaceScope.h"

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/ToolOutputFile.h>

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

} // namespace

using namespace llair;

//
int
main(int argc, char **argv) {
    llvm::cl::ParseCommandLineOptions(argc, argv, "llair-link\n");

    llvm::ExitOnError exit_on_err("llair-link: ");

    auto llvm_context  = std::make_unique<llvm::LLVMContext>();
    auto llair_context = std::make_unique<llair::LLAIRContext>(*llvm_context);

    std::vector<std::unique_ptr<llair::Module>> input_modules;

    std::transform(
        input_filenames.begin(), input_filenames.end(),
        std::back_inserter(input_modules),
        [&exit_on_err, &llair_context](auto input_filename) -> std::unique_ptr<llair::Module> {
            auto buffer =
                exit_on_err(errorOrToExpected(llvm::MemoryBuffer::getFileOrSTDIN(input_filename)));
            auto module = exit_on_err(
                llair::getBitcodeModule(llvm::MemoryBufferRef(*buffer), *llair_context));
            return module;
        });

    auto output = std::make_unique<llair::Module>(output_filename, *llair_context);

    std::for_each(
        input_modules.begin(), input_modules.end(),
        [&output](auto &input_module) -> void {
            linkModules(output.get(), input_module.get());
        });

    auto interface_scope = std::make_unique<InterfaceScope>("dispatchers", *llair_context);

    auto interfaces = output->getAllInterfacesFromABI();

    std::for_each(
        interfaces.begin(), interfaces.end(),
        [&interface_scope](auto interface) -> void {
            interface_scope->insertInterface(interface);
            interface->print(llvm::errs());
        });

    auto classes = output->getOrLoadAllClassesFromABI();

    std::for_each(
        classes.begin(), classes.end(),
        [&interface_scope](auto klass) -> void {
            interface_scope->insertClass(klass);
            klass->print(llvm::errs());
        });

    linkModules(output.get(), interface_scope->module());

    // Write it out:
    std::error_code                       error_code;
    std::unique_ptr<llvm::ToolOutputFile> output_file(
        new llvm::ToolOutputFile(output_filename, error_code, llvm::sys::fs::F_None));

    if (error_code) {
        llvm::errs() << error_code.message();
        return 1;
    }

    llvm::WriteBitcodeToFile(output->getLLModule(), output_file->os());
    output_file->keep();

    return 0;
}
