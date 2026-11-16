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
  add_executable(${target} ${source} test_tmpdir.c)
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
# Compile the real logger with a test-local syscall seam for partial ENOSPC.
# No write replacement or fault state is present in fipsd_lib/the daemon.
add_library(fipsd_log_fault OBJECT ../src/log.c)
target_link_libraries(fipsd_log_fault PRIVATE fipsd_lib)
target_compile_definitions(fipsd_log_fault PRIVATE write=fipsd_log_test_write)
set_target_properties(fipsd_log_fault PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fipsd_log_fault)
apply_sanitizers(fipsd_log_fault)
target_sources(fipsd_log_test PRIVATE $<TARGET_OBJECTS:fipsd_log_fault>)
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

# Node lifecycle, bounded worker pools and platform network monitor.
fipsd_unit_test(fipsd_node_lifecycle_test test_node_lifecycle.c)
fipsd_unit_test(fipsd_node_workers_test test_node_workers.c)
fipsd_unit_test(fipsd_node_netmon_test test_node_netmon.c)
fipsd_unit_test(fipsd_node_context_test test_node_context.c)
add_dependencies(fipsd_tests fipsd_node_lifecycle_test fipsd_node_workers_test
  fipsd_node_netmon_test fipsd_node_context_test)
add_test(NAME FipsdNodeLifecycleTest COMMAND fipsd_node_lifecycle_test)
add_test(NAME FipsdNodeLifecycleFailureTest COMMAND fipsd_node_lifecycle_test failure)
add_test(NAME FipsdNodeWorkersTest COMMAND fipsd_node_workers_test)
add_test(NAME FipsdNodeNetmonTest COMMAND fipsd_node_netmon_test)
add_test(NAME FipsdNodeNetmonNativeTest COMMAND fipsd_node_netmon_test native)
add_test(NAME FipsdNodeContextTest COMMAND fipsd_node_context_test)
add_test(NAME FipsdNodeIdentityFailureTest COMMAND fipsd_node_context_test failure)
foreach(_node_test FipsdNodeLifecycleTest FipsdNodeLifecycleFailureTest
    FipsdNodeWorkersTest FipsdNodeNetmonTest FipsdNodeNetmonNativeTest
    FipsdNodeContextTest FipsdNodeIdentityFailureTest)
  set_tests_properties(${_node_test} PROPERTIES TIMEOUT 30 LABELS "fips;node"
    ENVIRONMENT "FIPSD_WORKERS=2")
endforeach()
nostrc_describe_test(FipsdNodeLifecycleTest OWNER fipsd TARGETS fipsd_node_lifecycle_test)
nostrc_describe_test(FipsdNodeLifecycleFailureTest OWNER fipsd TARGETS fipsd_node_lifecycle_test)
nostrc_describe_test(FipsdNodeWorkersTest OWNER fipsd TARGETS fipsd_node_workers_test)
nostrc_describe_test(FipsdNodeNetmonTest OWNER fipsd TARGETS fipsd_node_netmon_test)
nostrc_describe_test(FipsdNodeNetmonNativeTest OWNER fipsd TARGETS fipsd_node_netmon_test)
nostrc_describe_test(FipsdNodeContextTest OWNER fipsd TARGETS fipsd_node_context_test)
nostrc_describe_test(FipsdNodeIdentityFailureTest OWNER fipsd TARGETS fipsd_node_context_test)

add_test(NAME FipsdNodeHandlerFailureTest COMMAND fipsd_node_context_test handler)
set_tests_properties(FipsdNodeHandlerFailureTest PROPERTIES TIMEOUT 30 LABELS "fips;node"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNodeHandlerFailureTest OWNER fipsd TARGETS fipsd_node_context_test)
# Control protocol uses the production listener/dispatcher and retained provider.
fipsd_unit_test(fipsd_control_test test_control.c)
target_compile_definitions(fipsd_control_test PRIVATE
  FIPSD_CONTROL_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/control_golden")
add_dependencies(fipsd_tests fipsd_control_test)
add_test(NAME FipsdControlCapacityTest COMMAND fipsd_control_test capacity)
set_tests_properties(FipsdControlCapacityTest PROPERTIES TIMEOUT 30 LABELS "fips;unit;control"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdControlCapacityTest OWNER fipsd TARGETS fipsd_control_test)
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
fipsd_unit_test(fipsd_test_tmpdir_run test_tmpdir_run.c)
add_dependencies(fipsd_tests fipsd_test_tmpdir_run)
add_test(NAME FipsdControlInteropTest COMMAND fipsd_test_tmpdir_run "/control.sock"
  "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/control_interop.py"
  --fipsd "$<TARGET_FILE:fipsd>" --upstream "${FIPSD_UPSTREAM_CONTROL_DIR}"
  --fipsctl "${FIPSD_FIPSCTL_EXECUTABLE}"
  --cargo-target "${CMAKE_CURRENT_BINARY_DIR}/cargo-control")
set_tests_properties(FipsdControlInteropTest PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
  LABELS "fips;control;interop")
nostrc_describe_test(FipsdControlInteropTest OWNER fipsd TARGETS fipsd fipsd_test_tmpdir_run
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/control_interop.py")

# Node protocol handlers (nostrc-9keoz.6): two-node loopback proof and the
# per-handler scenarios, both event-driven on two scheduler workers.
fipsd_unit_test(fipsd_node_two_nodes_test test_node_two_nodes.c)
add_dependencies(fipsd_tests fipsd_node_two_nodes_test)
add_test(NAME FipsdNodeTwoNodesTest COMMAND fipsd_node_two_nodes_test)
set_tests_properties(FipsdNodeTwoNodesTest PROPERTIES TIMEOUT 300 LABELS "fips;node"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNodeTwoNodesTest OWNER fipsd TARGETS fipsd_node_two_nodes_test)
fipsd_unit_test(fipsd_node_handlers_test test_node_handlers.c)
add_dependencies(fipsd_tests fipsd_node_handlers_test)
add_test(NAME FipsdNodeHandlersTest COMMAND fipsd_node_handlers_test)
set_tests_properties(FipsdNodeHandlersTest PROPERTIES TIMEOUT 300 LABELS "fips;node"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNodeHandlersTest OWNER fipsd TARGETS fipsd_node_handlers_test)

fipsd_unit_test(fipsd_node_stp_acks_test test_node_stp_acks.c)
target_sources(fipsd_node_stp_acks_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/fakes/host.c")
target_include_directories(fipsd_node_stp_acks_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests")
add_dependencies(fipsd_tests fipsd_node_stp_acks_test)
add_test(NAME FipsdNodeStpAcksTest COMMAND fipsd_node_stp_acks_test)
set_tests_properties(FipsdNodeStpAcksTest PROPERTIES TIMEOUT 60 LABELS "fips;node"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNodeStpAcksTest OWNER fipsd TARGETS fipsd_node_stp_acks_test)

add_test(NAME FipsdNodeStpAcksNoReportTest COMMAND fipsd_node_stp_acks_test no-reports)
set_tests_properties(FipsdNodeStpAcksNoReportTest PROPERTIES TIMEOUT 60 LABELS "fips;node"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNodeStpAcksNoReportTest OWNER fipsd TARGETS fipsd_node_stp_acks_test)
# Interface presence reaches the multipath Bundle through the real dispatcher.
fipsd_unit_test(fipsd_node_path_presence_test test_node_path_presence.c)
add_dependencies(fipsd_tests fipsd_node_path_presence_test)
add_test(NAME FipsdNodePathPresenceTest COMMAND fipsd_node_path_presence_test)
set_tests_properties(FipsdNodePathPresenceTest PROPERTIES TIMEOUT 120 LABELS "fips;node"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNodePathPresenceTest OWNER fipsd TARGETS fipsd_node_path_presence_test)
# Dataplane parity decisions and four-worker counter/backpressure contracts.
fipsd_unit_test(fipsd_dataplane_test test_dataplane.c)
target_sources(fipsd_dataplane_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/fakes/host.c")
target_include_directories(fipsd_dataplane_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests")
add_dependencies(fipsd_tests fipsd_dataplane_test)
add_test(NAME FipsdDataplaneTest COMMAND fipsd_dataplane_test)
set_tests_properties(FipsdDataplaneTest PROPERTIES TIMEOUT 60 LABELS "fips;node" ENVIRONMENT "FIPSD_WORKERS=4")
nostrc_describe_test(FipsdDataplaneTest OWNER fipsd TARGETS fipsd_dataplane_test)

# Explicit opt-in: benchmark executable is always built by the aggregate, but
# ordinary ctest (including -R '^Fips') does not run a throughput workload.
fipsd_unit_test(fipsd_dataplane_throughput test_dataplane_throughput.c)
add_dependencies(fipsd_tests fipsd_dataplane_throughput)
option(FIPSD_DATAPLANE_BENCH_TEST "Register the opt-in loopback throughput smoke" OFF)
if(FIPSD_DATAPLANE_BENCH_TEST)
  add_test(NAME FipsdDataplaneThroughput COMMAND fipsd_dataplane_throughput)
  set_tests_properties(FipsdDataplaneThroughput PROPERTIES TIMEOUT 120 LABELS "fips;bench" ENVIRONMENT "FIPSD_WORKERS=4")
  nostrc_describe_test(FipsdDataplaneThroughput OWNER fipsd TARGETS fipsd_dataplane_throughput)
endif()

# Upstream ACL semantics and live admission/reload proof (nostrc-9keoz.8).
fipsd_unit_test(fipsd_acl_test test_acl.c)
fipsd_unit_test(fipsd_node_acl_test test_node_acl.c)
add_dependencies(fipsd_tests fipsd_acl_test fipsd_node_acl_test)
add_test(NAME FipsdAclTest COMMAND fipsd_acl_test)
add_test(NAME FipsdNodeAclTest COMMAND fipsd_node_acl_test)
set_tests_properties(FipsdAclTest FipsdNodeAclTest PROPERTIES TIMEOUT 120 LABELS "fips;node;acl" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdAclTest OWNER fipsd TARGETS fipsd_acl_test)
nostrc_describe_test(FipsdNodeAclTest OWNER fipsd TARGETS fipsd_node_acl_test)

fipsd_unit_test(fipsd_stats_test test_stats.c)
add_dependencies(fipsd_tests fipsd_stats_test)
add_test(NAME FipsdStatsTest COMMAND fipsd_stats_test)
set_tests_properties(FipsdStatsTest PROPERTIES TIMEOUT 60 LABELS "fips;unit;stats")
nostrc_describe_test(FipsdStatsTest OWNER fipsd TARGETS fipsd_stats_test)

target_compile_definitions(fipsd_acl_test PRIVATE
  FIPSD_CONTROL_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/control_golden")

# A1 admission ordering through the real dispatcher, transport and IK/AEAD.
target_sources(fipsd_node_acl_test PRIVATE test_node_acl_admission.c handshake_fixture.c)
add_test(NAME FipsdNodeAclAdmissionTest COMMAND fipsd_node_acl_test admission)
set_tests_properties(FipsdNodeAclAdmissionTest PROPERTIES TIMEOUT 60 LABELS "fips;node;acl" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNodeAclAdmissionTest OWNER fipsd TARGETS fipsd_node_acl_test)

# H1 restart replay: a captured earlier-epoch msg1 must not evict the live
# link; eviction waits for a frame authenticated under the fresh msg2's keys.
fipsd_unit_test(fipsd_node_restart_replay_test test_node_restart_replay.c)
target_sources(fipsd_node_restart_replay_test PRIVATE handshake_fixture.c)
add_dependencies(fipsd_tests fipsd_node_restart_replay_test)
add_test(NAME FipsdNodeRestartReplayTest COMMAND fipsd_node_restart_replay_test)
set_tests_properties(FipsdNodeRestartReplayTest PROPERTIES TIMEOUT 60 LABELS "fips;node;acl" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNodeRestartReplayTest OWNER fipsd TARGETS fipsd_node_restart_replay_test)
