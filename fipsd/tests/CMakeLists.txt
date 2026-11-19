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

# fipsd_test_tmpdir(): mkdtemp/nftw on POSIX, %TEMP% and FindFirstFile on Windows.
if(WIN32)
  set(FIPSD_TEST_TMPDIR_SOURCE test_tmpdir_win32.c)
else()
  set(FIPSD_TEST_TMPDIR_SOURCE test_tmpdir.c)
endif()
# Publication pulses are test-only; the shipped library has no hook or wakeup.
add_library(fipsd_provider_hooked OBJECT ../src/node/handlers/provider.c)
target_link_libraries(fipsd_provider_hooked PRIVATE fipsd_lib)
target_compile_definitions(fipsd_provider_hooked PRIVATE FIPSD_TEST_HOOKS=1)
set_target_properties(fipsd_provider_hooked PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fipsd_provider_hooked)
apply_sanitizers(fipsd_provider_hooked)
function(fipsd_unit_test target source)
  add_executable(${target} ${source} ${FIPSD_TEST_TMPDIR_SOURCE})
  target_link_libraries(${target} PRIVATE fipsd_lib)
  set_target_properties(${target} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  apply_warnings(${target})
  apply_sanitizers(${target})
  if(source MATCHES "^test_((node|ble|mdns)_two_nodes|node_crossing_dials|mdns_multipath)\\.c$")
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:fipsd_provider_hooked>)
    target_compile_definitions(${target} PRIVATE FIPSD_TEST_HOOKS=1)
  endif()
endfunction()

if(WIN32)
  # Windows (nostrc-iq1n9.6.1): the portable subset. Everything below also
  # needs AF_UNIX control/native sockets, TUN devices or POSIX process tools.
  include("${CMAKE_CURRENT_SOURCE_DIR}/windows.cmake")
  return()
endif()

fipsd_unit_test(fipsd_host_os_test test_host_os.c)
add_test(NAME FipsdHostOsTest COMMAND fipsd_host_os_test)
set_tests_properties(FipsdHostOsTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsdHostOsTest OWNER fipsd TARGETS fipsd_host_os_test)

fipsd_unit_test(fipsd_cli_test test_cli.c)
add_test(NAME FipsdCliParseTest COMMAND fipsd_cli_test)
set_tests_properties(FipsdCliParseTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsdCliParseTest OWNER fipsd TARGETS fipsd_cli_test)

# The Windows service's SCM status sequence is pure data: tested everywhere.
fipsd_unit_test(fipsd_service_status_test test_service_status.c)
add_test(NAME FipsdServiceStatusTest COMMAND fipsd_service_status_test)
set_tests_properties(FipsdServiceStatusTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
nostrc_describe_test(FipsdServiceStatusTest OWNER fipsd TARGETS fipsd_service_status_test)

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
  fipsd_service_status_test fipsd_log_test fipsd_runtime_test fipsd_privilege_test fipsd_process_test)

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
# M7 (nostrc-f66sr.9): a control command's waiting timeout is an execution
# deadline. Production dispatcher/provider with the mailbox parked by a channel
# gate; expired commands never mutate, replies are owned exactly once.
fipsd_unit_test(fipsd_control_cancel_test test_control_cancel.c)
add_dependencies(fipsd_tests fipsd_control_cancel_test)
add_test(NAME FipsdControlCancelTest COMMAND fipsd_control_cancel_test)
set_tests_properties(FipsdControlCancelTest PROPERTIES TIMEOUT 120 LABELS "fips;node;control"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdControlCancelTest OWNER fipsd TARGETS fipsd_control_cancel_test)

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

# publish_snapshot latency with many retained churned peers (nostrc-igg4w):
# always built, ctest registration is opt-in. A Release build is the measurement.
fipsd_unit_test(fipsd_snapshot_bench test_snapshot_bench.c)
target_sources(fipsd_snapshot_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/fakes/host.c")
target_include_directories(fipsd_snapshot_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests")
add_dependencies(fipsd_tests fipsd_snapshot_bench)
# Dataplane coordinate-cache lookup and publish costs (nostrc-x345n): always
# built, run only by hand (fipsd/docs/perf.md), never by ctest.
fipsd_unit_test(fipsd_dp_cache_bench test_dp_cache_bench.c)
target_sources(fipsd_dp_cache_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/fakes/host.c")
target_include_directories(fipsd_dp_cache_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests")
add_dependencies(fipsd_tests fipsd_dp_cache_bench)
option(FIPSD_SNAPSHOT_BENCH_TEST "Register the opt-in publish_snapshot p99 gate (Release only)" OFF)
if(FIPSD_SNAPSHOT_BENCH_TEST)
  add_test(NAME FipsdSnapshotPublishBench COMMAND fipsd_snapshot_bench --max-p99-us 1000 --clock cpu)
  set_tests_properties(FipsdSnapshotPublishBench PROPERTIES TIMEOUT 300 LABELS "fips;bench" ENVIRONMENT "FIPSD_WORKERS=2")
  nostrc_describe_test(FipsdSnapshotPublishBench OWNER fipsd TARGETS fipsd_snapshot_bench)
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
fipsd_unit_test(fipsd_stats_view_test test_stats_view.c)
add_dependencies(fipsd_tests fipsd_stats_view_test)
add_test(NAME FipsdStatsViewTest COMMAND fipsd_stats_view_test)
set_tests_properties(FipsdStatsViewTest PROPERTIES TIMEOUT 120 LABELS "fips;unit;stats")
nostrc_describe_test(FipsdStatsViewTest OWNER fipsd TARGETS fipsd_stats_view_test)

target_compile_definitions(fipsd_acl_test PRIVATE
  FIPSD_CONTROL_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/control_golden")

# Shared transport codecs, OS facts and binder state machine (Tier B2).
fipsd_unit_test(fipsd_transport_test test_transport.c)
add_dependencies(fipsd_tests fipsd_transport_test)
add_test(NAME FipsdTransportTest COMMAND fipsd_transport_test)
set_tests_properties(FipsdTransportTest PROPERTIES TIMEOUT 30 LABELS "fips;transport")
nostrc_describe_test(FipsdTransportTest OWNER fipsd TARGETS fipsd_transport_test)
fipsd_unit_test(fipsd_transport_watcher_test test_transport_watcher.c)
add_dependencies(fipsd_tests fipsd_transport_watcher_test)
add_test(NAME FipsdTransportWatcherTest COMMAND fipsd_transport_watcher_test)
set_tests_properties(FipsdTransportWatcherTest PROPERTIES TIMEOUT 30 LABELS "fips;transport" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdTransportWatcherTest OWNER fipsd TARGETS fipsd_transport_watcher_test)
fipsd_unit_test(fipsd_nostr_events_test test_nostr_events.c)
target_sources(fipsd_nostr_events_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/fakes/host.c")
target_include_directories(fipsd_nostr_events_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests")
target_compile_definitions(fipsd_nostr_events_test PRIVATE
  FIPSD_NOSTR_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/vectors")
add_dependencies(fipsd_tests fipsd_nostr_events_test)
add_test(NAME FipsdNostrEventsTest COMMAND fipsd_nostr_events_test)
set_tests_properties(FipsdNostrEventsTest PROPERTIES TIMEOUT 60 LABELS "fips;nostr;vectors")
nostrc_describe_test(FipsdNostrEventsTest OWNER fipsd TARGETS fipsd_nostr_events_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/vectors/nostr.json")
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

# Tier B4: all privileged device syscalls replaced in the ordinary tests.
fipsd_unit_test(fipsd_tun_test test_tun.c)
fipsd_unit_test(fipsd_tun_runtime_test test_tun_runtime.c)
fipsd_unit_test(fipsd_tun_privileged_test test_tun_privileged.c)
add_dependencies(fipsd_tests fipsd_tun_test fipsd_tun_runtime_test fipsd_tun_privileged_test)
add_test(NAME FipsdTunTest COMMAND fipsd_tun_test)
add_test(NAME FipsdTunRuntimeTest COMMAND fipsd_tun_runtime_test)
add_test(NAME FipsdTunNotReadyTest COMMAND fipsd_tun_runtime_test not-ready)
add_test(NAME FipsdTunPrivilegedSmokeTest COMMAND fipsd_tun_privileged_test)
foreach(_tun_test FipsdTunTest FipsdTunRuntimeTest FipsdTunNotReadyTest FipsdTunPrivilegedSmokeTest)
  set_tests_properties(${_tun_test} PROPERTIES TIMEOUT 60 LABELS "fips;tun"
    ENVIRONMENT "FIPSD_WORKERS=2")
endforeach()
set_tests_properties(FipsdTunPrivilegedSmokeTest PROPERTIES SKIP_RETURN_CODE 77 LABELS "fips;tun;privileged")
nostrc_describe_test(FipsdTunTest OWNER fipsd TARGETS fipsd_tun_test)
nostrc_describe_test(FipsdTunRuntimeTest OWNER fipsd TARGETS fipsd_tun_runtime_test)
nostrc_describe_test(FipsdTunNotReadyTest OWNER fipsd TARGETS fipsd_tun_runtime_test)
nostrc_describe_test(FipsdTunPrivilegedSmokeTest OWNER fipsd TARGETS fipsd_tun_privileged_test)
# The Windows backend's IP Helper rows and codes are pure data (nostrc-iq1n9.6.2).
fipsd_unit_test(fipsd_tun_winnet_test test_tun_winnet.c)
add_dependencies(fipsd_tests fipsd_tun_winnet_test)
add_test(NAME FipsdTunWinnetTest COMMAND fipsd_tun_winnet_test)
set_tests_properties(FipsdTunWinnetTest PROPERTIES TIMEOUT 30 LABELS "fips;tun;unit")
nostrc_describe_test(FipsdTunWinnetTest OWNER fipsd TARGETS fipsd_tun_winnet_test)
# Re-run the complete node gate, not a reduced UDP handshake mock.
foreach(family IN ITEMS 4 6)
  fipsd_unit_test(fipsd_udp_two_nodes_v${family}_test test_node_two_nodes.c)
  target_compile_definitions(fipsd_udp_two_nodes_v${family}_test PRIVATE FIPSD_TEST_UDP=1)
  if(family EQUAL 6)
    target_compile_definitions(fipsd_udp_two_nodes_v${family}_test PRIVATE FIPSD_TEST_UDP_V6=1)
  endif()
  add_dependencies(fipsd_tests fipsd_udp_two_nodes_v${family}_test)
  add_test(NAME FipsdUdpTwoNodesV${family}Test COMMAND fipsd_udp_two_nodes_v${family}_test)
  set_tests_properties(FipsdUdpTwoNodesV${family}Test PROPERTIES TIMEOUT 300 LABELS "fips;transport;node"
    ENVIRONMENT "FIPSD_WORKERS=2")
  nostrc_describe_test(FipsdUdpTwoNodesV${family}Test OWNER fipsd TARGETS fipsd_udp_two_nodes_v${family}_test)
endforeach()
# Crossing dials and link loss (nostrc-8rtmi): a deterministic identity-keyed
# crossing over real UDP sockets.
fipsd_unit_test(fipsd_udp_crossing_dials_test test_node_crossing_dials.c)
target_compile_definitions(fipsd_udp_crossing_dials_test PRIVATE FIPSD_TEST_UDP=1)
add_dependencies(fipsd_tests fipsd_udp_crossing_dials_test)
add_test(NAME FipsdUdpTwoNodesCrossingDialsTest COMMAND fipsd_udp_crossing_dials_test)
set_tests_properties(FipsdUdpTwoNodesCrossingDialsTest PROPERTIES TIMEOUT 120 LABELS "fips;transport;node"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdUdpTwoNodesCrossingDialsTest OWNER fipsd TARGETS fipsd_udp_crossing_dials_test)

fipsd_unit_test(fipsd_udp_test test_udp.c)
add_dependencies(fipsd_tests fipsd_udp_test)
add_test(NAME FipsdUdpTest COMMAND fipsd_udp_test)
set_tests_properties(FipsdUdpTest PROPERTIES TIMEOUT 60 LABELS "fips;transport" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdUdpTest OWNER fipsd TARGETS fipsd_udp_test)

# Preserve shared probe sessions; retry only RTT-only no_echo (or, where a sample is
# required, a loopback quantized echo, UPSTREAM.md I-5), not other failures.
add_test(NAME FipsdInteropUdpHarnessTest COMMAND "${Python3_EXECUTABLE}" -B
  "${CMAKE_CURRENT_SOURCE_DIR}/test_udp_interop.py")
set_tests_properties(FipsdInteropUdpHarnessTest PROPERTIES TIMEOUT 10 LABELS "fips;unit;interop")
nostrc_describe_test(FipsdInteropUdpHarnessTest OWNER fipsd
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/udp_interop.py" "${CMAKE_CURRENT_SOURCE_DIR}/test_udp_interop.py")

# C<->Rust UDP interoperability gate (nostrc-f66sr.1): one fipsd and one daemon
# built from the libfips-core/UPSTREAM.md pin on loopback UDP, both role
# assignments per address family (IK link, tree/filters, XK session, datagrams,
# one session and one link rekey, clean shutdown). Skips (exit 77) without a
# Rust daemon. No Rust source is built or changed here: supply a binary built
# with `cargo build --locked --release --bin fips` in a scratch CARGO_TARGET_DIR.
# The longest socket the harness binds is Rust's `-ipv6tun` status sibling.
set(FIPSD_UDP_INTEROP_RUST_BINARY "$ENV{FIPSD_UDP_INTEROP_RUST_BINARY}" CACHE FILEPATH "fips daemon built from the pinned Rust source (libfips-core/UPSTREAM.md)")
foreach(family IN ITEMS 4 6)
  if(family EQUAL 4)
    set(_interop_test FipsdInteropUdpTest)
  else()
    set(_interop_test FipsdInteropUdpV6Test)
  endif()
  add_test(NAME ${_interop_test} COMMAND fipsd_test_tmpdir_run "/rrust6-ipv6tun.sock"
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/udp_interop.py"
    --fipsd "$<TARGET_FILE:fipsd>" --rust "${FIPSD_UDP_INTEROP_RUST_BINARY}" --family ${family})
  set_tests_properties(${_interop_test} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77
    LABELS "fips;transport;node;interop" ENVIRONMENT "FIPSD_WORKERS=2")
  nostrc_describe_test(${_interop_test} OWNER fipsd TARGETS fipsd fipsd_test_tmpdir_run fipsd_native_client_tool
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/udp_interop.py")
endforeach()
# Soup/WebSocket fixture adapted from Grotto with the FIPS gate's Jansson codec.
pkg_check_modules(FIPSD_RELAY_TEST REQUIRED IMPORTED_TARGET libsoup-3.0)
fipsd_unit_test(fipsd_nostr_runtime_test test_nostr_runtime.c)
target_sources(fipsd_nostr_runtime_test PRIVATE nostr_relay_fixture.c)
target_link_libraries(fipsd_nostr_runtime_test PRIVATE PkgConfig::FIPSD_RELAY_TEST)
add_dependencies(fipsd_tests fipsd_nostr_runtime_test)
add_test(NAME FipsdNostrRuntimeTest COMMAND fipsd_nostr_runtime_test)
set_tests_properties(FipsdNostrRuntimeTest PROPERTIES TIMEOUT 90 LABELS "fips;nostr;relay"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNostrRuntimeTest OWNER fipsd TARGETS fipsd_nostr_runtime_test)
# The node's own advert (nostrc-r4aeo): TCP/Tor endpoints from the real
# transport registry and driver, the onion after a fake control port's
# HS_DESC UPLOADED, one republish per change and none without one.
fipsd_unit_test(fipsd_nostr_adverts_test test_nostr_adverts.c)
target_sources(fipsd_nostr_adverts_test PRIVATE nostr_relay_fixture.c torctl_fixture.c)
target_link_libraries(fipsd_nostr_adverts_test PRIVATE PkgConfig::FIPSD_RELAY_TEST)
add_dependencies(fipsd_tests fipsd_nostr_adverts_test)
add_test(NAME FipsdNostrAdvertTest COMMAND fipsd_nostr_adverts_test)
set_tests_properties(FipsdNostrAdvertTest PROPERTIES TIMEOUT 90 LABELS "fips;nostr;relay;transport"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNostrAdvertTest OWNER fipsd TARGETS fipsd_nostr_adverts_test)

# Detected medium -> dispatcher -> Bundle selection with virtual time.
fipsd_unit_test(fipsd_node_path_medium_test test_node_path_medium.c)
target_sources(fipsd_node_path_medium_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/fakes/host.c")
target_include_directories(fipsd_node_path_medium_test PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests")
add_dependencies(fipsd_tests fipsd_node_path_medium_test)
add_test(NAME FipsdNodePathMediumTest COMMAND fipsd_node_path_medium_test)
set_tests_properties(FipsdNodePathMediumTest PROPERTIES TIMEOUT 60 LABELS "fips;node;transport"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNodePathMediumTest OWNER fipsd TARGETS fipsd_node_path_medium_test)
# L4 (nostrc-f66sr.10): queued plaintext is wiped at the end of each owner's
# tenure. The test links its own copy of handlers.c/session.c compiled with the
# FIPSD_TEST_HOOKS wipe seam (the fipsd_log_fault pattern); fipsd_lib and the
# daemon carry no seam.
add_library(fipsd_handlers_hooked OBJECT ../src/node/handlers/handlers.c ../src/node/handlers/session.c)
target_link_libraries(fipsd_handlers_hooked PRIVATE fipsd_lib)
target_compile_definitions(fipsd_handlers_hooked PRIVATE FIPSD_TEST_HOOKS=1)
set_target_properties(fipsd_handlers_hooked PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
apply_warnings(fipsd_handlers_hooked)
apply_sanitizers(fipsd_handlers_hooked)
fipsd_unit_test(fipsd_node_wipe_test test_node_wipe.c)
target_sources(fipsd_node_wipe_test PRIVATE $<TARGET_OBJECTS:fipsd_handlers_hooked>)
target_compile_definitions(fipsd_node_wipe_test PRIVATE FIPSD_TEST_HOOKS=1)
add_dependencies(fipsd_tests fipsd_node_wipe_test)
add_test(NAME FipsdNodeWipeTest COMMAND fipsd_node_wipe_test)
set_tests_properties(FipsdNodeWipeTest PROPERTIES TIMEOUT 300 LABELS "fips;node"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNodeWipeTest OWNER fipsd TARGETS fipsd_node_wipe_test)
# Real TCP v4/v6 runs of the entire node proof, including XK and datagrams.
foreach(family IN ITEMS 4 6)
  fipsd_unit_test(fipsd_tcp_two_nodes_v${family}_test test_node_two_nodes.c)
  target_compile_definitions(fipsd_tcp_two_nodes_v${family}_test PRIVATE FIPSD_TEST_TCP=1)
  if(family EQUAL 6)
    target_compile_definitions(fipsd_tcp_two_nodes_v${family}_test PRIVATE FIPSD_TEST_TCP_V6=1)
  endif()
  add_dependencies(fipsd_tests fipsd_tcp_two_nodes_v${family}_test)
  add_test(NAME FipsdTcpTwoNodesV${family}Test COMMAND fipsd_tcp_two_nodes_v${family}_test)
  set_tests_properties(FipsdTcpTwoNodesV${family}Test PROPERTIES TIMEOUT 300 LABELS "fips;transport;node"
    ENVIRONMENT "FIPSD_WORKERS=2")
  nostrc_describe_test(FipsdTcpTwoNodesV${family}Test OWNER fipsd TARGETS fipsd_tcp_two_nodes_v${family}_test)
endforeach()
# nostrc-085xw acceptance: a link rekey through saturated TCP data lanes.
fipsd_unit_test(fipsd_tcp_rekey_saturated_test test_tcp_rekey_saturated.c)
target_compile_definitions(fipsd_tcp_rekey_saturated_test PRIVATE FIPSD_TEST_TCP=1)
add_dependencies(fipsd_tests fipsd_tcp_rekey_saturated_test)
add_test(NAME FipsdTcpRekeySaturatedTest COMMAND fipsd_tcp_rekey_saturated_test)
set_tests_properties(FipsdTcpRekeySaturatedTest PROPERTIES TIMEOUT 120 LABELS "fips;transport;node"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdTcpRekeySaturatedTest OWNER fipsd TARGETS fipsd_tcp_rekey_saturated_test)
fipsd_unit_test(fipsd_tcp_test test_tcp.c)
add_dependencies(fipsd_tests fipsd_tcp_test)
add_test(NAME FipsdTcpTest COMMAND fipsd_tcp_test)
set_tests_properties(FipsdTcpTest PROPERTIES TIMEOUT 90 LABELS "fips;transport" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdTcpTest OWNER fipsd TARGETS fipsd_tcp_test)
set(FIPSD_TCP_INTEROP_RUST_BINARY "$ENV{FIPSD_TCP_INTEROP_RUST_BINARY}" CACHE FILEPATH "Optional pinned Rust fips daemon for TCP interop")
add_test(NAME FipsdTcpInteropTest COMMAND fipsd_test_tmpdir_run "/control.sock"
  "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tcp_interop.py"
  --fipsd "$<TARGET_FILE:fipsd>" --rust "${FIPSD_TCP_INTEROP_RUST_BINARY}")
set_tests_properties(FipsdTcpInteropTest PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77
  LABELS "fips;transport;interop" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdTcpInteropTest OWNER fipsd TARGETS fipsd fipsd_test_tmpdir_run
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tcp_interop.py")
# nostrc-3vl7v: both directions' TCP data lanes saturated through a 256 KiB/s
# relay across a link rekey; asserts the K flip, no re-handshake and no
# decrypt/replay/index/handshake-state counter on either side. One test per
# crossing winner: with C smaller C's msg1 rides its control lane past a full
# data lane; with Rust smaller Rust's msg1 waits in its own (upstream, FIFO)
# data lane. Upstream's handshake messages share that full FIFO lane (C's msg2
# answer or its msg1), and its refusals can cost whole rekey attempts, hence
# a Rust load just above the relay rate (still refusing) and longer deadlines.
foreach(_smaller IN ITEMS c rust)
  if(_smaller STREQUAL "c")
    set(_rekey_test FipsdTcpInteropRekeySaturatedTest)
    set(_rekey_args --extra-deadline 60 --rust-load 1.1)
  else()
    set(_rekey_test FipsdTcpInteropRekeySaturatedRustSmallerTest)
    set(_rekey_args --extra-deadline 120 --rust-load 1.1)
  endif()
  add_test(NAME ${_rekey_test} COMMAND fipsd_test_tmpdir_run "/r-ipv6tun.sock"
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tcp_interop.py"
    --fipsd "$<TARGET_FILE:fipsd>" --rust "${FIPSD_TCP_INTEROP_RUST_BINARY}"
    --scenario rekey-saturated --smaller ${_smaller} ${_rekey_args})
  set_tests_properties(${_rekey_test} PROPERTIES TIMEOUT 400 SKIP_RETURN_CODE 77
    LABELS "fips;transport;interop" ENVIRONMENT "FIPSD_WORKERS=2")
  nostrc_describe_test(${_rekey_test} OWNER fipsd TARGETS fipsd fipsd_test_tmpdir_run
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tcp_interop.py")
endforeach()
# Traversal vectors, complete table, loopback STUN and real relay -> IK proof.
fipsd_unit_test(fipsd_nostr_traversal_test test_nostr_traversal.c)
target_sources(fipsd_nostr_traversal_test PRIVATE stun_fixture.c nostr_relay_fixture.c)
target_link_libraries(fipsd_nostr_traversal_test PRIVATE PkgConfig::FIPSD_RELAY_TEST)
add_dependencies(fipsd_tests fipsd_nostr_traversal_test)
foreach(scenario IN ITEMS codecs states stun traverse guards)
  add_test(NAME FipsdNostrTraversal_${scenario} COMMAND fipsd_nostr_traversal_test ${scenario})
  set_tests_properties(FipsdNostrTraversal_${scenario} PROPERTIES TIMEOUT 90
    LABELS "fips;nostr;traversal" ENVIRONMENT "FIPSD_WORKERS=2")
  nostrc_describe_test(FipsdNostrTraversal_${scenario} OWNER fipsd TARGETS fipsd_nostr_traversal_test)
endforeach()
# IPv6 shim (nostrc-9nxwk.2): upstream vector tests, a two-node loopback proof
# over the TUN ops seam (fake devices on socketpairs) and an opt-in privileged
# smoke on a real device.
fipsd_unit_test(fipsd_ipv6_codec_test test_ipv6_codec.c)
fipsd_unit_test(fipsd_ipv6_icmp_test test_ipv6_icmp.c)
fipsd_unit_test(fipsd_ipv6_firewall_test test_ipv6_firewall.c)
fipsd_unit_test(fipsd_ipv6_two_nodes_test test_ipv6_two_nodes.c)
fipsd_unit_test(fipsd_ipv6_privileged_test test_ipv6_privileged.c)
add_dependencies(fipsd_tests fipsd_ipv6_codec_test fipsd_ipv6_icmp_test fipsd_ipv6_firewall_test
  fipsd_ipv6_two_nodes_test fipsd_ipv6_privileged_test)
add_test(NAME FipsdIpv6CodecTest COMMAND fipsd_ipv6_codec_test)
add_test(NAME FipsdIpv6IcmpTest COMMAND fipsd_ipv6_icmp_test)
add_test(NAME FipsdIpv6FirewallTest COMMAND fipsd_ipv6_firewall_test)
add_test(NAME FipsdIpv6TwoNodesTest COMMAND fipsd_ipv6_two_nodes_test)
add_test(NAME FipsdIpv6PrivilegedSmokeTest COMMAND fipsd_ipv6_privileged_test)
foreach(_ipv6_test FipsdIpv6CodecTest FipsdIpv6IcmpTest FipsdIpv6FirewallTest)
  set_tests_properties(${_ipv6_test} PROPERTIES TIMEOUT 120 LABELS "fips;tun;ipv6")
endforeach()
set_tests_properties(FipsdIpv6TwoNodesTest PROPERTIES TIMEOUT 300 LABELS "fips;tun;ipv6;node"
  ENVIRONMENT "FIPSD_WORKERS=2")
set_tests_properties(FipsdIpv6PrivilegedSmokeTest PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
  LABELS "fips;tun;ipv6;privileged" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdIpv6CodecTest OWNER fipsd TARGETS fipsd_ipv6_codec_test)
nostrc_describe_test(FipsdIpv6IcmpTest OWNER fipsd TARGETS fipsd_ipv6_icmp_test)
nostrc_describe_test(FipsdIpv6FirewallTest OWNER fipsd TARGETS fipsd_ipv6_firewall_test)
nostrc_describe_test(FipsdIpv6TwoNodesTest OWNER fipsd TARGETS fipsd_ipv6_two_nodes_test)
nostrc_describe_test(FipsdIpv6PrivilegedSmokeTest OWNER fipsd TARGETS fipsd_ipv6_privileged_test)

# Native datagram API (nostrc-9nxwk.4): the pure codec and registry, the
# socket pair and descriptor hand-off with the C client library, the
# subsystem's line-protocol scenarios on a live node, and a two-node loopback
# exchange between two C clients through the API in both directions.
fipsd_unit_test(fipsd_native_protocol_test test_native_protocol.c)
fipsd_unit_test(fipsd_native_registry_test test_native_registry.c)
fipsd_unit_test(fipsd_native_seqpacket_test test_native_seqpacket.c)
target_link_libraries(fipsd_native_seqpacket_test PRIVATE fipsnative_static)
find_package(Threads REQUIRED)
target_link_libraries(fipsd_native_seqpacket_test PRIVATE Threads::Threads)
add_dependencies(fipsd_tests fipsd_native_protocol_test fipsd_native_registry_test fipsd_native_seqpacket_test)
add_test(NAME FipsdNativeProtocolTest COMMAND fipsd_native_protocol_test)
add_test(NAME FipsdNativeRegistryTest COMMAND fipsd_native_registry_test)
add_test(NAME FipsdNativeSeqpacketTest COMMAND fipsd_native_seqpacket_test)
set_tests_properties(FipsdNativeProtocolTest FipsdNativeRegistryTest PROPERTIES TIMEOUT 60 LABELS "fips;unit;native")
set_tests_properties(FipsdNativeSeqpacketTest PROPERTIES TIMEOUT 120 LABELS "fips;unit;native" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNativeProtocolTest OWNER fipsd TARGETS fipsd_native_protocol_test)
nostrc_describe_test(FipsdNativeRegistryTest OWNER fipsd TARGETS fipsd_native_registry_test)
nostrc_describe_test(FipsdNativeSeqpacketTest OWNER fipsd TARGETS fipsd_native_seqpacket_test)
fipsd_unit_test(fipsd_native_api_test test_native_api.c)
fipsd_unit_test(fipsd_native_two_nodes_test test_native_two_nodes.c)
target_link_libraries(fipsd_native_two_nodes_test PRIVATE fipsnative_static)
# nostrc-pxwaj: a burst larger than every bounded originating queue, zero drops.
fipsd_unit_test(fipsd_native_burst_test test_native_burst.c)
target_link_libraries(fipsd_native_burst_test PRIVATE fipsnative_static Threads::Threads)
add_dependencies(fipsd_tests fipsd_native_api_test fipsd_native_two_nodes_test fipsd_native_burst_test)
add_test(NAME FipsdNativeApiTest COMMAND fipsd_native_api_test)
add_test(NAME FipsdNativeTwoNodesTest COMMAND fipsd_native_two_nodes_test)
add_test(NAME FipsdNativeBurstTest COMMAND fipsd_native_burst_test)
set_tests_properties(FipsdNativeApiTest FipsdNativeTwoNodesTest FipsdNativeBurstTest PROPERTIES TIMEOUT 300 LABELS "fips;node;native"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNativeApiTest OWNER fipsd TARGETS fipsd_native_api_test)
nostrc_describe_test(FipsdNativeTwoNodesTest OWNER fipsd TARGETS fipsd_native_two_nodes_test)
nostrc_describe_test(FipsdNativeBurstTest OWNER fipsd TARGETS fipsd_native_burst_test)
# nostrc-e3ps5: a client that never asks for framing (upstream's Rust client)
# behind a deliberately slow reader loses nothing: SOCK_SEQPACKET on Linux,
# the macOS datagram feeder's NOTE_LOWAT park, its overflow fallback,
# teardown and the FIPSD_NATIVE_DGRAM_PARK=0 switch. FreeBSD skips (77).
fipsd_unit_test(fipsd_native_dgram_park_test test_native_dgram_park.c)
target_link_libraries(fipsd_native_dgram_park_test PRIVATE Threads::Threads)
# The kernel behaviour that park relies on, characterised without fipsd or
# libgo (kqueue EVFILT_WRITE on AF_UNIX SOCK_DGRAM; macOS only, 77 elsewhere).
add_executable(fipsd_native_dgram_kqueue_test test_native_dgram_kqueue.c)
set_target_properties(fipsd_native_dgram_kqueue_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(fipsd_native_dgram_kqueue_test PRIVATE Threads::Threads)
apply_warnings(fipsd_native_dgram_kqueue_test)
apply_sanitizers(fipsd_native_dgram_kqueue_test)
add_dependencies(fipsd_tests fipsd_native_dgram_park_test fipsd_native_dgram_kqueue_test)
add_test(NAME FipsdNativeDgramParkTest COMMAND fipsd_native_dgram_park_test)
add_test(NAME FipsdNativeDgramKqueueTest COMMAND fipsd_native_dgram_kqueue_test)
set_tests_properties(FipsdNativeDgramParkTest PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77 LABELS "fips;node;native"
  ENVIRONMENT "FIPSD_WORKERS=2")
set_tests_properties(FipsdNativeDgramKqueueTest PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "fips;unit;native")
nostrc_describe_test(FipsdNativeDgramParkTest OWNER fipsd TARGETS fipsd_native_dgram_park_test)
nostrc_describe_test(FipsdNativeDgramKqueueTest OWNER fipsd TARGETS fipsd_native_dgram_kqueue_test)
# The harness driver for the C<->Rust gate below: a C client over libfipsnative.
add_executable(fipsd_native_client_tool native_client_tool.c)
set_target_properties(fipsd_native_client_tool PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(fipsd_native_client_tool PRIVATE fipsnative_static)
apply_warnings(fipsd_native_client_tool)
apply_sanitizers(fipsd_native_client_tool)
add_dependencies(fipsd_tests fipsd_native_client_tool)
# The C->Rust application datagram closes the nostrc-f66sr.1 gap: the harness
# drives fipsd_native_client_tool at fipsd's native API and reads its answer
# off the Rust daemon's flow. FIPSD_UDP_INTEROP_RUST_ECHO may name upstream's
# native-echo client built from the pinned tree (Rust client -> fipsd).
set(FIPSD_UDP_INTEROP_RUST_ECHO "$ENV{FIPSD_UDP_INTEROP_RUST_ECHO}" CACHE FILEPATH "Optional native-echo client built from the pinned Rust tree")
foreach(_interop_test FipsdInteropUdpTest FipsdInteropUdpV6Test)
  set_tests_properties(${_interop_test} PROPERTIES ENVIRONMENT
    "FIPSD_WORKERS=2;FIPSD_NATIVE_CLIENT_TOOL=${CMAKE_CURRENT_BINARY_DIR}/fipsd_native_client_tool;FIPSD_UDP_INTEROP_RUST_ECHO=${FIPSD_UDP_INTEROP_RUST_ECHO}")
endforeach()
# nostrc-f66sr.2: small real-UDP topology gates and optional mixed-tree soak.
fipsd_unit_test(fipsd_node_multi_test test_node_multi.c)
add_dependencies(fipsd_tests fipsd_node_multi_test)
foreach(topology IN ITEMS Line Star Ring FullMesh ReturnRoute)
  add_test(NAME FipsdNodeMulti${topology}Test COMMAND fipsd_node_multi_test ${topology})
  set_tests_properties(FipsdNodeMulti${topology}Test PROPERTIES TIMEOUT 300
    LABELS "fips;node;transport;multinode" ENVIRONMENT "FIPSD_WORKERS=2")
  nostrc_describe_test(FipsdNodeMulti${topology}Test OWNER fipsd TARGETS fipsd_node_multi_test)
endforeach()
add_test(NAME FipsdInteropMultiTest COMMAND fipsd_test_tmpdir_run "/m4-ipv6tun.sock"
  "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/udp_interop.py"
  --fipsd "$<TARGET_FILE:fipsd>" --rust "${FIPSD_UDP_INTEROP_RUST_BINARY}" --topology all)
set_tests_properties(FipsdInteropMultiTest PROPERTIES TIMEOUT 1200 SKIP_RETURN_CODE 77
  LABELS "fips;node;interop;multinode" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdInteropMultiTest OWNER fipsd TARGETS fipsd fipsd_test_tmpdir_run
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/udp_interop.py")
option(FIPSD_ENABLE_INTEROP_SOAK "Enable the opt-in ten-minute mixed UDP ring soak" OFF)
add_test(NAME FipsdInteropSoakTest COMMAND fipsd_test_tmpdir_run "/m4-ipv6tun.sock"
  "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/udp_interop.py"
  --fipsd "$<TARGET_FILE:fipsd>" --rust "${FIPSD_UDP_INTEROP_RUST_BINARY}"
  --topology ring --soak-minutes 10)
set_tests_properties(FipsdInteropSoakTest PROPERTIES TIMEOUT 1500 SKIP_RETURN_CODE 77
  LABELS "fips;node;interop;soak" ENVIRONMENT "FIPSD_WORKERS=2")
# The shared harness drives the libfipsnative client in every mode.
foreach(_interop_test FipsdInteropMultiTest FipsdInteropSoakTest)
  set_tests_properties(${_interop_test} PROPERTIES ENVIRONMENT
    "FIPSD_WORKERS=2;FIPSD_NATIVE_CLIENT_TOOL=${CMAKE_CURRENT_BINARY_DIR}/fipsd_native_client_tool;FIPSD_UDP_INTEROP_RUST_ECHO=${FIPSD_UDP_INTEROP_RUST_ECHO}")
endforeach()
if(NOT FIPSD_ENABLE_INTEROP_SOAK)
  set_tests_properties(FipsdInteropSoakTest PROPERTIES DISABLED TRUE)
endif()
nostrc_describe_test(FipsdInteropSoakTest OWNER fipsd TARGETS fipsd fipsd_test_tmpdir_run
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/udp_interop.py")

fipsd_unit_test(fipsd_coord_warm_test test_coord_warm.c)
target_sources(fipsd_coord_warm_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/fakes/host.c")
target_include_directories(fipsd_coord_warm_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests")
add_dependencies(fipsd_tests fipsd_coord_warm_test)
add_test(NAME FipsdCoordWarmTest COMMAND fipsd_coord_warm_test)
set_tests_properties(FipsdCoordWarmTest PROPERTIES TIMEOUT 30 LABELS "fips;node;multinode")
nostrc_describe_test(FipsdCoordWarmTest OWNER fipsd TARGETS fipsd_coord_warm_test)

fipsd_unit_test(fipsd_dp_cache_test test_dp_cache.c)
target_sources(fipsd_dp_cache_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/fakes/host.c")
target_include_directories(fipsd_dp_cache_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests")
add_dependencies(fipsd_tests fipsd_dp_cache_test)
add_test(NAME FipsdDataplaneCacheTest COMMAND fipsd_dp_cache_test)
set_tests_properties(FipsdDataplaneCacheTest PROPERTIES TIMEOUT 120 LABELS "fips;node")
nostrc_describe_test(FipsdDataplaneCacheTest OWNER fipsd TARGETS fipsd_dp_cache_test)

# The .fips DNS responder (nostrc-9nxwk.3): codec vectors, host map and
# reloader, resolver tables and loopback sockets, and the end-to-end gate
# (a real AAAA query over UDP; the npub in show_identity_cache).
fipsd_unit_test(fipsd_dns_msg_test test_dns_msg.c)
fipsd_unit_test(fipsd_dns_hosts_test test_dns_hosts.c)
fipsd_unit_test(fipsd_dns_resolver_test test_dns_resolver.c)
fipsd_unit_test(fipsd_dns_e2e_test test_dns_e2e.c)
add_dependencies(fipsd_tests fipsd_dns_msg_test fipsd_dns_hosts_test fipsd_dns_resolver_test fipsd_dns_e2e_test)
add_test(NAME FipsdDnsMsgTest COMMAND fipsd_dns_msg_test)
add_test(NAME FipsdDnsHostsTest COMMAND fipsd_dns_hosts_test)
add_test(NAME FipsdDnsResolverTest COMMAND fipsd_dns_resolver_test)
set_tests_properties(FipsdDnsMsgTest FipsdDnsHostsTest FipsdDnsResolverTest PROPERTIES TIMEOUT 60 LABELS "fips;unit;dns")
add_test(NAME FipsdDnsEndToEndTest COMMAND fipsd_dns_e2e_test)
add_test(NAME FipsdDnsEndToEndV4Test COMMAND fipsd_dns_e2e_test v4)
add_test(NAME FipsdDnsMeshFilterTest COMMAND fipsd_dns_e2e_test mesh)
set_tests_properties(FipsdDnsEndToEndTest FipsdDnsEndToEndV4Test FipsdDnsMeshFilterTest PROPERTIES TIMEOUT 120 LABELS "fips;node;dns"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdDnsMsgTest OWNER fipsd TARGETS fipsd_dns_msg_test)
nostrc_describe_test(FipsdDnsHostsTest OWNER fipsd TARGETS fipsd_dns_hosts_test)
nostrc_describe_test(FipsdDnsResolverTest OWNER fipsd TARGETS fipsd_dns_resolver_test)
nostrc_describe_test(FipsdDnsEndToEndTest OWNER fipsd TARGETS fipsd_dns_e2e_test)
nostrc_describe_test(FipsdDnsEndToEndV4Test OWNER fipsd TARGETS fipsd_dns_e2e_test)
nostrc_describe_test(FipsdDnsMeshFilterTest OWNER fipsd TARGETS fipsd_dns_e2e_test)
# fipsctl (nostrc-iq1n9.1): the C control client. Unit tests over fipsctl_lib
# (no libgo), an end-to-end run of every documented subcommand against two
# fipsd daemons, and the cross-implementation gate: the C fipsctl against the
# pinned Rust daemon and the pinned Rust fipsctl against fipsd, byte-compared.
add_executable(fipsd_fipsctl_test test_fipsctl.c test_tmpdir.c)
target_link_libraries(fipsd_fipsctl_test PRIVATE fipsctl_lib)
find_package(Threads REQUIRED)
target_link_libraries(fipsd_fipsctl_test PRIVATE Threads::Threads)
set_target_properties(fipsd_fipsctl_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_compile_definitions(fipsd_fipsctl_test PRIVATE
  FIPSD_CONTROL_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/control_golden")
apply_warnings(fipsd_fipsctl_test)
apply_sanitizers(fipsd_fipsctl_test)
add_dependencies(fipsd_tests fipsctl fipsd_fipsctl_test)
add_test(NAME FipsdFipsctlTest COMMAND fipsd_fipsctl_test)
set_tests_properties(FipsdFipsctlTest PROPERTIES TIMEOUT 60 LABELS "fips;unit;control;fipsctl")
nostrc_describe_test(FipsdFipsctlTest OWNER fipsd TARGETS fipsd_fipsctl_test
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/control_golden")
add_test(NAME FipsdFipsctlE2eTest COMMAND fipsd_test_tmpdir_run "/b-ipv6tun.sock"
  "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fipsctl_e2e.py"
  --fipsd "$<TARGET_FILE:fipsd>" --fipsctl "$<TARGET_FILE:fipsctl>")
set_tests_properties(FipsdFipsctlE2eTest PROPERTIES TIMEOUT 300 LABELS "fips;control;fipsctl"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdFipsctlE2eTest OWNER fipsd TARGETS fipsd fipsctl fipsd_test_tmpdir_run
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/fipsctl_e2e.py" "${CMAKE_CURRENT_SOURCE_DIR}/udp_interop.py")
# Needs the Rust daemon (FIPSD_UDP_INTEROP_RUST_BINARY) and the Rust fipsctl
# (FIPSD_FIPSCTL_EXECUTABLE, or a cargo build from FIPSD_UPSTREAM_CONTROL_DIR);
# SKIP (77) without either.
add_test(NAME FipsdFipsctlInteropTest COMMAND fipsd_test_tmpdir_run "/rrust-ipv6tun.sock"
  "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fipsctl_interop.py"
  --fipsd "$<TARGET_FILE:fipsd>" --fipsctl "$<TARGET_FILE:fipsctl>"
  --rust "${FIPSD_UDP_INTEROP_RUST_BINARY}" --upstream "${FIPSD_UPSTREAM_CONTROL_DIR}"
  --rust-fipsctl "${FIPSD_FIPSCTL_EXECUTABLE}"
  --cargo-target "${CMAKE_CURRENT_BINARY_DIR}/cargo-control")
set_tests_properties(FipsdFipsctlInteropTest PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77
  LABELS "fips;control;fipsctl;interop" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdFipsctlInteropTest OWNER fipsd TARGETS fipsd fipsctl fipsd_test_tmpdir_run
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/fipsctl_interop.py" "${CMAKE_CURRENT_SOURCE_DIR}/fipsctl_e2e.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/udp_interop.py")
# Performance is explicit opt-in, not part of ordinary ctest or installed tools.
if(FIPS_ENABLE_BENCH)
  add_executable(fipsd_perf_native perf_native.c)
  target_link_libraries(fipsd_perf_native PRIVATE fipsnative_static Threads::Threads)
  set_target_properties(fipsd_perf_native PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  apply_warnings(fipsd_perf_native)
  apply_sanitizers(fipsd_perf_native)
  option(FIPSD_ENABLE_PERF_TEST "Enable the loopback performance workload in ctest" OFF)
  add_test(NAME FipsdPerfLoopbackTest
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/perf_loopback.py"
      --fipsd "$<TARGET_FILE:fipsd>" --rust "${FIPSD_UDP_INTEROP_RUST_BINARY}"
      --native-tool "$<TARGET_FILE:fipsd_perf_native>"
      --work-dir "${CMAKE_CURRENT_BINARY_DIR}/perf-tmp")
  set_tests_properties(FipsdPerfLoopbackTest PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77
    LABELS "fips;perf;benchmark" DISABLED TRUE)
  if(FIPSD_ENABLE_PERF_TEST)
    set_tests_properties(FipsdPerfLoopbackTest PROPERTIES DISABLED FALSE)
  endif()
  nostrc_describe_test(FipsdPerfLoopbackTest OWNER fipsd TARGETS fipsd fipsd_perf_native
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/perf_loopback.py" "${CMAKE_CURRENT_SOURCE_DIR}/udp_interop.py")
endif()
# SOCKS5 client, Tor and Nym transports (nostrc-gfi8h.4): byte vectors and
# loopback fakes standing in for Tor's SocksPort/ControlPort and for
# nym-socks5-client; a Tor two-node node proof through the fake proxy.
fipsd_unit_test(fipsd_socks5_test test_socks5.c)
target_sources(fipsd_socks5_test PRIVATE socks5_fixture.c)
add_dependencies(fipsd_tests fipsd_socks5_test)
add_test(NAME FipsdSocks5Test COMMAND fipsd_socks5_test)
set_tests_properties(FipsdSocks5Test PROPERTIES TIMEOUT 60 LABELS "fips;transport" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdSocks5Test OWNER fipsd TARGETS fipsd_socks5_test)
fipsd_unit_test(fipsd_tor_test test_tor.c)
target_sources(fipsd_tor_test PRIVATE socks5_fixture.c torctl_fixture.c)
add_dependencies(fipsd_tests fipsd_tor_test)
add_test(NAME FipsdTorTest COMMAND fipsd_tor_test)
set_tests_properties(FipsdTorTest PROPERTIES TIMEOUT 120 LABELS "fips;transport" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdTorTest OWNER fipsd TARGETS fipsd_tor_test)
fipsd_unit_test(fipsd_tor_two_nodes_test test_node_two_nodes.c)
target_sources(fipsd_tor_two_nodes_test PRIVATE socks5_fixture.c)
target_compile_definitions(fipsd_tor_two_nodes_test PRIVATE FIPSD_TEST_TCP=1 FIPSD_TEST_TOR=1)
add_dependencies(fipsd_tests fipsd_tor_two_nodes_test)
add_test(NAME FipsdTorTwoNodesTest COMMAND fipsd_tor_two_nodes_test)
set_tests_properties(FipsdTorTwoNodesTest PROPERTIES TIMEOUT 300 LABELS "fips;transport;node"
  ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdTorTwoNodesTest OWNER fipsd TARGETS fipsd_tor_two_nodes_test)
fipsd_unit_test(fipsd_nym_test test_nym.c)
target_sources(fipsd_nym_test PRIVATE socks5_fixture.c)
add_dependencies(fipsd_tests fipsd_nym_test)
add_test(NAME FipsdNymTest COMMAND fipsd_nym_test)
set_tests_properties(FipsdNymTest PROPERTIES TIMEOUT 60 LABELS "fips;transport" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdNymTest OWNER fipsd TARGETS fipsd_nym_test)
# Opt-in C<->Rust link over real onion services through a local tor
# (docs/how-to/deploy-tor-onion.md); SKIP 77 without both binaries.
set(FIPSD_TOR_BINARY "$ENV{FIPSD_TOR_BINARY}" CACHE FILEPATH "Optional tor daemon for the C<->Rust onion interop")
add_test(NAME FipsdTorInteropTest COMMAND fipsd_test_tmpdir_run "/control.sock"
  "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tor_interop.py"
  --fipsd "$<TARGET_FILE:fipsd>" --rust "${FIPSD_UDP_INTEROP_RUST_BINARY}" --tor "${FIPSD_TOR_BINARY}")
set_tests_properties(FipsdTorInteropTest PROPERTIES TIMEOUT 1200 SKIP_RETURN_CODE 77
  LABELS "fips;transport;interop;tor" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdTorInteropTest OWNER fipsd TARGETS fipsd fipsd_test_tmpdir_run
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tor_interop.py" "${CMAKE_CURRENT_SOURCE_DIR}/udp_interop.py")
# Opt-in: the Rust daemon finds fipsd's onion from its Nostr advert alone, on
# a local stdlib relay (nostrc-r4aeo); SKIP 77 without both binaries.
add_test(NAME FipsdAdvertTorInteropTest COMMAND fipsd_test_tmpdir_run "/control.sock"
  "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/advert_interop.py"
  --fipsd "$<TARGET_FILE:fipsd>" --rust "${FIPSD_UDP_INTEROP_RUST_BINARY}" --tor "${FIPSD_TOR_BINARY}")
set_tests_properties(FipsdAdvertTorInteropTest PROPERTIES TIMEOUT 1200 SKIP_RETURN_CODE 77
  LABELS "fips;nostr;interop;tor" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdAdvertTorInteropTest OWNER fipsd TARGETS fipsd fipsd_test_tmpdir_run
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/advert_interop.py" "${CMAKE_CURRENT_SOURCE_DIR}/tor_interop.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/udp_interop.py" "${CMAKE_CURRENT_SOURCE_DIR}/../../Testing/fips-chaos/services/relay.py")

# fips-gateway (nostrc-iq1n9.3): one executable, a suite per module, each its
# own test. The portable core runs on every platform; Linux-only cases are
# compiled in on Linux. The root-only netns smoke is registered with the
# executable (fipsd/CMakeLists.txt), which exists on Linux alone.
fipsd_unit_test(fipsd_gateway_test test_gateway.c)
target_sources(fipsd_gateway_test PRIVATE
  test_gateway_pool.c test_gateway_conntrack.c test_gateway_state.c test_gateway_nat.c
  test_gateway_net.c test_gateway_dns.c test_gateway_control.c test_gateway_shutdown.c)
target_link_libraries(fipsd_gateway_test PRIVATE fipsd_gateway_core)
target_compile_definitions(fipsd_gateway_test PRIVATE
  "FIPSD_GATEWAY_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/gateway_golden\"")
add_dependencies(fipsd_tests fipsd_gateway_test)
file(GLOB _fipsd_gateway_goldens CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gateway_golden/*.json")
foreach(_suite Pool:pool Conntrack:conntrack State:state Nat:nat Net:net Dns:dns Control:control Shutdown:shutdown)
  string(REPLACE ":" ";" _pair "${_suite}")
  list(GET _pair 0 _name)
  list(GET _pair 1 _arg)
  add_test(NAME FipsdGateway${_name}Test COMMAND fipsd_gateway_test ${_arg})
  set_tests_properties(FipsdGateway${_name}Test PROPERTIES TIMEOUT 180 LABELS "fips;unit;gateway"
    ENVIRONMENT "FIPSD_WORKERS=2")
  nostrc_describe_test(FipsdGateway${_name}Test OWNER fipsd TARGETS fipsd_gateway_test
    INPUTS ${_fipsd_gateway_goldens})
endforeach()

# mDNS LAN rendezvous (nostrc-9nxwk.5): codec vectors, upstream tests.rs, the
# browser/responder tables and the LAN candidate set; then two nodes with no
# configured peers discovering each other over an in-memory multicast link.
fipsd_unit_test(fipsd_mdns_test test_mdns.c)
add_dependencies(fipsd_tests fipsd_mdns_test)
add_test(NAME FipsdMdnsTest COMMAND fipsd_mdns_test)
set_tests_properties(FipsdMdnsTest PROPERTIES TIMEOUT 60 LABELS "fips;unit;mdns")
nostrc_describe_test(FipsdMdnsTest OWNER fipsd TARGETS fipsd_mdns_test)
fipsd_unit_test(fipsd_mdns_two_nodes_test test_mdns_two_nodes.c)
target_compile_definitions(fipsd_mdns_two_nodes_test PRIVATE FIPSD_TEST_UDP=1)
add_dependencies(fipsd_tests fipsd_mdns_two_nodes_test)
add_test(NAME FipsdMdnsTwoNodesTest COMMAND fipsd_mdns_two_nodes_test)
set_tests_properties(FipsdMdnsTwoNodesTest PROPERTIES TIMEOUT 120 LABELS "fips;node;mdns")
nostrc_describe_test(FipsdMdnsTwoNodesTest OWNER fipsd TARGETS fipsd_mdns_two_nodes_test)
fipsd_unit_test(fipsd_mdns_multipath_test test_mdns_multipath.c)
target_compile_definitions(fipsd_mdns_multipath_test PRIVATE FIPSD_TEST_UDP=1)
add_dependencies(fipsd_tests fipsd_mdns_multipath_test)
add_test(NAME FipsdMdnsMultipathTest COMMAND fipsd_mdns_multipath_test)
set_tests_properties(FipsdMdnsMultipathTest PROPERTIES TIMEOUT 120 LABELS "fips;node;mdns")
nostrc_describe_test(FipsdMdnsMultipathTest OWNER fipsd TARGETS fipsd_mdns_multipath_test)

# Ethernet vectors and injected watcher transitions; no raw-socket privileges.
fipsd_unit_test(fipsd_ethernet_test test_ethernet.c)
add_dependencies(fipsd_tests fipsd_ethernet_test)
add_test(NAME FipsdEthernetTest COMMAND fipsd_ethernet_test)
set_tests_properties(FipsdEthernetTest PROPERTIES TIMEOUT 30 LABELS "fips;transport;ethernet")
nostrc_describe_test(FipsdEthernetTest OWNER fipsd TARGETS fipsd_ethernet_test)

add_test(NAME FipsdEthernetPrivilegedSmokeTest
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ethernet_privileged.py"
    --fipsd "$<TARGET_FILE:fipsd>" --native-tool "$<TARGET_FILE:fipsd_native_client_tool>")
set_tests_properties(FipsdEthernetPrivilegedSmokeTest PROPERTIES TIMEOUT 240
  SKIP_RETURN_CODE 77 LABELS "fips;transport;ethernet;privileged" ENVIRONMENT "FIPSD_WORKERS=2")
nostrc_describe_test(FipsdEthernetPrivilegedSmokeTest OWNER fipsd TARGETS fipsd fipsd_native_client_tool
  INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/ethernet_privileged.py")

add_test(NAME FipsdEthernetBpfSmokeTest COMMAND fipsd_ethernet_test --bpf)
set_tests_properties(FipsdEthernetBpfSmokeTest PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77
  LABELS "fips;transport;ethernet;privileged")
nostrc_describe_test(FipsdEthernetBpfSmokeTest OWNER fipsd TARGETS fipsd_ethernet_test)
# BLE transport core over the simulated radio (nostrc-gfi8h.6.1). The option
# is declared after this directory is read, so on a first configure it is not
# yet defined here: that is its default, ON; an explicit OFF is in the cache.
if(NOT DEFINED FIPSD_ENABLE_BLE OR FIPSD_ENABLE_BLE)
  fipsd_unit_test(fipsd_ble_test test_ble.c)
  add_dependencies(fipsd_tests fipsd_ble_test)
  add_test(NAME FipsdBleTest COMMAND fipsd_ble_test)
  set_tests_properties(FipsdBleTest PROPERTIES TIMEOUT 300 LABELS "fips;transport" ENVIRONMENT "FIPSD_WORKERS=2")
  nostrc_describe_test(FipsdBleTest OWNER fipsd TARGETS fipsd_ble_test)
  # Two nodes over the simulated radio: SDU channels, and the fd fast path.
  foreach(mode IN ITEMS Sdu Fd)
    fipsd_unit_test(fipsd_ble_two_nodes_${mode}_test test_ble_two_nodes.c)
    if(mode STREQUAL "Fd")
      target_compile_definitions(fipsd_ble_two_nodes_${mode}_test PRIVATE FIPSD_TEST_BLE_FD=1)
    endif()
    add_dependencies(fipsd_tests fipsd_ble_two_nodes_${mode}_test)
    add_test(NAME FipsdBleTwoNodes${mode}Test COMMAND fipsd_ble_two_nodes_${mode}_test)
    set_tests_properties(FipsdBleTwoNodes${mode}Test PROPERTIES TIMEOUT 300 LABELS "fips;transport;node"
      ENVIRONMENT "FIPSD_WORKERS=2")
    nostrc_describe_test(FipsdBleTwoNodes${mode}Test OWNER fipsd TARGETS fipsd_ble_two_nodes_${mode}_test)
  endforeach()
  # The BlueZ backend (nostrc-gfi8h.6.2): its pure half wherever it builds,
  # and the opt-in real run over two virtual controllers (btvirt), C<->C and,
  # with a pinned Rust daemon, C<->Rust (root, FIPSD_BLE_BLUEZ_SMOKE=1).
  if(FIPSD_ENABLE_BLE_BLUEZ)
    fipsd_unit_test(fipsd_ble_bluez_test test_ble_bluez.c)
    add_dependencies(fipsd_tests fipsd_ble_bluez_test)
    add_test(NAME FipsdBleBluezTest COMMAND fipsd_ble_bluez_test)
    set_tests_properties(FipsdBleBluezTest PROPERTIES TIMEOUT 30 LABELS "fips;transport;unit")
    nostrc_describe_test(FipsdBleBluezTest OWNER fipsd TARGETS fipsd_ble_bluez_test)
    set(FIPSD_BLE_BLUEZ_RUST_BINARY "$ENV{FIPSD_BLE_BLUEZ_RUST_BINARY}" CACHE FILEPATH
      "Optional pinned Rust fips daemon for the BlueZ smoke's C<->Rust scenarios")
    add_test(NAME FipsdBleBluezSmokeTest
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ble_bluez_smoke.py"
        --fipsd "$<TARGET_FILE:fipsd>" --native-tool "$<TARGET_FILE:fipsd_native_client_tool>"
        --rust "${FIPSD_BLE_BLUEZ_RUST_BINARY}")
    set_tests_properties(FipsdBleBluezSmokeTest PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77
      LABELS "fips;transport;ble;privileged" ENVIRONMENT "FIPSD_WORKERS=2")
    nostrc_describe_test(FipsdBleBluezSmokeTest OWNER fipsd TARGETS fipsd fipsd_native_client_tool
      INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/ble_bluez_smoke.py" "${CMAKE_CURRENT_SOURCE_DIR}/ble_bluez_gatt_peer.py")
  endif()
endif()
# BLE agent socket (nostrc-gfi8h.6.3): the protocol against upstream's own
# encoder, fipsd's backend against a scripted agent, and end to end against
# the agent's real session code (src/ble-agent/session.c) over mock radios.
# Portable: built wherever the BLE core is (not Windows: AF_UNIX agent socket).
if(NOT DEFINED FIPSD_ENABLE_BLE OR FIPSD_ENABLE_BLE)
  fipsd_unit_test(fipsd_ble_agent_test test_ble_agent.c)
  target_sources(fipsd_ble_agent_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/ble-agent/session.c")
  target_include_directories(fipsd_ble_agent_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src/ble-agent")
  add_dependencies(fipsd_tests fipsd_ble_agent_test)
  add_test(NAME FipsdBleAgentTest COMMAND fipsd_ble_agent_test)
  set_tests_properties(FipsdBleAgentTest PROPERTIES TIMEOUT 300 LABELS "fips;transport" ENVIRONMENT "FIPSD_WORKERS=2")
  nostrc_describe_test(FipsdBleAgentTest OWNER fipsd TARGETS fipsd_ble_agent_test)
endif()
