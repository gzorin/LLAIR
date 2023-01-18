#include <llair/IR/EntryPoint.h>
#include <llair/IR/LLAIRContext.h>
#include <llair/IR/Module.h>
#include <llair/Tools/MakeLibrary.h>
#include <llair/Tools/Program.h>

#include <llvm/ADT/StringSet.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Process.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Utils/Cloning.h>

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
            llvm::sys::path::append(path, "air-lld");
            //llvm::sys::path::append(path, "metallib");
        }
    }

    // Make the path absolute:
    if (!path.empty() && llvm::sys::path::is_relative(path)) {
        llvm::sys::fs::make_absolute(path);
    }

    return path;
}

namespace {

std::unique_ptr<llvm::Module>
finalizeLibrary(const Module& module) {
    llvm::StringSet gvs;

    std::for_each(
        module.entry_point_begin(), module.entry_point_end(),
        [&gvs](const auto& entry_point) -> void {
            gvs.insert(entry_point.getFunction()->getName());
        });

#if LLVM_VERSION_MAJOR >= 8
    auto finalized_module = llvm::CloneModule(*module.getLLModule());
#else
    auto finalized_module =  llvm::CloneModule(module.getLLModule());
#endif

    if (auto class_md = finalized_module->getNamedMetadata("llair.class"); class_md) {
        finalized_module->eraseNamedMetadata(class_md);
    }

    llvm::legacy::PassManager mpm;

    mpm.add(llvm::createInternalizePass([&gvs](const llvm::GlobalValue& gv) -> bool {
        return gvs.count(gv.getName()) == 1;
    }));

    mpm.add(llvm::createGlobalDCEPass());

    mpm.run(*finalized_module);

    return finalized_module;
}

}

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
makeLibrary(const llvm::Module &module) {
    auto path     = getPathToLibraryTool();
    auto filename = llvm::sys::path::filename(path).str();

    llvm::ArrayRef<std::string> args = {filename.data(), "--macos_version_min", "12.0", "-o", "-", "/dev/stdin"};
    //llvm::ArrayRef<std::string> args = {filename.data(), "-o", "-", "-"};

#if LLVM_VERSION_MAJOR >= 12
    auto program = llvm::errorOrToExpected(openProgram((std::string)path, args));
#else
    auto program = llvm::errorOrToExpected(openProgram(path.str(), args));
#endif

    if (program) {
        // Write the module:
 #if LLVM_VERSION_MAJOR >= 8
        llvm::WriteBitcodeToFile(module, *program->input);
#else
        llvm::WriteBitcodeToFile(&module, *program->input);
#endif
        program->input->close();

        // Read output:
        return llvm::errorOrToExpected(getMemoryBufferForStream(program->output, ""));
    }
    else {
        return program.takeError();
    }
}

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
makeLibrary(const Module &module) {
    auto finalized_module = finalizeLibrary(module);

    return makeLibrary(*finalized_module);
}

llvm::Expected<std::unique_ptr<llvm::MemoryBuffer>>
makeLibrary(llvm::MemoryBufferRef input) {
    auto path     = getPathToLibraryTool();
    auto filename = llvm::sys::path::filename(path).str();

    llvm::ArrayRef<std::string> args = {filename.data(), "--macos_version_min", "12.0", "-o", "-", "/dev/stdin"};
    //llvm::ArrayRef<std::string> args = {filename.data(), "-o", "-", "-"};

#if LLVM_VERSION_MAJOR >= 12
    return llvm::errorOrToExpected(runProgram((std::string)path, args, input));
#else
    return llvm::errorOrToExpected(runProgram(path.str(), args, input));
#endif
}

} // namespace llair
