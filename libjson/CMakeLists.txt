cmake_minimum_required(VERSION 3.10)
project(nostr_json_library C)

# JSON interop layer for nostrc. Wraps jansson APIs and links with libnostr and nsync.

# Specify the C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

# Find Jansson package (JSON parser/serializer)
find_path(JANSSON_INCLUDE_DIR NAMES jansson.h)
find_library(JANSSON_LIBRARY NAMES jansson)
if (JANSSON_LIBRARY AND JANSSON_INCLUDE_DIR)
    include_directories(${JANSSON_INCLUDE_DIR})
else ()
    message(FATAL_ERROR "libjansson not found (headers or library missing)")
endif ()

# Find libwebsockets via pkg-config to satisfy static libnostr deps
find_package(PkgConfig REQUIRED)
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
target_link_libraries(nostr_json PRIVATE ${NSYNC_LIB} libnostr ${JANSSON_LIBRARY} ${LWS_LIBRARIES})
