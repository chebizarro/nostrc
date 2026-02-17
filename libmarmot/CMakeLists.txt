cmake_minimum_required(VERSION 3.22)
project(libmarmot C)

set(MARMOT_VERSION_MAJOR 0)
set(MARMOT_VERSION_MINOR 1)
set(MARMOT_VERSION_PATCH 0)
set(MARMOT_VERSION_STRING "${MARMOT_VERSION_MAJOR}.${MARMOT_VERSION_MINOR}.${MARMOT_VERSION_PATCH}")

# Generate version header from template
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot/marmot-version.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/include/marmot/marmot-version.h
  @ONLY
)

# ─── Dependencies ─────────────────────────────────────────────────────────
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)

# ─── Source files ─────────────────────────────────────────────────────────
set(MARMOT_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot_error.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot_types.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/marmot_storage.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/extension.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/storage_memory.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mls/mls_crypto.c
  ${CMAKE_CURRENT_SOURCE_DIR}/src/mls/mls_tls.c
)

set(MARMOT_PUBLIC_HEADERS
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot/marmot.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot/marmot-error.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot/marmot-types.h
  ${CMAKE_CURRENT_SOURCE_DIR}/include/marmot/marmot-storage.h
  ${CMAKE_CURRENT_BINARY_DIR}/include/marmot/marmot-version.h
)

# ─── Library target ──────────────────────────────────────────────────────
add_library(marmot ${MARMOT_SOURCES})
set_target_properties(marmot PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  OUTPUT_NAME marmot
  VERSION ${MARMOT_VERSION_STRING}
  SOVERSION ${MARMOT_VERSION_MAJOR}
)

target_include_directories(marmot
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(marmot
  PUBLIC
    OpenSSL::Crypto
    PkgConfig::SODIUM
)

# Warn on everything for development
target_compile_options(marmot PRIVATE
  $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Wno-unused-parameter>
)

# ─── pkg-config ──────────────────────────────────────────────────────────
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/marmot.pc.in
  ${CMAKE_CURRENT_BINARY_DIR}/marmot.pc
  @ONLY
)

# ─── Install ─────────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS marmot
  EXPORT marmot-targets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(FILES ${MARMOT_PUBLIC_HEADERS}
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/marmot
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/marmot.pc
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

install(EXPORT marmot-targets
  FILE marmot-targets.cmake
  NAMESPACE Marmot::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/marmot
)

# ─── Tests ───────────────────────────────────────────────────────────────
if(BUILD_TESTING)
  enable_testing()

  # Test: TLS codec round-trip
  add_executable(test_mls_tls ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mls_tls.c)
  target_link_libraries(test_mls_tls PRIVATE marmot)
  target_include_directories(test_mls_tls PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_mls_tls COMMAND test_mls_tls)

  # Test: MLS crypto primitives
  add_executable(test_mls_crypto ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mls_crypto.c)
  target_link_libraries(test_mls_crypto PRIVATE marmot)
  target_include_directories(test_mls_crypto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_mls_crypto COMMAND test_mls_crypto)

  # Test: Extension serialize/deserialize
  add_executable(test_extension ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_extension.c)
  target_link_libraries(test_extension PRIVATE marmot)
  target_include_directories(test_extension PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME marmot_test_extension COMMAND test_extension)

  # Test: Storage memory backend
  add_executable(test_storage ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_storage.c)
  target_link_libraries(test_storage PRIVATE marmot)
  add_test(NAME marmot_test_storage COMMAND test_storage)

  # Test: Types and lifecycle
  add_executable(test_types ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_types.c)
  target_link_libraries(test_types PRIVATE marmot)
  add_test(NAME marmot_test_types COMMAND test_types)
endif()
