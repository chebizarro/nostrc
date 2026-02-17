cmake_minimum_required(VERSION 3.22)
project(libmarmot C)

set(MARMOT_VERSION_MAJOR 0)
set(MARMOT_VERSION_MINOR 1)
set(MARMOT_VERSION_PATCH 0)
set(MARMOT_VERSION_STRING "${MARMOT_VERSION_MAJOR}.${MARMOT_VERSION_MINOR}.${MARMOT_VERSION_PATCH}")

# Generate version header from template
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot/marmot-version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/marmot/marmot-version.h
  @ONLY
)

# ─── Dependencies ─────────────────────────────────────────────────────────
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
pkg_check_modules(SECP256K1 REQUIRED IMPORTED_TARGET libsecp256k1)

# Optional: SQLite3 for persistent storage backend
pkg_check_modules(SQLITE3 IMPORTED_TARGET sqlite3)
if(SQLITE3_FOUND)
  message(STATUS "SQLite3 found — building SQLite storage backend")
else()
  message(STATUS "SQLite3 not found — SQLite storage backend disabled")
endif()

# Optional: LMDB for nostrdb storage backend
pkg_check_modules(LMDB IMPORTED_TARGET lmdb)

# Find libnostr and NIP-44 (sibling libraries in the nostrc tree)
# These are expected to be available via the parent CMakeLists.txt
# or via pkg-config when installed system-wide.

# ─── Source files ─────────────────────────────────────────────────────────
set(MARMOT_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot_error.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot_types.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot_storage.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/extension.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/storage_memory.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mls/mls_crypto.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mls/mls_tls.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mls/mls_tree.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mls/mls_key_schedule.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mls/mls_framing.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mls/mls_key_package.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mls/mls_group.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mls/mls_welcome.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/credentials.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/groups.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/welcome.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/messages.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/media.c
)

# Conditionally add storage backends
if(SQLITE3_FOUND)
  list(APPEND MARMOT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/storage_sqlite.c)
endif()

# nostrdb backend — compiled if nostrdb.h is available (uses __has_include)
list(APPEND MARMOT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/storage_nostrdb.c)

set(MARMOT_PUBLIC_HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot/marmot.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot/marmot-error.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot/marmot-types.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot/marmot-storage.h
  ${CMAKE_CURRENT_BINARY_DIR}/include/marmot/marmot-version.h
)

# ─── Library target ──────────────────────────────────────────────────────
add_library(marmot ${MARMOT_SOURCES})
set_target_properties(marmot PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  OUTPUT_NAME marmot
  VERSION ${MARMOT_VERSION_STRING}
  SOVERSION ${MARMOT_VERSION_MAJOR}
)

target_include_directories(marmot
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(marmot
  PUBLIC
    OpenSSL::SSL
    OpenSSL::Crypto
    PkgConfig::SODIUM
    PkgConfig::SECP256K1
    nostr
    nostr_nip44_core
)

if(SQLITE3_FOUND)
  target_link_libraries(marmot PUBLIC PkgConfig::SQLITE3)
  target_compile_definitions(marmot PRIVATE MARMOT_HAVE_SQLITE=1)
endif()

if(LMDB_FOUND)
  target_link_libraries(marmot PUBLIC PkgConfig::LMDB)
endif()

# Link against sibling libraries if available as targets
if(TARGET nostr)
  target_link_libraries(marmot PUBLIC nostr)
endif()
if(TARGET nip44)
  target_link_libraries(marmot PUBLIC nip44)
endif()

# Warn on everything for development
target_compile_options(marmot PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wno-unused-parameter>
)

# ─── pkg-config ──────────────────────────────────────────────────────────
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/marmot.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/marmot.pc
  @ONLY
)

# ─── Install ─────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS marmot
  EXPORT marmot-targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES ${MARMOT_PUBLIC_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/marmot
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/marmot.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# Commented out to allow building without export dependency issues
# install(EXPORT marmot-targets
#   FILE marmot-targets.cmake
#   NAMESPACE Marmot::
#   DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/marmot
# )

# ─── Tests ───────────────────────────────────────────────────────────────
if(BUILD_TESTING)
  enable_testing()

  # Test: TLS codec round-trip
  add_executable(test_mls_tls ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mls_tls.c)
  target_link_libraries(test_mls_tls PRIVATE marmot)
  target_include_directories(test_mls_tls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_mls_tls COMMAND test_mls_tls)

  # Test: MLS crypto primitives
  add_executable(test_mls_crypto ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mls_crypto.c)
  target_link_libraries(test_mls_crypto PRIVATE marmot)
  target_include_directories(test_mls_crypto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_mls_crypto COMMAND test_mls_crypto)

  # Test: Extension serialize/deserialize
  add_executable(test_extension ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_extension.c)
  target_link_libraries(test_extension PRIVATE marmot)
  target_include_directories(test_extension PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_extension COMMAND test_extension)

  # Test: Storage memory backend
  add_executable(test_storage ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_storage.c)
  target_link_libraries(test_storage PRIVATE marmot)
  add_test(NAME marmot_test_storage COMMAND test_storage)

  # Test: Types and lifecycle
  add_executable(test_types ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_types.c)
  target_link_libraries(test_types PRIVATE marmot)
  add_test(NAME marmot_test_types COMMAND test_types)

  # Test: MLS TreeKEM ratchet tree
  add_executable(test_mls_tree ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mls_tree.c)
  target_link_libraries(test_mls_tree PRIVATE marmot)
  target_include_directories(test_mls_tree PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_mls_tree COMMAND test_mls_tree)

  # Test: MLS key schedule
  add_executable(test_mls_key_schedule ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mls_key_schedule.c)
  target_link_libraries(test_mls_key_schedule PRIVATE marmot)
  target_include_directories(test_mls_key_schedule PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_mls_key_schedule COMMAND test_mls_key_schedule)

  # Test: MLS message framing
  add_executable(test_mls_framing ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mls_framing.c)
  target_link_libraries(test_mls_framing PRIVATE marmot)
  target_include_directories(test_mls_framing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_mls_framing COMMAND test_mls_framing)

  # Test: MLS KeyPackage
  add_executable(test_mls_key_package ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mls_key_package.c)
  target_link_libraries(test_mls_key_package PRIVATE marmot)
  target_include_directories(test_mls_key_package PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_mls_key_package COMMAND test_mls_key_package)

  # Test: MLS Group state machine
  add_executable(test_mls_group ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mls_group.c)
  target_link_libraries(test_mls_group PRIVATE marmot)
  target_include_directories(test_mls_group PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_mls_group COMMAND test_mls_group)

  # Test: MLS Welcome
  add_executable(test_mls_welcome ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mls_welcome.c)
  target_link_libraries(test_mls_welcome PRIVATE marmot)
  target_include_directories(test_mls_welcome PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_mls_welcome COMMAND test_mls_welcome)

  # Test: Protocol layer (MIP-00 through MIP-03)
  add_executable(test_protocol ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_protocol.c)
  target_link_libraries(test_protocol PRIVATE marmot)
  target_include_directories(test_protocol PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_protocol COMMAND test_protocol)

  # Test: Storage contract tests (all backends)
  add_executable(test_storage_contract ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_storage_contract.c)
  target_link_libraries(test_storage_contract PRIVATE marmot)
  target_include_directories(test_storage_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_storage_contract COMMAND test_storage_contract)

  # Test: MIP-04 media encryption
  add_executable(test_media ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_media.c)
  target_link_libraries(test_media PRIVATE marmot)
  target_include_directories(test_media PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_media COMMAND test_media)

  # Test: RFC 9420 test vectors & crypto validation
  add_executable(test_rfc9420_vectors ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rfc9420_vectors.c)
  target_link_libraries(test_rfc9420_vectors PRIVATE marmot)
  target_include_directories(test_rfc9420_vectors PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_rfc9420_vectors COMMAND test_rfc9420_vectors)

  # Test: MDK interoperability
  add_executable(test_marmot_interop 
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_interop.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/mdk_vector_loader.c
  )
  target_link_libraries(test_marmot_interop PRIVATE marmot ${OPENSSL_LIBRARIES} PkgConfig::SODIUM)
  if(TARGET secp256k1)
    target_link_libraries(test_marmot_interop PRIVATE secp256k1)
  endif()
  target_include_directories(test_marmot_interop PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )
  add_test(NAME marmot_test_interop COMMAND test_marmot_interop)
endif()
