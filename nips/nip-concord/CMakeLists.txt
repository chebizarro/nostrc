# nip-concord: Concord protocol C library
# Concord is an adopted external standard (github.com/concord-protocol/concord)
# NIP-CAS-0008: Concord Adoption — Encrypted Community Backup Plane

project(nip_concord LANGUAGES C)

# The NIP-44 core and secp256k1 come from the in-tree build, not find_package:
# these are sibling targets in this repository, not installed packages.
if (NOT TARGET nostr_nip44_core)
  message(STATUS "nostr_nip44_core target not found — skipping nip-concord")
  return()
endif()

include(GNUInstallDirs)
find_package(OpenSSL REQUIRED)
find_library(SECP256K1_LIB NAMES secp256k1 REQUIRED)

add_library(nip_concord STATIC
  src/concord_crypto.c
  src/concord_derive.c
  src/concord_invite.c
)

target_include_directories(nip_concord PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_include_directories(nip_concord PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(nip_concord PUBLIC
  nostr_nip44_core
  OpenSSL::Crypto
  ${SECP256K1_LIB}
)

set_target_properties(nip_concord PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED YES
  POSITION_INDEPENDENT_CODE ON
)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(nip_concord PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Tests
if(BUILD_TESTING)
  add_executable(test_nip_concord tests/test_nip_concord.c)
  target_include_directories(test_nip_concord PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(test_nip_concord PRIVATE
    nip_concord
    nostr_nip44_core
    OpenSSL::Crypto
    ${SECP256K1_LIB}
  )
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(test_nip_concord PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME test_nip_concord COMMAND test_nip_concord)
  set_tests_properties(test_nip_concord PROPERTIES
    TIMEOUT 60 LABELS "concord;nip;unit")
endif()

# Install
install(TARGETS nip_concord
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/nip_concord
  FILES_MATCHING PATTERN "*.h"
)
