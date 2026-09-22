cmake_minimum_required(VERSION 3.22)
project(nostr_json_library C)

# JSON interop layer for nostrc. Wraps jansson APIs and links with libnostr and nsync.

# D-4 (packaging plan): the installed artifact and pkg-config module are renamed
# from `nostr_json` (underscore) to `libnostr-json` (hyphenated, Debian-friendly).
# The in-tree CMake target keeps its historical name `nostr_json` so ~40 sibling
# consumers do not need to change; only the on-disk names change.
#
# B1: SOVERSION comes from apply_versioning() so shared builds get a real SONAME.
if(NOT COMMAND declare_component_version)
  include("${CMAKE_CURRENT_LIST_DIR}/../cmake/VersionHelpers.cmake")
endif()
option(ENABLE_COMPONENT_VERSIONING "Enable generated component version headers" ON)
declare_component_version(NOSTR_JSON 1 0 0)

# Specify the C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

# Find Jansson via pkg-config and use its imported target for proper propagation
find_package(PkgConfig REQUIRED)
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)

# Find libwebsockets via pkg-config to satisfy static libnostr deps
pkg_check_modules(LWS REQUIRED libwebsockets)
include_directories(${LWS_INCLUDE_DIRS})
pkg_check_modules(GLIB QUIET IMPORTED_TARGET glib-2.0>=2.50 gobject-2.0>=2.50)

# Find the nsync library and headers manually (synchronization primitives)
find_library(NSYNC_LIB NAMES nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR NAMES nsync.h REQUIRED)

# Include the nsync headers
include_directories(${NSYNC_INCLUDE_DIR})

# Build shared library so consumers can dynamically link JSON helpers.
# The in-tree CMake target name stays `nostr_json`; OUTPUT_NAME rewrites the
# on-disk artifact to `libnostr-json.so.<SOVERSION>` (D-4).
add_library(nostr_json SHARED src/json.c)
set_target_properties(nostr_json PROPERTIES OUTPUT_NAME nostr-json)

# Bridge component version vars to the differently-cased target name so the
# shared VersionHelpers.cmake sets VERSION + SOVERSION uniformly.
set(nostr_json_VERSION   "${NOSTR_JSON_VERSION}")
set(nostr_json_SOVERSION "${NOSTR_JSON_VERSION_MAJOR}")
set_target_properties(nostr_json PROPERTIES
  COMPONENT_VERSION_HEADER "${NOSTR_JSON_VERSION_HEADER}"
  COMPONENT_VERSION_HEADER_DIR "${NOSTR_JSON_VERSION_HEADER_DIR}"
  COMPONENT_VERSION_HEADER_INSTALL_DIR
    "${CMAKE_INSTALL_INCLUDEDIR}/nostr")
apply_versioning(nostr_json)

# Ensure we don't inherit legacy alias remapping from libnostr
target_compile_definitions(nostr_json PRIVATE NOSTR_ENABLE_LEGACY_ALIASES=0)
target_compile_options(nostr_json PRIVATE -UNOSTR_ENABLE_LEGACY_ALIASES)

target_compile_options(nostr_json PRIVATE $<$<CONFIG:Debug>:-Werror>)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  # Mirror sanitizers so we can link against sanitized static libs cleanly
  if (GO_ENABLE_TSAN)
    target_compile_options(nostr_json PRIVATE -fsanitize=thread)
    target_link_options(nostr_json PRIVATE -fsanitize=thread)
  else()
    if (GO_ENABLE_ASAN)
      target_compile_options(nostr_json PRIVATE -fsanitize=address)
      target_link_options(nostr_json PRIVATE -fsanitize=address)
    endif()
    if (GO_ENABLE_UBSAN)
      target_compile_options(nostr_json PRIVATE -fsanitize=undefined)
      target_link_options(nostr_json PRIVATE -fsanitize=undefined)
    endif()
  endif()
endif()

# Export include dirs for downstream targets; expose public headers under this module's include/.
target_include_directories(nostr_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(nostr_json PUBLIC include)
# Also include libnostr headers for cross-module includes in monorepo builds
target_include_directories(nostr_json PRIVATE ${CMAKE_SOURCE_DIR}/libnostr/include)

# Link with Jansson, libnostr (types/util), and nsync.
target_link_directories(nostr_json PRIVATE ${LWS_LIBRARY_DIRS})
if (GLIB_FOUND)
  target_link_libraries(nostr_json PRIVATE PkgConfig::GLIB)
endif()
target_link_libraries(nostr_json PUBLIC PkgConfig::JANSSON)
target_link_libraries(nostr_json PRIVATE ${NSYNC_LIB} libnostr ${LWS_LIBRARIES})

# Install rules for libjson (nostr_json -> libnostr-json)
include(GNUInstallDirs)
install(TARGETS nostr_json
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr
        FILES_MATCHING PATTERN "*.h")

# pkg-config generation for libnostr-json (D-4). The canonical .pc is
# `libnostr-json.pc`; a one-release compat symlink `nostr_json.pc` keeps old
# consumers building during the transition.
set(LIBNOSTR_JSON_PKGCONF ${CMAKE_CURRENT_BINARY_DIR}/libnostr-json.pc)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/libnostr-json.pc.in ${LIBNOSTR_JSON_PKGCONF} @ONLY)
install(FILES ${LIBNOSTR_JSON_PKGCONF} DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
install(CODE "
  set(_pcdir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/pkgconfig\")
  file(MAKE_DIRECTORY \"\${_pcdir}\")
  execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink
    libnostr-json.pc \"\${_pcdir}/nostr_json.pc\")
")

# One-release compatibility SONAME symlink `libnostr_json.so` ->
# `libnostr-json.so.<SOVERSION>`. Existing meson/Makefile consumers still
# hardcode the underscore spelling (e.g. signet/meson.build).
if(BUILD_SHARED_LIBS OR TRUE)
  if(NOT WIN32)
    if(APPLE)
      set(_nj_shext "dylib")
      set(_nj_versioned "libnostr-json.${NOSTR_JSON_VERSION_MAJOR}.${_nj_shext}")
      set(_nj_compat "libnostr_json.${_nj_shext}")
    else()
      set(_nj_shext "so")
      set(_nj_versioned "libnostr-json.${_nj_shext}.${NOSTR_JSON_VERSION_MAJOR}")
      set(_nj_compat "libnostr_json.${_nj_shext}")
    endif()
    install(CODE "
      set(_libdir \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}\")
      file(MAKE_DIRECTORY \"\${_libdir}\")
      execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink
        ${_nj_versioned} \"\${_libdir}/${_nj_compat}\")
    ")
  endif()
endif()
