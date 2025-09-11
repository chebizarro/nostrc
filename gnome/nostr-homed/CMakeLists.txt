cmake_minimum_required(VERSION 3.18)
project(nostr_homed C)

# Compiler flags
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror)

# Dependencies
find_package(PkgConfig REQUIRED)
pkg_check_modules(JANSSON REQUIRED jansson)
pkg_check_modules(GLIB2 REQUIRED glib-2.0>=2.56 gobject-2.0>=2.56 gio-2.0>=2.56)
find_package(OpenSSL REQUIRED)
find_library(SECP256K1_LIB NAMES secp256k1 REQUIRED)
find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)

# FUSE3
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(FUSE3 QUIET)

# Optional libseccomp
find_package(Libseccomp)

# Include dirs
include_directories(
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_SOURCE_DIR}/libjson/include
  ${CMAKE_SOURCE_DIR}/libgo/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${JANSSON_INCLUDE_DIRS}
  ${GLIB2_INCLUDE_DIRS}
  ${SQLite3_INCLUDE_DIRS}
  ${FUSE3_INCLUDE_DIRS}
)

link_directories(
  ${JANSSON_LIBRARY_DIRS}
  ${GLIB2_LIBRARY_DIRS}
)

# Common sources
set(NH_COMMON
  src/common/nostr_manifest.c
  src/common/nostr_cache.c
  src/common/relay_fetch.c
  src/common/blossom_client.c
  src/common/secrets_tmpfs.c
  src/common/secrets_decrypt.c
  src/common/nip46_client_dbus.c
  src/common/util.c
)

add_library(nostr_homed_common STATIC ${NH_COMMON})
target_link_libraries(nostr_homed_common PUBLIC
  libnostr nostr_json libgo
  ${JANSSON_LIBRARIES}
  ${GLIB2_LIBRARIES}
  OpenSSL::Crypto
  ${SECP256K1_LIB}
  SQLite::SQLite3
  CURL::libcurl
  Threads::Threads
)

if (UNIX AND NOT APPLE)
  # NSS module (Linux)
  add_library(nss_nostr SHARED src/nss/nss_nostr.c)
  set_target_properties(nss_nostr PROPERTIES OUTPUT_NAME "nss_nostr" SOVERSION 2)
  target_link_libraries(nss_nostr PRIVATE nostr_homed_common ${GLIB2_LIBRARIES} SQLite::SQLite3)

  # PAM module (Linux)
  add_library(pam_nostr SHARED src/pam/pam_nostr.c)
  set_target_properties(pam_nostr PROPERTIES OUTPUT_NAME "pam_nostr")
  target_link_libraries(pam_nostr PRIVATE nostr_homed_common ${GLIB2_LIBRARIES})
endif()

# FUSE filesystem
if (FUSE3_FOUND)
  add_executable(nostrfs src/fs/nostrfs.c)
  target_link_libraries(nostrfs PRIVATE nostr_homed_common ${FUSE3_LIBRARIES})
  install(TARGETS nostrfs RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
  message(WARNING "FUSE3 not found; skipping nostrfs build (required on target distros)")
endif()

# Control CLI + DBus service
add_executable(nostr-homectl src/ctl/nostr-homectl.c)
target_link_libraries(nostr-homectl PRIVATE nostr_homed_common ${GLIB2_LIBRARIES} ${GLIB2_GIO_LIBRARIES})

# Install rules (distro libdirs vary; default to lib)
include(GNUInstallDirs)
if (TARGET nss_nostr)
  install(TARGETS nss_nostr LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
if (TARGET pam_nostr)
  install(TARGETS pam_nostr LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/security)
endif()
install(TARGETS nostr-homectl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# DBus and systemd files
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/dbus/org.nostr.Homed1.xml)
  install(FILES dbus/org.nostr.Homed1.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/interfaces)
endif()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/systemd/nostr-homectl.service)
  install(FILES systemd/nostr-homectl.service DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
endif()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/systemd/nostrfs@.service)
  install(FILES systemd/nostrfs@.service DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
endif()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/systemd/nostr-homed.target)
  install(FILES systemd/nostr-homed.target DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)
endif()

# Tests
enable_testing()
add_executable(test_manifest tests/unit/test_manifest.c src/common/nostr_manifest.c src/common/util.c)
target_link_libraries(test_manifest PRIVATE ${JANSSON_LIBRARIES})
add_executable(test_cache tests/unit/test_cache.c src/common/nostr_cache.c src/common/util.c)
target_link_libraries(test_cache PRIVATE SQLite::SQLite3 OpenSSL::Crypto)
add_test(NAME test_manifest COMMAND test_manifest)
add_test(NAME test_cache COMMAND test_cache)
add_executable(test_cache_config tests/unit/test_cache_config.c src/common/nostr_cache.c src/common/util.c)
target_link_libraries(test_cache_config PRIVATE SQLite::SQLite3 OpenSSL::Crypto)
add_test(NAME test_cache_config COMMAND test_cache_config)

# Integration test for nostrfs (requires FUSE3 and privileged/container env)
if (FUSE3_FOUND)
  add_test(NAME nostrfs_integration
           COMMAND bash ${CMAKE_SOURCE_DIR}/gnome/nostr-homed/tests/integration/run_nostrfs_basic.sh)
  set_tests_properties(nostrfs_integration PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

# Mock signer and secrets decrypt integration test
add_executable(mock_signer tests/integration/mock_signer.c)
target_link_libraries(mock_signer PRIVATE ${GLIB2_LIBRARIES} ${GLIB2_GIO_LIBRARIES})

add_executable(test_secrets_decrypt tests/integration/test_secrets_decrypt.c)
target_link_libraries(test_secrets_decrypt PRIVATE nostr_homed_common ${GLIB2_LIBRARIES} ${GLIB2_GIO_LIBRARIES})

add_test(NAME secrets_decrypt_integration
         COMMAND bash ${CMAKE_SOURCE_DIR}/gnome/nostr-homed/tests/integration/run_mock_signer_test.sh)
set_tests_properties(secrets_decrypt_integration PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# Packaging pkg-config where useful
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/nostr-homed.pc.in ${CMAKE_CURRENT_BINARY_DIR}/nostr-homed.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nostr-homed.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
