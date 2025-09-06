project(nostrc_relayd C)

include_directories(
  ${CMAKE_SOURCE_DIR}/libnostr/include
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
endif()

# Link NIP-11 module if available
if (TARGET nip11)
  target_link_libraries(nostrc-relayd PRIVATE nip11)
  target_include_directories(nostrc-relayd PRIVATE ${CMAKE_SOURCE_DIR}/nips/nip11/include)
endif()

install(TARGETS nostrc-relayd RUNTIME DESTINATION bin)
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/relay.toml.example)
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/relay.toml.example DESTINATION share/nostrc)
endif()
