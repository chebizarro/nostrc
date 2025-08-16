project(libnostr)

# Core Nostr library. Provides event types, relays, subscriptions, filters, and utils.
find_library(NSYNC_LIB nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)

# Include directories
include_directories(include)
include_directories(${NSYNC_INCLUDE_DIR})

# Optional GLib/GObject for integration layer (no hard dependency for core)
option(NOSTR_WITH_GLIB "Enable GLib integration headers" ON)
find_package(PkgConfig REQUIRED)
if(NOSTR_WITH_GLIB)
  pkg_check_modules(GLIB QUIET IMPORTED_TARGET glib-2.0>=2.50 gobject-2.0>=2.50)
  if(GLIB_FOUND)
    add_definitions(-DNOSTR_HAVE_GLIB=1)
  else()
    add_definitions(-DNOSTR_HAVE_GLIB=0)
  endif()
else()
  add_definitions(-DNOSTR_HAVE_GLIB=0)
endif()

# Find the libwebsockets package
pkg_check_modules(LWS REQUIRED libwebsockets)
# Optionally enable websockets support by uncommenting the lines below and
# ensuring libwebsockets is available on the system.
# pkg_check_modules(LWS REQUIRED libwebsockets)
# include_directories(${LWS_INCLUDE_DIRS})

set(LIBNOSTR_SOURCES
  src/timestamp.c
  src/tag.c
  src/event.c
  src/envelope.c
  src/init.c
  src/filter.c
  src/connection.c
  src/relay.c
  src/subscription.c
  src/keys.c
  src/json.c
  src/relay_store.c
  src/utils.c
  src/simplepool.c
  # Pluggable storage interface (Phase A: registry + stubs)
  src/store/registry.c
  src/store/store.c
  # Minimal internal crypto for NIP-06 (BIP39/BIP32 subset)
  src/crypto/bip39_wordlist_en.c
  src/crypto/bip39.c
  src/crypto/bip32.c
)

# Optional metrics instrumentation
option(NOSTR_ENABLE_METRICS "Enable internal metrics instrumentation" OFF)
if(NOSTR_ENABLE_METRICS)
  list(APPEND LIBNOSTR_SOURCES src/metrics.c)
  add_definitions(-DNOSTR_ENABLE_METRICS=1)
endif()

# GI thin wrappers for macros/aliases
# (pointer helpers moved into core utils.c)

# Optional GLib bridge for JSON provider
if(NOSTR_WITH_GLIB AND GLIB_FOUND)
  list(APPEND LIBNOSTR_SOURCES src/nostr_json_glib.c)
  # Boxed type registrations
  list(APPEND LIBNOSTR_SOURCES src/nostr_event_boxed.c src/nostr_filter_boxed.c)
  option(NOSTR_EXPERIMENTAL_RELAY_BOXED "Enable experimental Relay GBoxed registration" OFF)
  if(NOSTR_EXPERIMENTAL_RELAY_BOXED)
    list(APPEND LIBNOSTR_SOURCES src/nostr_relay_boxed.c)
  endif()
endif()

set(LIBNOSTR_LIBS ${NSYNC_LIB})
if(NOSTR_WITH_GLIB AND GLIB_FOUND)
  list(APPEND LIBNOSTR_LIBS ${GLIB_LIBRARIES})
endif()


set(CMAKE_POSITION_INDEPENDENT_CODE ON)

## External vendor integration removed: project uses in-tree minimal BIP39/BIP32.


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

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  if(GO_ENABLE_TSAN)
    target_compile_options(nostr PRIVATE -fsanitize=thread)
    target_link_options(nostr PRIVATE -fsanitize=thread)
  else()
    if(GO_ENABLE_ASAN)
      target_compile_options(nostr PRIVATE -fsanitize=address)
      target_link_options(nostr PRIVATE -fsanitize=address)
    endif()
    if(GO_ENABLE_UBSAN)
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
target_link_libraries(nostr PUBLIC ${LWS_LIBRARIES} libgo OpenSSL::Crypto ${SECP256K1_LIB})
if(NOSTR_WITH_GLIB AND GLIB_FOUND)
  target_link_libraries(nostr PUBLIC PkgConfig::GLIB)
endif()

# (Previous external vendor toggles fully removed.)

target_include_directories(nostr
  PUBLIC
  include
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/store
)

if(GLIB_FOUND)
  target_compile_definitions(nostr PUBLIC NOSTR_HAVE_GLIB=1)
  target_include_directories(nostr PUBLIC ${GLIB_INCLUDE_DIRS})
endif()
target_compile_definitions(nostr PUBLIC NOSTR_HAVE_LIBGO=1)

# Optional nostrdb backend (Phase A: flag only; backend implementation added in later phases)
option(LIBNOSTR_WITH_NOSTRDB "Enable nostrdb storage backend" OFF)
if(LIBNOSTR_WITH_NOSTRDB)
  target_compile_definitions(nostr PUBLIC LIBNOSTR_WITH_NOSTRDB=1)
  # Attach backend sources to the existing target
  target_sources(nostr PRIVATE src/store/nostrdb/ndb_backend.c)

  # Vendored nostrdb setup: expect a subdirectory (default: third_party/nostrdb)
  set(LIBNOSTR_NDB_VENDOR_PATH "${CMAKE_SOURCE_DIR}/third_party/nostrdb" CACHE PATH "Path to vendored nostrdb source")
  if(EXISTS "${LIBNOSTR_NDB_VENDOR_PATH}/src/nostrdb.c")
    message(STATUS "libnostr: using vendored nostrdb sources at ${LIBNOSTR_NDB_VENDOR_PATH}")
    # Collect nostrdb core sources and deps
    file(GLOB NDB_CORE_SOURCES
      "${LIBNOSTR_NDB_VENDOR_PATH}/src/*.c"
    )
    file(GLOB NDB_BOLT11_SOURCES
      "${LIBNOSTR_NDB_VENDOR_PATH}/src/bolt11/*.c"
    )
    file(GLOB_RECURSE NDB_CCAN_SOURCES
      "${LIBNOSTR_NDB_VENDOR_PATH}/ccan/**/*.c"
    )
    # Exclude CCAN tools and benchmarks
    list(FILTER NDB_CCAN_SOURCES EXCLUDE REGEX ".*/tools/.*")
    list(FILTER NDB_CCAN_SOURCES EXCLUDE REGEX ".*/benchmarks?/.*")
    # Add LMDB sources from vendored deps
    set(NDB_LMDB_SOURCES
      "${LIBNOSTR_NDB_VENDOR_PATH}/deps/lmdb/mdb.c"
      "${LIBNOSTR_NDB_VENDOR_PATH}/deps/lmdb/midl.c"
    )
    add_library(nostrdb STATIC ${NDB_CORE_SOURCES} ${NDB_LMDB_SOURCES} ${NDB_BOLT11_SOURCES} ${NDB_CCAN_SOURCES})
    target_include_directories(nostrdb PUBLIC
      "${LIBNOSTR_NDB_VENDOR_PATH}/src"
      "${LIBNOSTR_NDB_VENDOR_PATH}/src/bolt11"
      "${LIBNOSTR_NDB_VENDOR_PATH}/ccan"
      "${LIBNOSTR_NDB_VENDOR_PATH}/ccan/ccan"
      "${LIBNOSTR_NDB_VENDOR_PATH}/ccan/ccan/array_size"
      "${LIBNOSTR_NDB_VENDOR_PATH}/ccan/ccan/mem"
      "${LIBNOSTR_NDB_VENDOR_PATH}/ccan/ccan/short_types"
      "${LIBNOSTR_NDB_VENDOR_PATH}/ccan/ccan/crypto/sha256"
      "${LIBNOSTR_NDB_VENDOR_PATH}/ccan/ccan/tal"
      "${LIBNOSTR_NDB_VENDOR_PATH}/ccan/ccan/tal/str"
      "${LIBNOSTR_NDB_VENDOR_PATH}/deps/lmdb"
      "${LIBNOSTR_NDB_VENDOR_PATH}/deps/flatcc/include"
      "${LIBNOSTR_NDB_VENDOR_PATH}/deps/secp256k1/include"
    )
    # Threads, flatcc runtime, secp256k1, and Security framework on macOS
    find_package(Threads REQUIRED)
    # Prefer system flatcc runtime to match headers (e.g., Homebrew flatccrt)
    find_library(FLATCCRT_LIB flatccrt)
    if(NOT FLATCCRT_LIB)
      message(FATAL_ERROR "flatccrt library not found on system; please install flatcc or adjust build to compile vendored runtime.")
    endif()
    target_link_libraries(nostrdb PUBLIC Threads::Threads ${SECP256K1_LIB} ${FLATCCRT_LIB})
    if(APPLE)
      target_link_libraries(nostrdb PUBLIC "-framework Security")
    endif()
    # Link our libnostr against nostrdb
    target_link_libraries(nostr PUBLIC nostrdb)
  else()
    message(WARNING "LIBNOSTR_WITH_NOSTRDB=ON but nostrdb sources not found at ${LIBNOSTR_NDB_VENDOR_PATH}.\n"
                    "Please clone https://github.com/damus-io/nostrdb into third_party/nostrdb or set LIBNOSTR_NDB_VENDOR_PATH.")
  endif()
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
  FILES_MATCHING PATTERN "*.h" PATTERN "nostr-config.h.in" EXCLUDE PATTERN "nostr-pointer.h" EXCLUDE)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/nostr-config.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nostr)

# pkg-config generation
if(GLIB_FOUND)
  set(NOSTR_PKG_REQUIRES "glib-2.0 >= 2.50, gobject-2.0 >= 2.50")
else()
  set(NOSTR_PKG_REQUIRES "")
endif()
set(NOSTR_PKGCONF ${CMAKE_CURRENT_BINARY_DIR}/nostr.pc)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nostr.pc.in ${NOSTR_PKGCONF} @ONLY)
install(FILES ${NOSTR_PKGCONF} DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
