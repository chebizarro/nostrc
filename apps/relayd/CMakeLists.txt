project(nostrc_relayd C)

include_directories(
  ${CMAKE_SOURCE_DIR}/libnostr/include
  ${CMAKE_SOURCE_DIR}/apps/relayd/include
  ${CMAKE_SOURCE_DIR}/nips/nip11/include
  ${CMAKE_SOURCE_DIR}/nips/nip86/include
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LWS REQUIRED libwebsockets)

find_package(OpenSSL REQUIRED)
find_library(SECP256K1_LIB NAMES secp256k1 REQUIRED)
find_library(NSYNC_LIB nsync REQUIRED)
find_path(NSYNC_INCLUDE_DIR nsync.h REQUIRED)
include_directories(${NSYNC_INCLUDE_DIR})

add_executable(nostrc-relayd
  src/relayd_main.c
  src/relayd_config.c
  src/protocol_nip01.c
  src/protocol_nip45.c
  src/protocol_nip11.c
  src/protocol_nip42.c
  src/protocol_nip50.c
  src/protocol_nip77.c
  src/rate_limit.c
  src/retention.c
  src/metrics.c
)

target_include_directories(nostrc-relayd PRIVATE ${LWS_INCLUDE_DIRS})
target_link_directories(nostrc-relayd PRIVATE ${LWS_LIBRARY_DIRS})

target_link_libraries(nostrc-relayd
  PRIVATE
    nostr
    nostr_json
    libgo
    ${NSYNC_LIB}
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SECP256K1_LIB}
    ${LWS_LIBRARIES}
)

# Link nostrdb storage component if available
if (TARGET nostrdb_storage)
  target_link_libraries(nostrc-relayd PRIVATE nostrdb_storage)
  # Also link vendored static archives directly so ndb_* symbols resolve
  set(_NDB_VENDOR_DIR ${CMAKE_SOURCE_DIR}/third_party/nostrdb)
  if (EXISTS ${_NDB_VENDOR_DIR}/libnostrdb.a)
    target_link_libraries(nostrc-relayd PRIVATE ${_NDB_VENDOR_DIR}/libnostrdb.a)
  endif()
  if (EXISTS ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
    target_link_libraries(nostrc-relayd PRIVATE ${_NDB_VENDOR_DIR}/deps/lmdb/liblmdb.a)
  endif()
endif()

# Link NIP-11 module if available and set compile define
if (TARGET nip11)
  target_link_libraries(nostrc-relayd PRIVATE nip11)
  target_compile_definitions(nostrc-relayd PRIVATE HAVE_NIP11=1)
endif()

# NIP-86 integration intentionally disabled for relayd link; relayd has
# compile-time fallbacks and guards to avoid referencing nip86 symbols.

install(TARGETS nostrc-relayd RUNTIME DESTINATION bin)
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/relay.toml.example)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/relay.toml.example DESTINATION share/nostrc)
endif()

# Apply global sanitizer flags to ensure TSAN/ASAN/UBSAN DSOs are linked
if (COMMAND apply_sanitizers)
  apply_sanitizers(nostrc-relayd)
endif()
