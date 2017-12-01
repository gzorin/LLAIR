#include <llair/Tools/Program.h>

#include <llvm/ADT/SmallString.h>

#include "popen2.h"

#include <iostream>

#include <unistd.h>

namespace llair {

llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
runAndWait(llvm::StringRef command, llvm::MemoryBufferRef input)
{
  struct popen2 child;
  int e = popen2(command.data(), &child);

  if (e < 0) {
    return std::error_code(errno, std::generic_category());
  }

  auto nwrite = write(child.to_child, input.getBufferStart(), input.getBufferSize());
  close(child.to_child);

  if (nwrite < input.getBufferSize()) {
    return std::error_code(errno, std::generic_category());
  }

  const ssize_t ChunkSize = 4096*4;
  llvm::SmallString<ChunkSize> Buffer;
  ssize_t nread;

  do {
    Buffer.reserve(Buffer.size() + ChunkSize);

    nread = read(child.from_child, Buffer.end(), ChunkSize);
    
    if (nread == -1) {
      return std::error_code(errno, std::generic_category());
    }

    Buffer.set_size(Buffer.size() + nread);
  } while (nread == ChunkSize);

  return llvm::MemoryBuffer::getMemBufferCopy(Buffer, "");
}

} // End namespace llair
