# Find FUSE3
# Sets: FUSE3_FOUND, FUSE3_INCLUDE_DIRS, FUSE3_LIBRARIES

find_path(FUSE3_INCLUDE_DIR fuse3/fuse.h
  PATH_SUFFIXES include
)

find_library(FUSE3_LIBRARY NAMES fuse3)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FUSE3 DEFAULT_MSG FUSE3_LIBRARY FUSE3_INCLUDE_DIR)

if(FUSE3_FOUND)
  set(FUSE3_INCLUDE_DIRS ${FUSE3_INCLUDE_DIR})
  set(FUSE3_LIBRARIES ${FUSE3_LIBRARY})
endif()
