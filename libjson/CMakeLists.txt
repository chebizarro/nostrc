cmake_minimum_required(VERSION 3.10)
project(nostr_json_library C)

# JSON interop layer for nostrc. Wraps jansson APIs and links with libnostr and nsync.

# Specify the C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

# Find Jansson package (JSON parser/serializer)
find_path(JANSSON_INCLUDE_DIR NAMES jansson.h)
find_library(JANSSON_LIBRARY NAMES jansson)
if (JANSSON_LIBRARY)
    set(JANSSON_PKG libjansson)
    set(JANSSON_LIBRARIES ${JANSSON_LIBRARY})
    include_directories(${JANSSON_INCLUDE_DIRS})
    link_directories(${JANSSON_LIBRARY_DIRS})
else (JANSSON_LIBRARY)
    message(FATAL_ERROR "libjansson >=2.3 not found")
endif (JANSSON_LIBRARY)

# Find the nsync library and headers manually (synchronization primitives)
find_library(NYNC_LIB nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)

# Include the nsync headers
include_directories(${NSYNC_INCLUDE_DIR})

# Build shared library so consumers can dynamically link JSON helpers.
add_library(nostr_json SHARED src/json.c)

target_compile_options(nostr_json PRIVATE $<$<CONFIG:Debug>:-Werror>)

# Export include dirs for downstream targets; expose public headers under this module's include/.
target_include_directories(nostr_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(nostr_json PUBLIC include)

# Link with Jansson, libnostr (types/util), and nsync.
target_link_libraries(nostr_json PRIVATE ${NSYNC_LIB} libnostr jansson)
