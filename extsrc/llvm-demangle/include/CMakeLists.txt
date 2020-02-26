install(
  DIRECTORY llvm/
  DESTINATION include/llvm-demangle
  FILES_MATCHING
  PATTERN "*.h"
)
