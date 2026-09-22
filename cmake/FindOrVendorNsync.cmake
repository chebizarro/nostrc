# FindOrVendorNsync.cmake
#
# Resolves the nsync synchronization library exactly once for the whole
# nostrc build and exposes a single INTERFACE target ``nostrc::nsync`` (plus
# legacy ``NSYNC_LIB`` / ``NSYNC_INCLUDE_DIR`` variables kept for the many
# existing call sites).
#
# Cache option ``NOSTR_USE_SYSTEM_NSYNC`` selects the resolution mode:
#   * ``AUTO`` (default) -- prefer the system nsync when it can be found via
#     find_library/find_path; otherwise fall back to the vendored copy in
#     ``third_party/nsync`` (git submodule, pinned to a release tag).
#   * ``ON``  -- force use of the system nsync; fatal error if not found.
#   * ``OFF`` -- force use of the vendored copy; system nsync is ignored.
#
# The vendored copy is built as a STATIC, PIC library so it can be folded
# into the ``libnostrgo`` shared library without producing a DT_NEEDED
# ``libnsync.so`` entry.  Its full API is force-loaded into libnostrgo (see
# libgo/CMakeLists.txt) so downstream consumers get access to all nsync
# symbols via ``-lnostrgo``.
#
# Related tracked issue: nostrc-dd5y.

if(DEFINED _NOSTR_FIND_OR_VENDOR_NSYNC_DONE)
  return()
endif()
set(_NOSTR_FIND_OR_VENDOR_NSYNC_DONE TRUE)

set(NOSTR_USE_SYSTEM_NSYNC "AUTO" CACHE STRING
  "Use system nsync: AUTO (system if found, else vendored), ON (force system), OFF (force vendored)")
set_property(CACHE NOSTR_USE_SYSTEM_NSYNC PROPERTY STRINGS AUTO ON OFF)

# Normalise a few common spellings so ``-DNOSTR_USE_SYSTEM_NSYNC=OFF`` and
# ``-DNOSTR_USE_SYSTEM_NSYNC=FALSE`` behave identically.
string(TOUPPER "${NOSTR_USE_SYSTEM_NSYNC}" _nsync_mode)
if(_nsync_mode STREQUAL "TRUE"  OR _nsync_mode STREQUAL "1" OR _nsync_mode STREQUAL "YES")
  set(_nsync_mode "ON")
elseif(_nsync_mode STREQUAL "FALSE" OR _nsync_mode STREQUAL "0" OR _nsync_mode STREQUAL "NO")
  set(_nsync_mode "OFF")
elseif(NOT (_nsync_mode STREQUAL "AUTO" OR _nsync_mode STREQUAL "ON" OR _nsync_mode STREQUAL "OFF"))
  message(FATAL_ERROR
    "NOSTR_USE_SYSTEM_NSYNC must be AUTO, ON or OFF (got '${NOSTR_USE_SYSTEM_NSYNC}')")
endif()

# Root of the git-submodule checkout of google/nsync.
set(_nsync_vendor_dir "${CMAKE_SOURCE_DIR}/third_party/nsync")

# --------------------------------------------------------------------------
# Try to locate a system nsync (unless the caller forced OFF).
# --------------------------------------------------------------------------
set(_nsync_have_system FALSE)
if(NOT _nsync_mode STREQUAL "OFF")
  # Use fresh scratch variables so previous partial finds do not stick.
  unset(_nsync_sys_lib CACHE)
  unset(_nsync_sys_inc CACHE)
  find_library(_nsync_sys_lib NAMES nsync)
  find_path(_nsync_sys_inc   NAMES nsync.h)
  if(_nsync_sys_lib AND _nsync_sys_inc)
    set(_nsync_have_system TRUE)
  endif()
endif()

if(_nsync_mode STREQUAL "ON" AND NOT _nsync_have_system)
  message(FATAL_ERROR
    "NOSTR_USE_SYSTEM_NSYNC=ON but the system nsync library or headers were not found.\n"
    "  library search result: '${_nsync_sys_lib}'\n"
    "  header  search result: '${_nsync_sys_inc}'\n"
    "Install libnsync-dev (Debian/Ubuntu), nsync-devel (Fedora, when available),\n"
    "or re-run cmake without -DNOSTR_USE_SYSTEM_NSYNC=ON to use the vendored copy.")
endif()

# --------------------------------------------------------------------------
# Materialise nostrc::nsync via either the system lib or the vendored one.
# --------------------------------------------------------------------------
add_library(nostrc_nsync INTERFACE)
add_library(nostrc::nsync ALIAS nostrc_nsync)

if(_nsync_have_system AND NOT _nsync_mode STREQUAL "OFF")
  # -----  System nsync  ----------------------------------------------------
  set(NOSTRC_NSYNC_VENDORED FALSE CACHE INTERNAL "nsync is provided by the system")
  set(NSYNC_LIB         "${_nsync_sys_lib}" CACHE PATH "nsync library (resolved)" FORCE)
  set(NSYNC_INCLUDE_DIR "${_nsync_sys_inc}" CACHE PATH "nsync include dir (resolved)" FORCE)
  target_include_directories(nostrc_nsync INTERFACE "${NSYNC_INCLUDE_DIR}")
  target_link_libraries     (nostrc_nsync INTERFACE "${NSYNC_LIB}")
  message(STATUS "nsync: using SYSTEM copy (${NSYNC_LIB})")
else()
  # -----  Vendored nsync (third_party/nsync)  ------------------------------
  if(NOT EXISTS "${_nsync_vendor_dir}/CMakeLists.txt")
    message(FATAL_ERROR
      "Vendored nsync submodule is missing at ${_nsync_vendor_dir}.\n"
      "Run: git submodule update --init --depth 1 third_party/nsync\n"
      "(or install the system libnsync-dev / nsync-devel package and re-run\n"
      "cmake with -DNOSTR_USE_SYSTEM_NSYNC=ON).")
  endif()

  # Force the vendored nsync to build as a STATIC PIC archive regardless of
  # the parent project's BUILD_SHARED_LIBS setting -- we need it foldable
  # into libnostrgo.so without pulling a DT_NEEDED libnsync.so entry.
  set(_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
  set(BUILD_SHARED_LIBS OFF)
  set(NSYNC_ENABLE_TESTS OFF CACHE BOOL "Do not build nsync's test tree" FORCE)

  add_subdirectory(
    "${_nsync_vendor_dir}"
    "${CMAKE_BINARY_DIR}/third_party/nsync"
    EXCLUDE_FROM_ALL)

  # Restore parent BUILD_SHARED_LIBS for any subsequent add_subdirectory().
  if(DEFINED _saved_build_shared_libs)
    set(BUILD_SHARED_LIBS "${_saved_build_shared_libs}")
  else()
    unset(BUILD_SHARED_LIBS)
  endif()

  if(NOT TARGET nsync)
    message(FATAL_ERROR
      "Vendored nsync did not produce the expected 'nsync' CMake target.")
  endif()
  set_target_properties(nsync PROPERTIES POSITION_INDEPENDENT_CODE ON)

  set(NOSTRC_NSYNC_VENDORED TRUE CACHE INTERNAL "nsync is built from the vendored submodule")
  # NSYNC_LIB points at the CMake target so the many existing
  # `target_link_libraries(x PRIVATE ${NSYNC_LIB})` call sites keep working
  # unchanged.  NSYNC_INCLUDE_DIR points at the in-tree public headers.
  set(NSYNC_LIB         "nsync"                                  CACHE STRING "nsync library (resolved)" FORCE)
  set(NSYNC_INCLUDE_DIR "${_nsync_vendor_dir}/public"            CACHE PATH   "nsync include dir (resolved)" FORCE)
  target_include_directories(nostrc_nsync INTERFACE "${NSYNC_INCLUDE_DIR}")
  target_link_libraries     (nostrc_nsync INTERFACE nsync)

  # Install the vendored nsync public headers alongside libnostrgo's headers
  # so downstream consumers of the -devel packages can #include <nsync.h>
  # without needing a separate distro nsync-devel package installed.  The
  # vendored ``nsync`` static archive itself is EXCLUDE_FROM_ALL and folded
  # into libnostrgo.so via --whole-archive, so we do NOT install the .a.
  include(GNUInstallDirs)
  file(GLOB _nsync_public_headers "${_nsync_vendor_dir}/public/nsync*.h")
  install(FILES ${_nsync_public_headers}
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    COMPONENT Development)

  message(STATUS "nsync: using VENDORED copy (third_party/nsync)")
endif()
