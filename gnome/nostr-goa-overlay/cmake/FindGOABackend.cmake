# FindGOABackend.cmake
# Tries to locate goa-backend-1.0 built from the vendor tree.
# Usage:
#   find_package(GOABackend REQUIRED)
# Provides:
#   GOABACKEND_FOUND
#   GOABACKEND_INCLUDE_DIRS
#   GOABACKEND_LIBRARIES
#
# Env/Cache hints:
#   GOA_BUILD_DIR: path to vendor/gnome-online-accounts build dir (meson _build)

if (NOT DEFINED GOA_BUILD_DIR)
  set(GOA_BUILD_DIR "${CMAKE_CURRENT_LIST_DIR}/../vendor/gnome-online-accounts/_build" CACHE PATH "GOA vendor build dir")
endif()

# Try pkg-config first with PKG_CONFIG_PATH pointed at vendor build
find_package(PkgConfig REQUIRED)
if (EXISTS "${GOA_BUILD_DIR}")
  set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:${GOA_BUILD_DIR}")
endif()

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
