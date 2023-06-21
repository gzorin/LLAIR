#include <llair/Tools/Program.h>

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/raw_ostream.h>

#include "popen2.h"

#include <algorithm>
#include <iostream>
#include <vector>

#include <unistd.h>

namespace llair {

llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
getMemoryBufferForStream(int FD, const llvm::Twine &BufferName) {
    const ssize_t                ChunkSize = 4096 * 4;
    llvm::SmallString<ChunkSize> Buffer;
    ssize_t                      nread;

    do {
        Buffer.reserve(Buffer.size() + ChunkSize);

        nread = read(FD, Buffer.end(), ChunkSize);

        if (nread == -1) {
            return std::error_code(errno, std::generic_category());
        }

        Buffer.resize(Buffer.size() + nread);
    } while (nread == ChunkSize);

    return llvm::MemoryBuffer::getMemBufferCopy(Buffer, BufferName);
}

llvm::ErrorOr<llair::Program>
openProgram(const std::string& path, llvm::ArrayRef<std::string> args) {
    std::vector<char *> argv;
    argv.reserve(args.size());
    std::transform(
        args.begin(), args.end(), std::back_inserter(argv),
        [](const std::string& arg) -> char * { return const_cast<char *const>(arg.c_str()); });
    argv.push_back(nullptr);

    struct popen2 child;
    int           e = popen2(path.c_str(), argv.data(), &child);

    if (e < 0) {
        return std::error_code(errno, std::generic_category());
    }

    return Program(
        {child.child_pid,
         std::unique_ptr<llvm::raw_fd_ostream>(new llvm::raw_fd_ostream(child.to_child, true)),
         child.from_child});
}

llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
runProgram(const std::string& path, llvm::ArrayRef<std::string> args,
           llvm::MemoryBufferRef input) {
    auto program = openProgram(path, args);

    if (program) {
        // Write input:
        program->input->write(input.getBufferStart(), input.getBufferSize());
        program->input->close();

        // Read output:
        return getMemoryBufferForStream(program->output, "");
    }
    else {
        return program.getError();
    }
}

} // End namespace llair
