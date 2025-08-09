project(libnostr)

# Core Nostr library. Provides event types, relays, subscriptions, filters, and utils.
find_library(NSYNC_LIB nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)

# Include directories
include_directories(include)
include_directories(${NSYNC_INCLUDE_DIR})

# Optional GLib/GObject for integration layer (no hard dependency for core)
option(NOSTR_WITH_GLIB "Enable GLib integration headers" ON)
option(NOSTR_ENABLE_LEGACY_ALIASES "Enable temporary legacy compatibility aliases" OFF)
# Reset cache default to OFF if not explicitly provided by the user
if (NOT DEFINED CACHE{NOSTR_ENABLE_LEGACY_ALIASES})
  set(NOSTR_ENABLE_LEGACY_ALIASES OFF CACHE BOOL "Enable temporary legacy compatibility aliases" FORCE)
endif()
find_package(PkgConfig REQUIRED)
if (NOSTR_WITH_GLIB)
  pkg_check_modules(GLIB QUIET IMPORTED_TARGET glib-2.0>=2.50 gobject-2.0>=2.50)
  if (GLIB_FOUND)
    add_definitions(-DNOSTR_HAVE_GLIB=1)
  else()
    add_definitions(-DNOSTR_HAVE_GLIB=0)
  endif()
else()
  add_definitions(-DNOSTR_HAVE_GLIB=0)
endif()

# Find the libwebsockets package
pkg_check_modules(LWS REQUIRED libwebsockets)
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson)
# Optionally enable websockets support by uncommenting the lines below and
# ensuring libwebsockets is available on the system.
# pkg_check_modules(LWS REQUIRED libwebsockets)
# include_directories(${LWS_INCLUDE_DIRS})

set(LIBNOSTR_SOURCES
    src/timestamp.c
    src/tag.c
    src/event.c
	src/envelope.c
    src/filter.c
	src/connection.c
    src/relay.c
    src/subscription.c
	src/keys.c
	src/json.c
	src/relay_store.c
	src/utils.c
	src/event_extra.c
	src/pointer.c
	src/simplepool.c
)

# GI thin wrappers for macros/aliases
list(APPEND LIBNOSTR_SOURCES
  src/nostr_utils_wrap.c
  src/nostr_event_extra_wrap.c
  src/nostr_pointer_wrap.c
  src/nostr_simple_pool_wrap.c
)

# Optional GLib bridge for JSON provider
if (NOSTR_WITH_GLIB AND GLIB_FOUND)
  list(APPEND LIBNOSTR_SOURCES src/nostr_json_glib.c)
endif()

set(LIBNOSTR_LIBS ${NSYNC_LIB})
if (NOSTR_WITH_GLIB AND GLIB_FOUND)
  list(APPEND LIBNOSTR_LIBS ${GLIB_LIBRARIES})
endif()


set(CMAKE_POSITION_INDEPENDENT_CODE ON)


# Configure header: nostr-config.h
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/nostr-config.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/nostr-config.h @ONLY)

# Library target (named 'nostr' so pkg-config uses -lnostr). Provide 'libnostr' alias for compatibility.
add_library(nostr ${LIBNOSTR_SOURCES})
add_library(libnostr ALIAS nostr)

# Warnings and optional -Werror
option(GO_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
target_compile_options(nostr PRIVATE -Wall -Wextra -Wpedantic $<$<BOOL:${GO_WARNINGS_AS_ERRORS}>:-Werror>)

# Sanitizer options (Debug builds recommended)
option(GO_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(GO_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(GO_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  if (GO_ENABLE_TSAN)
    target_compile_options(nostr PRIVATE -fsanitize=thread)
    target_link_options(nostr PRIVATE -fsanitize=thread)
  else()
    if (GO_ENABLE_ASAN)
      target_compile_options(nostr PRIVATE -fsanitize=address)
      target_link_options(nostr PRIVATE -fsanitize=address)
    endif()
    if (GO_ENABLE_UBSAN)
      target_compile_options(nostr PRIVATE -fsanitize=undefined)
      target_link_options(nostr PRIVATE -fsanitize=undefined)
    endif()
  endif()
endif()

# Link against libgo (concurrency primitives) and libwebsockets if enabled.
target_include_directories(nostr PUBLIC ${LWS_INCLUDE_DIRS})
target_link_directories(nostr PUBLIC ${LWS_LIBRARY_DIRS})
find_package(OpenSSL REQUIRED)
find_library(SECP256K1_LIB NAMES secp256k1 REQUIRED)
target_link_libraries(nostr PUBLIC ${LWS_LIBRARIES} libgo OpenSSL::Crypto ${SECP256K1_LIB} PkgConfig::JANSSON)
if (NOSTR_WITH_GLIB AND GLIB_FOUND)
  target_link_libraries(nostr PUBLIC PkgConfig::GLIB)
endif()

target_include_directories(nostr
  PUBLIC
    include
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
)

if (GLIB_FOUND)
  target_compile_definitions(nostr PUBLIC NOSTR_HAVE_GLIB=1)
  target_include_directories(nostr PUBLIC ${GLIB_INCLUDE_DIRS})
endif()
target_compile_definitions(nostr PUBLIC NOSTR_HAVE_LIBGO=1)
if (NOSTR_ENABLE_LEGACY_ALIASES)
  # Keep legacy aliasing internal; do not leak to dependents to avoid macro conflicts
  target_compile_definitions(nostr PRIVATE NOSTR_ENABLE_LEGACY_ALIASES=1)
endif()

# Install rules
include(GNUInstallDirs)
install(TARGETS nostr
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr
        FILES_MATCHING PATTERN "*.h" PATTERN "nostr-config.h.in" EXCLUDE)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/nostr-config.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr)

# pkg-config generation
if (GLIB_FOUND)
  set(NOSTR_PKG_REQUIRES "glib-2.0 >= 2.50, gobject-2.0 >= 2.50")
else()
  set(NOSTR_PKG_REQUIRES "")
endif()
set(NOSTR_PKGCONF ${CMAKE_CURRENT_BINARY_DIR}/nostr.pc)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nostr.pc.in ${NOSTR_PKGCONF} @ONLY)
install(FILES ${NOSTR_PKGCONF} DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
