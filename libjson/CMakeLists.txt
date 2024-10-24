cmake_minimum_required(VERSION 3.10)
project(nostr_json_library C)

# Specify the C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED True)

# Find Jansson package
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

# Find the nsync library and headers manually
find_library(NYNC_LIB nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)

# Include the nsync headers
include_directories(${NSYNC_INCLUDE_DIR})

# Add the library source files
add_library(nostr_json SHARED src/json.c)

target_compile_options(nostr_json PRIVATE $<$<CONFIG:Debug>:-Werror>)

# Include headers
target_include_directories(nostr_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(nostr_json PUBLIC include)

# Link with Jansson library
target_link_libraries(nostr_json PRIVATE ${NSYNC_LIB} libnostr jansson)
