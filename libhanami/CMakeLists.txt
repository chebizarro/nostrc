cmake_minimum_required(VERSION 3.22)
project(libhanami C)

# =============================================================================
# libhanami: Blossom filesystem extension & tools for libgit2
#
# Provides custom libgit2 backends for storing Git objects on Blossom servers
# and coordinating refs via Nostr (NIP-34) events.
# =============================================================================

include(GNUInstallDirs)

# --- Dependencies ---

# libgit2 (required)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)

# libcurl (required — Blossom HTTP client)
find_package(CURL REQUIRED)

# SQLite3 (required — default index backend)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

# OpenSSL (required — SHA-1/SHA-256 computation, inherited from parent)
# OpenSSL::Crypto is already available from the top-level find_package(OpenSSL)

# --- Optional LMDB index backend ---
option(HANAMI_ENABLE_LMDB "Enable optional LMDB index backend" OFF)
set(HANAMI_HAVE_LMDB FALSE)
if(HANAMI_ENABLE_LMDB)
  set(_NDB_LMDB_DIR "${CMAKE_SOURCE_DIR}/third_party/nostrdb/deps/lmdb")
  if(EXISTS "${_NDB_LMDB_DIR}/lmdb.h")
    message(STATUS "libhanami: Found LMDB in nostrdb vendor (${_NDB_LMDB_DIR})")
    set(HANAMI_HAVE_LMDB TRUE)
    set(HANAMI_LMDB_INCLUDE_DIR "${_NDB_LMDB_DIR}")
  else()
    pkg_check_modules(LMDB QUIET IMPORTED_TARGET lmdb)
    if(LMDB_FOUND)
      message(STATUS "libhanami: Found system LMDB")
      set(HANAMI_HAVE_LMDB TRUE)
    else()
      message(STATUS "libhanami: LMDB not found — using SQLite-only index")
    endif()
  endif()
endif()

# --- Source files ---

set(HANAMI_SOURCES
  src/hanami.c
  # Future sources will be added here as components are implemented:
  src/hanami-index.c
  src/hanami-bud02-auth.c
  src/hanami-blossom-client.c
  src/hanami-odb-backend.c
  src/hanami-refdb-backend.c
  src/hanami-nostr.c
  src/hanami-transport.c
  # src/hanami-config.c
)

# --- Library target ---

add_library(hanami ${HANAMI_SOURCES})
add_library(libhanami ALIAS hanami)

target_compile_features(hanami PRIVATE c_std_11)
target_compile_options(hanami PRIVATE -Wall -Wextra -Wpedantic)

target_include_directories(hanami
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(hanami
  PUBLIC
    PkgConfig::LIBGIT2
  PRIVATE
    nostr              # libnostr for Nostr event/relay operations
    CURL::libcurl
    PkgConfig::SQLITE3
    OpenSSL::Crypto
)

# NIP-34 (required — Git collaboration protocol)
if(NOT TARGET nip34)
  message(FATAL_ERROR "libhanami requires nip34. Set ENABLE_NIP34=ON or BUILD_LIBHANAMI=OFF.")
endif()
target_link_libraries(hanami PRIVATE nip34)
target_compile_definitions(hanami PRIVATE HANAMI_HAVE_NIP34=1)

# NIP-98 (optional — HTTP auth, useful but not required)
if(TARGET nip98)
  target_link_libraries(hanami PRIVATE nip98)
  target_compile_definitions(hanami PRIVATE HANAMI_HAVE_NIP98=1)
endif()

# Optional LMDB backend
if(HANAMI_HAVE_LMDB)
  target_compile_definitions(hanami PRIVATE HANAMI_HAVE_LMDB=1)
  if(HANAMI_LMDB_INCLUDE_DIR)
    target_include_directories(hanami PRIVATE ${HANAMI_LMDB_INCLUDE_DIR})
  endif()
  if(TARGET PkgConfig::LMDB)
    target_link_libraries(hanami PRIVATE PkgConfig::LMDB)
  endif()
else()
  target_compile_definitions(hanami PRIVATE HANAMI_HAVE_LMDB=0)
endif()

# Sanitizer support
if(COMMAND apply_sanitizers)
  apply_sanitizers(hanami)
endif()

# --- pkg-config ---

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/hanami.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/hanami.pc
  @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/hanami.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# --- Install ---

install(TARGETS hanami
  EXPORT hanamiTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/hanami/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/hanami)

# --- Tests ---

if(BUILD_TESTING)
  add_executable(test-hanami-types tests/test_hanami_types.c)
  target_compile_features(test-hanami-types PRIVATE c_std_11)
  target_link_libraries(test-hanami-types PRIVATE hanami)
  add_test(NAME test-hanami-types COMMAND test-hanami-types)
  set_tests_properties(test-hanami-types PROPERTIES
    TIMEOUT 30
    LABELS "libhanami;types;unit"
  )

  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-types)
  endif()

  add_executable(test-hanami-index tests/test_hanami_index.c)
  target_compile_features(test-hanami-index PRIVATE c_std_11)
  target_link_libraries(test-hanami-index PRIVATE hanami PkgConfig::SQLITE3)
  add_test(NAME test-hanami-index COMMAND test-hanami-index)
  set_tests_properties(test-hanami-index PROPERTIES
    TIMEOUT 30
    LABELS "libhanami;index;unit"
  )

  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-index)
  endif()

  add_executable(test-hanami-bud02-auth tests/test_hanami_bud02_auth.c)
  target_compile_features(test-hanami-bud02-auth PRIVATE c_std_11)
  target_link_libraries(test-hanami-bud02-auth PRIVATE hanami nostr)
  add_test(NAME test-hanami-bud02-auth COMMAND test-hanami-bud02-auth)
  set_tests_properties(test-hanami-bud02-auth PROPERTIES
    TIMEOUT 30
    LABELS "libhanami;bud02;unit"
  )

  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-bud02-auth)
  endif()

  add_executable(test-hanami-blossom-client tests/test_hanami_blossom_client.c)
  target_compile_features(test-hanami-blossom-client PRIVATE c_std_11)
  target_link_libraries(test-hanami-blossom-client PRIVATE hanami)
  add_test(NAME test-hanami-blossom-client COMMAND test-hanami-blossom-client)
  set_tests_properties(test-hanami-blossom-client PROPERTIES
    TIMEOUT 30
    LABELS "libhanami;blossom;unit"
  )

  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-blossom-client)
  endif()

  add_executable(test-hanami-odb-backend tests/test_hanami_odb_backend.c)
  target_compile_features(test-hanami-odb-backend PRIVATE c_std_11)
  target_link_libraries(test-hanami-odb-backend PRIVATE hanami PkgConfig::LIBGIT2 PkgConfig::SQLITE3)
  add_test(NAME test-hanami-odb-backend COMMAND test-hanami-odb-backend)
  set_tests_properties(test-hanami-odb-backend PROPERTIES
    TIMEOUT 30
    LABELS "libhanami;odb;unit"
  )

  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-odb-backend)
  endif()

  add_executable(test-hanami-nostr tests/test_hanami_nostr.c)
  target_compile_features(test-hanami-nostr PRIVATE c_std_11)
  target_link_libraries(test-hanami-nostr PRIVATE hanami nostr nip34 PkgConfig::LIBGIT2)
  add_test(NAME test-hanami-nostr COMMAND test-hanami-nostr)
  set_tests_properties(test-hanami-nostr PROPERTIES
    TIMEOUT 30
    LABELS "libhanami;nostr;unit"
  )

  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-nostr)
  endif()

  add_executable(test-hanami-refdb-backend tests/test_hanami_refdb_backend.c)
  target_compile_features(test-hanami-refdb-backend PRIVATE c_std_11)
  target_link_libraries(test-hanami-refdb-backend PRIVATE hanami nostr nip34 PkgConfig::LIBGIT2)
  add_test(NAME test-hanami-refdb-backend COMMAND test-hanami-refdb-backend)
  set_tests_properties(test-hanami-refdb-backend PROPERTIES
    TIMEOUT 30
    LABELS "libhanami;refdb;unit"
  )

  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-refdb-backend)
  endif()

  add_executable(test-hanami-transport tests/test_hanami_transport.c)
  target_compile_features(test-hanami-transport PRIVATE c_std_11)
  target_link_libraries(test-hanami-transport PRIVATE hanami nostr nip34 PkgConfig::LIBGIT2)
  add_test(NAME test-hanami-transport COMMAND test-hanami-transport)
  set_tests_properties(test-hanami-transport PROPERTIES
    TIMEOUT 30
    LABELS "libhanami;transport;unit"
  )

  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-transport)
  endif()
endif()
