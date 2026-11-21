cmake_minimum_required(VERSION 3.22)
project(libfips-core C)

option(FIPS_ENABLE_FUZZERS "Build clang libFuzzer harnesses" OFF)

declare_component_version(fips_core 0 1 0)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FIPS_CORE_SECP256K1 REQUIRED IMPORTED_TARGET libsecp256k1)
pkg_check_modules(FIPS_CORE_SODIUM REQUIRED IMPORTED_TARGET libsodium)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)

# IETF ChaCha20-Poly1305 backend for src/noise/aead_primitive.c (nostrc-u19cz.2).
# Both produce identical bytes; libsodium stays required for everything else.
set(FIPS_AEAD_BACKEND "openssl" CACHE STRING "Noise AEAD backend: openssl or sodium")
set_property(CACHE FIPS_AEAD_BACKEND PROPERTY STRINGS openssl sodium)
if(FIPS_AEAD_BACKEND STREQUAL "openssl")
  set(FIPS_AEAD_BACKEND_DEFINE FIPS_AEAD_BACKEND_OPENSSL)
elseif(FIPS_AEAD_BACKEND STREQUAL "sodium")
  set(FIPS_AEAD_BACKEND_DEFINE FIPS_AEAD_BACKEND_SODIUM)
else()
  message(FATAL_ERROR "FIPS_AEAD_BACKEND must be openssl or sodium, not '${FIPS_AEAD_BACKEND}'")
endif()

# Follow BUILD_SHARED_LIBS, as the other nostrc libraries do.
add_library(fips_core src/core.c src/host.c)

# Proto: bounds-checked wire primitives.
target_sources(fips_core PRIVATE src/proto/codec.c)
# Transport value types (sans-IO).
target_sources(fips_core PRIVATE src/transport/types.c)
# Proto rate-limit primitives.
target_sources(fips_core PRIVATE src/proto/rate_limit.c)

# Node bookkeeping (sans-IO, explicit host instants).
target_sources(fips_core PRIVATE src/node/rate_limit.c src/node/stats_history.c)

# Coordinate cache (sans-IO; caller-owned slots and eviction indexes).
target_sources(fips_core PRIVATE src/cache/coord_cache.c src/cache/entry.c)
if(UNIX)
  target_link_libraries(fips_core PRIVATE m)
  set(FIPS_CORE_PC_LIBS_PRIVATE "-lm")
endif()
set_target_properties(fips_core PROPERTIES
  C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO
  COMPONENT_VERSION_HEADER_INSTALL_DIR "${CMAKE_INSTALL_INCLUDEDIR}/fips")
target_include_directories(fips_core PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE "${fips_core_VERSION_HEADER_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(fips_core PRIVATE
  PkgConfig::FIPS_CORE_SECP256K1 PkgConfig::FIPS_CORE_SODIUM OpenSSL::Crypto)
# The OpenSSL AEAD backend keeps one EVP context per thread in a pthread key.
if(FIPS_AEAD_BACKEND STREQUAL "openssl")
  find_package(Threads REQUIRED)
  target_link_libraries(fips_core PRIVATE Threads::Threads)
  if(CMAKE_THREAD_LIBS_INIT)
    string(APPEND FIPS_CORE_PC_LIBS_PRIVATE " ${CMAKE_THREAD_LIBS_INIT}")
  endif()
endif()
nostrc_library_visibility(fips_core FIPS_CORE_API fips/export.h
  "${CMAKE_INSTALL_INCLUDEDIR}/fips")
apply_versioning(fips_core)
apply_warnings(fips_core)
if(GO_ENABLE_TSAN)
  # Override inherited workspace instrumentation: threading belongs to fipsd.
  target_compile_options(fips_core PRIVATE -fno-sanitize=thread)
  # Uninstrumented, but the cached OpenSSL context is shared by fibers of one
  # OS thread: annotate its tenures for the TSan runtime fipsd links
  # (src/noise/aead_primitive.c, nostrc-u19cz.16).
  target_compile_definitions(fips_core PRIVATE FIPS_AEAD_TSAN_ANNOTATE=1)
else()
  apply_sanitizers(fips_core)
endif()

set(FIPS_CORE_PC_STATIC_CFLAGS "")
if(NOT BUILD_SHARED_LIBS)
  set(FIPS_CORE_PC_STATIC_CFLAGS "-DFIPS_CORE_STATIC_DEFINE")
endif()
configure_file(libfips-core.pc.in libfips-core.pc @ONLY)
install(TARGETS fips_core
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/fips DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libfips-core.pc"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

if(FIPS_ENABLE_FUZZERS)
  add_subdirectory(fuzz)
endif()

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

# Shared utilities (nostrc-u50yr.3): bounded caller storage, host entropy only.
target_sources(fips_core PRIVATE
  src/utils/index.c src/utils/hash.c src/utils/onlink.c src/proto/math.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(src/proto/math.c PROPERTIES
    COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
endif()
# Identity primitives (explicit host entropy, no hot-path allocations).
target_sources(fips_core PRIVATE
  src/identity/mod.c src/identity/key.c src/identity/local.c
  src/identity/peer.c src/identity/node_addr.c src/identity/address.c
  src/identity/encoding.c src/identity/auth.c src/identity/prefix.c)
option(FIPS_ENABLE_BENCH "Build libfips-core micro-benchmarks (not run by ctest)" OFF)
if(FIPS_ENABLE_BENCH)
  add_subdirectory(bench)
endif()

# Noise DH: SHA-256 of shared x, using the identity context.
target_sources(fips_core PRIVATE src/noise/ecdh.c)
# Noise AEAD (nostrc-pwo3h.1); independent of sibling Noise modules.
target_sources(fips_core PRIVATE src/noise/aead.c src/noise/aead_primitive.c)
set_source_files_properties(src/noise/aead_primitive.c PROPERTIES
  COMPILE_DEFINITIONS "${FIPS_AEAD_BACKEND_DEFINE}")
# Tree coordinate primitive and wire codec (nostrc-82g9n.1).
target_sources(fips_core PRIVATE src/proto/coord.c)
# Link message catalog, session datagram header and framing/path-MTU admit math
# (nostrc-82g9n.6): size constants by value, no FMP/FSP frame codec.
target_sources(fips_core PRIVATE src/proto/link.c src/proto/framing.c)

# FMP link wire/lifecycle (nostrc-82g9n.7); Noise/rekey profiles are separate.
target_sources(fips_core PRIVATE
  src/proto/fmp/wire.c src/proto/fmp/core.c
  src/proto/fmp/state.c src/proto/fmp/silent.c src/proto/fmp/limits.c)
# Spanning-tree protocol (nostrc-82g9n.2), sans-IO state + host action list.
target_sources(fips_core PRIVATE
  src/proto/stp/declaration.c src/proto/stp/wire.c src/proto/stp/limits.c
  src/proto/stp/state.c src/proto/stp/core.c src/proto/stp/acks.c)
# Noise SymmetricState (nostrc-pwo3h.3): ck/h chaining, HKDF over libsodium HMAC,
# handshake-phase encrypt/decrypt-and-hash over the AEAD, split.
target_sources(fips_core PRIVATE src/noise/symmetric.c)
# Bloom reachability, announcement wire codecs and bounded sans-IO state (nostrc-82g9n.3).
target_sources(fips_core PRIVATE
  src/proto/bloom/core.c src/proto/bloom/wire.c
  src/proto/bloom/state.c src/proto/bloom/delivery.c src/proto/bloom/log.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(src/proto/bloom/core.c src/proto/bloom/log.c PROPERTIES
    COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
endif()

# FSP end-to-end session wire/core/quorum (nostrc-82g9n.8); no rekey/Noise engine.
target_sources(fips_core PRIVATE
  src/proto/fsp/wire.c src/proto/fsp/core.c src/proto/fsp/quorum.c)
# Routing decision core and error signals (nostrc-82g9n.4).
target_sources(fips_core PRIVATE
  src/proto/routing/core.c src/proto/routing/wire.c
  src/proto/routing/state.c src/proto/routing/limits.c)
# Mesh lookup codecs, bounded retry/dedup state and signing (nostrc-82g9n.5).
target_sources(fips_core PRIVATE
  src/proto/lookup/wire.c src/proto/lookup/state.c
  src/proto/lookup/core.c src/proto/lookup/limits.c)
# IK/XK handshakes and transactional read-on-Fork (nostrc-pwo3h.4/.5/.6).
target_sources(fips_core PRIVATE src/noise/handshake.c)

# Transport ciphers, replay protection and worker key hand-outs (nostrc-pwo3h.7).
target_sources(fips_core PRIVATE src/noise/cipher.c src/noise/replay.c src/noise/session.c)

# Diagnostic probe stages and report vocabulary (nostrc-f12v2.5); no wire codec.
target_sources(fips_core PRIVATE src/proto/probe/core.c src/proto/probe/state.c)
# MMP link/session metrics, reports and delivery (nostrc-f12v2.4).
target_sources(fips_core PRIVATE
  src/proto/mmp/wire.c src/proto/mmp/algorithms.c src/proto/mmp/metrics.c
  src/proto/mmp/sender.c src/proto/mmp/receiver.c src/proto/mmp/delivery.c
  src/proto/mmp/path_mtu.c src/proto/mmp/limits.c src/proto/mmp/state.c src/proto/mmp/core.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(src/proto/mmp/algorithms.c src/proto/mmp/metrics.c
    src/proto/mmp/receiver.c src/proto/mmp/sender.c src/proto/mmp/path_mtu.c PROPERTIES
    COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
endif()
# Shared rekey machine (nostrc-f12v2.1); production profiles are separate.
set(FIPS_REKEY_SOURCES
  src/proto/rekey/mod.c
  src/proto/rekey/machine.c
  src/proto/rekey/params.c
  src/proto/rekey/profile.c
  src/proto/rekey/hooks.c
  src/proto/rekey/access.c
  src/proto/rekey/candidates.c
  src/proto/rekey/cycle.c
  src/proto/rekey/epoch.c
  src/proto/rekey/keys.c
  src/proto/rekey/ladder.c
  src/proto/rekey/lease.c
  src/proto/rekey/proof.c
  src/proto/rekey/reason.c
  src/proto/rekey/record.c
  src/proto/rekey/resolve.c
)
target_sources(fips_core PRIVATE ${FIPS_REKEY_SOURCES})

# Real IK/XK rekey profiles and bounded host/session binding (nostrc-f12v2.2).
target_sources(fips_core PRIVATE
  src/proto/fmp/rekey.c src/proto/fsp/rekey.c src/node/session.c)
# PathSet, path messages and the runtime-gated multipath Bundle (nostrc-f12v2.6).
target_sources(fips_core PRIVATE
  src/proto/path/wire.c src/proto/path/set.c src/proto/path/params.c
  src/proto/path/rules.c src/proto/path/candidate.c src/proto/path/capable.c
  src/proto/path/select.c src/proto/path/live.c src/proto/path/machine.c)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(src/proto/path/select.c src/proto/path/live.c PROPERTIES
    COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
endif()

# Peer settlement primitives at 64f5b4d482cf (table/gates/judge remain separate).
set(FIPS_SETTLE_SOURCES src/proto/settle/params.c src/proto/settle/links.c src/proto/settle/legs.c src/proto/settle/gates.c src/proto/settle/table.c)
target_sources(fips_core PRIVATE ${FIPS_SETTLE_SOURCES})
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(src/proto/settle/params.c PROPERTIES
    COMPILE_OPTIONS "-fno-fast-math;-ffp-contract=off")
endif()
