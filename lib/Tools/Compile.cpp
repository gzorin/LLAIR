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

namespace {

llvm::SmallString<256> &
pathToCompileTool() {
    static llvm::SmallString<256> s_pathToCompileTool;
    return s_pathToCompileTool;
}

} // namespace

void
setPathToCompileTool(llvm::StringRef path) {
    llvm::sys::path::native(path, pathToCompileTool());
}

llvm::SmallString<256>
getPathToCompileTool() {
    llvm::SmallString<256> path;

    // Is the path set via setPathToCompileTool()?
    path = pathToCompileTool();

    // Is the path set via an environment variable?
    if (path.empty()) {
        auto tmp = llvm::sys::Process::GetEnv("LLAIR_COMPILE_TOOL_PATH");
        if (tmp) {
            llvm::sys::path::native(*tmp, path);
        }
    }

    // Is the tools path set?
    if (path.empty()) {
        auto tmp = getPathToTools();
        if (llvm::sys::fs::is_directory(tmp)) {
            path = tmp;
            llvm::sys::path::append(path, "metal");
        }
    }

    // Make the path absolute:
    if (!path.empty() && llvm::sys::path::is_relative(path)) {
        llvm::sys::fs::make_absolute(path);
    }

    return path;
}

llvm::Expected<std::unique_ptr<Module>>
compileBuffer(llvm::MemoryBufferRef buffer, llvm::ArrayRef<llvm::StringRef> options,
              LLAIRContext &context) {
    auto path     = getPathToCompileTool();
    auto filename = llvm::sys::path::filename(path).str();

    std::vector<std::string> args = {filename.data(), "-c", "-x", "metal"};

    std::copy(options.begin(), options.end(), std::back_inserter(args));

    args.push_back("-o");
    args.push_back("-");
    args.push_back("-");

    auto bitcode = llvm::errorOrToExpected(runProgram(path.str(), args, buffer));

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
