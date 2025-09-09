# Find Libseccomp
# Sets: Libseccomp_FOUND, Libseccomp_INCLUDE_DIRS, Libseccomp_LIBRARIES

find_path(Libseccomp_INCLUDE_DIR seccomp.h)
find_library(Libseccomp_LIBRARY NAMES seccomp)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libseccomp DEFAULT_MSG Libseccomp_LIBRARY Libseccomp_INCLUDE_DIR)

if(Libseccomp_FOUND)
  set(Libseccomp_INCLUDE_DIRS ${Libseccomp_INCLUDE_DIR})
  set(Libseccomp_LIBRARIES ${Libseccomp_LIBRARY})
endif()
