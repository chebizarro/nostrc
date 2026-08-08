# nip-concord: Concord protocol C library
# Concord is an adopted external standard (github.com/concord-protocol/concord)
# NIP-CAS-0008: Concord Adoption — Encrypted Community Backup Plane

project(nip_concord LANGUAGES C)

# Dependencies
find_package(nip44 REQUIRED)
find_package(nip59 REQUIRED)
find_package(secp256k1 REQUIRED)

# Library
add_library(nip_concord STATIC
  src/nip_concord.c
)

target_include_directories(nip_concord PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)

target_link_libraries(nip_concord PUBLIC
  nip44
  nip59
  secp256k1
)

# Tests
if(BUILD_TESTING)
  add_executable(test_nip_concord
    tests/test_nip_concord.c
  )
  target_link_libraries(test_nip_concord PRIVATE
    nip_concord
    nip44
    nip59
    secp256k1
  )
  add_test(NAME test_nip_concord COMMAND test_nip_concord)
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
