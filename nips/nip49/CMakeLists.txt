
project(nip49)

cmake_minimum_required(VERSION 3.30)

set(NIP49_INC ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(NIP49_CORE_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip49.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip49_kdf.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip49_aead.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/core/nip49_bech.c
)

set(NIP49_GLIB_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/src/glib/nip49_g.c
)

add_library(nostr_nip49_core ${NIP49_CORE_SRC})
target_include_directories(nostr_nip49_core PUBLIC ${NIP49_INC}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../nip19/include)
find_path(SODIUM_INCLUDE_DIR sodium.h)
if (SODIUM_INCLUDE_DIR)
  target_include_directories(nostr_nip49_core PRIVATE ${SODIUM_INCLUDE_DIR})
endif()
find_library(SODIUM_LIB sodium)
if (SODIUM_LIB)
  target_link_libraries(nostr_nip49_core PUBLIC nostr nip19 ${SODIUM_LIB})
else()
  target_link_libraries(nostr_nip49_core PUBLIC nostr nip19 sodium)
endif()

# Build GLib wrapper when GLib is available (for GTK apps like gnostr-signer)
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_check_modules(GLIB2 QUIET glib-2.0)
endif()
if (GLIB2_FOUND)
  add_library(nostr_nip49_glib ${NIP49_GLIB_SRC})
  target_include_directories(nostr_nip49_glib PUBLIC ${NIP49_INC} ${GLIB2_INCLUDE_DIRS})
  target_link_directories(nostr_nip49_glib PUBLIC ${GLIB2_LIBRARY_DIRS})
  target_link_libraries(nostr_nip49_glib PUBLIC nostr_nip49_core ${GLIB2_LIBRARIES})
endif()

# Install headers
install(DIRECTORY ${NIP49_INC}/ DESTINATION include/nostr/nip49)

# SPEC linkage/layout guard
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spec_source.c)
  add_executable(test_nip49_spec_source tests/test_spec_source.c)
  target_link_libraries(test_nip49_spec_source PRIVATE nostr_nip49_core)
  add_test(NAME test_nip49_spec_source COMMAND test_nip49_spec_source)
endif()

# Optional simple payload test
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip49_payload.c)
  add_executable(test_nip49_payload tests/test_nip49_payload.c)
  target_link_libraries(test_nip49_payload PRIVATE nostr_nip49_core)
  add_test(NAME test_nip49_payload COMMAND test_nip49_payload)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip49_roundtrip.c)
  add_executable(test_nip49_roundtrip tests/test_nip49_roundtrip.c)
  target_link_libraries(test_nip49_roundtrip PRIVATE nostr_nip49_core)
  add_test(NAME test_nip49_roundtrip COMMAND test_nip49_roundtrip)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip49_negative.c)
  add_executable(test_nip49_negative tests/test_nip49_negative.c)
  target_link_libraries(test_nip49_negative PRIVATE nostr_nip49_core)
  add_test(NAME test_nip49_negative COMMAND test_nip49_negative)
endif()

if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_nip49_vectors.c)
  add_executable(test_nip49_vectors tests/test_nip49_vectors.c)
  target_link_libraries(test_nip49_vectors PRIVATE nostr_nip49_core)
  add_test(NAME test_nip49_vectors COMMAND test_nip49_vectors)
endif()

# CLI example
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/nostr-nip49.c)
  add_executable(nostr-nip49 examples/nostr-nip49.c)
  target_link_libraries(nostr-nip49 PRIVATE nostr_nip49_core)
endif()

