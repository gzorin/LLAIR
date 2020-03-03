#include <llair/Tools/Tools.h>

#include <llvm/Support/Path.h>
#include <llvm/Support/Process.h>

#include "ToolsImpl.h"

namespace llair {

namespace {

llvm::SmallString<256> &
pathToTools() {
    static llvm::SmallString<256> s_pathToTools;
    return s_pathToTools;
}

} // namespace

void
setPathToTools(llvm::StringRef path) {
    llvm::sys::path::native(path, pathToTools());
}

llvm::SmallString<256>
getPathToTools() {
    llvm::SmallString<256> path;

    // Is the path set via setPathToTools()?
    path = pathToTools();

    // Is the path set via an environment variable?
    if (path.empty()) {
        auto tmp = llvm::sys::Process::GetEnv("LLAIR_TOOLS_PATH");
        if (tmp) {
            llvm::sys::path::native(*tmp, path);
        }
    }

    return path;
}

} // End namespace llair
