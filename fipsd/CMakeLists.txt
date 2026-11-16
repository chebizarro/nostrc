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
