# FindGOABackend.cmake (system GOA)
# Resolve goa-backend-1.0 from system-installed pkg-config
# Usage:
#   find_package(GOABackend REQUIRED)
# Provides:
#   GOABACKEND_FOUND
#   GOABACKEND_INCLUDE_DIRS
#   GOABACKEND_LIBRARIES

find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_GOA_BACKEND QUIET goa-backend-1.0)

if (PC_GOA_BACKEND_FOUND)
  set(GOABACKEND_FOUND TRUE)
  set(GOABACKEND_INCLUDE_DIRS ${PC_GOA_BACKEND_INCLUDE_DIRS})
  set(GOABACKEND_LIBRARIES ${PC_GOA_BACKEND_LIBRARIES})
else()
  set(GOABACKEND_FOUND FALSE)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GOABackend DEFAULT_MSG GOABACKEND_FOUND)
