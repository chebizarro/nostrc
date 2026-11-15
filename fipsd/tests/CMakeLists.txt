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
# go_fiber carries no __tsan_switch_to_fiber annotations, so a fiber resumed
# on a different worker thread unbalances TSan's per-thread shadow stack and
# TSan itself faults at worker exit (seen in ~10% of two-worker runs; never in
# ASan, plain or single-worker runs). Single-worker runs push and pop every
# frame on one thread and are immune, and still exercise the main, poller,
# blocking-executor, worker and signal-handler interactions. Multi-worker
# migration stays covered by the ASan/UBSan and plain lanes until go_fiber
# gains the annotations.
if(GO_ENABLE_TSAN)
  set(_fipsd_runtime_workers 1)
else()
  set(_fipsd_runtime_workers 2)
endif()
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
