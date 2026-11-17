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
# FIPSD_GIT_REV overrides the probe: the package recipes under packaging/
# build an exported tree and pass the revision it came from.
set(FIPSD_GIT_REV "" CACHE STRING
  "Revision to embed in fipsd -V instead of probing git (exported-tree builds)")
set(FIPSD_BUILD_INFO_DIR "${CMAKE_CURRENT_BINARY_DIR}/build-info")
set(FIPSD_BUILD_INFO_HEADER "${FIPSD_BUILD_INFO_DIR}/fipsd_build_info.h")
add_custom_target(fipsd_build_info
  COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DOUTPUT=${FIPSD_BUILD_INFO_HEADER}"
    "-DTARGET_TRIPLE=${FIPSD_TARGET_TRIPLE}"
    "-DGIT_REV_OVERRIDE=${FIPSD_GIT_REV}"
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
# fipsd is added before the top-level CMakeLists applies its non-Apple
# -D_POSIX_C_SOURCE=200809L, and the sources compile as strict C11, so glibc
# would hide strnlen/strdup/clock_gettime from the files that do not define
# the macro themselves. PUBLIC so the daemon, tests and fuzzers see the same
# namespace as fipsd_lib; the value matches every in-file definition.
if(NOT APPLE)
  target_compile_definitions(fipsd_lib PUBLIC _POSIX_C_SOURCE=200809L)
endif()
apply_warnings(fipsd_lib)
apply_sanitizers(fipsd_lib)

add_executable(fipsd src/main.c)
set_target_properties(fipsd PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(fipsd PRIVATE fipsd_lib nostr)
apply_warnings(fipsd)
apply_sanitizers(fipsd)
include(CheckPIESupported)
check_pie_supported(LANGUAGES C)
set_target_properties(fipsd PROPERTIES POSITION_INDEPENDENT_CODE TRUE)
install(TARGETS fipsd RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# Peer owners and dispatcher (nostrc-9keoz.4); profiles remain in libfips-core.
target_sources(fipsd_lib PRIVATE
  src/peer/active.c src/peer/data.c src/peer/machine.c src/peer/budget.c
  src/peer/identities.c src/peer/table.c src/peer/dispatcher.c)
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

# Node host lifecycle and transport registration (nostrc-9keoz.5).
target_sources(fipsd_lib PRIVATE
  src/node/lifecycle.c src/node/context.c src/node/transport.c
  src/node/worker_set.c src/node/netmon.c)
# Unix control protocol and immutable provider projections (nostrc-9keoz.3).
pkg_check_modules(FIPSD_JANSSON REQUIRED IMPORTED_TARGET jansson)
target_link_libraries(fipsd_lib PUBLIC PkgConfig::FIPSD_JANSSON)
target_sources(fipsd_lib PRIVATE
  src/control/control.c src/control/protocol.c src/control/queries.c)
target_sources(fipsd_lib PRIVATE src/control/probe.c)
target_sources(fipsd_lib PRIVATE src/control/render.c)

# In-process loopback transport behind the transport seam (nostrc-9keoz.6).
target_sources(fipsd_lib PRIVATE src/node/transport_loopback.c)
# Node protocol handlers (nostrc-9keoz.6).
target_sources(fipsd_lib PRIVATE
  src/node/handlers/handlers.c src/node/handlers/handshake.c src/node/handlers/rekey.c
  src/node/handlers/session.c src/node/handlers/tree.c src/node/handlers/bloom.c
  src/node/handlers/lookup.c src/node/handlers/mmp.c src/node/handlers/path.c
  src/node/handlers/probe.c src/node/handlers/timeout.c src/node/handlers/native.c
  src/node/handlers/netmon.c src/node/handlers/provider.c)

# Snapshot dataplane and bounded packet crypto (nostrc-9keoz.7).
target_sources(fipsd_lib PRIVATE
  src/node/encrypt_worker.c src/node/decrypt_worker.c
  src/node/dataplane/snapshot.c src/node/dataplane/rx_loop.c
  src/node/dataplane/encrypted.c src/node/dataplane/forwarding.c
  src/node/dataplane/peer_actions.c src/node/dataplane/dispatch.c
  src/node/dataplane/reject.c src/node/dataplane/path_mtu.c)

# ACL, counter registry, history sampling and diagnostic vocabulary (9keoz.8).
target_sources(fipsd_lib PRIVATE src/node/acl.c src/node/metrics.c src/node/stats.c src/node/diag.c)
# FIPS_ENABLE_FUZZERS also includes the daemon's untrusted parser surfaces.
option(FIPSD_ENABLE_FUZZERS "Build fipsd parser libFuzzer targets" ${FIPS_ENABLE_FUZZERS})
if(FIPSD_ENABLE_FUZZERS)
  add_subdirectory(fuzz)
endif()

# Tier B2 shared transport owners (nostrc-gfi8h.1).
target_sources(fipsd_lib PRIVATE
  src/transport/transport.c src/transport/framing.c src/transport/stream.c
  src/transport/medium.c src/transport/presence.c src/transport/sockbuf.c
  src/transport/watcher.c src/transport/stats_common.c src/transport/loopback.c)
# Pure Nostr rendezvous event contracts (nostrc-8hvs6.1); no relay I/O.
target_sources(fipsd_lib PRIVATE src/nostr/events.c src/nostr/advert.c
  src/nostr/traversal.c src/nostr/inbox.c)
target_link_libraries(fipsd_lib PUBLIC nostr nostr_nip17_core nostr_nip59_core
  nostr_nip44_core nip40)

# Tier B4 device owner; IPv6 packet adaptation is a separate layer.
target_sources(fipsd_lib PRIVATE src/tun/tun.c src/tun/route.c
  src/tun/lifecycle.c src/tun/status.c)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(fipsd_lib PRIVATE src/tun/tun_linux.c)
elseif(APPLE)
  target_sources(fipsd_lib PRIVATE src/tun/tun_macos.c src/tun/tun_bsd.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  target_sources(fipsd_lib PRIVATE src/tun/tun_freebsd.c src/tun/tun_bsd.c)
else()
  message(FATAL_ERROR "fipsd TUN requires Linux, macOS or FreeBSD")
endif()
# UDP datagrams and authenticated connected-peer drains (nostrc-gfi8h.2).
target_sources(fipsd_lib PRIVATE src/transport/udp/udp.c src/transport/udp/io.c
  src/transport/udp/connected.c src/transport/udp/classify.c src/transport/udp/dns.c)
# Relay-backed rendezvous actor and node/traversal driver (nostrc-8hvs6.2).
target_sources(fipsd_lib PRIVATE src/nostr/runtime.c src/nostr/driver.c
  src/nostr/failure_state.c src/nostr/signal_gate.c src/nostr/signal_route.c)
target_link_libraries(fipsd_lib PUBLIC nip11)

# TCP stream owners (nostrc-gfi8h.3), consuming the shared framing layer.
target_sources(fipsd_lib PRIVATE src/transport/tcp/tcp.c src/transport/tcp/listener.c
  src/transport/tcp/conn.c src/transport/tcp/pool.c src/transport/tcp/stats.c)
# STUN and deadline-driven traversal (nostrc-8hvs6.3).
target_sources(fipsd_lib PRIVATE src/nostr/stun.c src/nostr/traversal_machine.c src/nostr/traversal_io.c)
# IPv6 shim between TUN frames and session datagrams (nostrc-9nxwk.2).
target_sources(fipsd_lib PRIVATE
  src/ipv6/mtu.c src/ipv6/shim.c src/ipv6/exthdr.c src/ipv6/tcp_mss.c src/ipv6/mss.c
  src/ipv6/icmp_rate_limit.c src/ipv6/icmp.c src/ipv6/frag.c src/ipv6/queue.c src/ipv6/inbound.c
  src/ipv6/outbound.c src/ipv6/link.c src/ipv6/listening.c src/ipv6/firewall_state.c)

# Native datagram API (nostrc-9nxwk.4): the SOCK_SEQPACKET line-protocol
# server, flow registry and SCM_RIGHTS hand-off inside the daemon, and
# libfipsnative, a plain-POSIX C client (no libgo, no libfips-core) that other
# programs link to speak the API.
target_sources(fipsd_lib PRIVATE
  src/native/protocol.c src/native/registry.c src/native/seqpacket.c src/native/fdpass.c
  src/native/link.c src/native/dgram_probe.c src/native/api.c)
declare_component_version(fipsnative 0 1 0)
add_library(fipsnative SHARED src/native/client.c)
add_library(fipsnative_static STATIC src/native/client.c)
foreach(_fipsnative_target fipsnative fipsnative_static)
  set_target_properties(${_fipsnative_target} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO OUTPUT_NAME fipsnative
    POSITION_INDEPENDENT_CODE TRUE PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/fips-native-client.h")
  target_include_directories(${_fipsnative_target} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
  target_include_directories(${_fipsnative_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  apply_warnings(${_fipsnative_target})
  apply_sanitizers(${_fipsnative_target})
endforeach()
set(fipsnative_SOVERSION "${fipsnative_VERSION_MAJOR}")
apply_versioning(fipsnative)
configure_file(libfipsnative.pc.in libfipsnative.pc @ONLY)
install(TARGETS fipsnative fipsnative_static
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libfipsnative.pc" DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
add_subdirectory(examples)
# Coordinate hints must precede multi-hop routing (nostrc-f66sr.2).
target_sources(fipsd_lib PRIVATE src/node/dataplane/coord_warm.c)

# Shared upstream transport control projection (nostrc-gfi8h.7).
target_sources(fipsd_lib PRIVATE src/control/transport.c)
# SOCKS5 client, the Tor onion transport with its control-port monitor, and
# the Nym mixnet transport behind a feature flag (nostrc-gfi8h.4). All three
# are stream owners over src/transport/tcp through its host-only dialer hook.
target_sources(fipsd_lib PRIVATE src/transport/socks5/socks5.c src/transport/tor/tor.c
  src/transport/tor/onion.c src/transport/tor/control.c)
option(FIPSD_ENABLE_NYM "Build the Nym mixnet transport (SOCKS5 through nym-socks5-client)" OFF)
if(FIPSD_ENABLE_NYM)
  target_sources(fipsd_lib PRIVATE src/transport/nym/nym.c)
  target_compile_definitions(fipsd_lib PUBLIC FIPSD_ENABLE_NYM=1)
else()
  target_sources(fipsd_lib PRIVATE src/transport/nym/nym_disabled.c)
endif()
# The .fips DNS responder (nostrc-9nxwk.3): message codec, host map with
# mtime reload, resolver/mesh filter and the dns runtime subsystem.
target_sources(fipsd_lib PRIVATE src/dns/msg.c src/dns/hosts.c src/dns/resolver.c src/dns/service.c)
# Packaging payload (nostrc-iq1n9.5). Every package format under packaging/
# (Debian, RPM, Arch, macOS .pkg) stages the daemon with
# `cmake --install <build>/fipsd`, so what a package ships is declared here
# once and the recipes only list it. Paths outside the prefix are the FULL
# GNUInstallDirs values: with prefix /usr the example config lands in
# /usr/share/fips and the conffiles in /etc/fips; with /usr/local (macOS,
# `make install`) in /usr/local/share/fips and /usr/local/etc/fips, which
# is where the daemon's default search order looks.
set(FIPSD_PACKAGING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/packaging")
install(FILES "${FIPSD_PACKAGING_DIR}/common/fips.yaml" RENAME fips.yaml.example
  DESTINATION "${CMAKE_INSTALL_DATADIR}/fips")
# hosts and fips.nft are operator-editable: the Debian package marks /etc
# conffiles automatically, the RPM spec uses %config(noreplace), Arch lists
# them in backup=, and the macOS installer renames them to .default and
# seeds the live copies only when absent.
install(FILES "${FIPSD_PACKAGING_DIR}/common/hosts" "${FIPSD_PACKAGING_DIR}/common/fips.nft"
  DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/fips")
# Drop-in directory for operator nftables rules included by fips.nft; the
# include glob in that file matches nothing cleanly when it is empty.
install(DIRECTORY DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/fips/fips.d")
# fipsd links libnostr-json, which libjson/ builds shared only (D-4) and the
# desktop apps ship as a private copy beside themselves. The daemon does the
# same: the library goes under <libdir>/fipsd and the binary carries that
# RPATH, so the packages depend on no libnostr-json package and cannot
# collide with the headless login stack's public one.
set(FIPSD_PRIVATE_LIBDIR "${CMAKE_INSTALL_LIBDIR}/fipsd")
install(TARGETS nostr_json LIBRARY DESTINATION "${FIPSD_PRIVATE_LIBDIR}" NAMELINK_SKIP)
if(APPLE)
  set(FIPSD_INSTALL_RPATH "@loader_path/../${FIPSD_PRIVATE_LIBDIR}")
else()
  set(FIPSD_INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}/fipsd")
endif()
set_property(TARGET fipsd APPEND PROPERTY INSTALL_RPATH "${FIPSD_INSTALL_RPATH}")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # systemd units are generated from packaging/systemd/*.service.in so an
  # install to another prefix runs the binaries it installed. Relative
  # destinations: /usr/lib/systemd/system for the packages, and
  # /usr/local/lib/systemd/system (which systemd also reads) for a local
  # `make install`.
  set(FIPSD_SYSTEMD_UNIT_DIR "lib/systemd/system" CACHE PATH
    "Where fipsd's systemd units are installed (relative to the prefix or absolute)")
  # The fips-dns.service helpers (packaging/common/fips-dns-setup and
  # -teardown) live under <prefix>/lib/fips, as upstream's packages place
  # them. The unit carries ConditionPathExists on the setup script, so a
  # build from a tree without the helpers installs a unit that is skipped
  # (inactive, not failed) rather than one that cannot start.
  set(FIPSD_DNS_HELPER_DIR "${CMAKE_INSTALL_PREFIX}/lib/fips")
  foreach(_fipsd_unit fipsd fips-dns fips-firewall)
    configure_file("${FIPSD_PACKAGING_DIR}/systemd/${_fipsd_unit}.service.in"
      "${CMAKE_CURRENT_BINARY_DIR}/packaging/${_fipsd_unit}.service" @ONLY)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/packaging/${_fipsd_unit}.service"
      DESTINATION "${FIPSD_SYSTEMD_UNIT_DIR}")
  endforeach()
  # /run/fips (0750 root:fips) for the control and native-API sockets, and
  # the fips system group that gates access to them (privilege.h).
  install(FILES "${FIPSD_PACKAGING_DIR}/common/fips.tmpfiles" RENAME fipsd.conf
    DESTINATION lib/tmpfiles.d)
  install(FILES "${FIPSD_PACKAGING_DIR}/common/fips.sysusers" RENAME fipsd.conf
    DESTINATION lib/sysusers.d)
  foreach(_fipsd_helper fips-dns-setup fips-dns-teardown)
    if(EXISTS "${FIPSD_PACKAGING_DIR}/common/${_fipsd_helper}")
      install(PROGRAMS "${FIPSD_PACKAGING_DIR}/common/${_fipsd_helper}" DESTINATION lib/fips)
    endif()
  endforeach()
endif()

# fipsctl (the control-socket client) is added to this directory by its own
# task; install it when the target exists. Deferred to the end of this
# directory so the check runs after every add_executable above or below.
function(fipsd_install_optional_tools)
  if(TARGET fipsctl)
    set_property(TARGET fipsctl APPEND PROPERTY INSTALL_RPATH "${FIPSD_INSTALL_RPATH}")
    install(TARGETS fipsctl RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" CALL fipsd_install_optional_tools)
# fipsctl (nostrc-iq1n9.1): the control client, plain POSIX C over jansson
# and libfips-core (identities, addresses, key files); no libgo, no fipsd_lib.
# Everything but main() is fipsctl_lib so the unit test links the code the
# tool runs. Installed beside fipsd.
declare_component_version(fipsctl 0 1 0)
add_library(fipsctl_lib STATIC
  src/fipsctl/commands.c src/fipsctl/client.c src/fipsctl/render.c
  src/fipsctl/npub.c src/fipsctl/path.c src/fipsctl/probe.c)
add_dependencies(fipsctl_lib fipsd_build_info)
set_target_properties(fipsctl_lib PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(fipsctl_lib PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/fipsctl" "${fipsctl_VERSION_HEADER_DIR}" "${FIPSD_BUILD_INFO_DIR}")
target_link_libraries(fipsctl_lib PUBLIC fips_core PkgConfig::FIPSD_JANSSON PkgConfig::FIPSD_SODIUM)
if(NOT APPLE)
  target_compile_definitions(fipsctl_lib PUBLIC _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE)
endif()
option(FIPSCTL_PROFILING "Carry the `profile tick` subcommand (upstream --features profiling)" OFF)
if(FIPSCTL_PROFILING)
  target_compile_definitions(fipsctl_lib PUBLIC FIPSCTL_PROFILING=1)
endif()
apply_warnings(fipsctl_lib)
apply_sanitizers(fipsctl_lib)
add_executable(fipsctl src/fipsctl/main.c)
set_target_properties(fipsctl PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO POSITION_INDEPENDENT_CODE TRUE)
target_link_libraries(fipsctl PRIVATE fipsctl_lib)
apply_warnings(fipsctl)
apply_sanitizers(fipsctl)
install(TARGETS fipsctl RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# fips-gateway (nostrc-iq1n9.3): the outbound LAN gateway. Its pool,
# conntrack, state, NAT rendering and worker, DNS proxy, control socket and
# shutdown modules are portable and form fipsd_gateway_core on every
# platform, so their tests run everywhere; the executable and its libnftables
# backend are Linux-only (nftables and proxy NDP exist nowhere else).
# libnftables (over libnftnl/libmnl) is the C library nft itself is built on:
# one JSON document per rebuild, sent as one netlink batch, so the rule shapes
# are golden-testable text rather than hand-encoded netlink.
declare_component_version(fips-gateway 0 1 0)
add_library(fipsd_gateway_core STATIC
  src/gateway/types.c src/gateway/addrmap.c src/gateway/pool.c src/gateway/conntrack.c
  src/gateway/state.c src/gateway/nat.c src/gateway/net.c src/gateway/dns.c
  src/gateway/control.c src/gateway/shutdown.c)
set_target_properties(fipsd_gateway_core PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(fipsd_gateway_core PUBLIC fipsd_lib)
apply_warnings(fipsd_gateway_core)
apply_sanitizers(fipsd_gateway_core)
# Declared everywhere so the shared package arguments can switch it off on
# every platform; it has an effect on Linux alone.
option(FIPSD_BUILD_GATEWAY "Build the fips-gateway executable (Linux; needs libnftables)" ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(FIPSD_BUILD_GATEWAY)
    pkg_check_modules(FIPSD_NFTABLES IMPORTED_TARGET libnftables)
    if(FIPSD_NFTABLES_FOUND)
      add_executable(fips-gateway src/gateway/main.c src/gateway/nat_nft.c)
      set_target_properties(fips-gateway PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO POSITION_INDEPENDENT_CODE TRUE)
      target_include_directories(fips-gateway PRIVATE "${fips-gateway_VERSION_HEADER_DIR}")
      target_link_libraries(fips-gateway PRIVATE fipsd_gateway_core PkgConfig::FIPSD_NFTABLES)
      apply_warnings(fips-gateway)
      apply_sanitizers(fips-gateway)
      set_property(TARGET fips-gateway APPEND PROPERTY INSTALL_RPATH "${FIPSD_INSTALL_RPATH}")
      install(TARGETS fips-gateway RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
      configure_file("${FIPSD_PACKAGING_DIR}/systemd/fips-gateway.service.in"
        "${CMAKE_CURRENT_BINARY_DIR}/packaging/fips-gateway.service" @ONLY)
      install(FILES "${CMAKE_CURRENT_BINARY_DIR}/packaging/fips-gateway.service"
        DESTINATION "${FIPSD_SYSTEMD_UNIT_DIR}")
      if(TARGET fipsd_tests)
        add_dependencies(fipsd_tests fips-gateway)
      endif()
    else()
      message(STATUS "fips-gateway: libnftables not found (install libnftables-dev); the gateway executable is not built")
    endif()
  endif()
endif()
# The root-only netns smoke of the gateway (nostrc-iq1n9.3): skips (77) unless
# run as root with FIPSD_GATEWAY_PRIVILEGED_SMOKE=1.
if(BUILD_TESTING AND TARGET fips-gateway)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME FipsdGatewayPrivilegedSmokeTest COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/gateway_netns_smoke.py"
    --fipsd "$<TARGET_FILE:fipsd>" --gateway "$<TARGET_FILE:fips-gateway>")
  set_tests_properties(FipsdGatewayPrivilegedSmokeTest PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77
    LABELS "fips;gateway;privileged")
  nostrc_describe_test(FipsdGatewayPrivilegedSmokeTest OWNER fipsd TARGETS fipsd fips-gateway
    INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/gateway_netns_smoke.py" "${CMAKE_CURRENT_SOURCE_DIR}/tests/udp_interop.py")
endif()

# fipstop model is terminal-independent and shares the fipsctl wire/format library.
add_library(fipstop_model STATIC src/fipstop/model.c)
set_target_properties(fipstop_model PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_include_directories(fipstop_model PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/fipstop")
target_link_libraries(fipstop_model PUBLIC fipsctl_lib)
apply_warnings(fipstop_model)
apply_sanitizers(fipstop_model)
if(BUILD_TESTING)
  add_executable(fipstop_model_test tests/test_fipstop_model.c)
  target_link_libraries(fipstop_model_test PRIVATE fipstop_model)
  apply_warnings(fipstop_model_test)
  apply_sanitizers(fipstop_model_test)
  add_dependencies(fipsd_tests fipstop_model_test)
  add_test(NAME FipsTopModelTest COMMAND fipstop_model_test)
  set_tests_properties(FipsTopModelTest PROPERTIES TIMEOUT 30 LABELS "fips;unit;fipstop")
  nostrc_describe_test(FipsTopModelTest OWNER fipsd TARGETS fipstop_model_test)
endif()

# Terminal UI: optional on systems with a pkg-config ncurses implementation.
pkg_search_module(FIPSTOP_CURSES QUIET IMPORTED_TARGET ncursesw ncurses)
option(FIPSD_BUILD_FIPSTOP "Build the fipstop ncurses dashboard" ${FIPSTOP_CURSES_FOUND})
if(FIPSD_BUILD_FIPSTOP)
  if(NOT FIPSTOP_CURSES_FOUND)
    message(FATAL_ERROR "FIPSD_BUILD_FIPSTOP requires ncursesw/ncurses (pkg-config)")
  endif()
  declare_component_version(fipstop 0 1 0)
  add_library(fipstop_ui STATIC src/fipstop/app.c src/fipstop/event.c
    src/fipstop/ui_common.c src/fipstop/ui_node.c src/fipstop/ui_peers.c
    src/fipstop/ui_sessions.c src/fipstop/ui_transports.c src/fipstop/ui_tree.c
    src/fipstop/ui_bloom.c src/fipstop/ui_mmp.c src/fipstop/ui_routing.c
    src/fipstop/ui_graphs.c src/fipstop/ui_gateway.c)
  target_link_libraries(fipstop_ui PUBLIC fipstop_model)
  set_target_properties(fipstop_ui PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  apply_warnings(fipstop_ui)
  apply_sanitizers(fipstop_ui)
  add_executable(fipstop src/fipstop/main.c)
  set_target_properties(fipstop PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  target_compile_definitions(fipstop PRIVATE "FIPSTOP_TARGET=\"${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_SYSTEM_NAME}\"")
  target_link_libraries(fipstop PRIVATE fipstop_ui PkgConfig::FIPSTOP_CURSES)
  apply_warnings(fipstop)
  apply_sanitizers(fipstop)
  set_property(TARGET fipstop APPEND PROPERTY INSTALL_RPATH "${FIPSD_INSTALL_RPATH}")
  install(TARGETS fipstop RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  if(TARGET fipsd_tests)
    add_dependencies(fipsd_tests fipstop)
  endif()
endif()

if(BUILD_TESTING AND TARGET fipstop)
  add_executable(fipstop_test tests/test_fipstop.c)
  target_link_libraries(fipstop_test PRIVATE fipstop_ui)
  target_compile_definitions(fipstop_test PRIVATE "FIPSTOP_FIXTURES=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/control_golden\"")
  set_target_properties(fipstop_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  apply_warnings(fipstop_test)
  apply_sanitizers(fipstop_test)
  add_dependencies(fipsd_tests fipstop_test)
  add_test(NAME FipsTopTest COMMAND fipstop_test)
  set_tests_properties(FipsTopTest PROPERTIES TIMEOUT 30 LABELS "fips;unit;fipstop")
  nostrc_describe_test(FipsTopTest OWNER fipsd TARGETS fipstop_test)
endif()

if(BUILD_TESTING AND TARGET fipstop)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME FipsTopGoldenTest COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fipstop_golden.py" --fipstop "$<TARGET_FILE:fipstop>"
    --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/control_golden"
    --goldens "${CMAKE_CURRENT_SOURCE_DIR}/tests/fipstop_golden")
  add_test(NAME FipsTopE2eTest COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fipstop_e2e.py" --fipsd "$<TARGET_FILE:fipsd>"
    --fipstop "$<TARGET_FILE:fipstop>" --unit "$<TARGET_FILE:fipstop_test>")
  set_tests_properties(FipsTopGoldenTest FipsTopE2eTest PROPERTIES TIMEOUT 180 LABELS "fips;fipstop;integration")
  nostrc_describe_test(FipsTopGoldenTest OWNER fipsd TARGETS fipstop INPUTS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fipstop_golden.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/control_golden" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fipstop_golden")
  nostrc_describe_test(FipsTopE2eTest OWNER fipsd TARGETS fipstop fipstop_test fipsd INPUTS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fipstop_e2e.py" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fipsctl_e2e.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/udp_interop.py")
endif()
