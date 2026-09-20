cmake_minimum_required(VERSION 3.18)
project(nostr_identity_portable_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Werror -pedantic)
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)
set(IDENTITY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
add_library(nostr_identity_test STATIC
  ${IDENTITY_ROOT}/src/identity/identity_common.c
  ${IDENTITY_ROOT}/src/identity/identity_config.c
  ${IDENTITY_ROOT}/src/identity/identity_store.c
  ${IDENTITY_ROOT}/src/identity/identity_reader.c
  ${IDENTITY_ROOT}/src/identity/identity_projection.c
  ${IDENTITY_ROOT}/src/identity/identity_home.c)
target_include_directories(nostr_identity_test PUBLIC
  ${IDENTITY_ROOT}/include ${IDENTITY_ROOT}/src/identity)
target_link_libraries(nostr_identity_test PUBLIC SQLite3::SQLite3 OpenSSL::Crypto)
add_executable(test_identity_store test_identity_store.c)
target_link_libraries(test_identity_store PRIVATE nostr_identity_test)
add_executable(test_identity_config test_identity_config.c)
target_link_libraries(test_identity_config PRIVATE nostr_identity_test)
add_executable(test_identity_fixtures test_identity_fixtures.c)
target_include_directories(test_identity_fixtures PRIVATE ${IDENTITY_ROOT}/include)
target_compile_definitions(test_identity_fixtures PRIVATE
  IDENTITY_FIXTURE_DIR="${CMAKE_CURRENT_LIST_DIR}/fixtures")
target_link_libraries(test_identity_fixtures PRIVATE SQLite3::SQLite3)
enable_testing()
add_test(NAME identity_store COMMAND test_identity_store)
add_test(NAME identity_config COMMAND test_identity_config)
add_test(NAME identity_fixtures COMMAND test_identity_fixtures)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(test_identity_home test_identity_home.c)
  target_link_libraries(test_identity_home PRIVATE nostr_identity_test)
  add_executable(test_nss_projection test_nss_projection.c
    ${IDENTITY_ROOT}/src/nss/nss_nostr.c
    ${IDENTITY_ROOT}/src/identity/identity_reader.c
    ${IDENTITY_ROOT}/src/identity/identity_common.c)
  target_include_directories(test_nss_projection PRIVATE ${IDENTITY_ROOT}/include ${IDENTITY_ROOT}/src/identity)
  target_compile_definitions(test_nss_projection PRIVATE NH_NSS_CONFIG_PATH="/run/nostrc-nss-test/nss.conf")
  target_link_libraries(test_nss_projection PRIVATE SQLite3::SQLite3)
  add_test(NAME identity_home COMMAND test_identity_home)
  add_test(NAME nss_projection COMMAND test_nss_projection)
  set_tests_properties(identity_home nss_projection PROPERTIES SKIP_RETURN_CODE 77)
endif()
