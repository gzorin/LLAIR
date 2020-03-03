#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Tools/MakeLibrary.h>
#include <llair/Tools/Program.h>

#include <llvm/Bitcode/BitcodeWriter.h>
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
pathToLibraryTool() {
    static llvm::SmallString<256> s_pathToLibraryTool;
    return s_pathToLibraryTool;
}

} // namespace

void
setPathToLibraryTool(llvm::StringRef path) {
    llvm::sys::path::native(path, pathToLibraryTool());
}

llvm::SmallString<256>
getPathToLibraryTool() {
    llvm::SmallString<256> path;

    // Is the path set via setPathToLibraryTool()?
    path = pathToLibraryTool();

    // Is the path set via an environment variable?
    if (path.empty()) {
        auto tmp = llvm::sys::Process::GetEnv("LLAIR_LIBRARY_TOOL_PATH");
        if (tmp) {
            llvm::sys::path::native(*tmp, path);
        }
    }

    // Is the tools path set?
    if (path.empty()) {
        auto tmp = getPathToTools();
        if (llvm::sys::fs::is_directory(tmp)) {
            path = tmp;
            llvm::sys::path::append(path, "metallib");
        }
    }

    // Make the path absolute:
    if (!path.empty() && llvm::sys::path::is_relative(path)) {
        llvm::sys::fs::make_absolute(path);
    }

    return path;
}

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
makeLibrary(const Module &module) {
    auto path     = getPathToLibraryTool();
    auto filename = llvm::sys::path::filename(path).str();

    llvm::ArrayRef<llvm::StringRef> args = {filename.data(), "-o", "-", "-"};

    auto program = llvm::errorOrToExpected(openProgram(path.str(), args));

    if (program) {
        // Write the module:
        llvm::WriteBitcodeToFile(module.getLLModule(), *program->input);
        program->input->close();

        // Read output:
        return llvm::errorOrToExpected(getMemoryBufferForStream(program->output, ""));
    }
    else {
        return program.takeError();
    }
}

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
makeLibrary(llvm::MemoryBufferRef input) {
    auto path     = getPathToLibraryTool();
    auto filename = llvm::sys::path::filename(path).str();

    llvm::ArrayRef<llvm::StringRef> args = {filename.data(), "-o", "-", "-"};

    return llvm::errorOrToExpected(runProgram(path.str(), args, input));
}

} // namespace llair
