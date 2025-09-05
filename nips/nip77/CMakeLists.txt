cmake_minimum_required(VERSION 3.16)
project(nip77)

# Option aliasing for consistency across subdirs/tests
option(ENABLE_NIP77_NOSTRDB "Enable building nostrdb-backed NIP-77 datasource and tests" OFF)
if(ENABLE_NIP77_NOSTRDB AND NOT WITH_NIP77_NOSTRDB)
  set(WITH_NIP77_NOSTRDB ON)
endif()

# Core library
add_library(nostr_nip77 STATIC
  src/neg_varint.c
  src/neg_bound.c
  src/neg_fingerprint.c
  src/neg_message.c
  src/neg_session.c
)

target_compile_features(nostr_nip77 PUBLIC c_std_99)
target_compile_definitions(nostr_nip77 PRIVATE NIP77_BUILD=1)

target_include_directories(nostr_nip77
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Note: avoid linking to core 'nostr' until integration points require it,
# to prevent pulling transitive deps (e.g., websockets) into simple unit tests.

# Optional dependencies placeholder: e.g., wally for SHA-256 if configured
# Users can extend OPTIONAL_SHA256_LIB via cache or parent CMake
if(DEFINED OPTIONAL_SHA256_LIB AND NOT "${OPTIONAL_SHA256_LIB}" STREQUAL "")
  target_link_libraries(nostr_nip77 PRIVATE ${OPTIONAL_SHA256_LIB})
endif()

# Default to OpenSSL::Crypto for SHA-256 if available (project already finds OpenSSL)
find_package(OpenSSL REQUIRED)
if(OpenSSL_FOUND)
  target_link_libraries(nostr_nip77 PRIVATE OpenSSL::Crypto)
endif()

# Install headers & lib (mirror other NIPs)
install(TARGETS nostr_nip77 EXPORT nostr-nip77-targets ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)

# Optional GLib wrapper
if(WITH_NIP77_GLIB AND NOSTR_HAVE_GLIB)
  add_library(nostr_nip77_glib STATIC
    src/negentropy-glib.c
  )
  target_link_libraries(nostr_nip77_glib PUBLIC nostr_nip77)
  target_include_directories(nostr_nip77_glib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
  install(TARGETS nostr_nip77_glib EXPORT nostr-nip77-targets ARCHIVE DESTINATION lib)
endif()

# Optional nostrdb backend
if(WITH_NIP77_NOSTRDB)
  if(TARGET nostrdb)
    add_library(nostr_nip77_ndb STATIC
      backends/nostrdb/nostr-negentropy-ndb.c
    )
    target_compile_definitions(nostr_nip77_ndb PRIVATE NIP77_HAVE_NOSTRDB)
    target_link_libraries(nostr_nip77_ndb PRIVATE nostr_nip77 nostrdb)
    # Inherit nostrdb's public include directories so we can include <nostrdb.h>
    get_target_property(NDB_INCLUDE_DIRS nostrdb INTERFACE_INCLUDE_DIRECTORIES)
    if(NDB_INCLUDE_DIRS)
      target_include_directories(nostr_nip77_ndb PRIVATE ${NDB_INCLUDE_DIRS})
    endif()
    # Fallback include dirs for vendored third_party tree if the target doesn't export includes
    if(NOT NDB_INCLUDE_DIRS)
      target_include_directories(nostr_nip77_ndb PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/nostrdb
        ${CMAKE_SOURCE_DIR}/third_party/nostrdb/src)
    endif()
    target_include_directories(nostr_nip77_ndb PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:include>)
    install(TARGETS nostr_nip77_ndb EXPORT nostr-nip77-targets ARCHIVE DESTINATION lib)
  else()
    message(WARNING "WITH_NIP77_NOSTRDB=ON but target 'nostrdb' not found; skipping nostr_nip77_ndb")
  endif()
endif()

# CLI
add_executable(gnostr-neg tools/gnostr-neg.c)
target_link_libraries(gnostr-neg PRIVATE nostr_nip77)
if(WITH_NIP77_NOSTRDB AND TARGET nostr_nip77_ndb)
  target_link_libraries(gnostr-neg PRIVATE nostr_nip77_ndb)
endif()
install(TARGETS gnostr-neg RUNTIME DESTINATION bin)

# Tests
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

## Export targets omitted to avoid cross-export dependency on core targets.
