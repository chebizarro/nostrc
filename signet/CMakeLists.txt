# SPDX-License-Identifier: MIT
#
# Signet - CMake shim for monorepo builds
#
# This is intentionally minimal and follows existing apps/* patterns.
# The primary build system for signet/ is Meson, but monorepo builds use CMake.

cmake_minimum_required(VERSION 3.22)

project(signet C)

if (NOT DEFINED SIGNET_ENABLE)
  option(SIGNET_ENABLE "Build Signet NIP-46 bunker service (signetd + signetctl)" ON)
endif()

if (NOT SIGNET_ENABLE)
  message(STATUS "Signet disabled (SIGNET_ENABLE=OFF)")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if (SIGNET_ENABLE)

# Optional pkg-config deps (GLib/JSON-GLib) to match monorepo patterns
find_package(PkgConfig QUIET)
if (PKG_CONFIG_FOUND)
  pkg_check_modules(GLIB QUIET IMPORTED_TARGET glib-2.0)
  pkg_check_modules(JSON_GLIB QUIET IMPORTED_TARGET json-glib-1.0)
endif()

if (NOT TARGET OpenSSL::SSL OR NOT TARGET OpenSSL::Crypto)
  find_package(OpenSSL REQUIRED)
endif()

if (PKG_CONFIG_FOUND)
  pkg_check_modules(SQLITE3 QUIET IMPORTED_TARGET sqlite3)
  pkg_check_modules(SODIUM QUIET IMPORTED_TARGET libsodium)
  pkg_check_modules(MHD QUIET IMPORTED_TARGET libmicrohttpd)
  pkg_check_modules(CURL QUIET IMPORTED_TARGET libcurl)
endif()

set(SIGNET_CORE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/signet_config.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/store.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/audit_logger.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/replay_cache.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/key_store.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/policy_store.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/policy_engine.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/relay_pool.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/nip46_server.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mgmt_protocol.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/health_server.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/store_tokens.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/store_secrets.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/store_leases.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/store_audit.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/nostr_auth.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/capability.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap_server.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/revocation.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dbus_unix.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/bootstrap_delivery.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/dbus_tcp.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/nip5l_transport.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/session_broker.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/ssh_agent.c
)

add_library(signet_core STATIC ${SIGNET_CORE_SOURCES})
target_include_directories(signet_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/nips/nip46/include
  PRIVATE
    ${CMAKE_SOURCE_DIR}/libnostr/include
    ${CMAKE_SOURCE_DIR}/nips/nip01/include
    ${CMAKE_SOURCE_DIR}/nips/nip04/include
    ${CMAKE_SOURCE_DIR}/nips/nip17/include
    ${CMAKE_SOURCE_DIR}/nips/nip19/include
    ${CMAKE_SOURCE_DIR}/nips/nip44/include
    ${CMAKE_SOURCE_DIR}/nips/nip46/include
    ${CMAKE_SOURCE_DIR}/nips/nip11/include
)

# Link in-tree targets. NIP targets are added later in the root CMake via
# NipOptions.cmake, so use generator expressions to resolve at generate time.
target_link_libraries(signet_core PUBLIC
  nostr
  nostr_json
  $<$<TARGET_EXISTS:nip19>:nip19>
  $<$<TARGET_EXISTS:nip04>:nip04>
  $<$<TARGET_EXISTS:nip44>:nip44>
  $<$<TARGET_EXISTS:nostr_nip46_core>:nostr_nip46_core>
  $<$<TARGET_EXISTS:nip11>:nip11>
)

# External deps
target_link_libraries(signet_core PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if (TARGET PkgConfig::SQLITE3)
  target_link_libraries(signet_core PRIVATE PkgConfig::SQLITE3)
endif()

if (TARGET PkgConfig::SODIUM)
  target_link_libraries(signet_core PRIVATE PkgConfig::SODIUM)
endif()

if (TARGET PkgConfig::MHD)
  target_link_libraries(signet_core PRIVATE PkgConfig::MHD)
endif()

if (TARGET PkgConfig::CURL)
  target_compile_definitions(signet_core PRIVATE SIGNET_HAVE_CURL=1)
  target_link_libraries(signet_core PRIVATE PkgConfig::CURL)
endif()

if (TARGET PkgConfig::GLIB)
  target_link_libraries(signet_core PUBLIC PkgConfig::GLIB)
endif()

if (TARGET PkgConfig::JSON_GLIB)
  target_link_libraries(signet_core PUBLIC PkgConfig::JSON_GLIB)
endif()

add_executable(signetd ${CMAKE_CURRENT_SOURCE_DIR}/src/signetd_main.c)
target_link_libraries(signetd PRIVATE signet_core)
target_include_directories(signetd PRIVATE ${CMAKE_SOURCE_DIR}/nips/nip11/include)

add_executable(signetctl ${CMAKE_CURRENT_SOURCE_DIR}/src/signetctl_main.c)
target_link_libraries(signetctl PRIVATE signet_core)
target_include_directories(signetctl PRIVATE ${CMAKE_SOURCE_DIR}/nips/nip19/include)

install(TARGETS signetd signetctl RUNTIME DESTINATION bin)

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/signet.conf.example)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/signet.conf.example DESTINATION share/nostrc)
endif()

# Tests
if (BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()

endif() # SIGNET_ENABLE