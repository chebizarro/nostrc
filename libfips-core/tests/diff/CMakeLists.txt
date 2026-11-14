pkg_check_modules(FIPS_DIFF_JANSSON REQUIRED IMPORTED_TARGET jansson)

# Use the same machine-readable pin as the upstream watch tool, never a second
# hand-maintained constant. CMAKE_CONFIGURE_DEPENDS also catches edits after configure.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/../../UPSTREAM.md")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../UPSTREAM.md" pin_line
  REGEX "^Pinned commit: `[0-9a-f]+`$")
string(REGEX REPLACE "^Pinned commit: `([0-9a-f]+)`$" "\\1" diff_pin "${pin_line}")
string(LENGTH "${diff_pin}" pin_length)
if(NOT pin_length EQUAL 40)
  message(FATAL_ERROR "FIPS differential tests need one full Pinned commit in UPSTREAM.md")
endif()

add_executable(fips_diff main.c loader.c schema.c cases.c ../fakes/host.c)
target_link_libraries(fips_diff PRIVATE fips_core PkgConfig::FIPS_DIFF_JANSSON
  PkgConfig::FIPS_CORE_SECP256K1 OpenSSL::Crypto)
set_target_properties(fips_diff PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_definitions(fips_diff PRIVATE
  FIPS_DIFF_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/../vectors"
  FIPS_DIFF_PIN="${diff_pin}")
apply_warnings(fips_diff)
apply_sanitizers(fips_diff)

# Existing fips-ci.yml builds explicit targets, including only the host in TSan.
# Keep its build closure complete without changing the workflow or linking the
# test-only loader/Jansson into the shipped library/daemon.
add_dependencies(fips_core_host_test fips_diff)
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL
  add_dependencies fipsd_host_os_test fips_diff)

file(GLOB vector_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../vectors/*")
foreach(family Entropy Identity Codec Noise Proto Rekey)
  string(TOLOWER "${family}" lower)
  add_test(NAME FipsDiff${family} COMMAND fips_diff "${lower}")
  set_tests_properties(FipsDiff${family} PROPERTIES TIMEOUT 15 LABELS "fips;diff")
  nostrc_describe_test(FipsDiff${family} OWNER libfips-core TARGETS fips_diff
    INPUTS ${vector_inputs} "${CMAKE_CURRENT_SOURCE_DIR}/../../UPSTREAM.md")
endforeach()
add_test(NAME FipsDiffSelfTest COMMAND "${CMAKE_COMMAND}"
  "-DRUNNER=$<TARGET_FILE:fips_diff>"
  "-DVECTORS=${CMAKE_CURRENT_SOURCE_DIR}/../vectors"
  "-DTEMP=${CMAKE_CURRENT_BINARY_DIR}/self-test"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/selftest.cmake")
set_tests_properties(FipsDiffSelfTest PROPERTIES TIMEOUT 30 LABELS "fips;diff;negative")
nostrc_describe_test(FipsDiffSelfTest OWNER libfips-core TARGETS fips_diff
  INPUTS ${vector_inputs} "${CMAKE_CURRENT_SOURCE_DIR}/selftest.cmake")
