cmake_minimum_required(VERSION 3.20)

set(NIP5F_INC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(NIP5F_CORE_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/sock_server.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/sock_client.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/sock_handlers_builtin.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/sock_framing.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/sock_internal.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/sock_conn.c
)

set(NIP5F_GLIB_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/sock_server_g.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/sock_client_g.c
)

add_library(nostr_nip5f_core ${NIP5F_CORE_SRC})
target_include_directories(nostr_nip5f_core PUBLIC ${NIP5F_INC}
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_BINARY_DIR}/libnostr/generated
)
# Link libnostr core, json, and nip44 core helpers
find_package(OpenSSL REQUIRED)
find_library(SECP256K1_LIB NAMES secp256k1 REQUIRED)
find_package(Threads REQUIRED)
target_include_directories(nostr_nip5f_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core)
target_include_directories(nostr_nip5f_core PUBLIC ${CMAKE_SOURCE_DIR}/nips/nip19/include)
target_link_libraries(nostr_nip5f_core PUBLIC nostr nostr_json nostr_nip44_core nip19 OpenSSL::Crypto ${SECP256K1_LIB} Threads::Threads)

# GLib (optional)
find_package(PkgConfig QUIET)
if (PkgConfig)
  pkg_check_modules(GIO QUIET IMPORTED_TARGET gio-2.0>=2.50 gobject-2.0>=2.50)
endif()

if (GIO_FOUND)
  add_library(nostr_nip5f_glib ${NIP5F_GLIB_SRC})
  target_include_directories(nostr_nip5f_glib PUBLIC ${NIP5F_INC})
  target_link_libraries(nostr_nip5f_glib PUBLIC nostr_nip5f_core PkgConfig::GIO)
endif()

add_executable(nostr-signer-sockd ${CMAKE_CURRENT_SOURCE_DIR}/src/core/sockd_main.c)
target_link_libraries(nostr-signer-sockd PRIVATE nostr_nip5f_core)

# Tests
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip5f_loopback.c)
  add_executable(test_nip5f_loopback ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip5f_loopback.c)
  target_link_libraries(test_nip5f_loopback PRIVATE nostr_nip5f_core nostr nostr_json)
  add_test(NAME test_nip5f_loopback COMMAND test_nip5f_loopback)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip5f_negative_env.c)
  add_executable(test_nip5f_negative_env ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip5f_negative_env.c)
  target_link_libraries(test_nip5f_negative_env PRIVATE nostr_nip5f_core nostr nostr_json)
  add_test(NAME test_nip5f_negative_env COMMAND test_nip5f_negative_env)
endif()

# Examples
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/nip5f_client_example.c)
  add_executable(nip5f_client_example ${CMAKE_CURRENT_SOURCE_DIR}/examples/nip5f_client_example.c)
  target_link_libraries(nip5f_client_example PRIVATE nostr_nip5f_core nostr nostr_json)
endif()

install(TARGETS nostr_nip5f_core nostr-signer-sockd)
if (TARGET nostr_nip5f_glib)
  install(TARGETS nostr_nip5f_glib)
endif()
install(DIRECTORY ${NIP5F_INC}/ DESTINATION include/nostr/nip5f)
