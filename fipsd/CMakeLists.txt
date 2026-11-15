cmake_minimum_required(VERSION 3.22)
project(fipsd C)

declare_component_version(FIPSD 0 1 0)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FIPSD_SODIUM REQUIRED IMPORTED_TARGET libsodium)

# Build target triple reported by `fipsd --version`.
execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
  RESULT_VARIABLE _fipsd_triple_rc OUTPUT_VARIABLE FIPSD_TARGET_TRIPLE
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT _fipsd_triple_rc EQUAL 0 OR FIPSD_TARGET_TRIPLE STREQUAL "")
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_SYSTEM_NAME}" FIPSD_TARGET_TRIPLE)
endif()

# Git revision for `fipsd -V`, captured at build time (see cmake/BuildInfo.cmake).
set(FIPSD_BUILD_INFO_DIR "${CMAKE_CURRENT_BINARY_DIR}/build-info")
set(FIPSD_BUILD_INFO_HEADER "${FIPSD_BUILD_INFO_DIR}/fipsd_build_info.h")
add_custom_target(fipsd_build_info
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DOUTPUT=${FIPSD_BUILD_INFO_HEADER}"
    "-DTARGET_TRIPLE=${FIPSD_TARGET_TRIPLE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildInfo.cmake"
  BYPRODUCTS "${FIPSD_BUILD_INFO_HEADER}"
  COMMENT "fipsd: capturing git revision"
  VERBATIM)

# Everything but main(), so the tests link the code the daemon runs.
add_library(fipsd_lib STATIC
  src/cli.c src/log.c src/runtime.c src/privilege.c src/host_os.c)
add_dependencies(fipsd_lib fipsd_build_info)
set_target_properties(fipsd_lib PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(fipsd_lib PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${FIPSD_VERSION_HEADER_DIR}" "${FIPSD_BUILD_INFO_DIR}")
# go_fiber precedes libgo deliberately: libgo's channel code calls weak
# gof_hook_* stubs that go_fiber overrides in fiber_hooks_impl.o. With static
# archives the linker satisfies the first undefined hook from whichever archive
# it scans first, and runtime.c references gof_hook_current() so that scan pulls
# the real implementation. The runtime also verifies this at start-up.
target_link_libraries(fipsd_lib PUBLIC
  fips_core go_fiber libgo nostrc::nsync PkgConfig::FIPSD_SODIUM)
apply_warnings(fipsd_lib)
apply_sanitizers(fipsd_lib)

add_executable(fipsd src/main.c)
set_target_properties(fipsd PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(fipsd PRIVATE fipsd_lib nostr)
apply_warnings(fipsd)
apply_sanitizers(fipsd)
install(TARGETS fipsd RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# YAML belongs to the host, never libfips-core.
pkg_check_modules(FIPSD_YAML REQUIRED IMPORTED_TARGET yaml-0.1)
target_link_libraries(fipsd_lib PUBLIC PkgConfig::FIPSD_YAML)
option(FIPSD_TEST_OVERRIDES "Honour test-only config lifetimes below protocol floors" OFF)
if(FIPSD_TEST_OVERRIDES)
  target_compile_definitions(fipsd_lib PUBLIC FIPSD_TEST_OVERRIDES=1)
endif()
target_sources(fipsd_lib PRIVATE
  src/config/config.c src/config/value.c src/config/normalize.c
  src/config/schema.c src/config/node.c src/config/transport.c
  src/config/peer.c src/config/gateway.c src/config/floors.c
  src/config/loadlog.c src/config/validate.c src/config/core.c src/config/reload.c)
