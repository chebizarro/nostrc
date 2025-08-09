cmake_minimum_required(VERSION 3.10)
project(nostr_json_library C)

# JSON interop layer for nostrc. Wraps jansson APIs and links with libnostr and nsync.

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
add_library(nostr_json SHARED src/json.c)

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

# Link with Jansson, libnostr (types/util), and nsync.
target_link_directories(nostr_json PRIVATE ${LWS_LIBRARY_DIRS})
if (GLIB_FOUND)
  target_link_libraries(nostr_json PRIVATE PkgConfig::GLIB)
endif()
target_link_libraries(nostr_json PUBLIC PkgConfig::JANSSON)
target_link_libraries(nostr_json PRIVATE ${NSYNC_LIB} libnostr ${LWS_LIBRARIES})

# Install rules for libjson (nostr_json)
include(GNUInstallDirs)
install(TARGETS nostr_json
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr
        FILES_MATCHING PATTERN "*.h")

# pkg-config generation for nostr_json
set(NOSTR_JSON_PKGCONF ${CMAKE_CURRENT_BINARY_DIR}/nostr_json.pc)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nostr_json.pc.in ${NOSTR_JSON_PKGCONF} @ONLY)
install(FILES ${NOSTR_JSON_PKGCONF} DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
