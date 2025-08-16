cmake_minimum_required(VERSION 3.30)
project(nip44)

set(NIP44_INC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(NIP44_CORE_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip44.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip44_convkey.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip44_padding.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip44_hkdf_hmac.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip44_chacha.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip44_base64.c
)

add_library(nostr_nip44_core ${NIP44_CORE_SRC})
target_include_directories(nostr_nip44_core PUBLIC ${NIP44_INC})
find_package(OpenSSL REQUIRED)
find_library(SECP256K1_LIB NAMES secp256k1 REQUIRED)
target_link_libraries(nostr_nip44_core PUBLIC nostr OpenSSL::Crypto ${SECP256K1_LIB})

# Optional GLib layer
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB QUIET IMPORTED_TARGET glib-2.0>=2.50 gobject-2.0>=2.50)
if (GLIB_FOUND)
  set(NIP44_GLIB_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/nip44_g.c)
  add_library(nostr_nip44_glib ${NIP44_GLIB_SRC})
  target_include_directories(nostr_nip44_glib PUBLIC ${NIP44_INC})
  target_link_libraries(nostr_nip44_glib PUBLIC nostr nostr_nip44_core PkgConfig::GLIB)
endif()

# Install only the public headers under include/nostr/nip44
install(DIRECTORY ${NIP44_INC}/nostr/nip44/ DESTINATION include/nostr/nip44)

# Tests
enable_testing()
add_executable(test_nip44_roundtrip ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip44_roundtrip.c)
target_include_directories(test_nip44_roundtrip PRIVATE ${NIP44_INC})
target_link_libraries(test_nip44_roundtrip PRIVATE nostr nostr_nip44_core OpenSSL::Crypto)
add_test(NAME test_nip44_roundtrip COMMAND test_nip44_roundtrip)

add_executable(test_nip44_convkey ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip44_convkey.c)
target_include_directories(test_nip44_convkey PRIVATE ${NIP44_INC})
target_link_libraries(test_nip44_convkey PRIVATE nostr nostr_nip44_core OpenSSL::Crypto)
add_test(NAME test_nip44_convkey COMMAND test_nip44_convkey)

add_executable(test_nip44_vectors ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip44_vectors.c)
target_include_directories(test_nip44_vectors PRIVATE ${NIP44_INC})
target_link_libraries(test_nip44_vectors PRIVATE nostr nostr_nip44_core OpenSSL::Crypto)

# Optional JSON vectors support via Jansson
pkg_check_modules(JANSSON QUIET IMPORTED_TARGET jansson)
if (JANSSON_FOUND)
  target_link_libraries(test_nip44_vectors PRIVATE PkgConfig::JANSSON)
  target_compile_definitions(test_nip44_vectors PRIVATE VECTORS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/nip44.vectors.json")
else()
  message(STATUS "Jansson not found; test_nip44_vectors will run built-in single vector only")
endif()

add_test(NAME test_nip44_vectors COMMAND test_nip44_vectors)

# Examples
add_executable(nip44_demo_encrypt ${CMAKE_CURRENT_SOURCE_DIR}/examples/nip44_demo_encrypt.c)
target_include_directories(nip44_demo_encrypt PRIVATE ${NIP44_INC})
target_link_libraries(nip44_demo_encrypt PRIVATE nostr nostr_nip44_core OpenSSL::Crypto)

add_executable(nip44_vector_demo ${CMAKE_CURRENT_SOURCE_DIR}/examples/nip44_vector_demo.c)
target_include_directories(nip44_vector_demo PRIVATE ${NIP44_INC})
target_link_libraries(nip44_vector_demo PRIVATE nostr nostr_nip44_core OpenSSL::Crypto)
