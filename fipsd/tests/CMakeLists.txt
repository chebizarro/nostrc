# Real-binary CLI contract: documented flags, exit codes and output shape.
add_test(NAME FipsdCliTest
  COMMAND "${CMAKE_COMMAND}"
    "-DFIPSD=$<TARGET_FILE:fipsd>"
    "-DEXPECTED_VERSION=${FIPSD_VERSION}"
    "-DEXPECTED_CORE_VERSION=${fips_core_VERSION}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cli.cmake")
set_tests_properties(FipsdCliTest PROPERTIES TIMEOUT 30 LABELS "fips;smoke"
  ENVIRONMENT "FIPSD_WORKERS=1")
nostrc_describe_test(FipsdCliTest OWNER fipsd TARGETS fipsd
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/cli.cmake")

function(fipsd_unit_test target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE fipsd_lib)
  set_target_properties(${target} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  apply_warnings(${target})
  apply_sanitizers(${target})
endfunction()

fipsd_unit_test(fipsd_host_os_test test_host_os.c)
add_test(NAME FipsdHostOsTest COMMAND fipsd_host_os_test)
set_tests_properties(FipsdHostOsTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsdHostOsTest OWNER fipsd TARGETS fipsd_host_os_test)

fipsd_unit_test(fipsd_cli_test test_cli.c)
add_test(NAME FipsdCliParseTest COMMAND fipsd_cli_test)
set_tests_properties(FipsdCliParseTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsdCliParseTest OWNER fipsd TARGETS fipsd_cli_test)

fipsd_unit_test(fipsd_log_test test_log.c)
add_test(NAME FipsdLogTest COMMAND fipsd_log_test)
set_tests_properties(FipsdLogTest PROPERTIES TIMEOUT 60 LABELS "fips;unit")
nostrc_describe_test(FipsdLogTest OWNER fipsd TARGETS fipsd_log_test)

fipsd_unit_test(fipsd_runtime_test test_runtime.c)
add_test(NAME FipsdRuntimeSignalsTest COMMAND fipsd_runtime_test signals)
add_test(NAME FipsdRuntimeStartFailureTest COMMAND fipsd_runtime_test start-failure)
# go_fiber now carries ThreadSanitizer fiber annotations (nostrc-9965o), so the
# runtime tests migrate fibers across workers under TSan too.
set(_fipsd_runtime_workers 2)
set_tests_properties(FipsdRuntimeSignalsTest FipsdRuntimeStartFailureTest PROPERTIES
  TIMEOUT 60 LABELS "fips;unit" ENVIRONMENT "FIPSD_WORKERS=${_fipsd_runtime_workers}")
nostrc_describe_test(FipsdRuntimeSignalsTest OWNER fipsd TARGETS fipsd_runtime_test)
nostrc_describe_test(FipsdRuntimeStartFailureTest OWNER fipsd TARGETS fipsd_runtime_test)

fipsd_unit_test(fipsd_privilege_test test_privilege.c)
add_test(NAME FipsdPrivilegeTest COMMAND fipsd_privilege_test)
set_tests_properties(FipsdPrivilegeTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsdPrivilegeTest OWNER fipsd TARGETS fipsd_privilege_test)

# Drives the real executable: SIGTERM -> exit 0, --log-file ownership, and
# the start-up failure exit code.
fipsd_unit_test(fipsd_process_test test_process.c)
add_dependencies(fipsd_process_test fipsd)
add_test(NAME FipsdProcessTest COMMAND fipsd_process_test "$<TARGET_FILE:fipsd>")
set_tests_properties(FipsdProcessTest PROPERTIES TIMEOUT 60 LABELS "fips;smoke"
  ENVIRONMENT "FIPSD_WORKERS=1")
nostrc_describe_test(FipsdProcessTest OWNER fipsd TARGETS fipsd_process_test fipsd)

# One target that builds every fipsd test executable (for CI target lists).
add_custom_target(fipsd_tests DEPENDS fipsd fipsd_host_os_test fipsd_cli_test
  fipsd_log_test fipsd_runtime_test fipsd_privilege_test fipsd_process_test)

# Peer reducer + real dispatcher/snapshot boundary; deterministic injected clock.
fipsd_unit_test(fipsd_peer_test test_peer.c)
target_sources(fipsd_peer_test PRIVATE
  test_peer_machine.c test_peer_budget.c test_peer_data.c test_peer_link.c
  test_peer_dispatcher.c "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/fakes/host.c")
target_include_directories(fipsd_peer_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests")
add_test(NAME FipsdPeerTest COMMAND fipsd_peer_test)
set_tests_properties(FipsdPeerTest PROPERTIES TIMEOUT 60 LABELS "fips;unit"
  ENVIRONMENT "FIPSD_WORKERS=${_fipsd_runtime_workers}")
nostrc_describe_test(FipsdPeerTest OWNER fipsd TARGETS fipsd_peer_test)
add_dependencies(fipsd_tests fipsd_peer_test)
fipsd_unit_test(fipsd_config_test test_config.c)
add_dependencies(fipsd_tests fipsd_config_test)
add_test(NAME FipsdConfigTest COMMAND fipsd_config_test)
set_tests_properties(FipsdConfigTest PROPERTIES TIMEOUT 60 LABELS "fips;unit;config"
  ENVIRONMENT "FIPSD_WORKERS=1")
nostrc_describe_test(FipsdConfigTest OWNER fipsd TARGETS fipsd_config_test)
file(GLOB_RECURSE _fipsd_config_fixtures CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/config_fixtures/*.yaml")
foreach(_fixture IN LISTS _fipsd_config_fixtures)
  file(RELATIVE_PATH _name "${CMAKE_CURRENT_SOURCE_DIR}/config_fixtures" "${_fixture}")
  string(REPLACE "/" "_" _test_name "${_name}")
  add_test(NAME "FipsdConfigFixture_${_test_name}" COMMAND fipsd_config_test fixture
    "${_fixture}" "${CMAKE_CURRENT_SOURCE_DIR}/config_golden/${_name}.txt")
  set_tests_properties("FipsdConfigFixture_${_test_name}" PROPERTIES TIMEOUT 30 LABELS "fips;unit;config")
  nostrc_describe_test("FipsdConfigFixture_${_test_name}" OWNER fipsd TARGETS fipsd_config_test
    INPUTS "${_fixture}" "${CMAKE_CURRENT_SOURCE_DIR}/config_golden/${_name}.txt")
endforeach()
file(GLOB _fipsd_config_corpus CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/config_corpus/*")
foreach(_seed IN LISTS _fipsd_config_corpus)
  get_filename_component(_name "${_seed}" NAME)
  add_test(NAME "FipsdConfigCorpus_${_name}" COMMAND fipsd_config_test corpus "${_seed}")
  set_tests_properties("FipsdConfigCorpus_${_name}" PROPERTIES TIMEOUT 30 LABELS "fips;unit;config")
  nostrc_describe_test("FipsdConfigCorpus_${_name}" OWNER fipsd TARGETS fipsd_config_test INPUTS "${_seed}")
endforeach()

# Optional direct replay of the read-only Rust checkout, in addition to the
# self-contained comment-stripped copies. No Rust source is built or changed.
set(FIPSD_UPSTREAM_CONFIG_DIR "" CACHE PATH "Read-only upstream FIPS checkout for direct fixture replay")
if(FIPSD_UPSTREAM_CONFIG_DIR)
  foreach(_fixture IN LISTS _fipsd_config_fixtures)
    file(RELATIVE_PATH _name "${CMAKE_CURRENT_SOURCE_DIR}/config_fixtures" "${_fixture}")
    string(REPLACE "/" "_" _test_name "${_name}")
    add_test(NAME "FipsdConfigUpstream_${_test_name}" COMMAND fipsd_config_test fixture
      "${FIPSD_UPSTREAM_CONFIG_DIR}/${_name}" "${CMAKE_CURRENT_SOURCE_DIR}/config_golden/${_name}.txt")
    set_tests_properties("FipsdConfigUpstream_${_test_name}" PROPERTIES TIMEOUT 30 LABELS "fips;unit;config;upstream")
    nostrc_describe_test("FipsdConfigUpstream_${_test_name}" OWNER fipsd TARGETS fipsd_config_test
      INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/config_golden/${_name}.txt")
  endforeach()
endif()

# Control protocol uses the production listener/dispatcher and retained provider.
fipsd_unit_test(fipsd_control_test test_control.c)
target_compile_definitions(fipsd_control_test PRIVATE
  FIPSD_CONTROL_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/control_golden"
  FIPSD_CONTROL_TMP="${CMAKE_SOURCE_DIR}")
add_dependencies(fipsd_tests fipsd_control_test)
add_test(NAME FipsdControlTest COMMAND fipsd_control_test)
set_tests_properties(FipsdControlTest PROPERTIES TIMEOUT 60 LABELS "fips;unit;control"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdControlTest OWNER fipsd TARGETS fipsd_control_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/control_golden")

# Optional Rust interop: only cargo's target directory is writable; upstream is
# read-only. No source checkout download or lockfile update is performed.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(FIPSD_UPSTREAM_CONTROL_DIR "" CACHE PATH "Read-only pinned Rust FIPS checkout for fipsctl interop")
set(FIPSD_FIPSCTL_EXECUTABLE "" CACHE FILEPATH "Already-built pinned Rust fipsctl (optional)")
add_test(NAME FipsdControlInteropTest COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/control_interop.py"
  --fipsd "$<TARGET_FILE:fipsd>" --upstream "${FIPSD_UPSTREAM_CONTROL_DIR}"
  --fipsctl "${FIPSD_FIPSCTL_EXECUTABLE}"
  --cargo-target "${CMAKE_CURRENT_BINARY_DIR}/cargo-control"
  --temp-root "${CMAKE_SOURCE_DIR}")
set_tests_properties(FipsdControlInteropTest PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  LABELS "fips;control;interop")
nostrc_describe_test(FipsdControlInteropTest OWNER fipsd TARGETS fipsd
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/control_interop.py")
