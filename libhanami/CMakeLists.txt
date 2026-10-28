cmake_minimum_required(VERSION 3.22)
project(libhanami C)

# =============================================================================
# libhanami: Blossom filesystem extension & tools for libgit2
#
# Provides custom libgit2 backends for storing Git objects on Blossom servers
# and coordinating refs via Nostr (NIP-34) events.
# =============================================================================

include(GNUInstallDirs)

# B1 (packaging plan): declare a component version so apply_versioning() can
# stamp VERSION + SOVERSION on the installed shared library.
if(NOT COMMAND declare_component_version)
  include("${CMAKE_CURRENT_LIST_DIR}/../cmake/VersionHelpers.cmake")
endif()
option(ENABLE_COMPONENT_VERSIONING "Enable generated component version headers" ON)
set(HANAMI_VERSION_MAJOR 0)
set(HANAMI_VERSION_MINOR 8)
set(HANAMI_VERSION_MICRO 0)
declare_component_version(HANAMI
  ${HANAMI_VERSION_MAJOR}
  ${HANAMI_VERSION_MINOR}
  ${HANAMI_VERSION_MICRO})
set(HANAMI_VERSION_STRING "${HANAMI_VERSION}")

# --- Dependencies ---
if(NOT TARGET nostrc::blossom)
  message(FATAL_ERROR "libhanami requires libblossom. Set BUILD_LIBBLOSSOM=ON or BUILD_LIBHANAMI=OFF.")
endif()

if(NOT TARGET nostrc::porthome)
  message(FATAL_ERROR "libhanami requires libporthome. Set BUILD_LIBPORTHOME=ON or BUILD_LIBHANAMI=OFF.")
endif()

if(NOT TARGET nostrc::vault)
  message(FATAL_ERROR "libhanami requires libvault. Set BUILD_LIBVAULT=ON or BUILD_LIBHANAMI=OFF.")
endif()

# libgit2 (required)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)

# SQLite3 (required — default index backend)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(HANAMI_JANSSON REQUIRED IMPORTED_TARGET jansson)

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
  src/hanami-backend-owner.c
  src/hanami-sealed.c
  src/hanami-index-sqlite.c
  src/hanami-odb-backend.c
  src/hanami-refdb-backend.c
  src/hanami-nostr.c
  src/hanami-transport.c
  src/hanami-config.c
  src/hanami-grasp.c
  src/hanami-discovery.c
  src/hanami-manifest.c
  src/hanami-fetch.c
)

# --- Library target ---

add_library(hanami ${HANAMI_SOURCES})
add_library(libhanami ALIAS hanami)
add_library(nostrc::hanami ALIAS hanami)

# Bridge component version vars (declare_component_version stored them under the
# HANAMI_ prefix; the target is lowercase `hanami`) and apply VERSION + SOVERSION
# (B1). The generated HANAMI-version.h is installed into include/hanami/.
set(hanami_VERSION   "${HANAMI_VERSION}")
set(hanami_SOVERSION "0.${HANAMI_VERSION_MINOR}")
set_target_properties(hanami PROPERTIES
  COMPONENT_VERSION_HEADER "${HANAMI_VERSION_HEADER}"
  COMPONENT_VERSION_HEADER_DIR "${HANAMI_VERSION_HEADER_DIR}"
  COMPONENT_VERSION_HEADER_INSTALL_DIR
    "${CMAKE_INSTALL_INCLUDEDIR}/hanami")
apply_versioning(hanami)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/NostrcVisibility.cmake")
nostrc_library_visibility(hanami HANAMI_API hanami-export.h "${CMAKE_INSTALL_INCLUDEDIR}/hanami")

target_compile_features(hanami PRIVATE c_std_11)
target_compile_definitions(hanami PRIVATE
  HANAMI_VERSION_MAJOR=${HANAMI_VERSION_MAJOR}
  HANAMI_VERSION_MINOR=${HANAMI_VERSION_MINOR}
  HANAMI_VERSION_PATCH=${HANAMI_VERSION_MICRO}
  HANAMI_VERSION_STRING="${HANAMI_VERSION_STRING}")
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
    nostrc::blossom
    nostrc::porthome
  PRIVATE
    nostrc::vault
    PkgConfig::HANAMI_JANSSON
    nostr              # libnostr for Nostr event/relay operations
    PkgConfig::SQLITE3
    OpenSSL::Crypto
    pthread            # Serialize synchronous NIP-34 operations
)

# NIP-34 (required — Git collaboration protocol)
if(NOT TARGET nip34)
  message(FATAL_ERROR "libhanami requires nip34. Set ENABLE_NIP34=ON or BUILD_LIBHANAMI=OFF.")
endif()
target_link_libraries(hanami PRIVATE nip34)

# GRASP capability parsing and bounded HTTP preflight.
if(NOT TARGET nip11)
  message(FATAL_ERROR "libhanami requires nip11. Set ENABLE_NIP11=ON or BUILD_LIBHANAMI=OFF.")
endif()
find_package(CURL REQUIRED)
target_link_libraries(hanami PRIVATE nip11 CURL::libcurl)
target_compile_definitions(hanami PRIVATE HANAMI_HAVE_NIP34=1)

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

set(HANAMI_PKG_VERSION "${HANAMI_VERSION}")
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

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_package(CURL REQUIRED)
  add_executable(test_hanami_sealed_roundtrip tests/test_hanami_sealed_roundtrip.c)
  target_compile_options(test_hanami_sealed_roundtrip PRIVATE -Wall -Wextra -UNDEBUG)
  target_link_libraries(test_hanami_sealed_roundtrip PRIVATE hanami nostr nip34
    PkgConfig::HANAMI_JANSSON CURL::libcurl)
  add_test(NAME test_hanami_sealed_roundtrip
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/sealed_fixture.py"
            $<TARGET_FILE:test_hanami_sealed_roundtrip>)
  set_tests_properties(test_hanami_sealed_roundtrip PROPERTIES
    TIMEOUT 180 LABELS "libhanami;sealed;integration")
  if(COMMAND apply_sanitizers)
    apply_sanitizers(test_hanami_sealed_roundtrip)
  endif()
  add_executable(test-hanami-clone tests/test_hanami_clone.c)
  target_compile_options(test-hanami-clone PRIVATE -Wall -Wextra -UNDEBUG)
  target_link_libraries(test-hanami-clone PRIVATE hanami nostr nip34
    PkgConfig::LIBGIT2 PkgConfig::HANAMI_JANSSON PkgConfig::SQLITE3 CURL::libcurl)
  add_test(NAME test-hanami-clone
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/clone_fixture.py"
            $<TARGET_FILE:test-hanami-clone> discovery seed clone seed2 fetch pushfail checkpoint republish negative
            relayfail outbox unreachable)
  set_tests_properties(test-hanami-clone PROPERTIES
    TIMEOUT 180 LABELS "libhanami;clone;integration")
  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-clone)
  endif()
  add_executable(test-hanami-nostr-protocol tests/test_hanami_nostr_protocol.c)
  target_link_libraries(test-hanami-nostr-protocol PRIVATE hanami nostr nip34)
  add_test(NAME test-hanami-nostr-protocol
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/relay_fixture.py"
            $<TARGET_FILE:test-hanami-nostr-protocol>)
  set_tests_properties(test-hanami-nostr-protocol PROPERTIES
    TIMEOUT 90 LABELS "libhanami;nostr;integration")
  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-nostr-protocol)
  endif()

  add_executable(test-hanami-grasp-workflow tests/test_hanami_grasp_workflow.c)
  target_compile_options(test-hanami-grasp-workflow PRIVATE -Wall -Wextra -UNDEBUG)
  target_link_libraries(test-hanami-grasp-workflow PRIVATE hanami nostr nip34 PkgConfig::LIBGIT2)
  add_test(NAME test-hanami-grasp-workflow
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/grasp_fixture.py"
            $<TARGET_FILE:test-hanami-grasp-workflow>)
  set_tests_properties(test-hanami-grasp-workflow PROPERTIES
    TIMEOUT 180 LABELS "libhanami;grasp;integration")
  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-grasp-workflow)
  endif()

  add_executable(test-hanami-grasp-fetch-workflow tests/test_hanami_grasp_fetch_workflow.c)
  target_compile_options(test-hanami-grasp-fetch-workflow PRIVATE -Wall -Wextra -UNDEBUG)
  target_link_libraries(test-hanami-grasp-fetch-workflow PRIVATE hanami nostr nip34 PkgConfig::LIBGIT2)
  add_test(NAME test-hanami-grasp-fetch-workflow
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/grasp_fetch_fixture.py"
            $<TARGET_FILE:test-hanami-grasp-fetch-workflow>)
  set_tests_properties(test-hanami-grasp-fetch-workflow PROPERTIES
    TIMEOUT 300 LABELS "libhanami;grasp;integration")
  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-grasp-fetch-workflow)
  endif()

  add_executable(test-hanami-refdb-backend tests/test_hanami_refdb_backend.c)
  target_compile_features(test-hanami-refdb-backend PRIVATE c_std_11)
  target_link_libraries(test-hanami-refdb-backend PRIVATE hanami nostr nip34 PkgConfig::LIBGIT2)
  add_test(NAME test-hanami-refdb-backend
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/relay_fixture.py"
            $<TARGET_FILE:test-hanami-refdb-backend> refdb)
  set_tests_properties(test-hanami-refdb-backend PROPERTIES
    TIMEOUT 30
    LABELS "libhanami;refdb;unit"
  )

  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-refdb-backend)
  endif()

  add_executable(test-hanami-refdb-acceptance tests/test_hanami_refdb_acceptance.c)
  target_compile_options(test-hanami-refdb-acceptance PRIVATE -Wall -Wextra -UNDEBUG)
  target_link_libraries(test-hanami-refdb-acceptance PRIVATE hanami nostr nip34 PkgConfig::LIBGIT2)
  add_test(NAME test-hanami-refdb-acceptance
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/relay_fixture.py"
            $<TARGET_FILE:test-hanami-refdb-acceptance> refdb-acceptance)
  set_tests_properties(test-hanami-refdb-acceptance PROPERTIES
    TIMEOUT 30 LABELS "libhanami;refdb;integration")
  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-refdb-acceptance)
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

  add_executable(test-hanami-config tests/test_hanami_config.c)
  target_compile_features(test-hanami-config PRIVATE c_std_11)
  target_link_libraries(test-hanami-config PRIVATE hanami PkgConfig::LIBGIT2)
  add_test(NAME test-hanami-config COMMAND test-hanami-config)
  set_tests_properties(test-hanami-config PROPERTIES
    TIMEOUT 30
    LABELS "libhanami;config;unit"
  )

  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-config)
  endif()

  add_executable(test-hanami-integration tests/test_hanami_integration.c)
  target_compile_features(test-hanami-integration PRIVATE c_std_11)
  target_link_libraries(test-hanami-integration PRIVATE hanami nostr nip34 PkgConfig::LIBGIT2 PkgConfig::SQLITE3)
  add_test(NAME test-hanami-integration
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/relay_fixture.py"
            $<TARGET_FILE:test-hanami-integration> integration)
  set_tests_properties(test-hanami-integration PROPERTIES
    TIMEOUT 60
    LABELS "libhanami;integration"
  )

  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-integration)
  endif()

  add_executable(test-hanami-grasp tests/test_hanami_grasp.c)
  target_compile_features(test-hanami-grasp PRIVATE c_std_11)
  target_link_libraries(test-hanami-grasp PRIVATE hanami nostr nip34 PkgConfig::LIBGIT2)
  add_test(NAME test-hanami-grasp COMMAND test-hanami-grasp)
  set_tests_properties(test-hanami-grasp PROPERTIES
    TIMEOUT 30
    LABELS "libhanami;grasp;unit"
  )

  if(COMMAND apply_sanitizers)
    apply_sanitizers(test-hanami-grasp)
  endif()
endif()
