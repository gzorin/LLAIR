#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Tools/Compile.h>
#include <llair/Tools/Program.h>

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Process.h>

#include <iostream>
#include <string>

#include "ToolsImpl.h"

namespace llair {

llvm::Expected<std::unique_ptr<Module>>
compileBuffer(llvm::MemoryBufferRef buffer, llvm::ArrayRef<llvm::StringRef> options,
              LLAIRContext &context) {
    auto path     = getPathToTools();
    auto filename = llvm::sys::path::filename(path).str();

    std::vector<std::string> args = {filename.data(), "metal", "-c", "-x", "metal"};

    std::transform(
        options.begin(), options.end(),
        std::back_inserter(args),
        [](auto option) -> auto {
            return option.str();
        });

    args.push_back("-o");
    args.push_back("-");
    args.push_back("-");

#if LLVM_VERSION_MAJOR >= 12
    auto bitcode = llvm::errorOrToExpected(runProgram((std::string)path, args, buffer));
#else
    auto bitcode = llvm::errorOrToExpected(runProgram(path.str(), args, buffer));
#endif

    if (bitcode) {
        auto llmodule = llvm::getLazyBitcodeModule(**bitcode, context.getLLContext());
        if (llmodule) {
            auto error = (*llmodule)->materializeAll();
            if (!error) {
                return std::unique_ptr<Module>(new Module(std::move(*llmodule)));
            }
            else {
                return std::move(error);
            }
        }
        else {
            return llmodule.takeError();
        }
    }
    else {
        return bitcode.takeError();
    }
}

} // namespace llair
