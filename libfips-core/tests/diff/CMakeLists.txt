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

file(GLOB_RECURSE vector_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../vectors/*")
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

# Identity replay wipes its explicit secret exports.
target_link_libraries(fips_diff PRIVATE PkgConfig::FIPS_CORE_SODIUM)

# Proto link-catalog/session-datagram/framing checks (nostrc-82g9n.6). cases.c's
# per-name dispatch calls into proto_link.h once both land.
target_sources(fips_diff PRIVATE proto_link.c)

# Per-name table registration is owned by the integration orchestrator.
target_sources(fips_diff PRIVATE proto_fmp.c)
# Orchestrator owns cases.c dispatch after merge (nostrc-82g9n.2).
target_sources(fips_diff PRIVATE proto_stp.c)
# Bloom and FilterAnnounce replay (nostrc-82g9n.3); coordinator wires cases.c.
target_sources(fips_diff PRIVATE proto_bloom.c)

# Per-name fsp rows remain orchestrator-owned in cases.c until integration.
target_sources(fips_diff PRIVATE proto_fsp.c)
# Coordinator owns cases.c dispatch for lookup-request/lookup-response.
target_sources(fips_diff PRIVATE proto_lookup.c)

# One selection for every differential family, including harness negatives.
foreach(family Entropy Identity Codec Noise Proto Rekey SelfTest)
  set_property(TEST FipsDiff${family} APPEND PROPERTY LABELS fips-diff)
endforeach()
add_test(NAME FipsProtoSweep COMMAND "${CMAKE_COMMAND}"
  "-DRUNNER=$<TARGET_FILE:fips_diff>" -P "${CMAKE_CURRENT_SOURCE_DIR}/proto_sweep.cmake")
set_tests_properties(FipsProtoSweep PROPERTIES TIMEOUT 30 LABELS "fips;diff;proto-sweep;fips-diff")
nostrc_describe_test(FipsProtoSweep OWNER libfips-core TARGETS fips_diff
  INPUTS ${vector_inputs} "${CMAKE_CURRENT_SOURCE_DIR}/proto_sweep.cmake")

# The orchestrator wires these five per-name rows after merge.
target_sources(fips_diff PRIVATE proto_mmp.c)

# Path rows remain orchestrator-owned; direct vector test covers them here.
target_sources(fips_diff PRIVATE proto_path.c)

add_test(NAME FipsDiffRekeyLegacy COMMAND fips_diff rekey-legacy)
set_tests_properties(FipsDiffRekeyLegacy PROPERTIES TIMEOUT 15 LABELS "fips;diff;fips-diff")
nostrc_describe_test(FipsDiffRekeyLegacy OWNER libfips-core TARGETS fips_diff
  INPUTS ${vector_inputs} "${CMAKE_CURRENT_SOURCE_DIR}/../../UPSTREAM.md")

add_test(NAME FipsDiffRekeyLegacySelfTest COMMAND "${CMAKE_COMMAND}"
  "-DRUNNER=$<TARGET_FILE:fips_diff>"
  "-DVECTORS=${CMAKE_CURRENT_SOURCE_DIR}/../vectors"
  "-DTEMP=${CMAKE_CURRENT_BINARY_DIR}/legacy-self-test"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/legacy_selftest.cmake")
set_tests_properties(FipsDiffRekeyLegacySelfTest PROPERTIES TIMEOUT 30 LABELS "fips;diff;negative;fips-diff")
nostrc_describe_test(FipsDiffRekeyLegacySelfTest OWNER libfips-core TARGETS fips_diff
  INPUTS ${vector_inputs} "${CMAKE_CURRENT_SOURCE_DIR}/legacy_selftest.cmake")

# Separate runner keeps D2's new family out of D1's shared exporter/replay edits.
add_executable(fips_diff_settle settle.c ../fakes/host.c
  ../../src/proto/settle/params.c ../../src/proto/settle/links.c ../../src/proto/settle/legs.c ../../src/proto/settle/gates.c ../../src/proto/settle/table.c ../../src/proto/settle/join.c ../../src/proto/settle/model/history.c ../../src/proto/settle/model/judge.c ../../src/proto/settle/parity.c)
target_link_libraries(fips_diff_settle PRIVATE fips_core PkgConfig::FIPS_DIFF_JANSSON PkgConfig::FIPS_CORE_SODIUM)
set_target_properties(fips_diff_settle PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_definitions(fips_diff_settle PRIVATE FIPS_TEST_HOOKS FIPS_SETTLE_VECTOR="${CMAKE_CURRENT_SOURCE_DIR}/../vectors/settle.json")
apply_warnings(fips_diff_settle)
apply_sanitizers(fips_diff_settle)
add_test(NAME FipsDiffSettle COMMAND fips_diff_settle)
set_tests_properties(FipsDiffSettle PROPERTIES TIMEOUT 30 LABELS "fips;diff;fips-diff")
nostrc_describe_test(FipsDiffSettle OWNER libfips-core TARGETS fips_diff_settle
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/../vectors/settle.json")
add_dependencies(fips_core_host_test fips_diff_settle)
