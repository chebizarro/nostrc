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

set(SIGNET_CORE_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/signet_config.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/audit_logger.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/replay_cache.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/key_store.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/policy_store.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/policy_engine.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/relay_pool.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/nip46_server.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mgmt_protocol.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/health_server.c
)

add_library(signet_core STATIC ${SIGNET_CORE_SOURCES})
target_include_directories(signet_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE
    ${CMAKE_SOURCE_DIR}/libnostr/include
    ${CMAKE_SOURCE_DIR}/nips/nip01/include
    ${CMAKE_SOURCE_DIR}/nips/nip04/include
    ${CMAKE_SOURCE_DIR}/nips/nip44/include
    ${CMAKE_SOURCE_DIR}/nips/nip46/include
)

# Link in-tree targets when available
if (TARGET nostr)
  target_link_libraries(signet_core PUBLIC nostr)
endif()

if (TARGET nostr_json)
  target_link_libraries(signet_core PUBLIC nostr_json)
endif()

if (TARGET nip46)
  target_link_libraries(signet_core PUBLIC nip46)
endif()

# External deps
target_link_libraries(signet_core PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if (TARGET PkgConfig::GLIB)
  target_link_libraries(signet_core PRIVATE PkgConfig::GLIB)
endif()

if (TARGET PkgConfig::JSON_GLIB)
  target_link_libraries(signet_core PRIVATE PkgConfig::JSON_GLIB)
endif()

add_executable(signetd ${CMAKE_CURRENT_SOURCE_DIR}/src/signetd_main.c)
target_link_libraries(signetd PRIVATE signet_core)

add_executable(signetctl ${CMAKE_CURRENT_SOURCE_DIR}/src/signetctl_main.c)
target_link_libraries(signetctl PRIVATE signet_core)

install(TARGETS signetd signetctl RUNTIME DESTINATION bin)

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/signet.toml.example)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/signet.toml.example DESTINATION share/nostrc)
endif()

# Tests
if (BUILD_TESTING)
  enable_testing()
  add_subdirectory(tests)
endif()

endif() # SIGNET_ENABLE