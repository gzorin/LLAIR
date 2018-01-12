install(
  DIRECTORY llair/
  DESTINATION include/llair
  FILES_MATCHING
  PATTERN "*.gen"
  PATTERN "*.h"
  PATTERN "*.td"
)
