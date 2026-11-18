# Windows test subset for fipsd (nostrc-iq1n9.6.1), included by
# CMakeLists.txt in place of the POSIX suite. Same targets, names, labels and
# timeouts as there; only suites whose code paths exist on Windows are listed.
# The control socket is TCP 127.0.0.1 here (FipsdControlTcpTest); omitted for
# now: the AF_UNIX control/native/interop suites, DNS/mDNS/Tor/TCP/nostr
# runtime (stubbed subsystems), the POSIX TUN suites (FipsdTunTest's BSD/Linux
# route codecs and socketpair fakes, FipsdIpv6TwoNodesTest's socketpair
# devices), runtime signals (POSIX signals), FipsdUdpTest (socketpair and the
# SO_REUSEPORT connected pool), config/log (POSIX file modes, fifos,
# symlinks), netmon/transport/watcher (ifaddrs, socketpair fixtures).
add_custom_target(fipsd_tests DEPENDS fipsd)

fipsd_unit_test(fipsd_host_os_test test_host_os.c)
add_test(NAME FipsdHostOsTest COMMAND fipsd_host_os_test)
set_tests_properties(FipsdHostOsTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
fipsd_unit_test(fipsd_cli_test test_cli.c)
add_test(NAME FipsdCliParseTest COMMAND fipsd_cli_test)
set_tests_properties(FipsdCliParseTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
fipsd_unit_test(fipsd_service_status_test test_service_status.c)
add_test(NAME FipsdServiceStatusTest COMMAND fipsd_service_status_test)
set_tests_properties(FipsdServiceStatusTest PROPERTIES TIMEOUT 10 LABELS "fips;unit")
add_dependencies(fipsd_tests fipsd_host_os_test fipsd_cli_test fipsd_service_status_test)

# The control socket on TCP 127.0.0.1 (nostrc-iq1n9.6.3), through the
# production listener and the production fipsctl client, on ephemeral ports.
fipsd_unit_test(fipsd_control_tcp_test test_control_tcp.c)
target_link_libraries(fipsd_control_tcp_test PRIVATE fipsctl_lib)
add_test(NAME FipsdControlTcpTest COMMAND fipsd_control_tcp_test)
add_test(NAME FipsdControlTcpCapacityTest COMMAND fipsd_control_tcp_test capacity)
set_tests_properties(FipsdControlTcpTest FipsdControlTcpCapacityTest PROPERTIES TIMEOUT 60
  LABELS "fips;unit;control" ENVIRONMENT "FIPSD_WORKERS=2")
add_dependencies(fipsd_tests fipsd_control_tcp_test fipsctl)

fipsd_unit_test(fipsd_peer_test test_peer.c)
target_sources(fipsd_peer_test PRIVATE
  test_peer_machine.c test_peer_budget.c test_peer_data.c test_peer_link.c
  test_peer_dispatcher.c "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/fakes/host.c")
target_include_directories(fipsd_peer_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests")
add_test(NAME FipsdPeerTest COMMAND fipsd_peer_test)
set_tests_properties(FipsdPeerTest PROPERTIES TIMEOUT 60 LABELS "fips;unit" ENVIRONMENT "FIPSD_WORKERS=2")
add_dependencies(fipsd_tests fipsd_peer_test)

fipsd_unit_test(fipsd_node_lifecycle_test test_node_lifecycle.c)
fipsd_unit_test(fipsd_node_workers_test test_node_workers.c)
fipsd_unit_test(fipsd_node_context_test test_node_context.c)
add_dependencies(fipsd_tests fipsd_node_lifecycle_test fipsd_node_workers_test fipsd_node_context_test)
add_test(NAME FipsdNodeLifecycleTest COMMAND fipsd_node_lifecycle_test)
add_test(NAME FipsdNodeLifecycleFailureTest COMMAND fipsd_node_lifecycle_test failure)
add_test(NAME FipsdNodeWorkersTest COMMAND fipsd_node_workers_test)
add_test(NAME FipsdNodeContextTest COMMAND fipsd_node_context_test)
add_test(NAME FipsdNodeIdentityFailureTest COMMAND fipsd_node_context_test failure)
add_test(NAME FipsdNodeHandlerFailureTest COMMAND fipsd_node_context_test handler)
set_tests_properties(FipsdNodeLifecycleTest FipsdNodeLifecycleFailureTest FipsdNodeWorkersTest
  FipsdNodeContextTest FipsdNodeIdentityFailureTest FipsdNodeHandlerFailureTest
  PROPERTIES TIMEOUT 30 LABELS "fips;node" ENVIRONMENT "FIPSD_WORKERS=2")

# The two-node proof on the in-process loopback transport and on real UDP
# sockets (IPv4 and IPv6 loopback), i.e. through libgo's completion port:
# IK link, tree, bloom, XK session, datagrams, rekey, disconnect.
fipsd_unit_test(fipsd_node_two_nodes_test test_node_two_nodes.c)
add_test(NAME FipsdNodeTwoNodesTest COMMAND fipsd_node_two_nodes_test)
set_tests_properties(FipsdNodeTwoNodesTest PROPERTIES TIMEOUT 300 LABELS "fips;node" ENVIRONMENT "FIPSD_WORKERS=2")
add_dependencies(fipsd_tests fipsd_node_two_nodes_test)
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
endforeach()

fipsd_unit_test(fipsd_node_handlers_test test_node_handlers.c)
add_test(NAME FipsdNodeHandlersTest COMMAND fipsd_node_handlers_test)
set_tests_properties(FipsdNodeHandlersTest PROPERTIES TIMEOUT 300 LABELS "fips;node" ENVIRONMENT "FIPSD_WORKERS=2")
add_dependencies(fipsd_tests fipsd_node_handlers_test)

fipsd_unit_test(fipsd_node_stp_acks_test test_node_stp_acks.c)
target_sources(fipsd_node_stp_acks_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/fakes/host.c")
target_include_directories(fipsd_node_stp_acks_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests")
add_test(NAME FipsdNodeStpAcksTest COMMAND fipsd_node_stp_acks_test)
add_test(NAME FipsdNodeStpAcksNoReportTest COMMAND fipsd_node_stp_acks_test no-reports)
set_tests_properties(FipsdNodeStpAcksTest FipsdNodeStpAcksNoReportTest PROPERTIES TIMEOUT 60 LABELS "fips;node"
  ENVIRONMENT "FIPSD_WORKERS=2")
add_dependencies(fipsd_tests fipsd_node_stp_acks_test)

fipsd_unit_test(fipsd_node_path_presence_test test_node_path_presence.c)
add_test(NAME FipsdNodePathPresenceTest COMMAND fipsd_node_path_presence_test)
set_tests_properties(FipsdNodePathPresenceTest PROPERTIES TIMEOUT 120 LABELS "fips;node" ENVIRONMENT "FIPSD_WORKERS=2")
add_dependencies(fipsd_tests fipsd_node_path_presence_test)

fipsd_unit_test(fipsd_dataplane_test test_dataplane.c)
target_sources(fipsd_dataplane_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/fakes/host.c")
target_include_directories(fipsd_dataplane_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests")
add_test(NAME FipsdDataplaneTest COMMAND fipsd_dataplane_test)
set_tests_properties(FipsdDataplaneTest PROPERTIES TIMEOUT 60 LABELS "fips;node" ENVIRONMENT "FIPSD_WORKERS=4")
add_dependencies(fipsd_tests fipsd_dataplane_test)

fipsd_unit_test(fipsd_acl_test test_acl.c)
target_compile_definitions(fipsd_acl_test PRIVATE FIPSD_CONTROL_GOLDEN="${CMAKE_CURRENT_SOURCE_DIR}/control_golden")
fipsd_unit_test(fipsd_node_acl_test test_node_acl.c)
target_sources(fipsd_node_acl_test PRIVATE test_node_acl_admission.c handshake_fixture.c)
add_test(NAME FipsdAclTest COMMAND fipsd_acl_test)
add_test(NAME FipsdNodeAclTest COMMAND fipsd_node_acl_test)
add_test(NAME FipsdNodeAclAdmissionTest COMMAND fipsd_node_acl_test admission)
set_tests_properties(FipsdAclTest FipsdNodeAclTest PROPERTIES TIMEOUT 120 LABELS "fips;node;acl" ENVIRONMENT "FIPSD_WORKERS=2")
set_tests_properties(FipsdNodeAclAdmissionTest PROPERTIES TIMEOUT 60 LABELS "fips;node;acl" ENVIRONMENT "FIPSD_WORKERS=2")
add_dependencies(fipsd_tests fipsd_acl_test fipsd_node_acl_test)

fipsd_unit_test(fipsd_node_restart_replay_test test_node_restart_replay.c)
target_sources(fipsd_node_restart_replay_test PRIVATE handshake_fixture.c)
add_test(NAME FipsdNodeRestartReplayTest COMMAND fipsd_node_restart_replay_test)
set_tests_properties(FipsdNodeRestartReplayTest PROPERTIES TIMEOUT 60 LABELS "fips;node;acl" ENVIRONMENT "FIPSD_WORKERS=2")
add_dependencies(fipsd_tests fipsd_node_restart_replay_test)

fipsd_unit_test(fipsd_stats_test test_stats.c)
add_test(NAME FipsdStatsTest COMMAND fipsd_stats_test)
set_tests_properties(FipsdStatsTest PROPERTIES TIMEOUT 60 LABELS "fips;unit;stats")
add_dependencies(fipsd_tests fipsd_stats_test)

fipsd_unit_test(fipsd_nostr_events_test test_nostr_events.c)
target_sources(fipsd_nostr_events_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/fakes/host.c")
target_include_directories(fipsd_nostr_events_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests")
target_compile_definitions(fipsd_nostr_events_test PRIVATE
  FIPSD_NOSTR_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/../../libfips-core/tests/vectors")
add_test(NAME FipsdNostrEventsTest COMMAND fipsd_nostr_events_test)
set_tests_properties(FipsdNostrEventsTest PROPERTIES TIMEOUT 60 LABELS "fips;nostr;vectors")
add_dependencies(fipsd_tests fipsd_nostr_events_test)

# Wintun TUN, IP Helper rows and the IPv6 shim (nostrc-iq1n9.6.2).
fipsd_unit_test(fipsd_tun_winnet_test test_tun_winnet.c)
target_sources(fipsd_tun_winnet_test PRIVATE test_tun_winnet_mib.c)
add_test(NAME FipsdTunWinnetTest COMMAND fipsd_tun_winnet_test)
set_tests_properties(FipsdTunWinnetTest PROPERTIES TIMEOUT 30 LABELS "fips;tun;unit")
# The backend on a fake Wintun session: no driver, no elevation.
fipsd_unit_test(fipsd_tun_windows_test test_tun_windows.c)
add_test(NAME FipsdTunWindowsTest COMMAND fipsd_tun_windows_test)
set_tests_properties(FipsdTunWindowsTest PROPERTIES TIMEOUT 120 LABELS "fips;tun" ENVIRONMENT "FIPSD_WORKERS=2")
# Real adapters: skip (77) unless elevated and FIPSD_TUN_PRIVILEGED_SMOKE=1;
# then wintun.dll must sit beside the test executable.
fipsd_unit_test(fipsd_tun_privileged_test test_tun_windows_privileged.c)
add_test(NAME FipsdTunPrivilegedSmokeTest COMMAND fipsd_tun_privileged_test device)
add_test(NAME FipsdIpv6PrivilegedSmokeTest COMMAND fipsd_tun_privileged_test shim)
set_tests_properties(FipsdTunPrivilegedSmokeTest PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
  LABELS "fips;tun;privileged" ENVIRONMENT "FIPSD_WORKERS=2")
set_tests_properties(FipsdIpv6PrivilegedSmokeTest PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
  LABELS "fips;tun;ipv6;privileged" ENVIRONMENT "FIPSD_WORKERS=2")
# Two daemons on two Wintun adapters, ping and TCP across the mesh between
# their addresses; same opt-in, wintun.dll beside fipsd.exe.
find_program(FIPSD_POWERSHELL powershell)
if(FIPSD_POWERSHELL)
  add_test(NAME FipsdWindowsTwoNodeSmokeTest COMMAND "${FIPSD_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/windows_two_node_smoke.ps1" -Fipsd "$<TARGET_FILE:fipsd>")
  set_tests_properties(FipsdWindowsTwoNodeSmokeTest PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
    LABELS "fips;tun;ipv6;privileged;integration")
  nostrc_describe_test(FipsdWindowsTwoNodeSmokeTest OWNER fipsd TARGETS fipsd
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/windows_two_node_smoke.ps1")
  # The service end to end (nostrc-iq1n9.6.3): ZIP, install-service.ps1, the
  # SCM, fipsctl over TCP 127.0.0.1:21210, a second node pinging across the
  # mesh, stop and uninstall. Installs machine-wide: its own opt-in.
  get_filename_component(_fipsd_toolchain_bin "${CMAKE_C_COMPILER}" DIRECTORY)
  add_test(NAME FipsdWindowsServiceSmokeTest COMMAND "${FIPSD_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
    -File "${CMAKE_CURRENT_SOURCE_DIR}/windows_service_smoke.ps1" -BuildDir "${CMAKE_BINARY_DIR}"
    -DllPath "${_fipsd_toolchain_bin}")
  set_tests_properties(FipsdWindowsServiceSmokeTest PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77
    LABELS "fips;tun;ipv6;privileged;integration;packaging")
  nostrc_describe_test(FipsdWindowsServiceSmokeTest OWNER fipsd TARGETS fipsd fipsctl
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/windows_service_smoke.ps1"
      "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/windows")
endif()
fipsd_unit_test(fipsd_ipv6_codec_test test_ipv6_codec.c)
fipsd_unit_test(fipsd_ipv6_icmp_test test_ipv6_icmp.c)
fipsd_unit_test(fipsd_ipv6_firewall_test test_ipv6_firewall.c)
add_test(NAME FipsdIpv6CodecTest COMMAND fipsd_ipv6_codec_test)
add_test(NAME FipsdIpv6IcmpTest COMMAND fipsd_ipv6_icmp_test)
add_test(NAME FipsdIpv6FirewallTest COMMAND fipsd_ipv6_firewall_test)
set_tests_properties(FipsdIpv6CodecTest FipsdIpv6IcmpTest FipsdIpv6FirewallTest PROPERTIES TIMEOUT 120 LABELS "fips;tun;ipv6")
# One unreachable peer does not fail the shared UDP socket (SIO_UDP_CONNRESET).
fipsd_unit_test(fipsd_udp_connreset_test test_udp_connreset.c)
add_test(NAME FipsdUdpConnResetTest COMMAND fipsd_udp_connreset_test)
set_tests_properties(FipsdUdpConnResetTest PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 LABELS "fips;transport;unit")
add_dependencies(fipsd_tests fipsd_tun_winnet_test fipsd_tun_windows_test fipsd_tun_privileged_test
  fipsd_ipv6_codec_test fipsd_ipv6_icmp_test fipsd_ipv6_firewall_test fipsd_udp_connreset_test)

foreach(_test FipsdHostOsTest FipsdCliParseTest FipsdServiceStatusTest FipsdControlTcpTest
    FipsdControlTcpCapacityTest FipsdPeerTest FipsdNodeLifecycleTest
    FipsdNodeLifecycleFailureTest FipsdNodeWorkersTest FipsdNodeContextTest FipsdNodeIdentityFailureTest
    FipsdNodeHandlerFailureTest FipsdNodeTwoNodesTest FipsdUdpTwoNodesV4Test FipsdUdpTwoNodesV6Test
    FipsdNodeHandlersTest FipsdNodeStpAcksTest FipsdNodeStpAcksNoReportTest FipsdNodePathPresenceTest
    FipsdDataplaneTest FipsdAclTest FipsdNodeAclTest FipsdNodeAclAdmissionTest FipsdNodeRestartReplayTest
    FipsdStatsTest FipsdNostrEventsTest FipsdTunWinnetTest FipsdTunWindowsTest FipsdTunPrivilegedSmokeTest
    FipsdIpv6PrivilegedSmokeTest FipsdIpv6CodecTest FipsdIpv6IcmpTest FipsdIpv6FirewallTest FipsdUdpConnResetTest)
  get_test_property(${_test} COMMAND _cmd)
  list(GET _cmd 0 _target)
  nostrc_describe_test(${_test} OWNER fipsd TARGETS ${_target})
endforeach()
